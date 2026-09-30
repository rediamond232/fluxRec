#include "gpu/direct_nv12.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/log.h>
}

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace fluxrec::gpu::detail {
namespace {

GpuError color_vk_error(std::string operation, VkResult result) {
    operation += " failed with VkResult ";
    operation += std::to_string(static_cast<int>(result));
    return GpuError{std::move(operation), static_cast<int>(result)};
}

AVVkFrame* color_vk_frame(AVFrame* frame) {
    if (!frame || frame->format != AV_PIX_FMT_VULKAN || !frame->data[0]) return nullptr;
    return reinterpret_cast<AVVkFrame*>(frame->data[0]);
}

AVHWFramesContext* color_frames_context(AVFrame* frame) {
    return frame && frame->hw_frames_ctx
        ? reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data) : nullptr;
}

VkImageMemoryBarrier2 color_image_barrier(VkImage image, VkImageLayout old_layout,
                                           VkAccessFlags2 old_access,
                                           std::uint32_t old_queue_family,
                                           std::uint32_t queue_family,
                                           VkAccessFlags2 new_access,
                                           VkImageLayout new_layout,
                                           VkImageAspectFlags aspect,
                                           VkPipelineStageFlags2 new_stage) {
    const bool state_defined = old_layout != VK_IMAGE_LAYOUT_UNDEFINED;
    const bool ownership = old_queue_family != VK_QUEUE_FAMILY_IGNORED &&
                           old_queue_family != queue_family;
    const bool old_video = old_layout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
    return VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = state_defined && !old_video
            ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = state_defined && !old_video ? old_access : 0,
        .dstStageMask = new_stage,
        .dstAccessMask = new_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = ownership ? old_queue_family : VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = ownership ? queue_family : VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = VkImageSubresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
}

} // namespace

struct Nv12ColorCorrector::Impl {
    AVHWFramesContext* input_frames = nullptr;
    AVHWFramesContext* output_frames = nullptr;
    AVVulkanFramesContext* input_vk_frames = nullptr;
    AVVulkanFramesContext* output_vk_frames = nullptr;
    AVHWDeviceContext* device_context = nullptr;
    AVVulkanDeviceContext* vk_context = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    std::vector<Nv12ColorCorrectionResources> available;

    ~Impl() {
        if (device == VK_NULL_HANDLE) return;
        (void)vkDeviceWaitIdle(device);
        for (auto& resources : available) destroy_resources(resources);
        available.clear();
        if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
        if (sampler) vkDestroySampler(device, sampler, nullptr);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (shader) vkDestroyShaderModule(device, shader, nullptr);
        if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        if (descriptor_layout) vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
    }

    bool supports(VkFormat format, VkFormatFeatureFlags features) const {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
        return (properties.optimalTilingFeatures & features) == features;
    }

