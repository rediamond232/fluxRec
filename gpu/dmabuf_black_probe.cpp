#include "gpu/dmabuf_black_probe.hpp"

#include "gpu/dmabuf_black_probe_metrics.hpp"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <vulkan/vulkan.h>

#include <linux/dma-buf.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <poll.h>
#include <span>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fluxrec::gpu::detail {
namespace {

constexpr std::uint32_t kWorkgroupWidth = 16U;
constexpr std::uint32_t kWorkgroupHeight = 16U;
constexpr std::uint32_t kWorkgroupInvocations =
    kWorkgroupWidth * kWorkgroupHeight;
constexpr int kSyncFileWaitTimeoutMs = 10'000;

GpuError probe_error(std::string message, int code = 0) {
    return GpuError{std::move(message), code};
}

GpuError vk_error(std::string message, VkResult result) {
    message += " (VkResult ";
    message += std::to_string(static_cast<int>(result));
    message += ')';
    return probe_error(std::move(message), static_cast<int>(result));
}

bool extension_enabled(const AVVulkanDeviceContext* context, const char* name) {
    if (!context || !name) return false;
    for (int index = 0; index < context->nb_enabled_dev_extensions; ++index) {
        if (context->enabled_dev_extensions[index] &&
            std::strcmp(context->enabled_dev_extensions[index], name) == 0) {
            return true;
        }
    }
    return false;
}

std::expected<std::vector<std::uint32_t>, GpuError> read_spirv(
    const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return std::unexpected(probe_error(
            "opening raw DMA-BUF black probe shader failed: " + path));
    }
    const std::streamoff end = input.tellg();
    if (end <= 0 || (end % 4) != 0) {
        return std::unexpected(probe_error(
            "raw DMA-BUF black probe shader has invalid byte size: " + path));
    }
    std::vector<std::uint32_t> words(static_cast<std::size_t>(end) / 4U);
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(words.data()), end);
    if (!input || words.front() != 0x07230203U) {
        return std::unexpected(probe_error(
            "reading raw DMA-BUF black probe SPIR-V failed: " + path));
    }
    return words;
}

std::expected<void, GpuError> wait_for_dma_buf_read_dependency_impl(
    int fd, std::uint64_t sequence) {
    dma_buf_export_sync_file exported{};
    exported.flags = DMA_BUF_SYNC_READ;
    exported.fd = -1;
    int ioctl_result = 0;
    do {
        ioctl_result = ::ioctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported);
    } while (ioctl_result < 0 && errno == EINTR);
    if (ioctl_result < 0) {
        const int saved_errno = errno;
        return std::unexpected(probe_error(
            "exporting DMA-BUF read dependency failed before observation at sequence " +
            std::to_string(sequence), AVERROR(saved_errno)));
    }
    capture::UniqueFd sync_file{exported.fd};
    if (!sync_file) {
        return std::unexpected(probe_error(
            "DMA-BUF read dependency export returned no sync-file fd at sequence " +
            std::to_string(sequence)));
    }

    pollfd descriptor{};
    descriptor.fd = sync_file.get();
    descriptor.events = POLLIN;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds{kSyncFileWaitTimeoutMs};
    int poll_result = -1;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            poll_result = 0;
            break;
        }
        const auto remaining_ms = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - now);
        poll_result = ::poll(&descriptor, 1,
            static_cast<int>(remaining_ms.count()));
        if (poll_result < 0 && errno == EINTR) continue;
        break;
    }
    if (poll_result == 0) {
        return std::unexpected(probe_error(
            "DMA-BUF read dependency did not signal within 10 seconds at sequence " +
            std::to_string(sequence), AVERROR(ETIMEDOUT)));
    }
    if (poll_result < 0) {
        const int saved_errno = errno;
        return std::unexpected(probe_error(
            "polling DMA-BUF writer dependency failed at sequence " +
            std::to_string(sequence), AVERROR(saved_errno)));
    }
    if ((descriptor.revents & POLLIN) == 0 ||
        (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return std::unexpected(probe_error(
            "DMA-BUF read dependency signaled with unexpected poll events at sequence " +
            std::to_string(sequence), AVERROR(EIO)));
    }
    return {};
}

