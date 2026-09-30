#include "gpu/frame_identity.hpp"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fluxrec::gpu {
namespace {

constexpr std::uint32_t kLocalSize = 128;
constexpr VkDeviceSize kDigestBytes = 4U * sizeof(std::uint32_t);

FrameIdentityError failure(std::string message, int code = 0) {
    return FrameIdentityError{std::move(message), code};
}

FrameIdentityError vk_failure(std::string message, VkResult result) {
    message += " (VkResult ";
    message += std::to_string(static_cast<int>(result));
    message += ')';
    return failure(std::move(message), static_cast<int>(result));
}

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize allocation_size = 0;
    VkMemoryPropertyFlags properties = 0;
    void* mapped = nullptr;
};

struct PushExtent {
    std::uint32_t width;
    std::uint32_t height;
};

struct PushReduce {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t partial_count;
    std::uint32_t pixel_count;
};

struct PushWorkload {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t iterations;
    std::uint32_t reserved;
};

std::expected<std::vector<std::uint32_t>, FrameIdentityError> load_spirv(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::unexpected(failure("opening frame identity SPIR-V failed: " + path.string()));
    const std::streamoff end = input.tellg();
    if (end <= 0 || (end % 4) != 0) {
        return std::unexpected(failure("frame identity SPIR-V has an invalid byte size: " + path.string()));
    }
    std::vector<std::uint32_t> words(static_cast<std::size_t>(end) / 4U);
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(words.data()), end);
    if (!input) return std::unexpected(failure("reading frame identity SPIR-V failed: " + path.string()));
    return words;
}

std::optional<VkComponentMapping> source_mapping(VkFormat format, AVPixelFormat sw_format) {
    const bool bgra = sw_format == AV_PIX_FMT_BGRA || sw_format == AV_PIX_FMT_BGR0;
    const bool rgba = sw_format == AV_PIX_FMT_RGBA || sw_format == AV_PIX_FMT_RGB0;
    if (!bgra && !rgba) return std::nullopt;

    VkComponentMapping identity{};
    identity.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    identity.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    identity.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    identity.a = VK_COMPONENT_SWIZZLE_IDENTITY;

    if ((format == VK_FORMAT_R8G8B8A8_UNORM && bgra) ||
        (format == VK_FORMAT_B8G8R8A8_UNORM && rgba)) {
        identity.r = VK_COMPONENT_SWIZZLE_B;
        identity.g = VK_COMPONENT_SWIZZLE_G;
        identity.b = VK_COMPONENT_SWIZZLE_R;
    } else if (!((format == VK_FORMAT_R8G8B8A8_UNORM && rgba) ||
                 (format == VK_FORMAT_B8G8R8A8_UNORM && bgra))) {
        return std::nullopt;
    }
    return identity;
}

} // namespace

struct FrameIdentity::Impl {
    struct Slot {
        Buffer partial;
        Buffer digest;
        Buffer readback;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkQueryPool query_pool = VK_NULL_HANDLE;
        VkImageView source_view = VK_NULL_HANDLE;
        AVFrame* source_ref = nullptr;
        std::uint64_t sequence = 0;
        bool busy = false;
    };

    FrameIdentityOptions options;
    AVBufferRef* device_ref = nullptr;
    AVHWDeviceContext* av_device = nullptr;
    AVVulkanDeviceContext* vulkan = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t timestamp_valid_bits = 0;
    float timestamp_period = 0.0F;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceProperties physical_properties{};
    std::uint32_t pixel_count = 0;
    std::uint32_t partial_count = 0;
    VkDeviceSize partial_bytes = 0;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout digest_set_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout reduce_set_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout unrelated_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout digest_pipeline_layout = VK_NULL_HANDLE;
    VkPipelineLayout reduce_pipeline_layout = VK_NULL_HANDLE;
    VkPipelineLayout unrelated_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline digest_pipeline = VK_NULL_HANDLE;
    VkPipeline reduce_pipeline = VK_NULL_HANDLE;
    VkPipeline unrelated_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::vector<Slot> slots;
    std::vector<VkDescriptorSet> digest_sets;
    std::vector<VkDescriptorSet> reduce_sets;
    std::vector<VkDescriptorSet> unrelated_sets;
    std::deque<FrameIdentityDigest> ready;
    std::optional<std::uint64_t> last_submitted_sequence;
    std::optional<std::uint64_t> previous_digest_sequence;
    std::optional<std::array<std::uint32_t, 4>> previous_digest;
    std::size_t busy_count = 0;
    mutable std::mutex mutex;
    bool initialized = false;

    ~Impl() { destroy(); }

    void destroy_buffer(Buffer& buffer) noexcept {
        if (buffer.mapped && device != VK_NULL_HANDLE && buffer.memory != VK_NULL_HANDLE)
            vkUnmapMemory(device, buffer.memory);
        buffer.mapped = nullptr;
        if (buffer.handle != VK_NULL_HANDLE && device != VK_NULL_HANDLE)
            vkDestroyBuffer(device, buffer.handle, nullptr);
        if (buffer.memory != VK_NULL_HANDLE && device != VK_NULL_HANDLE)
            vkFreeMemory(device, buffer.memory, nullptr);
        buffer = {};
    }