    std::expected<VkImage, GpuError> create_temp_image(
        std::uint32_t image_width, std::uint32_t image_height, VkFormat format,
        VkDeviceMemory& memory) {
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = format;
        image_info.extent = VkExtent3D{image_width, image_height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage image = VK_NULL_HANDLE;
        VkResult result = vkCreateImage(device, &image_info, nullptr, &image);
        if (result != VK_SUCCESS)
            return std::unexpected(color_vk_error("vkCreateImage for NV12 color-correction plane", result));

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
        std::uint32_t memory_type = UINT32_MAX;
        std::uint32_t fallback_memory_type = UINT32_MAX;
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1U << i)) == 0) continue;
            if (fallback_memory_type == UINT32_MAX) fallback_memory_type = i;
            if ((memory_properties.memoryTypes[i].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                memory_type = i;
                break;
            }
        }
        if (memory_type == UINT32_MAX) memory_type = fallback_memory_type;
        if (memory_type == UINT32_MAX) {
            vkDestroyImage(device, image, nullptr);
            return std::unexpected(GpuError{"no Vulkan memory type can back NV12 correction plane", 0});
        }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        result = vkAllocateMemory(device, &allocation, nullptr, &memory);
        if (result != VK_SUCCESS) {
            vkDestroyImage(device, image, nullptr);
            return std::unexpected(color_vk_error("vkAllocateMemory for NV12 correction plane", result));
        }
        result = vkBindImageMemory(device, image, memory, 0);
        if (result != VK_SUCCESS) {
            vkFreeMemory(device, memory, nullptr);
            memory = VK_NULL_HANDLE;
            vkDestroyImage(device, image, nullptr);
            return std::unexpected(color_vk_error("vkBindImageMemory for NV12 correction plane", result));
        }
        return image;
    }

    std::expected<VkImageView, GpuError> make_view(
        VkImage image, VkFormat format, VkImageAspectFlags aspect) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange = VkImageSubresourceRange{
            .aspectMask = aspect,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        VkImageView view = VK_NULL_HANDLE;
        const VkResult result = vkCreateImageView(device, &view_info, nullptr, &view);
        if (result != VK_SUCCESS)
            return std::unexpected(color_vk_error("vkCreateImageView for NV12 correction", result));
        return view;
    }

    void destroy_resources(Nv12ColorCorrectionResources& resources) noexcept {
        if (resources.descriptors && descriptor_pool)
            (void)vkFreeDescriptorSets(device, descriptor_pool, 1, &resources.descriptors);
        if (resources.source_y_view) vkDestroyImageView(device, resources.source_y_view, nullptr);
        if (resources.source_uv_view) vkDestroyImageView(device, resources.source_uv_view, nullptr);
        if (resources.y_view) vkDestroyImageView(device, resources.y_view, nullptr);
        if (resources.uv_view) vkDestroyImageView(device, resources.uv_view, nullptr);
        if (resources.command && command_pool)
            vkFreeCommandBuffers(device, command_pool, 1, &resources.command);
        if (resources.temp_y) vkDestroyImage(device, resources.temp_y, nullptr);
        if (resources.temp_uv) vkDestroyImage(device, resources.temp_uv, nullptr);
        if (resources.temp_y_memory) vkFreeMemory(device, resources.temp_y_memory, nullptr);
        if (resources.temp_uv_memory) vkFreeMemory(device, resources.temp_uv_memory, nullptr);
        resources = {};
    }

    std::expected<void, GpuError> initialize(
        AVBufferRef* input_ref, AVBufferRef* output_ref,
        std::uint32_t frame_width, std::uint32_t frame_height,
        std::uint32_t max_pending) {
        if (!input_ref || !output_ref)
            return std::unexpected(GpuError{"NV12 color correction needs input/output Vulkan frames", 0});
        input_frames = reinterpret_cast<AVHWFramesContext*>(input_ref->data);
        output_frames = reinterpret_cast<AVHWFramesContext*>(output_ref->data);
        if (!input_frames || !output_frames || input_frames->format != AV_PIX_FMT_VULKAN ||
            output_frames->format != AV_PIX_FMT_VULKAN || input_frames->sw_format != AV_PIX_FMT_NV12 ||
            output_frames->sw_format != AV_PIX_FMT_NV12 ||
            input_frames->device_ctx != output_frames->device_ctx) {
            return std::unexpected(GpuError{"NV12 color correction requires same-device Vulkan NV12 frames", 0});
        }
        input_vk_frames = static_cast<AVVulkanFramesContext*>(input_frames->hwctx);
        output_vk_frames = static_cast<AVVulkanFramesContext*>(output_frames->hwctx);
        device_context = input_frames->device_ctx;
        vk_context = static_cast<AVVulkanDeviceContext*>(device_context->hwctx);
        device = vk_context->act_dev;
        physical_device = vk_context->phys_dev;
        width = frame_width;
        height = frame_height;
        if (width == 0 || height == 0 || (width & 1U) || (height & 1U))
            return std::unexpected(GpuError{"NV12 color correction needs even dimensions", 0});
        if (output_vk_frames->format[1] != VK_FORMAT_UNDEFINED ||
            output_vk_frames->format[0] != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ||
            !(output_vk_frames->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
            !(output_vk_frames->usage & VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR)) {
            return std::unexpected(GpuError{
                "NV12 correction output must be a multiplanar NV12 Vulkan Video frame with transfer-destination usage", 0});
        }
        if (input_vk_frames->format[1] != VK_FORMAT_UNDEFINED ||
            input_vk_frames->format[0] != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ||
            !(input_vk_frames->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            return std::unexpected(GpuError{
                "in-place NV12 correction needs a multiplanar filter frame with transfer-destination usage", 0});
        }
        if (!supports(VK_FORMAT_R8_UNORM,
                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) ||
            !supports(VK_FORMAT_R8G8_UNORM,
                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) ||
            !supports(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
            return std::unexpected(GpuError{"Vulkan lacks required NV12 correction plane features", 0});
        }

        bool found_compute = false;
        for (int i = 0; i < vk_context->nb_qf; ++i) {
            if ((vk_context->qf[i].flags & VK_QUEUE_COMPUTE_BIT) != 0 &&
                vk_context->qf[i].num > 0) {
                queue_family = static_cast<std::uint32_t>(vk_context->qf[i].idx);
                vkGetDeviceQueue(device, queue_family, 0, &queue);
                found_compute = queue != VK_NULL_HANDLE;
                if (found_compute) break;
            }
        }
        if (!found_compute)
            return std::unexpected(GpuError{"FFmpeg Vulkan device has no enabled compute queue for NV12 correction", 0});

        const std::array<VkDescriptorSetLayoutBinding, 4> bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo descriptor_info{};
        descriptor_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptor_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        descriptor_info.pBindings = bindings.data();
        VkResult result = vkCreateDescriptorSetLayout(device, &descriptor_info, nullptr,
                                                       &descriptor_layout);
        if (result != VK_SUCCESS)
            return std::unexpected(color_vk_error("vkCreateDescriptorSetLayout for NV12 correction", result));

        const std::uint32_t set_count = std::max<std::uint32_t>(max_pending + 32U, 128U);
        const std::array<VkDescriptorPoolSize, 2> pool_sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, set_count * 2U},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, set_count * 2U},
        };
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = set_count;
        pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        result = vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool);
        if (result != VK_SUCCESS)
            return std::unexpected(color_vk_error("vkCreateDescriptorPool for NV12 correction", result));

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.offset = 0;
        push.size = sizeof(std::uint32_t) * 2U;
        VkPipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &descriptor_layout;
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push;
        result = vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout);
        if (result != VK_SUCCESS)
            return std::unexpected(color_vk_error("vkCreatePipelineLayout for NV12 correction", result));