void log_interval(const DmabufBlackInterval& interval) noexcept {
    std::fprintf(stderr,
        "{\"type\":\"raw_dmabuf_black_interval\",\"first_sequence\":%llu,"
        "\"last_sequence\":%llu,\"first_presentation_time_ns\":%llu,"
        "\"last_presentation_time_ns\":%llu,\"black_frames\":%llu,"
        "\"closed_at_end\":%s}\n",
        static_cast<unsigned long long>(interval.first_sequence),
        static_cast<unsigned long long>(interval.last_sequence),
        static_cast<unsigned long long>(interval.first_presentation_time_ns),
        static_cast<unsigned long long>(interval.last_presentation_time_ns),
        static_cast<unsigned long long>(interval.black_frames),
        interval.closed_at_end ? "true" : "false");
}

void log_sample(std::uint64_t sequence, std::uint64_t presentation_time_ns,
                bool has_presentation_time, std::size_t slot,
                std::uint32_t black_pixels, std::uint32_t total_pixels,
                bool near_black) noexcept {
    std::fprintf(stderr,
        "{\"type\":\"raw_dmabuf_black_sample\",\"capture_sequence\":%llu,"
        "\"presentation_time_ns\":%llu,\"has_presentation_time\":%s,"
        "\"slot\":%zu,\"black_pixels\":%u,\"total_pixels\":%u,"
        "\"near_black\":%s}\n",
        static_cast<unsigned long long>(sequence),
        static_cast<unsigned long long>(presentation_time_ns),
        has_presentation_time ? "true" : "false", slot,
        black_pixels, total_pixels, near_black ? "true" : "false");
}

} // namespace

std::expected<void, GpuError> wait_for_dma_buf_read_dependency(
    int dma_buf_fd, std::uint64_t sequence) {
    return wait_for_dma_buf_read_dependency_impl(dma_buf_fd, sequence);
}