    void destroy() noexcept {
        if (device != VK_NULL_HANDLE) {
            for (auto& slot : slots) {
                if (slot.busy && slot.fence != VK_NULL_HANDLE)
                    (void)vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
            }
            for (auto& slot : slots) {
                if (slot.source_ref) av_frame_free(&slot.source_ref);
                if (slot.source_view != VK_NULL_HANDLE)
                    vkDestroyImageView(device, slot.source_view, nullptr);
                if (slot.fence != VK_NULL_HANDLE) vkDestroyFence(device, slot.fence, nullptr);
                if (slot.query_pool != VK_NULL_HANDLE)
                    vkDestroyQueryPool(device, slot.query_pool, nullptr);
                destroy_buffer(slot.partial);
                destroy_buffer(slot.digest);
                destroy_buffer(slot.readback);
            }
            if (!slots.empty() && command_pool != VK_NULL_HANDLE) {
                std::vector<VkCommandBuffer> command_buffers;
                command_buffers.reserve(slots.size());
                for (const auto& slot : slots)
                    if (slot.command_buffer != VK_NULL_HANDLE)
                        command_buffers.push_back(slot.command_buffer);
                if (!command_buffers.empty()) {
                    vkFreeCommandBuffers(device, command_pool,
                                         static_cast<std::uint32_t>(command_buffers.size()),
                                         command_buffers.data());
                }
            }
            if (sampler != VK_NULL_HANDLE) vkDestroySampler(device, sampler, nullptr);
            if (digest_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, digest_pipeline, nullptr);
            if (reduce_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, reduce_pipeline, nullptr);
            if (unrelated_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, unrelated_pipeline, nullptr);
            if (digest_pipeline_layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, digest_pipeline_layout, nullptr);
            if (reduce_pipeline_layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, reduce_pipeline_layout, nullptr);
            if (unrelated_pipeline_layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, unrelated_pipeline_layout, nullptr);
            if (digest_set_layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, digest_set_layout, nullptr);
            if (reduce_set_layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, reduce_set_layout, nullptr);
            if (unrelated_set_layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, unrelated_set_layout, nullptr);
            if (descriptor_pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            if (command_pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(device, command_pool, nullptr);
        }
        slots.clear();
        if (device_ref) av_buffer_unref(&device_ref);
        device = VK_NULL_HANDLE;
        initialized = false;
    }

    std::expected<std::uint32_t, FrameIdentityError> memory_type(
        std::uint32_t bits, VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred) const {
        std::optional<std::uint32_t> best;
        int best_score = -1;
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((bits & (1U << i)) == 0) continue;
            const auto flags = memory_properties.memoryTypes[i].propertyFlags;
            if ((flags & required) != required) continue;
            const auto matching = static_cast<unsigned>(std::popcount(flags & preferred));
            const int score = static_cast<int>(matching);
            if (score > best_score) {
                best = i;
                best_score = score;
            }
        }
        if (!best) return std::unexpected(failure("no Vulkan memory type satisfies frame identity buffer requirements"));
        return *best;
    }

    std::expected<void, FrameIdentityError> create_buffer(
        VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred, bool map, Buffer& result) {
        VkBufferCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        create_info.size = size;
        create_info.usage = usage;
        create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult vr = vkCreateBuffer(device, &create_info, nullptr, &result.handle);
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("creating frame identity buffer failed", vr));

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.handle, &requirements);
        auto type = memory_type(requirements.memoryTypeBits, required, preferred);
        if (!type) return std::unexpected(type.error());
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *type;
        vr = vkAllocateMemory(device, &allocation, nullptr, &result.memory);
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("allocating frame identity buffer memory failed", vr));
        vr = vkBindBufferMemory(device, result.handle, result.memory, 0);
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("binding frame identity buffer memory failed", vr));
        result.allocation_size = requirements.size;
        result.properties = memory_properties.memoryTypes[*type].propertyFlags;
        if (map) {
            vr = vkMapMemory(device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped);
            if (vr != VK_SUCCESS) return std::unexpected(vk_failure("mapping 16-byte digest readback buffer failed", vr));
        }
        return {};
    }

    std::expected<VkShaderModule, FrameIdentityError> create_shader_module(
        const std::filesystem::path& path) {
        auto words = load_spirv(path);
        if (!words) return std::unexpected(words.error());
        VkShaderModuleCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        create_info.codeSize = words->size() * sizeof(std::uint32_t);
        create_info.pCode = words->data();
        VkShaderModule module = VK_NULL_HANDLE;
        const VkResult vr = vkCreateShaderModule(device, &create_info, nullptr, &module);
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("creating frame identity shader module failed", vr));
        return module;
    }

    std::expected<void, FrameIdentityError> create_layouts_and_pipelines() {
        const std::array<VkDescriptorSetLayoutBinding, 2> digest_bindings{{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        }};
        VkDescriptorSetLayoutCreateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_info.bindingCount = static_cast<std::uint32_t>(digest_bindings.size());
        set_info.pBindings = digest_bindings.data();
        VkResult vr = vkCreateDescriptorSetLayout(device, &set_info, nullptr, &digest_set_layout);
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("creating frame digest descriptor layout failed", vr));

        const std::array<VkDescriptorSetLayoutBinding, 2> reduce_bindings{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        }};
        set_info.pBindings = reduce_bindings.data();
        vr = vkCreateDescriptorSetLayout(device, &set_info, nullptr, &reduce_set_layout);
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("creating frame digest reducer descriptor layout failed", vr));

        const VkDescriptorSetLayoutBinding unrelated_binding{
            0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        set_info.bindingCount = 1;
        set_info.pBindings = &unrelated_binding;
        vr = vkCreateDescriptorSetLayout(device, &set_info, nullptr, &unrelated_set_layout);
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("creating unrelated workload descriptor layout failed", vr));

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.offset = 0;
        push.size = sizeof(PushExtent);
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &digest_set_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        vr = vkCreatePipelineLayout(device, &layout_info, nullptr, &digest_pipeline_layout);
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("creating frame digest pipeline layout failed", vr));

        push.size = sizeof(PushReduce);
        layout_info.pSetLayouts = &reduce_set_layout;
        vr = vkCreatePipelineLayout(device, &layout_info, nullptr, &reduce_pipeline_layout);
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("creating frame digest reducer pipeline layout failed", vr));

        push.size = sizeof(PushWorkload);
        layout_info.pSetLayouts = &unrelated_set_layout;
        vr = vkCreatePipelineLayout(device, &layout_info, nullptr, &unrelated_pipeline_layout);
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("creating unrelated workload pipeline layout failed", vr));

        const auto digest_path = options.shader_directory / "frame_digest.comp.spv";
        auto digest_module = create_shader_module(digest_path);
        if (!digest_module) return std::unexpected(digest_module.error());
        const auto reduce_path = options.shader_directory / "frame_digest_reduce.comp.spv";
        auto reduce_module = create_shader_module(reduce_path);
        if (!reduce_module) {
            vkDestroyShaderModule(device, *digest_module, nullptr);
            return std::unexpected(reduce_module.error());
        }
        const auto unrelated_path = options.shader_directory / "frame_identity_unrelated.comp.spv";
        auto unrelated_module = create_shader_module(unrelated_path);
        if (!unrelated_module) {
            vkDestroyShaderModule(device, *digest_module, nullptr);
            vkDestroyShaderModule(device, *reduce_module, nullptr);
            return std::unexpected(unrelated_module.error());
        }

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = *digest_module;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage = stage;
        pipeline_info.layout = digest_pipeline_layout;
        vr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                      &digest_pipeline);
        if (vr == VK_SUCCESS) {
            stage.module = *reduce_module;
            pipeline_info.stage = stage;
            pipeline_info.layout = reduce_pipeline_layout;
            vr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                          nullptr, &reduce_pipeline);
        }
        if (vr == VK_SUCCESS) {
            stage.module = *unrelated_module;
            pipeline_info.stage = stage;
            pipeline_info.layout = unrelated_pipeline_layout;
            vr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                          nullptr, &unrelated_pipeline);
        }
        vkDestroyShaderModule(device, *digest_module, nullptr);
        vkDestroyShaderModule(device, *reduce_module, nullptr);
        vkDestroyShaderModule(device, *unrelated_module, nullptr);
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("creating frame identity compute pipeline failed", vr));
        return {};
    }

    std::expected<void, FrameIdentityError> initialize(AVBufferRef* source_device) {
        if (!source_device || !source_device->data)
            return std::unexpected(failure("frame identity requires an initialized FFmpeg Vulkan device"));
        if (options.width == 0 || options.height == 0 || options.ring_size == 0 || options.ring_size > 64)
            return std::unexpected(failure("frame identity dimensions must be nonzero and ring size must be 1..64"));
        if (options.unrelated_iterations == 0 || options.unrelated_iterations > 64)
            return std::unexpected(failure("unrelated workload iterations must be 1..64"));
        if (options.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            options.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            return std::unexpected(failure("frame identity dimensions exceed FFmpeg frame limits"));
        const std::uint64_t count64 = static_cast<std::uint64_t>(options.width) * options.height;
        if (count64 > std::numeric_limits<std::uint32_t>::max())
            return std::unexpected(failure("frame identity pixel count exceeds the shader's uint32 extent"));
        pixel_count = static_cast<std::uint32_t>(count64);
        partial_count = static_cast<std::uint32_t>((count64 + kLocalSize - 1U) / kLocalSize);
        partial_bytes = static_cast<VkDeviceSize>(partial_count) * 16U;

        device_ref = av_buffer_ref(source_device);
        if (!device_ref) return std::unexpected(failure("retaining FFmpeg Vulkan device failed"));
        av_device = reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
        if (!av_device || av_device->type != AV_HWDEVICE_TYPE_VULKAN || !av_device->hwctx)
            return std::unexpected(failure("provided FFmpeg device is not Vulkan"));
        vulkan = static_cast<AVVulkanDeviceContext*>(av_device->hwctx);
        device = vulkan->act_dev;
        physical = vulkan->phys_dev;
        if (device == VK_NULL_HANDLE || physical == VK_NULL_HANDLE)
            return std::unexpected(failure("FFmpeg Vulkan context has no active VkDevice/physical device"));

        int selected_qf = -1;
        for (int i = 0; i < vulkan->nb_qf; ++i) {
            if (vulkan->qf[i].idx >= 0 && vulkan->qf[i].num > 0 &&
                (vulkan->qf[i].flags & VK_QUEUE_COMPUTE_BIT) != 0) {
                selected_qf = vulkan->qf[i].idx;
                break;
            }
        }
        if (selected_qf < 0)
            return std::unexpected(failure("FFmpeg Vulkan device exposes no compute queue"));
        queue_family = static_cast<std::uint32_t>(selected_qf);
        VkDeviceQueueInfo2 queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2;
        queue_info.flags = vulkan->queue_flags;
        queue_info.queueFamilyIndex = queue_family;
        queue_info.queueIndex = 0;
        vkGetDeviceQueue2(device, &queue_info, &queue);
        if (queue == VK_NULL_HANDLE)
            return std::unexpected(failure("retrieving FFmpeg Vulkan compute queue failed"));

        vkGetPhysicalDeviceProperties(physical, &physical_properties);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
        if (partial_count > physical_properties.limits.maxComputeWorkGroupCount[0])
            return std::unexpected(failure("frame dimensions exceed Vulkan's X dispatch workgroup limit"));
        if (partial_bytes > physical_properties.limits.maxStorageBufferRange)
            return std::unexpected(failure("frame dimensions exceed Vulkan's storage-buffer range"));

        std::uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> qf_properties(qf_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &qf_count, qf_properties.data());
        if (queue_family >= qf_count)
            return std::unexpected(failure("FFmpeg compute queue family index is invalid"));
        timestamp_valid_bits = qf_properties[queue_family].timestampValidBits;
        timestamp_period = physical_properties.limits.timestampPeriod;
        if (!options.measure_gpu_time) timestamp_valid_bits = 0;

        VkCommandPoolCreateInfo command_pool_info{};
        command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        command_pool_info.queueFamilyIndex = queue_family;
        VkResult vr = vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool);
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("creating frame identity command pool failed", vr));

        auto pipeline_result = create_layouts_and_pipelines();
        if (!pipeline_result) return pipeline_result;

        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = 0.0F;
        vr = vkCreateSampler(device, &sampler_info, nullptr, &sampler);
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("creating frame identity sampler failed", vr));

        const auto storage_count = static_cast<std::uint32_t>(options.ring_size * 3U);
        const auto sampler_count = static_cast<std::uint32_t>(options.ring_size);
        const std::array<VkDescriptorPoolSize, 2> pool_sizes{{
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storage_count},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampler_count},
        }};
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = static_cast<std::uint32_t>(options.ring_size * 2U);
        pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        vr = vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool);
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("creating frame identity descriptor pool failed", vr));

        VkDescriptorSetAllocateInfo set_allocate{};
        set_allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set_allocate.descriptorPool = descriptor_pool;
        set_allocate.descriptorSetCount = static_cast<std::uint32_t>(options.ring_size);
        if (options.mode != FrameIdentityMode::unrelated_workload) {
            std::vector<VkDescriptorSetLayout> digest_layouts(options.ring_size, digest_set_layout);
            digest_sets.resize(options.ring_size);
            set_allocate.pSetLayouts = digest_layouts.data();
            vr = vkAllocateDescriptorSets(device, &set_allocate, digest_sets.data());
            if (vr != VK_SUCCESS) return std::unexpected(vk_failure("allocating frame digest descriptor sets failed", vr));
        }
        if (options.mode == FrameIdentityMode::digest) {
            std::vector<VkDescriptorSetLayout> reduce_layouts(options.ring_size, reduce_set_layout);
            reduce_sets.resize(options.ring_size);
            set_allocate.pSetLayouts = reduce_layouts.data();
            vr = vkAllocateDescriptorSets(device, &set_allocate, reduce_sets.data());
            if (vr != VK_SUCCESS) return std::unexpected(vk_failure("allocating frame reducer descriptor sets failed", vr));
        } else if (options.mode == FrameIdentityMode::unrelated_workload) {
            std::vector<VkDescriptorSetLayout> unrelated_layouts(options.ring_size, unrelated_set_layout);
            unrelated_sets.resize(options.ring_size);
            set_allocate.pSetLayouts = unrelated_layouts.data();
            vr = vkAllocateDescriptorSets(device, &set_allocate, unrelated_sets.data());
            if (vr != VK_SUCCESS) return std::unexpected(vk_failure("allocating unrelated workload descriptor sets failed", vr));
        }

        VkCommandBufferAllocateInfo command_allocate{};
        command_allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_allocate.commandPool = command_pool;
        command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocate.commandBufferCount = static_cast<std::uint32_t>(options.ring_size);
        slots.resize(options.ring_size);
        std::vector<VkCommandBuffer> commands(options.ring_size);
        vr = vkAllocateCommandBuffers(device, &command_allocate, commands.data());
        if (vr != VK_SUCCESS) return std::unexpected(vk_failure("allocating frame identity command buffers failed", vr));

        for (std::size_t i = 0; i < options.ring_size; ++i) {
            auto& slot = slots[i];
            slot.command_buffer = commands[i];
            auto partial_result = create_buffer(partial_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, slot.partial);
            if (!partial_result) return partial_result;
            if (options.mode == FrameIdentityMode::digest) {
                auto digest_result = create_buffer(kDigestBytes,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, slot.digest);
                if (!digest_result) return digest_result;
                auto readback_result = create_buffer(kDigestBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                    true, slot.readback);
                if (!readback_result) return readback_result;
            }

            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            vr = vkCreateFence(device, &fence_info, nullptr, &slot.fence);
            if (vr != VK_SUCCESS) return std::unexpected(vk_failure("creating frame identity fence failed", vr));
            if (timestamp_valid_bits != 0) {
                VkQueryPoolCreateInfo query_info{};
                query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
                query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
                query_info.queryCount = 2;
                vr = vkCreateQueryPool(device, &query_info, nullptr, &slot.query_pool);
                if (vr != VK_SUCCESS) return std::unexpected(vk_failure("creating frame identity timestamp query pool failed", vr));
            }

            VkDescriptorBufferInfo partial_info{slot.partial.handle, 0, partial_bytes};
            if (options.mode != FrameIdentityMode::unrelated_workload) {
                const VkWriteDescriptorSet digest_write{
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, digest_sets[i], 1, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &partial_info, nullptr};
                vkUpdateDescriptorSets(device, 1, &digest_write, 0, nullptr);
            }
            if (options.mode == FrameIdentityMode::digest) {
                VkDescriptorBufferInfo digest_info{slot.digest.handle, 0, kDigestBytes};
                const std::array<VkWriteDescriptorSet, 2> reduce_writes{{
                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, reduce_sets[i], 0, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &partial_info, nullptr},
                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, reduce_sets[i], 1, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &digest_info, nullptr},
                }};
                vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(reduce_writes.size()),
                                       reduce_writes.data(), 0, nullptr);
            } else if (options.mode == FrameIdentityMode::unrelated_workload) {
                const VkWriteDescriptorSet unrelated_write{
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, unrelated_sets[i], 0, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &partial_info, nullptr};
                vkUpdateDescriptorSets(device, 1, &unrelated_write, 0, nullptr);
            }
        }
        initialized = true;
        return {};
    }

    std::expected<void, FrameIdentityError> collect_completed_locked() {
        struct Completed {
            std::size_t slot_index;
            std::array<std::uint32_t, 4> words;
            std::optional<std::uint64_t> gpu_time_ns;
            std::uint64_t sequence;
            bool digest_available;
        };
        std::vector<Completed> finished;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            auto& slot = slots[i];
            if (!slot.busy) continue;
            const VkResult status = vkGetFenceStatus(device, slot.fence);
            if (status == VK_NOT_READY) continue;
            if (status != VK_SUCCESS)
                return std::unexpected(vk_failure("polling frame identity fence failed", status));

            Completed result{};
            result.slot_index = i;
            result.sequence = slot.sequence;
            result.digest_available = options.mode == FrameIdentityMode::digest;
            if (result.digest_available) {
                if ((slot.readback.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
                    VkMappedMemoryRange range{};
                    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                    range.memory = slot.readback.memory;
                    range.offset = 0;
                    range.size = VK_WHOLE_SIZE;
                    const VkResult invalidate = vkInvalidateMappedMemoryRanges(device, 1, &range);
                    if (invalidate != VK_SUCCESS)
                        return std::unexpected(vk_failure("invalidating 16-byte frame digest readback failed", invalidate));
                }
                std::memcpy(result.words.data(), slot.readback.mapped, kDigestBytes);
            }
            if (slot.query_pool != VK_NULL_HANDLE) {
                std::array<std::uint64_t, 2> timestamps{};
                const VkResult query_result = vkGetQueryPoolResults(
                    device, slot.query_pool, 0, 2, sizeof(timestamps), timestamps.data(),
                    sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
                if (query_result != VK_SUCCESS)
                    return std::unexpected(vk_failure("reading frame identity timestamp queries failed", query_result));
                const std::uint64_t mask = timestamp_valid_bits >= 64
                    ? std::numeric_limits<std::uint64_t>::max()
                    : ((std::uint64_t{1} << timestamp_valid_bits) - 1U);
                const std::uint64_t ticks = (timestamps[1] - timestamps[0]) & mask;
                const long double nanoseconds = static_cast<long double>(ticks) * timestamp_period;
                if (nanoseconds <= static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
                    result.gpu_time_ns = static_cast<std::uint64_t>(nanoseconds + 0.5L);
            }
            finished.push_back(result);
        }
        std::sort(finished.begin(), finished.end(), [](const auto& a, const auto& b) {
            return a.sequence < b.sequence;
        });

        for (const auto& done : finished) {
            FrameIdentityDigest digest{};
            digest.sequence = done.sequence;
            digest.words = done.words;
            digest.digest_available = done.digest_available;
            digest.gpu_time_ns = done.gpu_time_ns;
            if (done.digest_available) {
                if (previous_digest_sequence && previous_digest) {
                    digest.compared_to_sequence = previous_digest_sequence;
                    digest.same_as_compared_digest = *previous_digest == done.words;
                    digest.comparison_is_adjacent_capture =
                        *previous_digest_sequence != std::numeric_limits<std::uint64_t>::max() &&
                        *previous_digest_sequence + 1U == done.sequence;
                }
                previous_digest_sequence = done.sequence;
                previous_digest = done.words;
            }
            ready.push_back(digest);

            auto& slot = slots[done.slot_index];
            if (slot.source_ref) av_frame_free(&slot.source_ref);
            if (slot.source_view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, slot.source_view, nullptr);
                slot.source_view = VK_NULL_HANDLE;
            }
            slot.busy = false;
            --busy_count;
        }
        return {};
    }

    std::expected<FrameIdentitySubmit, FrameIdentityError> submit_locked(
        const AVFrame* source, std::uint64_t sequence) {
        auto collected = collect_completed_locked();
        if (!collected) return std::unexpected(collected.error());
        if (last_submitted_sequence && sequence <= *last_submitted_sequence)
            return std::unexpected(failure("frame identity sequence numbers must increase monotonically"));

        // Bound both GPU jobs and completed-but-unconsumed digest records by
        // the configured ring. A delayed stats reader must not grow host state.
        if (ready.size() + busy_count >= slots.size()) {
            last_submitted_sequence = sequence;
            return FrameIdentitySubmit::skipped_ring_full;
        }

        auto free_it = std::find_if(slots.begin(), slots.end(), [](const Slot& slot) {
            return !slot.busy;
        });
        if (free_it == slots.end()) {
            last_submitted_sequence = sequence;
            return FrameIdentitySubmit::skipped_ring_full;
        }
        if (!source || source->format != AV_PIX_FMT_VULKAN || !source->hw_frames_ctx || !source->data[0])
            return std::unexpected(failure("frame identity input must be a live AV_PIX_FMT_VULKAN frame"));
        if (source->width != static_cast<int>(options.width) ||
            source->height != static_cast<int>(options.height))
            return std::unexpected(failure("frame identity input dimensions do not match the configured extent"));

        auto* frames = reinterpret_cast<AVHWFramesContext*>(source->hw_frames_ctx->data);
        if (!frames || frames->format != AV_PIX_FMT_VULKAN || !frames->hwctx)
            return std::unexpected(failure("frame identity input has no Vulkan hardware frames context"));
        auto* frame_context = static_cast<AVVulkanFramesContext*>(frames->hwctx);
        auto* vk_frame = reinterpret_cast<AVVkFrame*>(source->data[0]);
        const VkFormat source_format = frame_context->format[0];
        const bool reads_source = options.mode != FrameIdentityMode::unrelated_workload;
        const auto mapping = reads_source
            ? source_mapping(source_format, frames->sw_format)
            : std::optional<VkComponentMapping>{};
        if (reads_source && !mapping)
            return std::unexpected(failure("unsupported Vulkan input format/sw_format for BGRA/RGB frame identity"));

        AVFrame* source_ref = av_frame_clone(source);
        if (!source_ref) return std::unexpected(failure("retaining Vulkan input frame for async identity read failed"));
        auto& slot = *free_it;
        const auto slot_index = static_cast<std::size_t>(free_it - slots.begin());

        if (!frame_context->lock_frame || !frame_context->unlock_frame) {
            av_frame_free(&source_ref);
            return std::unexpected(failure("FFmpeg Vulkan frame lock callbacks are unavailable"));
        }
        frame_context->lock_frame(frames, vk_frame);
        bool frame_locked = true;
        auto unlock_frame = [&]() {
            if (frame_locked) {
                frame_context->unlock_frame(frames, vk_frame);
                frame_locked = false;
            }
        };

        if ((reads_source && vk_frame->img[0] == VK_NULL_HANDLE) ||
            vk_frame->sem[0] == VK_NULL_HANDLE) {
            unlock_frame();
            av_frame_free(&source_ref);
            return std::unexpected(failure("frame identity input has no required Vulkan image or timeline semaphore"));
        }
        if (reads_source && vk_frame->layout[0] == VK_IMAGE_LAYOUT_UNDEFINED) {
            unlock_frame();
            av_frame_free(&source_ref);
            return std::unexpected(failure("frame identity source image is still in UNDEFINED layout"));
        }
        const std::uint32_t old_family = vk_frame->queue_family[0];
        const bool external_family = old_family == VK_QUEUE_FAMILY_EXTERNAL
#ifdef VK_QUEUE_FAMILY_FOREIGN_EXT
            || old_family == VK_QUEUE_FAMILY_FOREIGN_EXT
#endif
            ;
        if (reads_source && old_family != queue_family && old_family != VK_QUEUE_FAMILY_IGNORED && !external_family) {
            unlock_frame();
            av_frame_free(&source_ref);
            return std::unexpected(failure("frame identity cannot acquire the source image from an unrelated Vulkan queue family"));
        }
        if (vk_frame->sem_value[0] == std::numeric_limits<std::uint64_t>::max()) {
            unlock_frame();
            av_frame_free(&source_ref);
            return std::unexpected(failure("frame identity timeline semaphore value overflow"));
        }

        VkResult vr = VK_SUCCESS;
        VkImageViewCreateInfo view_info{};
        if (reads_source) {
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = vk_frame->img[0];
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = source_format;
            view_info.components = *mapping;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_info.subresourceRange.baseMipLevel = 0;
            view_info.subresourceRange.levelCount = 1;
            view_info.subresourceRange.baseArrayLayer = 0;
            view_info.subresourceRange.layerCount = 1;
            vr = vkCreateImageView(device, &view_info, nullptr, &slot.source_view);
            if (vr != VK_SUCCESS) {
                unlock_frame();
                av_frame_free(&source_ref);
                return std::unexpected(vk_failure("creating swizzled Vulkan source view failed", vr));
            }
        }

        const VkDescriptorBufferInfo partial_info{slot.partial.handle, 0, partial_bytes};
        if (reads_source) {
            const VkDescriptorImageInfo image_info{sampler, slot.source_view, VK_IMAGE_LAYOUT_GENERAL};
            const VkWriteDescriptorSet source_write{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, digest_sets[slot_index], 0, 0, 1,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &image_info, nullptr, nullptr};
            const VkWriteDescriptorSet partial_write{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, digest_sets[slot_index], 1, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &partial_info, nullptr};
            const std::array<VkWriteDescriptorSet, 2> descriptor_writes{source_write, partial_write};
            vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(descriptor_writes.size()),
                                   descriptor_writes.data(), 0, nullptr);
        } else {
            const VkWriteDescriptorSet workload_write{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, unrelated_sets[slot_index], 0, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &partial_info, nullptr};
            vkUpdateDescriptorSets(device, 1, &workload_write, 0, nullptr);
        }

        auto destroy_source_view = [&]() {
            if (slot.source_view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, slot.source_view, nullptr);
                slot.source_view = VK_NULL_HANDLE;
            }
        };
        vr = vkResetCommandBuffer(slot.command_buffer, 0);
        if (vr != VK_SUCCESS) {
            destroy_source_view();
            unlock_frame();
            av_frame_free(&source_ref);
            return std::unexpected(vk_failure("resetting frame identity command buffer failed", vr));
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vr = vkBeginCommandBuffer(slot.command_buffer, &begin);
        if (vr != VK_SUCCESS) {
            destroy_source_view();
            unlock_frame();
            av_frame_free(&source_ref);
            return std::unexpected(vk_failure("beginning frame identity command buffer failed", vr));
        }

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        if (reads_source) {
            VkImageMemoryBarrier2 source_barrier{};
            source_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            source_barrier.srcStageMask = external_family ? VK_PIPELINE_STAGE_2_NONE
                                                           : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            source_barrier.srcAccessMask = external_family ? VK_ACCESS_2_NONE
                                                           : vk_frame->access[0];
            source_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            source_barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            source_barrier.oldLayout = vk_frame->layout[0];
            source_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            source_barrier.srcQueueFamilyIndex = old_family == VK_QUEUE_FAMILY_IGNORED
                ? VK_QUEUE_FAMILY_IGNORED : old_family;
            source_barrier.dstQueueFamilyIndex = old_family == VK_QUEUE_FAMILY_IGNORED
                ? VK_QUEUE_FAMILY_IGNORED : queue_family;
            source_barrier.image = vk_frame->img[0];
            source_barrier.subresourceRange = view_info.subresourceRange;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &source_barrier;
            vkCmdPipelineBarrier2(slot.command_buffer, &dependency);
        }

        if (slot.query_pool != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(slot.command_buffer, slot.query_pool, 0, 2);
            vkCmdWriteTimestamp2(slot.command_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 slot.query_pool, 0);
        }

        if (options.mode == FrameIdentityMode::unrelated_workload) {
            vkCmdBindPipeline(slot.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                              unrelated_pipeline);
            vkCmdBindDescriptorSets(slot.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    unrelated_pipeline_layout, 0, 1,
                                    &unrelated_sets[slot_index], 0, nullptr);
            const PushWorkload workload{options.width, options.height,
                                        options.unrelated_iterations, 0};
            vkCmdPushConstants(slot.command_buffer, unrelated_pipeline_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(workload), &workload);
        } else {
            vkCmdBindPipeline(slot.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                              digest_pipeline);
            vkCmdBindDescriptorSets(slot.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    digest_pipeline_layout, 0, 1,
                                    &digest_sets[slot_index], 0, nullptr);
            const PushExtent push_extent{options.width, options.height};
            vkCmdPushConstants(slot.command_buffer, digest_pipeline_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push_extent), &push_extent);
        }
        vkCmdDispatch(slot.command_buffer, partial_count, 1, 1);

        if (options.mode == FrameIdentityMode::digest) {
            VkBufferMemoryBarrier2 partial_barrier{};
            partial_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            partial_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            partial_barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            partial_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            partial_barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            partial_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            partial_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            partial_barrier.buffer = slot.partial.handle;
            partial_barrier.offset = 0;
            partial_barrier.size = partial_bytes;
            dependency.imageMemoryBarrierCount = 0;
            dependency.pImageMemoryBarriers = nullptr;
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &partial_barrier;
            vkCmdPipelineBarrier2(slot.command_buffer, &dependency);

            vkCmdBindPipeline(slot.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                              reduce_pipeline);
            vkCmdBindDescriptorSets(slot.command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    reduce_pipeline_layout, 0, 1,
                                    &reduce_sets[slot_index], 0, nullptr);
            const PushReduce push_reduce{options.width, options.height,
                                         partial_count, pixel_count};
            vkCmdPushConstants(slot.command_buffer, reduce_pipeline_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push_reduce), &push_reduce);
            vkCmdDispatch(slot.command_buffer, 1, 1, 1);
        }

        if (slot.query_pool != VK_NULL_HANDLE)
            vkCmdWriteTimestamp2(slot.command_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 slot.query_pool, 1);

        if (options.mode == FrameIdentityMode::digest) {
            VkBufferMemoryBarrier2 digest_barrier{};
            digest_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            digest_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            digest_barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            digest_barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            digest_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            digest_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            digest_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            digest_barrier.buffer = slot.digest.handle;
            digest_barrier.offset = 0;
            digest_barrier.size = kDigestBytes;
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &digest_barrier;
            vkCmdPipelineBarrier2(slot.command_buffer, &dependency);

            VkBufferCopy copy{};
            copy.size = kDigestBytes;
            vkCmdCopyBuffer(slot.command_buffer, slot.digest.handle, slot.readback.handle, 1, &copy);

            VkBufferMemoryBarrier2 host_barrier{};
            host_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host_barrier.buffer = slot.readback.handle;
            host_barrier.offset = 0;
            host_barrier.size = kDigestBytes;
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &host_barrier;
            vkCmdPipelineBarrier2(slot.command_buffer, &dependency);
        }

        vr = vkEndCommandBuffer(slot.command_buffer);
        if (vr != VK_SUCCESS) {
            destroy_source_view();
            unlock_frame();
            av_frame_free(&source_ref);
            return std::unexpected(vk_failure("ending frame identity command buffer failed", vr));
        }

        VkSemaphoreSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = vk_frame->sem[0];
        wait.value = vk_frame->sem_value[0];
        wait.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkSemaphoreSubmitInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = vk_frame->sem[0];
        signal.value = vk_frame->sem_value[0] + 1U;
        signal.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkCommandBufferSubmitInfo command{};
        command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command.commandBuffer = slot.command_buffer;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;

        if (vkResetFences(device, 1, &slot.fence) != VK_SUCCESS) {
            destroy_source_view();
            unlock_frame();
            av_frame_free(&source_ref);
            return std::unexpected(failure("resetting frame identity completion fence failed"));
        }