#ifndef FLUXREC_GPU_NV12_RB_SWAP_SHADER_PATH
        return std::unexpected(GpuError{"build did not provide the NV12 color correction SPIR-V path", 0});
#else
        std::ifstream shader_file(FLUXREC_GPU_NV12_RB_SWAP_SHADER_PATH,
                                  std::ios::binary | std::ios::ate);
        if (!shader_file)
            return std::unexpected(GpuError{"opening compiled NV12 correction SPIR-V failed", 0});
        const std::streamsize shader_bytes = shader_file.tellg();
        if (shader_bytes <= 0 || shader_bytes % static_cast<std::streamsize>(sizeof(std::uint32_t)) != 0)
            return std::unexpected(GpuError{"compiled NV12 correction SPIR-V has invalid size", 0});
        shader_file.seekg(0, std::ios::beg);
        std::vector<std::uint32_t> shader_code(
            static_cast<std::size_t>(shader_bytes) / sizeof(std::uint32_t));
        if (!shader_file.read(reinterpret_cast<char*>(shader_code.data()), shader_bytes))
            return std::unexpected(GpuError{"reading NV12 correction SPIR-V failed", 0});
        VkShaderModuleCreateInfo shader_info{};
        shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize = static_cast<std::size_t>(shader_bytes);
        shader_info.pCode = shader_code.data();
        result = vkCreateShaderModule(device, &shader_info, nullptr, &shader);
        if (result != VK_SUCCESS)
            return std::unexpected(color_vk_error("vkCreateShaderModule for NV12 correction", result));