struct DmabufBlackProbe::Impl {
    struct ImportedImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        dev_t device_number{};
        ino_t inode_number{};
        std::uint32_t descriptor_width = 0;
        std::uint32_t descriptor_height = 0;
        std::uint32_t descriptor_fourcc = 0;
        std::uint32_t descriptor_offset = 0;
        std::uint32_t descriptor_stride = 0;
        std::uint64_t descriptor_modifier = 0;
        bool initialized = false;
    };

    struct ReadbackBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkMemoryPropertyFlags properties = 0;
        void* mapped = nullptr;
    };

    AVBufferRef* device_ref = nullptr;
    AVHWDeviceContext* av_device = nullptr;
    AVVulkanDeviceContext* vulkan = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t group_count_x = 0;
    std::uint32_t group_count_y = 0;
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    std::uint32_t total_pixels = 0;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkPhysicalDeviceProperties physical_properties{};
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = nullptr;
    std::vector<ImportedImage> images;
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence completion_fence = VK_NULL_HANDLE;
    ReadbackBuffer readback;
    DmabufBlackIntervalTracker intervals;
    std::uint64_t sampled_frames = 0;
    std::uint64_t near_black_frames = 0;
    std::uint64_t emitted_intervals = 0;
    bool initialized = false;
    bool finished = false;

    ~Impl() { destroy(); }

    std::expected<std::uint32_t, GpuError> choose_memory_type(
        std::uint32_t allowed_bits, VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred) const {
        std::uint32_t selected = std::numeric_limits<std::uint32_t>::max();
        unsigned selected_score = 0;
        bool found = false;
        for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
            if ((allowed_bits & (1U << index)) == 0U) continue;
            const auto flags = memory_properties.memoryTypes[index].propertyFlags;
            if ((flags & required) != required) continue;
            const unsigned score = static_cast<unsigned>(
                std::popcount(flags & preferred));
            if (!found || score > selected_score) {
                selected = index;
                selected_score = score;
                found = true;
            }
        }
        if (!found) {
            return std::unexpected(probe_error(
                "no Vulkan memory type is compatible with the raw DMA-BUF import"));
        }
        return selected;
    }

    void destroy_buffer() noexcept {
        if (device != VK_NULL_HANDLE && readback.mapped && readback.memory != VK_NULL_HANDLE)
            vkUnmapMemory(device, readback.memory);
        readback.mapped = nullptr;
        if (device != VK_NULL_HANDLE && readback.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, readback.buffer, nullptr);
        if (device != VK_NULL_HANDLE && readback.memory != VK_NULL_HANDLE)
            vkFreeMemory(device, readback.memory, nullptr);
        readback = {};
    }

    void destroy() noexcept {
        if (device != VK_NULL_HANDLE) {
            if (descriptor_pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            for (auto& imported : images) {
                if (imported.view != VK_NULL_HANDLE)
                    vkDestroyImageView(device, imported.view, nullptr);
                if (imported.image != VK_NULL_HANDLE)
                    vkDestroyImage(device, imported.image, nullptr);
                if (imported.memory != VK_NULL_HANDLE)
                    vkFreeMemory(device, imported.memory, nullptr);
            }
            if (completion_fence != VK_NULL_HANDLE)
                vkDestroyFence(device, completion_fence, nullptr);
            if (command_pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(device, command_pool, nullptr);
            if (sampler != VK_NULL_HANDLE)
                vkDestroySampler(device, sampler, nullptr);
            if (pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, pipeline, nullptr);
            if (pipeline_layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            if (descriptor_layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
            destroy_buffer();
        }
        images.clear();
        if (device_ref) av_buffer_unref(&device_ref);
        device = VK_NULL_HANDLE;
        initialized = false;
    }

    std::expected<void, GpuError> initialize(
        AVBufferRef* supplied_device,
        std::span<const capture::DmabufImage> pool,
        std::uint32_t requested_width,
        std::uint32_t requested_height,
        std::uint32_t requested_fourcc,
        std::uint64_t requested_modifier,
        const std::string& shader_path) {
        if (!supplied_device || !supplied_device->data || pool.empty()) {
            return std::unexpected(probe_error(
                "raw DMA-BUF black probe needs a Vulkan device and nonempty capture pool"));
        }
        if (requested_width == 0U || requested_height == 0U ||
            static_cast<std::uint64_t>(requested_width) * requested_height >
                std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(probe_error(
                "raw DMA-BUF black probe dimensions do not fit its aggregate counter"));
        }

        device_ref = av_buffer_ref(supplied_device);
        if (!device_ref) {
            return std::unexpected(probe_error(
                "retaining Vulkan device for raw DMA-BUF black probe failed"));
        }
        av_device = reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
        if (av_device->type != AV_HWDEVICE_TYPE_VULKAN || !av_device->hwctx) {
            return std::unexpected(probe_error(
                "raw DMA-BUF black probe received a non-Vulkan device context"));
        }
        vulkan = static_cast<AVVulkanDeviceContext*>(av_device->hwctx);
        device = vulkan->act_dev;
        physical_device = vulkan->phys_dev;
        if (device == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE) {
            return std::unexpected(probe_error(
                "raw DMA-BUF black probe received incomplete Vulkan device handles"));
        }

        for (const char* required : {
                 VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                 VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                 VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME}) {
            if (!extension_enabled(vulkan, required)) {
                return std::unexpected(probe_error(
                    std::string("raw DMA-BUF black probe requires enabled Vulkan extension ") +
                    required));
            }
        }
        get_memory_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
            vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
        if (!get_memory_fd_properties) {
            return std::unexpected(probe_error(
                "vkGetMemoryFdPropertiesKHR is unavailable for raw DMA-BUF import"));
        }

        if (vulkan->nb_qf <= 0) {
            return std::unexpected(probe_error(
                "Vulkan device exposes no queue families for raw DMA-BUF probe"));
        }
        for (int index = 0; index < vulkan->nb_qf; ++index) {
            if ((vulkan->qf[index].flags & VK_QUEUE_COMPUTE_BIT) == 0 ||
                vulkan->qf[index].num <= 0) {
                continue;
            }
            queue_family = static_cast<std::uint32_t>(vulkan->qf[index].idx);
            vkGetDeviceQueue(device, queue_family, 0, &queue);
            if (queue != VK_NULL_HANDLE) break;
            queue_family = VK_QUEUE_FAMILY_IGNORED;
        }
        if (queue == VK_NULL_HANDLE || queue_family == VK_QUEUE_FAMILY_IGNORED) {
            return std::unexpected(probe_error(
                "Vulkan device exposes no enabled compute queue for raw DMA-BUF probe"));
        }
#if FF_API_VULKAN_SYNC_QUEUES
        if (!vulkan->lock_queue || !vulkan->unlock_queue) {
            return std::unexpected(probe_error(
                "raw DMA-BUF probe cannot safely share the FFmpeg queue without both queue-lock callbacks"));
        }
#else
        return std::unexpected(probe_error(
            "raw DMA-BUF probe requires FFmpeg's Vulkan queue-lock callback API"));
#endif

        width = requested_width;
        height = requested_height;
        fourcc = requested_fourcc;
        modifier = requested_modifier;
        total_pixels = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(width) * height);
        group_count_x = width / kWorkgroupWidth +
            static_cast<std::uint32_t>((width % kWorkgroupWidth) != 0U);
        group_count_y = height / kWorkgroupHeight +
            static_cast<std::uint32_t>((height % kWorkgroupHeight) != 0U);
        vkGetPhysicalDeviceProperties(physical_device, &physical_properties);
        const auto& limits = physical_properties.limits;
        if (limits.maxComputeWorkGroupSize[0] < kWorkgroupWidth ||
            limits.maxComputeWorkGroupSize[1] < kWorkgroupHeight ||
            limits.maxComputeWorkGroupInvocations < kWorkgroupInvocations ||
            limits.maxComputeWorkGroupCount[0] < group_count_x ||
            limits.maxComputeWorkGroupCount[1] < group_count_y ||
            limits.maxComputeSharedMemorySize < sizeof(std::uint32_t)) {
            return std::unexpected(probe_error(
                "Vulkan device limits do not support raw DMA-BUF black-count workgroups"));
        }
        vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
        images.resize(pool.size());

        for (std::size_t index = 0; index < pool.size(); ++index) {
            const auto& image = pool[index];
            if (image.width != width || image.height != height ||
                image.fourcc != fourcc || image.modifier != modifier ||
                image.planes.size() != 1U || image.planes[0].fd < 0 ||
                static_cast<std::uint64_t>(image.planes[0].stride) <
                    static_cast<std::uint64_t>(width) * 4U) {
                return std::unexpected(probe_error(
                    "raw DMA-BUF black probe pool descriptor is incompatible with its capture format"));
            }
            auto created = import_image(image, images[index]);
            if (!created) return created;
        }

        auto output = create_readback_buffer();
        if (!output) return output;
        auto compute = create_compute_pipeline(shader_path);
        if (!compute) return compute;
        auto descriptors = create_descriptors();
        if (!descriptors) return descriptors;
        auto command_resources = create_command_resources();
        if (!command_resources) return command_resources;
        initialized = true;
        return {};
    }

    std::expected<void, GpuError> import_image(
        const capture::DmabufImage& descriptor, ImportedImage& imported) {
        struct stat identity {};
        if (::fstat(descriptor.planes[0].fd, &identity) < 0) {
            return std::unexpected(probe_error(
                "fstat on raw DMA-BUF pool fd failed", AVERROR(errno)));
        }
        imported.device_number = identity.st_dev;
        imported.inode_number = identity.st_ino;
        imported.descriptor_width = descriptor.width;
        imported.descriptor_height = descriptor.height;
        imported.descriptor_fourcc = descriptor.fourcc;
        imported.descriptor_modifier = descriptor.modifier;
        imported.descriptor_offset = descriptor.planes[0].offset;
        imported.descriptor_stride = descriptor.planes[0].stride;

        VkSubresourceLayout plane_layout{};
        plane_layout.offset = descriptor.planes[0].offset;
        plane_layout.rowPitch = descriptor.planes[0].stride;

        VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info{};
        modifier_info.sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
        modifier_info.drmFormatModifier = descriptor.modifier;
        modifier_info.drmFormatModifierPlaneCount = 1;
        modifier_info.pPlaneLayouts = &plane_layout;

        VkExternalMemoryImageCreateInfo external_image{};
        external_image.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external_image.pNext = &modifier_info;
        external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT query_modifier{};
        query_modifier.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
        query_modifier.drmFormatModifier = descriptor.modifier;
        query_modifier.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkPhysicalDeviceExternalImageFormatInfo query_external{};
        query_external.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        query_external.pNext = &query_modifier;
        query_external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        VkPhysicalDeviceImageFormatInfo2 query{};
        query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        query.pNext = &query_external;
        query.format = VK_FORMAT_B8G8R8A8_UNORM;
        query.type = VK_IMAGE_TYPE_2D;
        query.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        query.usage = usage;

        VkExternalImageFormatProperties external_properties{};
        external_properties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 format_properties{};
        format_properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        format_properties.pNext = &external_properties;
        VkResult result = vkGetPhysicalDeviceImageFormatProperties2(
            physical_device, &query, &format_properties);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                "querying raw DMA-BUF modifier import support", result));
        }
        if (format_properties.imageFormatProperties.maxExtent.width < width ||
            format_properties.imageFormatProperties.maxExtent.height < height) {
            return std::unexpected(probe_error(
                "raw DMA-BUF modifier import exceeds Vulkan image extent limits"));
        }
        const auto& external_memory = external_properties.externalMemoryProperties;
        if ((external_memory.externalMemoryFeatures &
             VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0U ||
            (external_memory.compatibleHandleTypes &
             VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) == 0U) {
            return std::unexpected(probe_error(
                "selected raw DMA-BUF modifier is not Vulkan-importable"));
        }

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.pNext = &external_image;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        image_info.extent = VkExtent3D{width, height, 1U};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        image_info.usage = usage;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        result = vkCreateImage(device, &image_info, nullptr, &imported.image);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("creating raw imported DMA-BUF image", result));
        }

        VkImageMemoryRequirementsInfo2 requirements_info{};
        requirements_info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
        requirements_info.image = imported.image;
        VkMemoryDedicatedRequirements dedicated_requirements{};
        dedicated_requirements.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
        VkMemoryRequirements2 requirements{};
        requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        requirements.pNext = &dedicated_requirements;
        vkGetImageMemoryRequirements2(device, &requirements_info, &requirements);

        const int raw_import_fd = ::fcntl(descriptor.planes[0].fd, F_DUPFD_CLOEXEC, 0);
        if (raw_import_fd < 0) {
            return std::unexpected(probe_error(
                "duplicating DMA-BUF fd for raw Vulkan import failed", AVERROR(errno)));
        }
        capture::UniqueFd import_fd{raw_import_fd};
        VkMemoryFdPropertiesKHR fd_properties{};
        fd_properties.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        result = get_memory_fd_properties(
            device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            import_fd.get(), &fd_properties);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                "querying raw DMA-BUF memory fd properties", result));
        }
        const std::uint32_t memory_bits =
            requirements.memoryRequirements.memoryTypeBits & fd_properties.memoryTypeBits;
        auto memory_type = choose_memory_type(
            memory_bits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!memory_type) return std::unexpected(memory_type.error());

        VkImportMemoryFdInfoKHR import_info{};
        import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        import_info.fd = import_fd.get();
        import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkMemoryDedicatedAllocateInfo dedicated_info{};
        dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated_info.pNext = &import_info;
        dedicated_info.image = imported.image;
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.pNext = (dedicated_requirements.prefersDedicatedAllocation ||
                            dedicated_requirements.requiresDedicatedAllocation)
            ? static_cast<const void*>(&dedicated_info)
            : static_cast<const void*>(&import_info);
        allocation.allocationSize = requirements.memoryRequirements.size;
        allocation.memoryTypeIndex = *memory_type;
        result = vkAllocateMemory(device, &allocation, nullptr, &imported.memory);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("importing raw DMA-BUF Vulkan memory", result));
        }
        // A successful import transfers ownership of the duplicated fd to Vulkan.
        (void)import_fd.release();
        result = vkBindImageMemory(device, imported.image, imported.memory, 0);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("binding raw DMA-BUF Vulkan memory", result));
        }

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = imported.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_B8G8R8A8_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        result = vkCreateImageView(device, &view_info, nullptr, &imported.view);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("creating raw DMA-BUF image view", result));
        }
        return {};
    }

    std::expected<void, GpuError> create_readback_buffer() {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = sizeof(std::uint32_t);
        buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = vkCreateBuffer(device, &buffer_info, nullptr, &readback.buffer);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("creating raw DMA-BUF counter buffer", result));
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, readback.buffer, &requirements);
        auto memory_type = choose_memory_type(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!memory_type) return std::unexpected(memory_type.error());
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *memory_type;
        result = vkAllocateMemory(device, &allocation, nullptr, &readback.memory);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("allocating raw DMA-BUF counter memory", result));
        }
        result = vkBindBufferMemory(device, readback.buffer, readback.memory, 0);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("binding raw DMA-BUF counter memory", result));
        }
        readback.properties = memory_properties.memoryTypes[*memory_type].propertyFlags;
        result = vkMapMemory(device, readback.memory, 0, VK_WHOLE_SIZE, 0,
                             &readback.mapped);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("mapping raw DMA-BUF aggregate counter", result));
        }
        return {};
    }

    std::expected<void, GpuError> create_compute_pipeline(
        const std::string& shader_path) {
        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
             VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        }};
        VkDescriptorSetLayoutCreateInfo descriptor_info{};
        descriptor_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptor_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        descriptor_info.pBindings = bindings.data();
        VkResult result = vkCreateDescriptorSetLayout(
            device, &descriptor_info, nullptr, &descriptor_layout);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("creating raw DMA-BUF descriptor layout", result));
        }

        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.size = 2U * sizeof(std::uint32_t);
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &descriptor_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_range;
        result = vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("creating raw DMA-BUF pipeline layout", result));
        }

        auto code = read_spirv(shader_path);
        if (!code) return std::unexpected(code.error());
        VkShaderModuleCreateInfo shader_info{};
        shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize = code->size() * sizeof(std::uint32_t);
        shader_info.pCode = code->data();
        VkShaderModule shader = VK_NULL_HANDLE;
        result = vkCreateShaderModule(device, &shader_info, nullptr, &shader);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("creating raw DMA-BUF probe shader module", result));
        }
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage = stage;
        pipeline_info.layout = pipeline_layout;
        result = vkCreateComputePipelines(
            device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
        vkDestroyShaderModule(device, shader, nullptr);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("creating raw DMA-BUF compute pipeline", result));
        }

        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = 0.0F;
        result = vkCreateSampler(device, &sampler_info, nullptr, &sampler);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("creating raw DMA-BUF probe sampler", result));
        }
        return {};
    }

    std::expected<void, GpuError> create_descriptors() {
        const auto slot_count = static_cast<std::uint32_t>(images.size());
        const std::array<VkDescriptorPoolSize, 2> sizes{{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, slot_count},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, slot_count},
        }};
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = slot_count;
        pool_info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pool_info.pPoolSizes = sizes.data();
        VkResult result = vkCreateDescriptorPool(
            device, &pool_info, nullptr, &descriptor_pool);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("creating raw DMA-BUF descriptor pool", result));
        }
        std::vector<VkDescriptorSetLayout> layouts(images.size(), descriptor_layout);
        std::vector<VkDescriptorSet> sets(images.size(), VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate.descriptorPool = descriptor_pool;
        allocate.descriptorSetCount = slot_count;
        allocate.pSetLayouts = layouts.data();
        result = vkAllocateDescriptorSets(device, &allocate, sets.data());
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("allocating raw DMA-BUF descriptor sets", result));
        }

        for (std::size_t index = 0; index < images.size(); ++index) {
            images[index].descriptor = sets[index];
            const VkDescriptorImageInfo image_info{
                sampler, images[index].view, VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorBufferInfo buffer_info{
                readback.buffer, 0, sizeof(std::uint32_t)};
            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = sets[index];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &image_info;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = sets[index];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].pBufferInfo = &buffer_info;
            vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
        return {};
    }

    std::expected<void, GpuError> create_command_resources() {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = queue_family;
        VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &command_pool);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("creating raw DMA-BUF command pool", result));
        }
        VkCommandBufferAllocateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = command_pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(device, &command_info, &command_buffer);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("allocating raw DMA-BUF command buffer", result));
        }
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        result = vkCreateFence(device, &fence_info, nullptr, &completion_fence);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("creating raw DMA-BUF completion fence", result));
        }
        return {};
    }

    std::expected<void, GpuError> record_and_submit(ImportedImage& imported,
                                                   std::uint64_t sequence) {
        VkResult result = vkResetCommandBuffer(command_buffer, 0);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("resetting raw DMA-BUF command buffer", result));
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer, &begin);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("beginning raw DMA-BUF command buffer", result));
        }

        VkImageMemoryBarrier2 acquire{};
        acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        acquire.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        acquire.srcAccessMask = 0;
        acquire.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        acquire.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        acquire.oldLayout = imported.initialized
            ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        acquire.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        acquire.dstQueueFamilyIndex = queue_family;
        acquire.image = imported.image;
        acquire.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        acquire.subresourceRange.levelCount = 1;
        acquire.subresourceRange.layerCount = 1;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &acquire;
        vkCmdPipelineBarrier2(command_buffer, &dependency);

        vkCmdFillBuffer(command_buffer, readback.buffer, 0,
                        sizeof(std::uint32_t), 0U);
        VkMemoryBarrier2 clear_to_shader{};
        clear_to_shader.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        clear_to_shader.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        clear_to_shader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        clear_to_shader.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        clear_to_shader.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        dependency = {};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &clear_to_shader;
        vkCmdPipelineBarrier2(command_buffer, &dependency);

        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline_layout, 0, 1, &imported.descriptor, 0, nullptr);
        const std::array<std::uint32_t, 2> extent{width, height};
        vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, static_cast<std::uint32_t>(sizeof(extent)), extent.data());
        vkCmdDispatch(command_buffer, group_count_x, group_count_y, 1U);

        VkImageMemoryBarrier2 release{};
        release.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        release.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        release.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        release.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        release.dstAccessMask = 0;
        release.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        release.srcQueueFamilyIndex = queue_family;
        release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        release.image = imported.image;
        release.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        release.subresourceRange.levelCount = 1;
        release.subresourceRange.layerCount = 1;
        VkMemoryBarrier2 shader_to_host{};
        shader_to_host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        shader_to_host.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        shader_to_host.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        shader_to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        shader_to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        dependency = {};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &shader_to_host;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &release;
        vkCmdPipelineBarrier2(command_buffer, &dependency);
        result = vkEndCommandBuffer(command_buffer);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("ending raw DMA-BUF command buffer", result));
        }

        result = vkResetFences(device, 1, &completion_fence);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("resetting raw DMA-BUF completion fence", result));
        }
        VkCommandBufferSubmitInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command_info.commandBuffer = command_buffer;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command_info;