#if FF_API_VULKAN_SYNC_QUEUES
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        if (!vulkan->lock_queue || !vulkan->unlock_queue) {
            destroy_source_view();
            unlock_frame();
            av_frame_free(&source_ref);
            return std::unexpected(failure("FFmpeg exposes no Vulkan queue lock for safe shared-queue submission"));
        }
        vulkan->lock_queue(av_device, queue_family, 0);
#pragma GCC diagnostic pop
#else
        destroy_source_view();
        unlock_frame();
        av_frame_free(&source_ref);
        return std::unexpected(failure("this FFmpeg version removed the public Vulkan queue lock; caller-side queue serialization is required"));
#endif
        vr = vkQueueSubmit2(queue, 1, &submit, slot.fence);
#if FF_API_VULKAN_SYNC_QUEUES
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        vulkan->unlock_queue(av_device, queue_family, 0);
#pragma GCC diagnostic pop
#endif
        if (vr != VK_SUCCESS) {
            destroy_source_view();
            unlock_frame();
            av_frame_free(&source_ref);
            return std::unexpected(vk_failure("submitting frame identity compute work failed", vr));
        }

        vk_frame->sem_value[0] += 1U;
        if (reads_source) {
            vk_frame->layout[0] = VK_IMAGE_LAYOUT_GENERAL;
            vk_frame->access[0] = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            if (old_family != VK_QUEUE_FAMILY_IGNORED)
                vk_frame->queue_family[0] = queue_family;
        }
        unlock_frame();

        slot.source_ref = source_ref;
        slot.sequence = sequence;
        slot.busy = true;
        ++busy_count;
        last_submitted_sequence = sequence;
        return FrameIdentitySubmit::submitted;
    }
};