#endif

        VkPipelineShaderStageCreateInfo shader_stage{};
        shader_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shader_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        shader_stage.module = shader;
        shader_stage.pName = "main";
        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage = shader_stage;
        pipeline_info.layout = pipeline_layout;
        result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                          nullptr, &pipeline);
        if (result != VK_SUCCESS)
            return std::unexpected(color_vk_error("vkCreateComputePipelines for NV12 correction", result));

        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxAnisotropy = 1.0F;
        sampler_info.minLod = 0.0F;
        sampler_info.maxLod = 0.0F;
        result = vkCreateSampler(device, &sampler_info, nullptr, &sampler);
        if (result != VK_SUCCESS)
            return std::unexpected(color_vk_error("vkCreateSampler for NV12 correction", result));

        VkCommandPoolCreateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        command_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                             VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        command_info.queueFamilyIndex = queue_family;
        result = vkCreateCommandPool(device, &command_info, nullptr, &command_pool);
        if (result != VK_SUCCESS)
            return std::unexpected(color_vk_error("vkCreateCommandPool for NV12 correction", result));

        av_log(nullptr, AV_LOG_INFO,
               "NV12 correction source frames format[0]=%d format[1]=%d usage=0x%x; destination format[0]=%d format[1]=%d\n",
               static_cast<int>(input_vk_frames->format[0]),
               static_cast<int>(input_vk_frames->format[1]),
               static_cast<unsigned int>(input_vk_frames->usage),
               static_cast<int>(output_vk_frames->format[0]),
               static_cast<int>(output_vk_frames->format[1]));
        return {};
    }

    std::expected<void, GpuError> ensure_scratch(Nv12ColorCorrectionResources& resources) {
        if (resources.temp_y && resources.temp_uv && resources.y_view && resources.uv_view &&
            resources.command) return {};
        auto y_image = create_temp_image(width, height, VK_FORMAT_R8_UNORM,
                                         resources.temp_y_memory);
        if (!y_image) return std::unexpected(y_image.error());
        resources.temp_y = *y_image;
        auto uv_image = create_temp_image(width / 2U, height / 2U, VK_FORMAT_R8G8_UNORM,
                                          resources.temp_uv_memory);
        if (!uv_image) return std::unexpected(uv_image.error());
        resources.temp_uv = *uv_image;
        auto y_view = make_view(resources.temp_y, VK_FORMAT_R8_UNORM,
                                VK_IMAGE_ASPECT_COLOR_BIT);
        if (!y_view) return std::unexpected(y_view.error());
        resources.y_view = *y_view;
        auto uv_view = make_view(resources.temp_uv, VK_FORMAT_R8G8_UNORM,
                                 VK_IMAGE_ASPECT_COLOR_BIT);
        if (!uv_view) return std::unexpected(uv_view.error());
        resources.uv_view = *uv_view;
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = command_pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        const VkResult result = vkAllocateCommandBuffers(device, &allocate, &resources.command);
        if (result != VK_SUCCESS)
            return std::unexpected(color_vk_error("vkAllocateCommandBuffers for NV12 correction", result));
        return {};
    }

    std::expected<std::pair<VkImageView, VkImageView>, GpuError> make_source_views(
        AVVulkanFramesContext* frames, AVVkFrame* frame) {
        if (frames->format[1] != VK_FORMAT_UNDEFINED) {
            if (!frame->img[0] || !frame->img[1] ||
                frames->format[0] != VK_FORMAT_R8_UNORM ||
                frames->format[1] != VK_FORMAT_R8G8_UNORM) {
                return std::unexpected(GpuError{"separate Vulkan NV12 source planes must be R8 and RG8 images", 0});
            }
            auto y_view = make_view(frame->img[0], VK_FORMAT_R8_UNORM,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
            if (!y_view) return std::unexpected(y_view.error());
            auto uv_view = make_view(frame->img[1], VK_FORMAT_R8G8_UNORM,
                                     VK_IMAGE_ASPECT_COLOR_BIT);
            if (!uv_view) {
                vkDestroyImageView(device, *y_view, nullptr);
                return std::unexpected(uv_view.error());
            }
            return std::pair{*y_view, *uv_view};
        }
        if (!frame->img[0] ||
            frames->format[0] != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
            return std::unexpected(GpuError{"multiplanar Vulkan NV12 source has an unsupported image format", 0});
        }
        auto y_view = make_view(frame->img[0], VK_FORMAT_R8_UNORM,
                                VK_IMAGE_ASPECT_PLANE_0_BIT);
        if (!y_view) return std::unexpected(y_view.error());
        auto uv_view = make_view(frame->img[0], VK_FORMAT_R8G8_UNORM,
                                 VK_IMAGE_ASPECT_PLANE_1_BIT);
        if (!uv_view) {
            vkDestroyImageView(device, *y_view, nullptr);
            return std::unexpected(uv_view.error());
        }
        return std::pair{*y_view, *uv_view};
    }

    std::expected<Nv12ColorCorrectionResources, GpuError> dispatch(
        AVFrame* input, AVFrame* output) {
        AVVkFrame* source = color_vk_frame(input);
        AVVkFrame* destination = color_vk_frame(output);
        AVHWFramesContext* source_fc = color_frames_context(input);
        AVHWFramesContext* destination_fc = color_frames_context(output);
        if (!source || !destination || !source_fc || !destination_fc ||
            !destination->img[0] || !destination->sem[0]) {
            return std::unexpected(GpuError{"NV12 correction got an invalid Vulkan source/destination frame", 0});
        }
        if (source != destination || source_fc != destination_fc ||
            input_vk_frames != output_vk_frames || input_vk_frames->format[1] != VK_FORMAT_UNDEFINED) {
            return std::unexpected(GpuError{
                "NV12 correction requires two AVFrame headers for the same multiplanar AVVkFrame", 0});
        }
        if (input->width != static_cast<int>(width) || input->height != static_cast<int>(height) ||
            output->width != static_cast<int>(width) || output->height != static_cast<int>(height)) {
            return std::unexpected(GpuError{"NV12 correction frame dimensions changed after initialization", 0});
        }

        Nv12ColorCorrectionResources resources;
        if (!available.empty()) {
            resources = available.back();
            available.pop_back();
        }
        auto scratch = ensure_scratch(resources);
        if (!scratch) {
            destroy_resources(resources);
            return std::unexpected(scratch.error());
        }
        auto source_views = make_source_views(input_vk_frames, source);
        if (!source_views) {
            destroy_resources(resources);
            return std::unexpected(source_views.error());
        }
        resources.source_y_view = source_views->first;
        resources.source_uv_view = source_views->second;

        VkDescriptorSetAllocateInfo descriptor_allocate{};
        descriptor_allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descriptor_allocate.descriptorPool = descriptor_pool;
        descriptor_allocate.descriptorSetCount = 1;
        descriptor_allocate.pSetLayouts = &descriptor_layout;
        VkResult result = vkAllocateDescriptorSets(device, &descriptor_allocate,
                                                   &resources.descriptors);
        if (result != VK_SUCCESS) {
            destroy_resources(resources);
            return std::unexpected(color_vk_error("vkAllocateDescriptorSets for NV12 correction", result));
        }
        const std::array<VkDescriptorImageInfo, 4> image_infos{
            VkDescriptorImageInfo{sampler, resources.source_y_view, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{sampler, resources.source_uv_view, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{VK_NULL_HANDLE, resources.y_view, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{VK_NULL_HANDLE, resources.uv_view, VK_IMAGE_LAYOUT_GENERAL},
        };
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = resources.descriptors;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i < 2
                ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[i].pImageInfo = &image_infos[i];
        }
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);

        if (input_vk_frames->lock_frame) input_vk_frames->lock_frame(source_fc, source);
        const auto unlock_frame = [&]() {
            if (input_vk_frames->unlock_frame) input_vk_frames->unlock_frame(source_fc, source);
        };

        const bool separate_source_planes = input_vk_frames->format[1] != VK_FORMAT_UNDEFINED;
        const std::uint32_t source_count = separate_source_planes ? 2U : 1U;
        std::array<VkImageMemoryBarrier2, 4> barriers{};
        for (std::uint32_t plane = 0; plane < source_count; ++plane) {
            const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            barriers[plane] = color_image_barrier(
                source->img[plane], source->layout[plane], source->access[plane],
                source->queue_family[plane], queue_family,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, aspect,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        }
        const std::uint32_t temp_barrier_base = source_count;
        barriers[temp_barrier_base] = color_image_barrier(
            resources.temp_y, VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_QUEUE_FAMILY_IGNORED,
            queue_family, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        barriers[temp_barrier_base + 1U] = color_image_barrier(
            resources.temp_uv, VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_QUEUE_FAMILY_IGNORED,
            queue_family, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(resources.command, &begin);
        if (result != VK_SUCCESS) {
            unlock_frame();
            destroy_resources(resources);
            return std::unexpected(color_vk_error("vkBeginCommandBuffer for NV12 correction", result));
        }
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = source_count + 2U;
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(resources.command, &dependency);
        vkCmdBindPipeline(resources.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(resources.command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline_layout, 0, 1, &resources.descriptors, 0, nullptr);
        const std::array<std::uint32_t, 2> extent{width, height};
        vkCmdPushConstants(resources.command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(extent), extent.data());
        vkCmdDispatch(resources.command, (width + 31U) / 32U, (height + 31U) / 32U, 1);

        const VkImageMemoryBarrier2 temp_to_copy[2]{
            color_image_barrier(resources.temp_y, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, queue_family, queue_family,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT),
            color_image_barrier(resources.temp_uv, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, queue_family, queue_family,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT),
        };
        VkImageMemoryBarrier2 source_to_copy = color_image_barrier(
            source->img[0], VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, source->queue_family[0], queue_family,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        // Dispatch sampled the same image we now overwrite. Make that
        // compute-read -> transfer-write dependency explicit before copying
        // the corrected temporary planes back into the filter sink image.
        source_to_copy.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        source_to_copy.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        std::array<VkImageMemoryBarrier2, 3> copy_barriers{
            temp_to_copy[0], temp_to_copy[1], source_to_copy};
        dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(copy_barriers.size());
        dependency.pImageMemoryBarriers = copy_barriers.data();
        vkCmdPipelineBarrier2(resources.command, &dependency);

        VkImageCopy copy{};
        copy.srcSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
        copy.extent = VkExtent3D{width, height, 1};
        vkCmdCopyImage(resources.command, resources.temp_y, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       destination->img[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        copy.extent = VkExtent3D{width / 2U, height / 2U, 1};
        vkCmdCopyImage(resources.command, resources.temp_uv, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       destination->img[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        const VkImageMemoryBarrier2 destination_to_encoder = color_image_barrier(
            destination->img[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, queue_family, queue_family,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &destination_to_encoder;
        vkCmdPipelineBarrier2(resources.command, &dependency);
        result = vkEndCommandBuffer(resources.command);
        if (result != VK_SUCCESS) {
            unlock_frame();
            destroy_resources(resources);
            return std::unexpected(color_vk_error("vkEndCommandBuffer for NV12 correction", result));
        }

        struct Timeline {
            VkSemaphore semaphore = VK_NULL_HANDLE;
            std::uint64_t value = 0;
            VkPipelineStageFlags2 wait_stage = VK_PIPELINE_STAGE_2_NONE;
        };
        std::array<Timeline, 3> timelines{};
        std::uint32_t timeline_count = 0;
        const auto add_timeline = [&](VkSemaphore semaphore, std::uint64_t value,
                                      VkPipelineStageFlags2 stage) {
            if (!semaphore) return false;
            for (std::uint32_t i = 0; i < timeline_count; ++i) {
                if (timelines[i].semaphore == semaphore) {
                    timelines[i].value = std::max(timelines[i].value, value);
                    timelines[i].wait_stage |= stage;
                    return true;
                }
            }
            if (timeline_count == timelines.size()) return false;
            timelines[timeline_count++] = {semaphore, value, stage};
            return true;
        };
        for (std::uint32_t plane = 0; plane < source_count; ++plane) {
            if (!add_timeline(source->sem[plane], source->sem_value[plane],
                              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)) {
                unlock_frame();
                destroy_resources(resources);
                return std::unexpected(GpuError{"NV12 source frame is missing a timeline semaphore", 0});
            }
        }
        if (!add_timeline(destination->sem[0], destination->sem_value[0],
                          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)) {
            unlock_frame();
            destroy_resources(resources);
            return std::unexpected(GpuError{"NV12 encode frame is missing a timeline semaphore", 0});
        }
        std::array<VkSemaphoreSubmitInfo, 3> waits{};
        std::array<VkSemaphoreSubmitInfo, 3> signals{};
        for (std::uint32_t i = 0; i < timeline_count; ++i) {
            waits[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            waits[i].semaphore = timelines[i].semaphore;
            waits[i].value = timelines[i].value;
            waits[i].stageMask = timelines[i].wait_stage;
            signals[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            signals[i].semaphore = timelines[i].semaphore;
            signals[i].value = timelines[i].value + 1U;
            signals[i].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
        VkCommandBufferSubmitInfo command_submit{};
        command_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command_submit.commandBuffer = resources.command;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = timeline_count;
        submit.pWaitSemaphoreInfos = waits.data();
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command_submit;
        submit.signalSemaphoreInfoCount = timeline_count;
        submit.pSignalSemaphoreInfos = signals.data();
#if FF_API_VULKAN_SYNC_QUEUES
        if (vk_context->lock_queue) vk_context->lock_queue(device_context, queue_family, 0);
#endif
        result = vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE);
#if FF_API_VULKAN_SYNC_QUEUES
        if (vk_context->unlock_queue) vk_context->unlock_queue(device_context, queue_family, 0);
#endif
        if (result != VK_SUCCESS) {
            unlock_frame();
            destroy_resources(resources);
            return std::unexpected(color_vk_error("vkQueueSubmit2 for NV12 correction", result));
        }

        resources.submitted = true;
        for (std::uint32_t plane = 0; plane < source_count; ++plane) {
            source->layout[plane] = VK_IMAGE_LAYOUT_GENERAL;
            source->access[plane] = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            if (source->queue_family[plane] != VK_QUEUE_FAMILY_IGNORED)
                source->queue_family[plane] = queue_family;
            for (std::uint32_t i = 0; i < timeline_count; ++i) {
                if (source->sem[plane] == timelines[i].semaphore)
                    source->sem_value[plane] = timelines[i].value + 1U;
            }
        }
        unlock_frame();
        return resources;
    }

    void release(Nv12ColorCorrectionResources& resources, AVFrame* input,
                 AVFrame* output) noexcept {
        if (device == VK_NULL_HANDLE) return;
        if (resources.submitted) {
            std::array<VkSemaphore, 3> semaphores{};
            std::array<std::uint64_t, 3> values{};
            std::uint32_t count = 0;
            const auto collect = [&](AVFrame* frame) {
                AVVkFrame* vkf = color_vk_frame(frame);
                AVHWFramesContext* fc = color_frames_context(frame);
                if (!vkf || !fc) return;
                auto* vkfc = static_cast<AVVulkanFramesContext*>(fc->hwctx);
                if (vkfc->lock_frame) vkfc->lock_frame(fc, vkf);
                if (frame == output && vkf->layout[0] == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR)
                    vkf->access[0] = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
                for (std::size_t plane = 0; plane < 2; ++plane) {
                    if (!vkf->sem[plane]) continue;
                    bool found = false;
                    for (std::uint32_t i = 0; i < count; ++i) {
                        if (semaphores[i] == vkf->sem[plane]) {
                            values[i] = std::max(values[i], vkf->sem_value[plane]);
                            found = true;
                            break;
                        }
                    }
                    if (!found && count < semaphores.size()) {
                        semaphores[count] = vkf->sem[plane];
                        values[count] = vkf->sem_value[plane];
                        ++count;
                    }
                }
                if (vkfc->unlock_frame) vkfc->unlock_frame(fc, vkf);
            };
            if (color_vk_frame(input) == color_vk_frame(output)) {
                collect(output);
            } else {
                collect(input);
                collect(output);
            }
            if (count != 0) {
                VkSemaphoreWaitInfo wait{};
                wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
                wait.semaphoreCount = count;
                wait.pSemaphores = semaphores.data();
                wait.pValues = values.data();
                (void)vkWaitSemaphores(device, &wait, std::numeric_limits<std::uint64_t>::max());
            }
        }
        resources.submitted = false;
        if (resources.command) (void)vkResetCommandBuffer(resources.command, 0);
        if (resources.descriptors) {
            (void)vkFreeDescriptorSets(device, descriptor_pool, 1, &resources.descriptors);
            resources.descriptors = VK_NULL_HANDLE;
        }
        if (resources.source_y_view) vkDestroyImageView(device, resources.source_y_view, nullptr);
        if (resources.source_uv_view) vkDestroyImageView(device, resources.source_uv_view, nullptr);
        resources.source_y_view = VK_NULL_HANDLE;
        resources.source_uv_view = VK_NULL_HANDLE;
        available.push_back(resources);
        resources = {};
    }
};

Nv12ColorCorrector::Nv12ColorCorrector(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
Nv12ColorCorrector::~Nv12ColorCorrector() = default;
Nv12ColorCorrector::Nv12ColorCorrector(Nv12ColorCorrector&&) noexcept = default;
Nv12ColorCorrector& Nv12ColorCorrector::operator=(Nv12ColorCorrector&&) noexcept = default;

std::expected<Nv12ColorCorrector, GpuError> Nv12ColorCorrector::create(
    AVBufferRef* input_frames, AVBufferRef* output_frames,
    std::uint32_t width, std::uint32_t height, std::uint32_t max_pending) {
    auto impl = std::make_unique<Impl>();
    auto initialized = impl->initialize(input_frames, output_frames, width, height, max_pending);
    if (!initialized) return std::unexpected(initialized.error());
    return Nv12ColorCorrector{std::move(impl)};
}

std::expected<Nv12ColorCorrectionResources, GpuError> Nv12ColorCorrector::dispatch(
    AVFrame* input, AVFrame* output) {
    if (!impl_) return std::unexpected(GpuError{"NV12 color corrector is not initialized", 0});
    return impl_->dispatch(input, output);
}

void Nv12ColorCorrector::release(Nv12ColorCorrectionResources& resources,
                                 AVFrame* input, AVFrame* output) noexcept {
    if (impl_) impl_->release(resources, input, output);
}

} // namespace fluxrec::gpu::detail