#if FF_API_VULKAN_SYNC_QUEUES
        vulkan->lock_queue(av_device, queue_family, 0);
#endif
        result = vkQueueSubmit2(queue, 1, &submit, completion_fence);
#if FF_API_VULKAN_SYNC_QUEUES
        vulkan->unlock_queue(av_device, queue_family, 0);
#endif
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("submitting raw DMA-BUF sample", result));
        }
        constexpr std::uint64_t kCompletionFenceTimeoutNs =
            static_cast<std::uint64_t>(kSyncFileWaitTimeoutMs) * 1'000'000ULL;
        result = vkWaitForFences(device, 1, &completion_fence, VK_TRUE,
                                 kCompletionFenceTimeoutNs);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr,
                "{\"type\":\"raw_dmabuf_black_probe_fatal\","
                "\"reason\":\"completion_fence_wait_failed\","
                "\"capture_sequence\":%llu,\"vk_result\":%d,"
                "\"timeout_ms\":%d}\n",
                static_cast<unsigned long long>(sequence),
                static_cast<int>(result),
                kSyncFileWaitTimeoutMs);
            std::fflush(stderr);
            // Any unsuccessful wait leaves completion uncertain. The command
            // may still read this leased DMA-BUF and reference probe Vulkan
            // objects, so do not unwind, release the lease, or destroy them.
            std::_Exit(EXIT_FAILURE);
        }
        imported.initialized = true;
        return {};
    }

    std::expected<void, GpuError> inspect_frame(
        const capture::CapturedFrame& captured) {
        if (!initialized || finished) {
            return std::unexpected(probe_error(
                "raw DMA-BUF black probe is not available for another sample"));
        }
        if (!captured.lease) {
            return std::unexpected(probe_error(
                "raw DMA-BUF black probe received a frame without a live lease"));
        }
        const std::size_t index = captured.lease.slot_index();
        if (index >= images.size()) {
            return std::unexpected(probe_error(
                "captured frame slot is outside the raw DMA-BUF probe pool"));
        }
        const auto& actual = captured.lease.image();
        const auto& imported = images[index];
        if (actual.width != imported.descriptor_width ||
            actual.height != imported.descriptor_height ||
            actual.fourcc != imported.descriptor_fourcc ||
            actual.modifier != imported.descriptor_modifier ||
            actual.planes.size() != 1U ||
            actual.planes[0].offset != imported.descriptor_offset ||
            actual.planes[0].stride != imported.descriptor_stride) {
            return std::unexpected(probe_error(
                "captured frame descriptor differs from its independently imported raw DMA-BUF slot"));
        }
        if (actual.planes.empty() || actual.planes[0].fd < 0) {
            return std::unexpected(probe_error(
                "captured raw DMA-BUF descriptor has no valid plane fd"));
        }
        struct stat identity {};
        if (::fstat(actual.planes[0].fd, &identity) < 0) {
            return std::unexpected(probe_error(
                "fstat on leased DMA-BUF fd failed", AVERROR(errno)));
        }
        if (identity.st_dev != imported.device_number ||
            identity.st_ino != imported.inode_number) {
            return std::unexpected(probe_error(
                "leased DMA-BUF fd identity differs from its independently imported pool slot"));
        }
        auto dependency = wait_for_dma_buf_read_dependency(
            actual.planes[0].fd, captured.sequence);
        if (!dependency) return dependency;

        auto submitted = record_and_submit(images[index], captured.sequence);
        if (!submitted) return submitted;
        if ((readback.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0U) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = readback.memory;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            const VkResult result = vkInvalidateMappedMemoryRanges(device, 1, &range);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error(
                    "invalidating raw DMA-BUF aggregate counter", result));
            }
        }
        std::uint32_t black_pixels = 0;
        std::memcpy(&black_pixels, readback.mapped, sizeof(black_pixels));
        if (black_pixels > total_pixels) {
            return std::unexpected(probe_error(
                "raw DMA-BUF shader aggregate exceeds the frame pixel count"));
        }
        ++sampled_frames;
        const bool near_black = is_near_black_frame(black_pixels, total_pixels);
        if (near_black) ++near_black_frames;
        log_sample(captured.sequence, captured.presentation_time_ns,
                   captured.has_presentation_time, index, black_pixels,
                   total_pixels, near_black);
        auto completed = intervals.observe(captured.sequence,
            captured.presentation_time_ns, black_pixels, total_pixels);
        if (completed) {
            log_interval(*completed);
            ++emitted_intervals;
        }
        return {};
    }

    void finish_probe() noexcept {
        if (finished) return;
        if (auto trailing = intervals.finish()) {
            log_interval(*trailing);
            ++emitted_intervals;
        }
        if (sampled_frames != 0U) {
            std::fprintf(stderr,
                "{\"type\":\"raw_dmabuf_black_summary\",\"sampled_frames\":%llu,"
                "\"near_black_frames\":%llu,\"near_black_intervals\":%llu,"
                "\"black_threshold_luma\":0.10,\"minimum_black_pixels_percent\":98}\n",
                static_cast<unsigned long long>(sampled_frames),
                static_cast<unsigned long long>(near_black_frames),
                static_cast<unsigned long long>(emitted_intervals));
        }
        finished = true;
    }
};