FrameIdentity::FrameIdentity(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
FrameIdentity::~FrameIdentity() = default;
FrameIdentity::FrameIdentity(FrameIdentity&&) noexcept = default;
FrameIdentity& FrameIdentity::operator=(FrameIdentity&&) noexcept = default;

std::expected<FrameIdentity, FrameIdentityError> FrameIdentity::create(
    AVBufferRef* vulkan_device, FrameIdentityOptions options) {
    auto impl = std::make_unique<Impl>();
    impl->options = std::move(options);
    auto result = impl->initialize(vulkan_device);
    if (!result) return std::unexpected(result.error());
    return FrameIdentity(std::move(impl));
}

std::expected<FrameIdentitySubmit, FrameIdentityError> FrameIdentity::try_submit(
    const AVFrame* source, std::uint64_t sequence) {
    if (!impl_ || !impl_->initialized)
        return std::unexpected(failure("frame identity utility is not initialized"));
    std::lock_guard lock(impl_->mutex);
    return impl_->submit_locked(source, sequence);
}

std::expected<std::vector<FrameIdentityDigest>, FrameIdentityError> FrameIdentity::poll() {
    if (!impl_ || !impl_->initialized)
        return std::unexpected(failure("frame identity utility is not initialized"));
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->collect_completed_locked();
    if (!result) return std::unexpected(result.error());
    std::vector<FrameIdentityDigest> completed;
    completed.reserve(impl_->ready.size());
    while (!impl_->ready.empty()) {
        completed.push_back(std::move(impl_->ready.front()));
        impl_->ready.pop_front();
    }
    return completed;
}

std::expected<void, FrameIdentityError> FrameIdentity::drain() {
    if (!impl_ || !impl_->initialized)
        return std::unexpected(failure("frame identity utility is not initialized"));
    std::lock_guard lock(impl_->mutex);
    for (auto& slot : impl_->slots) {
        if (!slot.busy) continue;
        const VkResult vr = vkWaitForFences(impl_->device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
        if (vr != VK_SUCCESS)
            return std::unexpected(vk_failure("waiting for frame identity work failed", vr));
    }
    return impl_->collect_completed_locked();
}

std::size_t FrameIdentity::in_flight() const noexcept {
    if (!impl_) return 0;
    std::lock_guard lock(impl_->mutex);
    return impl_->busy_count;
}

std::size_t FrameIdentity::ring_size() const noexcept {
    return impl_ ? impl_->slots.size() : 0;
}

} // namespace fluxrec::gpu