DmabufBlackProbe::DmabufBlackProbe(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

DmabufBlackProbe::~DmabufBlackProbe() = default;
DmabufBlackProbe::DmabufBlackProbe(DmabufBlackProbe&&) noexcept = default;
DmabufBlackProbe& DmabufBlackProbe::operator=(DmabufBlackProbe&&) noexcept = default;

std::expected<DmabufBlackProbe, GpuError> DmabufBlackProbe::create(
    AVBufferRef* vulkan_device,
    std::span<const capture::DmabufImage> pool,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t fourcc,
    std::uint64_t modifier,
    std::string shader_path) {
    auto impl = std::make_unique<Impl>();
    auto initialized = impl->initialize(vulkan_device, pool, width, height,
                                        fourcc, modifier, shader_path);
    if (!initialized) return std::unexpected(initialized.error());
    return DmabufBlackProbe(std::move(impl));
}

std::expected<void, GpuError> DmabufBlackProbe::inspect(
    const capture::CapturedFrame& captured) {
    if (!impl_) {
        return std::unexpected(probe_error("raw DMA-BUF black probe has no resources"));
    }
    return impl_->inspect_frame(captured);
}

void DmabufBlackProbe::finish() noexcept {
    if (impl_) impl_->finish_probe();
}

} // namespace fluxrec::gpu::detail
