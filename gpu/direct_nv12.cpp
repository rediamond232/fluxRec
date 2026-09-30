#include "gpu/direct_nv12.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fluxrec::gpu::detail {
namespace {

GpuError vk_error(std::string operation, VkResult result) {
    operation += " failed with VkResult ";
    operation += std::to_string(static_cast<int>(result));
    return GpuError{std::move(operation), static_cast<int>(result)};
}

AVVkFrame* vk_frame(AVFrame* frame) {
    if (!frame || frame->format != AV_PIX_FMT_VULKAN || !frame->data[0]) return nullptr;
    return reinterpret_cast<AVVkFrame*>(frame->data[0]);
}

AVHWFramesContext* frames_context(AVFrame* frame) {
    return frame && frame->hw_frames_ctx
        ? reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data) : nullptr;
}

VkImageMemoryBarrier2 image_barrier(VkImage image, VkImageLayout old_layout,
                                     VkAccessFlags2 old_access,
                                     std::uint32_t old_queue_family,
                                     std::uint32_t compute_family,
                                     VkAccessFlags2 new_access,
                                     VkImageLayout new_layout = VK_IMAGE_LAYOUT_GENERAL,
                                     VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                     VkPipelineStageFlags2 new_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) {
    const bool ownership_transfer = old_queue_family != VK_QUEUE_FAMILY_IGNORED &&
        old_queue_family != compute_family;
    const bool old_state_defined = old_layout != VK_IMAGE_LAYOUT_UNDEFINED;
    // Vulkan Video encode ran on a distinct queue. The frame's timeline
    // semaphore wait provides the cross-queue dependency; a barrier recorded
    // on this compute queue must not name VIDEO_ENCODE_READ as a source access
    // (that access stage is unsupported by the compute queue). Keep the old
    // layout for the transition, with an empty local source scope.
    const bool previous_video_encode = old_layout == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
    return VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = old_state_defined && !previous_video_encode
            ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = old_state_defined && !previous_video_encode ? old_access : 0,
        .dstStageMask = new_stage,
        .dstAccessMask = new_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = ownership_transfer ? old_queue_family : VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = ownership_transfer ? compute_family : VK_QUEUE_FAMILY_IGNORED,
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

VkImageMemoryBarrier2 external_acquire_barrier(VkImage image,
                                                VkImageLayout old_layout,
                                                std::uint32_t external_family,
                                                std::uint32_t compute_family) {
    return VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        // The producer's work is ordered by the imported DMA-BUF sync file
        // semaphore. There is no Vulkan source queue scope to name here.
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = old_layout,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = external_family,
        .dstQueueFamilyIndex = compute_family,
        .image = image,
        .subresourceRange = VkImageSubresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
}

VkImageMemoryBarrier2 external_release_barrier(VkImage image,
                                                std::uint32_t compute_family,
                                                std::uint32_t external_family) {
    return VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        // The compositor takes ownership after the DMA-BUF lease is returned.
        .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = compute_family,
        .dstQueueFamilyIndex = external_family,
        .image = image,
        .subresourceRange = VkImageSubresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
}

} // namespace

struct DirectNv12Converter::Impl {
    enum class SourceSyncProbe {
        none,
        wait_only,
        barrier_only,
    };

    AVBufferRef* device_ref = nullptr; // Borrowed from GpuEncoder::Impl.
    AVHWDeviceContext* device_context = nullptr;
    AVVulkanDeviceContext* vk_context = nullptr;
    AVHWFramesContext* input_frames = nullptr;
    AVHWFramesContext* output_frames = nullptr;
    AVVulkanFramesContext* input_vk_frames = nullptr;
    AVVulkanFramesContext* output_vk_frames = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkQueue compute_queue = VK_NULL_HANDLE;
    std::uint32_t compute_queue_family = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool trace_sync = false;
    bool disable_resource_reuse = false;
    bool debug_readback_nv12 = false;
    bool debug_source_probe = false;
    bool wait_conversion_before_encode = false;
    enum class SourceExternalOwnership : std::uint8_t {
        none,
        external,
        foreign,
    };
    SourceExternalOwnership source_external_ownership = SourceExternalOwnership::none;
    std::uint32_t source_external_queue_family = VK_QUEUE_FAMILY_IGNORED;
    SourceSyncProbe source_sync_probe = SourceSyncProbe::none;
    std::uint64_t dispatch_sequence = 0;
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shader_module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkBuffer source_probe_dummy = VK_NULL_HANDLE;
    VkDeviceMemory source_probe_dummy_memory = VK_NULL_HANDLE;
    struct ProbeHistory {
        std::uint64_t capture_sequence = 0;
        std::uint64_t hash = 0;
    };
    std::unordered_map<std::size_t, ProbeHistory> previous_probe_by_slot;
    std::vector<DirectFrameResources> available_resources;

    ~Impl() {
        if (device == VK_NULL_HANDLE) return;
        (void)vkDeviceWaitIdle(device);
        for (auto& resources : available_resources) destroy_resources(resources);
        available_resources.clear();
        if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
        if (sampler) vkDestroySampler(device, sampler, nullptr);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (shader_module) vkDestroyShaderModule(device, shader_module, nullptr);
        if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        if (descriptor_layout) vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
        if (source_probe_dummy) vkDestroyBuffer(device, source_probe_dummy, nullptr);
        if (source_probe_dummy_memory) vkFreeMemory(device, source_probe_dummy_memory, nullptr);
    }

    void destroy_resources(DirectFrameResources& resources) noexcept {
        if (resources.descriptors && descriptor_pool)
            (void)vkFreeDescriptorSets(device, descriptor_pool, 1, &resources.descriptors);
        if (resources.source_view) vkDestroyImageView(device, resources.source_view, nullptr);
        if (resources.y_view) vkDestroyImageView(device, resources.y_view, nullptr);
        if (resources.uv_view) vkDestroyImageView(device, resources.uv_view, nullptr);
        if (resources.command && command_pool)
            vkFreeCommandBuffers(device, command_pool, 1, &resources.command);
        if (resources.source_sync_probe_command && command_pool)
            vkFreeCommandBuffers(device, command_pool, 1,
                                 &resources.source_sync_probe_command);
        if (resources.temp_y) vkDestroyImage(device, resources.temp_y, nullptr);
        if (resources.temp_uv) vkDestroyImage(device, resources.temp_uv, nullptr);
        if (resources.temp_y_memory) vkFreeMemory(device, resources.temp_y_memory, nullptr);
        if (resources.temp_uv_memory) vkFreeMemory(device, resources.temp_uv_memory, nullptr);
        if (resources.nv12_readback) vkDestroyBuffer(device, resources.nv12_readback, nullptr);
        if (resources.nv12_readback_memory)
            vkFreeMemory(device, resources.nv12_readback_memory, nullptr);
        if (resources.source_probe) vkDestroyBuffer(device, resources.source_probe, nullptr);
        if (resources.source_probe_memory)
            vkFreeMemory(device, resources.source_probe_memory, nullptr);
        resources = {};
    }

    std::expected<void, GpuError> create_probe_buffer(
        VkDeviceSize size, VkBuffer& buffer, VkDeviceMemory& memory,
        bool require_host_visible, bool& host_coherent) {
        VkBufferCreateInfo buffer_create{};
        buffer_create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_create.size = size;
        buffer_create.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        buffer_create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = vkCreateBuffer(device, &buffer_create, nullptr, &buffer);
        if (result != VK_SUCCESS)
            return std::unexpected(vk_error("vkCreateBuffer for direct source probe", result));

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
        std::uint32_t memory_type = UINT32_MAX;
        int best_score = -1;
        for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1U << i)) == 0) continue;
            const VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
            if (require_host_visible && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
                continue;
            const int score = ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 4 : 0) +
                              ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 2 : 0) +
                              ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 1 : 0);
            if (score > best_score) {
                memory_type = i;
                best_score = score;
            }
        }
        if (memory_type == UINT32_MAX) {
            vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return std::unexpected(GpuError{require_host_visible
                ? "no host-visible Vulkan memory for direct source probe"
                : "no Vulkan memory type for direct source probe dummy", 0});
        }

        const VkMemoryPropertyFlags memory_flags = properties.memoryTypes[memory_type].propertyFlags;
        host_coherent = (memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        result = vkAllocateMemory(device, &allocation, nullptr, &memory);
        if (result != VK_SUCCESS) {
            vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return std::unexpected(vk_error("vkAllocateMemory for direct source probe", result));
        }
        result = vkBindBufferMemory(device, buffer, memory, 0);
        if (result != VK_SUCCESS) {
            vkFreeMemory(device, memory, nullptr);
            vkDestroyBuffer(device, buffer, nullptr);
            memory = VK_NULL_HANDLE;
            buffer = VK_NULL_HANDLE;
            return std::unexpected(vk_error("vkBindBufferMemory for direct source probe", result));
        }
        return {};
    }

    std::expected<void, GpuError> create_source_probe(DirectFrameResources& resources) {
        const std::uint64_t groups_x = (static_cast<std::uint64_t>(width) + 31U) / 32U;
        const std::uint64_t groups_y = (static_cast<std::uint64_t>(height) + 31U) / 32U;
        const VkDeviceSize record_count = static_cast<VkDeviceSize>(groups_x * groups_y);
        resources.source_probe_size = record_count * 2U * sizeof(std::uint32_t);
        auto result = create_probe_buffer(resources.source_probe_size,
                                          resources.source_probe,
                                          resources.source_probe_memory,
                                          true,
                                          resources.source_probe_host_coherent);
        if (!result) resources.source_probe_size = 0;
        return result;
    }

    std::expected<void, GpuError> create_nv12_readback(DirectFrameResources& resources) {
        const VkDeviceSize y_bytes = static_cast<VkDeviceSize>(width) * height;
        const VkDeviceSize uv_bytes = y_bytes / 2U;
        VkBufferCreateInfo buffer_create{};
        buffer_create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_create.size = 2U * (y_bytes + uv_bytes);
        buffer_create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult result = vkCreateBuffer(device, &buffer_create, nullptr,
                                         &resources.nv12_readback);
        if (result != VK_SUCCESS)
            return std::unexpected(vk_error("vkCreateBuffer for NV12 diagnostic readback", result));

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, resources.nv12_readback, &requirements);
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
        std::uint32_t memory_type = UINT32_MAX;
        int best_score = -1;
        for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1U << i)) == 0) continue;
            const VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
            if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) continue;
            const int score = ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 2 : 0) +
                              ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 1 : 0);
            if (score > best_score) {
                memory_type = i;
                best_score = score;
            }
        }
        if (memory_type == UINT32_MAX) {
            vkDestroyBuffer(device, resources.nv12_readback, nullptr);
            resources.nv12_readback = VK_NULL_HANDLE;
            return std::unexpected(GpuError{"no host-visible Vulkan memory for NV12 diagnostic readback", 0});
        }

        const VkMemoryPropertyFlags memory_flags = properties.memoryTypes[memory_type].propertyFlags;
        resources.nv12_readback_host_coherent =
            (memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        result = vkAllocateMemory(device, &allocation, nullptr,
                                  &resources.nv12_readback_memory);
        if (result != VK_SUCCESS) {
            vkDestroyBuffer(device, resources.nv12_readback, nullptr);
            resources.nv12_readback = VK_NULL_HANDLE;
            return std::unexpected(vk_error("vkAllocateMemory for NV12 diagnostic readback", result));
        }
        result = vkBindBufferMemory(device, resources.nv12_readback,
                                    resources.nv12_readback_memory, 0);
        if (result != VK_SUCCESS) {
            vkFreeMemory(device, resources.nv12_readback_memory, nullptr);
            vkDestroyBuffer(device, resources.nv12_readback, nullptr);
            resources.nv12_readback_memory = VK_NULL_HANDLE;
            resources.nv12_readback = VK_NULL_HANDLE;
            return std::unexpected(vk_error("vkBindBufferMemory for NV12 diagnostic readback", result));
        }
        return {};
    }

    void consume_nv12_readback(DirectFrameResources& resources, std::int64_t pts) noexcept {
        if (!resources.nv12_readback_pending || !resources.nv12_readback_memory) return;
        const VkDeviceSize y_bytes = static_cast<VkDeviceSize>(width) * height;
        const VkDeviceSize one_nv12_size = y_bytes + y_bytes / 2U;
        const VkDeviceSize byte_count = one_nv12_size * 2U;
        void* mapped = nullptr;
        VkResult result = vkMapMemory(device, resources.nv12_readback_memory, 0,
                                      VK_WHOLE_SIZE, 0, &mapped);
        if (result == VK_SUCCESS && mapped) {
            if (!resources.nv12_readback_host_coherent) {
                VkMappedMemoryRange range{};
                range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
                range.memory = resources.nv12_readback_memory;
                range.offset = 0;
                range.size = VK_WHOLE_SIZE;
                result = vkInvalidateMappedMemoryRanges(device, 1, &range);
            }
            if (result == VK_SUCCESS) {
                const auto* bytes = static_cast<const std::uint8_t*>(mapped);
                const auto digest = [bytes](VkDeviceSize offset, VkDeviceSize size) {
                    std::uint64_t hash = 1469598103934665603ULL;
                    std::uint8_t minimum = 255;
                    std::uint8_t maximum = 0;
                    for (VkDeviceSize i = 0; i < size; ++i) {
                        const std::uint8_t value = bytes[offset + i];
                        hash ^= value;
                        hash *= 1099511628211ULL;
                        minimum = std::min(minimum, value);
                        maximum = std::max(maximum, value);
                    }
                    return std::array<std::uint64_t, 3>{hash, minimum, maximum};
                };
                const auto temp_y = digest(0, y_bytes);
                const auto temp_uv = digest(y_bytes, y_bytes / 2U);
                const auto output_y = digest(one_nv12_size, y_bytes);
                const auto output_uv = digest(one_nv12_size + y_bytes, y_bytes / 2U);
                std::fprintf(stderr,
                             "gpu NV12 readback: pts=%lld temp{Y=%016llx[%u,%u] UV=%016llx[%u,%u]} output{Y=%016llx[%u,%u] UV=%016llx[%u,%u]} bytes=%llu\n",
                             static_cast<long long>(pts),
                             static_cast<unsigned long long>(temp_y[0]),
                             static_cast<unsigned int>(temp_y[1]),
                             static_cast<unsigned int>(temp_y[2]),
                             static_cast<unsigned long long>(temp_uv[0]),
                             static_cast<unsigned int>(temp_uv[1]),
                             static_cast<unsigned int>(temp_uv[2]),
                             static_cast<unsigned long long>(output_y[0]),
                             static_cast<unsigned int>(output_y[1]),
                             static_cast<unsigned int>(output_y[2]),
                             static_cast<unsigned long long>(output_uv[0]),
                             static_cast<unsigned int>(output_uv[1]),
                             static_cast<unsigned int>(output_uv[2]),
                             static_cast<unsigned long long>(byte_count));
            } else {
                std::fprintf(stderr, "gpu NV12 readback: invalidate failed with VkResult %d\n",
                             static_cast<int>(result));
            }
            vkUnmapMemory(device, resources.nv12_readback_memory);
        } else {
            std::fprintf(stderr, "gpu NV12 readback: vkMapMemory failed with VkResult %d\n",
                         static_cast<int>(result));
        }
        if (resources.nv12_readback) vkDestroyBuffer(device, resources.nv12_readback, nullptr);
        if (resources.nv12_readback_memory)
            vkFreeMemory(device, resources.nv12_readback_memory, nullptr);
        resources.nv12_readback = VK_NULL_HANDLE;
        resources.nv12_readback_memory = VK_NULL_HANDLE;
        resources.nv12_readback_pending = false;
        resources.nv12_readback_host_coherent = false;
    }

    void consume_source_probe(DirectFrameResources& resources, std::int64_t pts) noexcept {
        if (!resources.source_probe_pending || !resources.source_probe_memory) return;
        void* mapped = nullptr;
        VkResult result = vkMapMemory(device, resources.source_probe_memory, 0,
                                      VK_WHOLE_SIZE, 0, &mapped);
        if (result != VK_SUCCESS || !mapped) {
            std::fprintf(stderr,
                         "{\"type\":\"direct_source_probe_error\",\"capture_sequence\":%llu,\"pts\":%lld,\"vk_result\":%d}\n",
                         static_cast<unsigned long long>(resources.source_probe_sequence),
                         static_cast<long long>(pts), static_cast<int>(result));
            resources.source_probe_pending = false;
            return;
        }
        if (!resources.source_probe_host_coherent) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = resources.source_probe_memory;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            result = vkInvalidateMappedMemoryRanges(device, 1, &range);
        }
        if (result != VK_SUCCESS) {
            std::fprintf(stderr,
                         "{\"type\":\"direct_source_probe_error\",\"capture_sequence\":%llu,\"pts\":%lld,\"vk_result\":%d}\n",
                         static_cast<unsigned long long>(resources.source_probe_sequence),
                         static_cast<long long>(pts), static_cast<int>(result));
            vkUnmapMemory(device, resources.source_probe_memory);
            resources.source_probe_pending = false;
            return;
        }

        const auto* records = static_cast<const std::uint32_t*>(mapped);
        const std::uint64_t groups_x = (static_cast<std::uint64_t>(width) + 31U) / 32U;
        const std::uint64_t groups_y = (static_cast<std::uint64_t>(height) + 31U) / 32U;
        const std::size_t record_count = static_cast<std::size_t>(groups_x * groups_y);
        const std::uint32_t expected_tag = static_cast<std::uint32_t>(
            resources.source_probe_sequence & 0xffff'ffffULL);
        std::uint64_t hash = 1469598103934665603ULL;
        std::uint32_t written = 0;
        std::uint32_t non_black = 0;
        std::uint32_t tag_mismatches = 0;
        std::array<std::uint8_t, 3> minimum{255, 255, 255};
        std::array<std::uint8_t, 3> maximum{0, 0, 0};
        for (std::size_t i = 0; i < record_count; ++i) {
            const std::uint32_t rgb = records[i * 2U];
            const std::uint32_t tag = records[i * 2U + 1U];
            if (tag != expected_tag) {
                ++tag_mismatches;
                continue;
            }
            ++written;
            if ((rgb & 0x00ff'ffffU) != 0) ++non_black;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto value = static_cast<std::uint8_t>((rgb >> (8U * channel)) & 0xffU);
                minimum[channel] = std::min(minimum[channel], value);
                maximum[channel] = std::max(maximum[channel], value);
                hash ^= value;
                hash *= 1099511628211ULL;
            }
        }

        bool same_slot = false;
        std::uint64_t previous_sequence = 0;
        if (written != 0) {
            const auto previous = previous_probe_by_slot.find(resources.source_probe_slot);
            if (previous != previous_probe_by_slot.end()) {
                previous_sequence = previous->second.capture_sequence;
                same_slot = previous->second.hash == hash;
            }
            previous_probe_by_slot[resources.source_probe_slot] =
                ProbeHistory{resources.source_probe_sequence, hash};
        }
        std::fprintf(stderr,
                     "{\"type\":\"direct_source_probe\",\"capture_sequence\":%llu,\"dispatch\":%llu,\"pts\":%lld,\"slot\":%zu,\"dmabuf_fd\":%d,\"fourcc\":\"%08x\",\"modifier\":\"%016llx\",\"offset\":%u,\"stride\":%u,\"source_vk_image\":\"%p\",\"sample_count\":%zu,\"workgroups_written\":%u,\"tag_mismatches\":%u,\"non_black_samples\":%u,\"rgb_hash\":\"%016llx\",\"rgb_min\":[%u,%u,%u],\"rgb_max\":[%u,%u,%u],\"same_as_previous_sampled_frame_for_slot\":%s,\"previous_slot_capture_sequence\":%llu}\n",
                     static_cast<unsigned long long>(resources.source_probe_sequence),
                     static_cast<unsigned long long>(resources.source_probe_dispatch),
                     static_cast<long long>(pts), resources.source_probe_slot,
                     resources.source_probe_dma_buf_fd, resources.source_probe_fourcc,
                     static_cast<unsigned long long>(resources.source_probe_modifier),
                     resources.source_probe_offset, resources.source_probe_stride,
                     reinterpret_cast<void*>(resources.source_probe_vk_image), record_count,
                     written, tag_mismatches, non_black,
                     static_cast<unsigned long long>(hash),
                     static_cast<unsigned int>(minimum[0]),
                     static_cast<unsigned int>(minimum[1]),
                     static_cast<unsigned int>(minimum[2]),
                     static_cast<unsigned int>(maximum[0]),
                     static_cast<unsigned int>(maximum[1]),
                     static_cast<unsigned int>(maximum[2]),
                     same_slot ? "true" : "false",
                     static_cast<unsigned long long>(previous_sequence));
        vkUnmapMemory(device, resources.source_probe_memory);
        resources.source_probe_pending = false;
    }

    std::expected<void, GpuError> initialize(AVBufferRef* input_ref,
                                             AVBufferRef* output_ref,
                                             std::uint32_t frame_width,
                                             std::uint32_t frame_height,
                                             std::uint32_t max_pending) {
        if (!input_ref || !output_ref) {
            return std::unexpected(GpuError{"Vulkan input/output frame contexts are required", 0});
        }
        input_frames = reinterpret_cast<AVHWFramesContext*>(input_ref->data);
        output_frames = reinterpret_cast<AVHWFramesContext*>(output_ref->data);
        input_vk_frames = static_cast<AVVulkanFramesContext*>(input_frames->hwctx);
        output_vk_frames = static_cast<AVVulkanFramesContext*>(output_frames->hwctx);
        device_context = input_frames->device_ctx;
        vk_context = static_cast<AVVulkanDeviceContext*>(device_context->hwctx);
        trace_sync = std::getenv("FLUXREC_GPU_TRACE_SYNC") != nullptr;
        disable_resource_reuse = std::getenv("FLUXREC_GPU_DISABLE_RESOURCE_REUSE") != nullptr;
        const char* source_probe_env = std::getenv("FLUXREC_GPU_DIRECT_SOURCE_PROBE");
        debug_source_probe = source_probe_env && std::strcmp(source_probe_env, "1") == 0;
        const char* ownership_env = std::getenv("FLUXREC_GPU_DIRECT_ACQUIRE_FAMILY");
        if (ownership_env && std::strcmp(ownership_env, "foreign") == 0) {
            source_external_ownership = SourceExternalOwnership::foreign;
            source_external_queue_family = VK_QUEUE_FAMILY_FOREIGN_EXT;
        } else if (ownership_env && std::strcmp(ownership_env, "external") == 0) {
            source_external_ownership = SourceExternalOwnership::external;
            source_external_queue_family = VK_QUEUE_FAMILY_EXTERNAL;
        }
        const char* readback_env = std::getenv("FLUXREC_GPU_READBACK_NV12");
        debug_readback_nv12 = readback_env && std::strcmp(readback_env, "1") == 0;
        wait_conversion_before_encode =
            std::getenv("FLUXREC_GPU_WAIT_CONVERSION_BEFORE_ENCODE") != nullptr;
        const char* source_sync_probe_env = std::getenv("FLUXREC_GPU_SOURCE_SYNC_PROBE");
        if (source_sync_probe_env && std::strcmp(source_sync_probe_env, "wait-only") == 0)
            source_sync_probe = SourceSyncProbe::wait_only;
        else if (source_sync_probe_env && std::strcmp(source_sync_probe_env, "barrier-only") == 0)
            source_sync_probe = SourceSyncProbe::barrier_only;
        if (debug_readback_nv12) {
            std::fprintf(stderr,
                         "WARNING: FLUXREC_GPU_READBACK_NV12=1 enables diagnostic CPU raw-pixel readback; it is disabled by default.\n");
        }
        if (debug_source_probe) {
            std::fprintf(stderr,
                         "WARNING: FLUXREC_GPU_DIRECT_SOURCE_PROBE=1 samples one RGB texel per 32x32 source tile on selected direct-conversion frames; it reads only the tagged sample buffer after completion.\n");
        }
        if (source_external_ownership != SourceExternalOwnership::none) {
            const char* family_name = source_external_ownership == SourceExternalOwnership::foreign
                ? "VK_QUEUE_FAMILY_FOREIGN_EXT" : "VK_QUEUE_FAMILY_EXTERNAL";
            std::fprintf(stderr,
                         "WARNING: FLUXREC_GPU_DIRECT_ACQUIRE_FAMILY enables experimental DMA-BUF external ownership acquire/release via %s.\n",
                         family_name);
        }
        if (ownership_env && ownership_env[0] != '\0' &&
            source_external_ownership == SourceExternalOwnership::none) {
            return std::unexpected(GpuError{
                "FLUXREC_GPU_DIRECT_ACQUIRE_FAMILY must be 'foreign' or 'external'", 0});
        }
        if (source_sync_probe != SourceSyncProbe::none) {
            std::fprintf(stderr,
                         "WARNING: FLUXREC_GPU_SOURCE_SYNC_PROBE enables an experimental diagnostic source wait/barrier submit.\n");
        }
        if (output_frames->device_ctx != device_context ||
            output_frames->format != AV_PIX_FMT_VULKAN || output_frames->sw_format != AV_PIX_FMT_NV12 ||
            input_frames->format != AV_PIX_FMT_VULKAN) {
            return std::unexpected(GpuError{"direct converter requires same-device Vulkan frames and NV12 output", 0});
        }
        if (output_vk_frames->format[1] != VK_FORMAT_UNDEFINED ||
            output_vk_frames->format[0] != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ||
            !(output_vk_frames->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
            !(output_vk_frames->usage & VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR) ||
            (debug_readback_nv12 &&
             !(output_vk_frames->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))) {
            return std::unexpected(GpuError{"Vulkan NV12 encoder frames must be one multiplane G8_B8R8_2PLANE_420 image with transfer-destination and video-encode usage", 0});
        }
        if (input_vk_frames->format[1] != VK_FORMAT_UNDEFINED) {
            return std::unexpected(GpuError{"direct conversion currently supports single-image BGRA DMA-BUF sources", 0});
        }

        device = vk_context->act_dev;
        physical_device = vk_context->phys_dev;
        if (source_external_ownership == SourceExternalOwnership::foreign) {
            bool foreign_family_extension_enabled = false;
            for (int i = 0; i < vk_context->nb_enabled_dev_extensions; ++i) {
                if (vk_context->enabled_dev_extensions[i] &&
                    std::strcmp(vk_context->enabled_dev_extensions[i],
                                VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) == 0) {
                    foreign_family_extension_enabled = true;
                    break;
                }
            }
            if (!foreign_family_extension_enabled) {
                return std::unexpected(GpuError{
                    "VK_EXT_queue_family_foreign is not enabled on the FFmpeg Vulkan device", 0});
            }
        }
        width = frame_width;
        height = frame_height;
        if (trace_sync) {
            std::fprintf(stderr, "gpu sync trace: Vulkan queue families=%d\n", vk_context->nb_qf);
            for (int i = 0; i < vk_context->nb_qf; ++i) {
                std::fprintf(stderr, "gpu sync trace: qf[%d] idx=%d num=%d flags=0x%x video=0x%x\n",
                             i, vk_context->qf[i].idx, vk_context->qf[i].num,
                             static_cast<unsigned int>(vk_context->qf[i].flags),
                             static_cast<unsigned int>(vk_context->qf[i].video_caps));
            }
        }
        if (width == 0 || height == 0 || (width & 1U) || (height & 1U)) {
            return std::unexpected(GpuError{"direct NV12 conversion needs even dimensions", 0});
        }

        bool dummy_host_coherent = false;
        auto dummy_probe = create_probe_buffer(2U * sizeof(std::uint32_t),
                                               source_probe_dummy,
                                               source_probe_dummy_memory,
                                               false,
                                               dummy_host_coherent);
        if (!dummy_probe) return dummy_probe;

        bool found_compute_queue = false;
        for (int i = 0; i < vk_context->nb_qf; ++i) {
            if ((vk_context->qf[i].flags & VK_QUEUE_COMPUTE_BIT) != 0 &&
                vk_context->qf[i].num > 0) {
                compute_queue_family = static_cast<std::uint32_t>(vk_context->qf[i].idx);
                vkGetDeviceQueue(device, compute_queue_family, 0, &compute_queue);
                found_compute_queue = compute_queue != VK_NULL_HANDLE;
                if (found_compute_queue) break;
            }
        }
        if (!found_compute_queue) {
            return std::unexpected(GpuError{"Vulkan device has no FFmpeg-enabled compute queue", 0});
        }

        if (!format_support(input_vk_frames->format[0], VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
            !format_support(VK_FORMAT_R8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) ||
            !format_support(VK_FORMAT_R8G8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) ||
            !format_support(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
                            VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
            return std::unexpected(GpuError{"selected Vulkan image formats lack sampled/storage image support", 0});
        }
        if (debug_readback_nv12 &&
            !format_support(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
                            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
            return std::unexpected(GpuError{"NV12 diagnostic readback needs transfer-source support", 0});
        }
        const std::uint32_t set_count = std::max<std::uint32_t>(max_pending + 32U, 128U);
        auto setup = setup_pipeline(set_count);
        if (!setup) return setup;
        return {};
    }

    std::expected<void, GpuError> submit_source_sync_probe(
        DirectFrameResources& resources, AVVkFrame* source_frame) {
        if (source_sync_probe == SourceSyncProbe::none) return {};
        if (!source_frame || !source_frame->img[0] || !source_frame->sem[0]) {
            return std::unexpected(GpuError{"source sync probe needs a Vulkan image and timeline semaphore", 0});
        }
        if (source_frame->sem_value[0] == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(GpuError{"source sync probe timeline value overflow", 0});
        }

        const bool record_barrier = source_sync_probe == SourceSyncProbe::barrier_only;
        if (record_barrier) {
            VkCommandBufferAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool = command_pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            VkResult result = VK_SUCCESS;
            if (!resources.source_sync_probe_command) {
                result = vkAllocateCommandBuffers(device, &allocate,
                                                  &resources.source_sync_probe_command);
                if (result != VK_SUCCESS)
                    return std::unexpected(vk_error("allocating source sync probe command buffer", result));
            } else {
                result = vkResetCommandBuffer(resources.source_sync_probe_command, 0);
                if (result != VK_SUCCESS)
                    return std::unexpected(vk_error("resetting source sync probe command buffer", result));
            }
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result = vkBeginCommandBuffer(resources.source_sync_probe_command, &begin);
            if (result != VK_SUCCESS)
                return std::unexpected(vk_error("beginning source sync probe command buffer", result));
            const VkImageMemoryBarrier2 barrier = image_barrier(
                source_frame->img[0], source_frame->layout[0], source_frame->access[0],
                source_frame->queue_family[0], compute_queue_family,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(resources.source_sync_probe_command, &dependency);
            result = vkEndCommandBuffer(resources.source_sync_probe_command);
            if (result != VK_SUCCESS)
                return std::unexpected(vk_error("ending source sync probe command buffer", result));
        }

        VkSemaphoreSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = source_frame->sem[0];
        wait.value = source_frame->sem_value[0];
        wait.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkSemaphoreSubmitInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = source_frame->sem[0];
        signal.value = source_frame->sem_value[0] + 1U;
        signal.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        VkCommandBufferSubmitInfo command{};
        command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command.commandBuffer = resources.source_sync_probe_command;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = record_barrier ? 1U : 0U;
        submit.pCommandBufferInfos = record_barrier ? &command : nullptr;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;

        VkResult result = VK_SUCCESS;
#if FF_API_VULKAN_SYNC_QUEUES
        if (vk_context->lock_queue)
            vk_context->lock_queue(device_context, compute_queue_family, 0);
#endif
        result = vkQueueSubmit2(compute_queue, 1, &submit, VK_NULL_HANDLE);
#if FF_API_VULKAN_SYNC_QUEUES
        if (vk_context->unlock_queue)
            vk_context->unlock_queue(device_context, compute_queue_family, 0);
#endif
        if (result != VK_SUCCESS)
            return std::unexpected(vk_error("submitting source sync probe", result));

        source_frame->sem_value[0] = signal.value;
        if (record_barrier) {
            source_frame->layout[0] = VK_IMAGE_LAYOUT_GENERAL;
            source_frame->access[0] = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            if (source_frame->queue_family[0] != VK_QUEUE_FAMILY_IGNORED)
                source_frame->queue_family[0] = compute_queue_family;
        }
        return {};
    }

    bool format_support(VkFormat format, VkFormatFeatureFlags required) const {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
        return (properties.optimalTilingFeatures & required) == required;
    }

    std::expected<void, GpuError> create_temp_image(
        std::uint32_t image_width, std::uint32_t image_height, VkFormat format,
        VkImage& image, VkDeviceMemory& memory) {
        VkImageCreateInfo image_create{};
        image_create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_create.imageType = VK_IMAGE_TYPE_2D;
        image_create.format = format;
        image_create.extent = VkExtent3D{image_width, image_height, 1};
        image_create.mipLevels = 1;
        image_create.arrayLayers = 1;
        image_create.samples = VK_SAMPLE_COUNT_1_BIT;
        image_create.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_create.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult result = vkCreateImage(device, &image_create, nullptr, &image);
        if (result != VK_SUCCESS) return std::unexpected(vk_error("vkCreateImage for NV12 plane temporary", result));

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);
        VkPhysicalDeviceMemoryProperties memory_properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
        std::uint32_t memory_type = UINT32_MAX;
        std::uint32_t fallback_memory_type = UINT32_MAX;
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1U << i)) == 0) continue;
            if (fallback_memory_type == UINT32_MAX) fallback_memory_type = i;
            if ((memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                memory_type = i;
                break;
            }
        }
        if (memory_type == UINT32_MAX) memory_type = fallback_memory_type;
        if (memory_type == UINT32_MAX) {
            vkDestroyImage(device, image, nullptr);
            image = VK_NULL_HANDLE;
            return std::unexpected(GpuError{"no Vulkan memory type can back NV12 plane temporary", 0});
        }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type;
        result = vkAllocateMemory(device, &allocation, nullptr, &memory);
        if (result != VK_SUCCESS) {
            vkDestroyImage(device, image, nullptr);
            image = VK_NULL_HANDLE;
            return std::unexpected(vk_error("vkAllocateMemory for NV12 plane temporary", result));
        }
        result = vkBindImageMemory(device, image, memory, 0);
        if (result != VK_SUCCESS) {
            vkFreeMemory(device, memory, nullptr);
            vkDestroyImage(device, image, nullptr);
            memory = VK_NULL_HANDLE;
            image = VK_NULL_HANDLE;
            return std::unexpected(vk_error("vkBindImageMemory for NV12 plane temporary", result));
        }
        return {};
    }

    std::expected<void, GpuError> setup_pipeline(std::uint32_t set_count) {
        const std::array<VkDescriptorSetLayoutBinding, 4> bindings{
            VkDescriptorSetLayoutBinding{
                .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
            VkDescriptorSetLayoutBinding{
                .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
        };
        VkDescriptorSetLayoutCreateInfo descriptor_create{};
        descriptor_create.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptor_create.bindingCount = static_cast<std::uint32_t>(bindings.size());
        descriptor_create.pBindings = bindings.data();
        VkResult result = vkCreateDescriptorSetLayout(device, &descriptor_create, nullptr,
                                                       &descriptor_layout);
        if (result != VK_SUCCESS) return std::unexpected(vk_error("vkCreateDescriptorSetLayout", result));

        const std::array<VkDescriptorPoolSize, 3> pool_sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, set_count},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, set_count * 2U},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, set_count},
        };
        VkDescriptorPoolCreateInfo pool_create{};
        pool_create.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_create.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_create.maxSets = set_count;
        pool_create.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
        pool_create.pPoolSizes = pool_sizes.data();
        result = vkCreateDescriptorPool(device, &pool_create, nullptr, &descriptor_pool);
        if (result != VK_SUCCESS) return std::unexpected(vk_error("vkCreateDescriptorPool", result));

        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(std::uint32_t) * 4U;
        VkPipelineLayoutCreateInfo layout_create{};
        layout_create.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_create.setLayoutCount = 1;
        layout_create.pSetLayouts = &descriptor_layout;
        layout_create.pushConstantRangeCount = 1;
        layout_create.pPushConstantRanges = &push_range;
        result = vkCreatePipelineLayout(device, &layout_create, nullptr, &pipeline_layout);
        if (result != VK_SUCCESS) return std::unexpected(vk_error("vkCreatePipelineLayout", result));

#ifndef FLUXREC_GPU_NV12_SHADER_PATH
        return std::unexpected(GpuError{"build did not provide the NV12 SPIR-V shader path", 0});
#else
        std::ifstream shader_file(FLUXREC_GPU_NV12_SHADER_PATH, std::ios::binary | std::ios::ate);
        if (!shader_file) {
            return std::unexpected(GpuError{"opening compiled NV12 SPIR-V shader failed", 0});
        }
        const std::streamsize bytes = shader_file.tellg();
        if (bytes <= 0 || (bytes % static_cast<std::streamsize>(sizeof(std::uint32_t))) != 0) {
            return std::unexpected(GpuError{"compiled NV12 SPIR-V shader has invalid size", 0});
        }
        shader_file.seekg(0, std::ios::beg);
        std::vector<std::uint32_t> code(static_cast<std::size_t>(bytes) / sizeof(std::uint32_t));
        if (!shader_file.read(reinterpret_cast<char*>(code.data()), bytes)) {
            return std::unexpected(GpuError{"reading compiled NV12 SPIR-V shader failed", 0});
        }
        VkShaderModuleCreateInfo shader_create{};
        shader_create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_create.codeSize = static_cast<std::size_t>(bytes);
        shader_create.pCode = code.data();
        result = vkCreateShaderModule(device, &shader_create, nullptr, &shader_module);
        if (result != VK_SUCCESS) return std::unexpected(vk_error("vkCreateShaderModule", result));
#endif

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader_module;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipeline_create{};
        pipeline_create.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_create.stage = stage;
        pipeline_create.layout = pipeline_layout;
        result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_create,
                                          nullptr, &pipeline);
        if (result != VK_SUCCESS) return std::unexpected(vk_error("vkCreateComputePipelines", result));

        VkSamplerCreateInfo sampler_create{};
        sampler_create.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_create.magFilter = VK_FILTER_NEAREST;
        sampler_create.minFilter = VK_FILTER_NEAREST;
        sampler_create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_create.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_create.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_create.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_create.maxAnisotropy = 1.0F;
        sampler_create.minLod = 0.0F;
        sampler_create.maxLod = 0.0F;
        result = vkCreateSampler(device, &sampler_create, nullptr, &sampler);
        if (result != VK_SUCCESS) return std::unexpected(vk_error("vkCreateSampler", result));

        VkCommandPoolCreateInfo command_pool_create{};
        command_pool_create.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        command_pool_create.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        command_pool_create.queueFamilyIndex = compute_queue_family;
        result = vkCreateCommandPool(device, &command_pool_create, nullptr, &command_pool);
        if (result != VK_SUCCESS) return std::unexpected(vk_error("vkCreateCommandPool", result));
        return {};
    }

    std::expected<VkImageView, GpuError> make_view(VkImage image, VkFormat format,
                                                   VkComponentMapping components) {
        VkImageViewCreateInfo view_create{};
        view_create.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_create.image = image;
        view_create.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_create.format = format;
        view_create.components = components;
        view_create.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_create.subresourceRange.levelCount = 1;
        view_create.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        const VkResult result = vkCreateImageView(device, &view_create, nullptr, &view);
        if (result != VK_SUCCESS) return std::unexpected(vk_error("vkCreateImageView", result));
        return view;
    }

    std::expected<DirectFrameResources, GpuError> dispatch(
        AVFrame* input, AVFrame* output,
        const capture::CapturedFrame& captured) {
        AVVkFrame* input_vk = vk_frame(input);
        AVVkFrame* output_vk = vk_frame(output);
        AVHWFramesContext* input_fc = frames_context(input);
        AVHWFramesContext* output_fc = frames_context(output);
        if (!input_vk || !output_vk || !input_fc || !output_fc ||
            !input_vk->img[0] || !output_vk->img[0] || output_vk->img[1]) {
            return std::unexpected(GpuError{"direct compute needs one Vulkan source and a single multiplane NV12 encode image", 0});
        }

        DirectFrameResources resources;
        if (!disable_resource_reuse && !available_resources.empty()) {
            resources = available_resources.back();
            available_resources.pop_back();
        }
        const VkComponentMapping source_mapping =
            input_vk_frames->format[0] == VK_FORMAT_R8G8B8A8_UNORM
                ? VkComponentMapping{VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G,
                                     VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A}
                : VkComponentMapping{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        auto view = make_view(input_vk->img[0], input_vk_frames->format[0], source_mapping);
        if (!view) return std::unexpected(view.error());
        resources.source_view = *view;
        std::expected<void, GpuError> temporary;
        if (!resources.temp_y) {
            temporary = create_temp_image(width, height, VK_FORMAT_R8_UNORM,
                                          resources.temp_y, resources.temp_y_memory);
            if (!temporary) {
                release(resources, input, output);
                return std::unexpected(temporary.error());
            }
        }
        if (!resources.temp_uv) {
            temporary = create_temp_image(width / 2U, height / 2U, VK_FORMAT_R8G8_UNORM,
                                          resources.temp_uv, resources.temp_uv_memory);
            if (!temporary) {
                release(resources, input, output);
                return std::unexpected(temporary.error());
            }
        }
        if (!resources.y_view) {
            view = make_view(resources.temp_y, VK_FORMAT_R8_UNORM,
                             VkComponentMapping{VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY});
            if (!view) {
                release(resources, input, output);
                return std::unexpected(view.error());
            }
            resources.y_view = *view;
        }
        if (!resources.uv_view) {
            view = make_view(resources.temp_uv, VK_FORMAT_R8G8_UNORM,
                             VkComponentMapping{VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY});
            if (!view) {
                release(resources, input, output);
                return std::unexpected(view.error());
            }
            resources.uv_view = *view;
        }
        if (debug_source_probe && !resources.source_probe) {
            auto probe = create_source_probe(resources);
            if (!probe) {
                release(resources, input, output);
                return std::unexpected(probe.error());
            }
        }

        VkDescriptorSetAllocateInfo descriptor_alloc{};
        descriptor_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descriptor_alloc.descriptorPool = descriptor_pool;
        descriptor_alloc.descriptorSetCount = 1;
        descriptor_alloc.pSetLayouts = &descriptor_layout;
        VkResult result = VK_SUCCESS;
        if (!resources.descriptors) {
            result = vkAllocateDescriptorSets(device, &descriptor_alloc,
                                               &resources.descriptors);
            if (result != VK_SUCCESS) {
                release(resources, input, output);
                return std::unexpected(vk_error("vkAllocateDescriptorSets", result));
            }
        }

        VkDescriptorImageInfo source_info{sampler, resources.source_view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo y_info{VK_NULL_HANDLE, resources.y_view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo uv_info{VK_NULL_HANDLE, resources.uv_view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo probe_info{
            resources.source_probe ? resources.source_probe : source_probe_dummy,
            0,
            resources.source_probe ? resources.source_probe_size
                                   : 2U * sizeof(std::uint32_t),
        };
        std::array<VkWriteDescriptorSet, 4> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = resources.descriptors;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                : i < 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                        : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        writes[0].pImageInfo = &source_info;
        writes[1].pImageInfo = &y_info;
        writes[2].pImageInfo = &uv_info;
        writes[3].pBufferInfo = &probe_info;
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

        VkCommandBufferAllocateInfo command_alloc{};
        command_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_alloc.commandPool = command_pool;
        command_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_alloc.commandBufferCount = 1;
        if (!resources.command) {
            result = vkAllocateCommandBuffers(device, &command_alloc, &resources.command);
            if (result != VK_SUCCESS) {
                release(resources, input, output);
                return std::unexpected(vk_error("vkAllocateCommandBuffers", result));
            }
        } else {
            result = vkResetCommandBuffer(resources.command, 0);
            if (result != VK_SUCCESS) {
                release(resources, input, output);
                return std::unexpected(vk_error("vkResetCommandBuffer", result));
            }
        }

        auto* input_vulkan_frames = static_cast<AVVulkanFramesContext*>(input_fc->hwctx);
        auto* output_vulkan_frames = static_cast<AVVulkanFramesContext*>(output_fc->hwctx);
        if (input_vulkan_frames->lock_frame) input_vulkan_frames->lock_frame(input_fc, input_vk);
        if (output_vulkan_frames->lock_frame) output_vulkan_frames->lock_frame(output_fc, output_vk);
        const std::uint64_t sequence = ++dispatch_sequence;
        const std::uint64_t sequence_phase = sequence % 9U;
        const bool source_probe_frame = debug_source_probe &&
            (sequence <= 2U || sequence_phase == 8U || sequence_phase == 0U);
        resources.source_probe_pending = source_probe_frame;
        if (source_probe_frame) {
            const auto& capture_image = captured.lease.image();
            resources.source_probe_sequence = captured.sequence;
            resources.source_probe_dispatch = sequence;
            resources.source_probe_slot = captured.lease.slot_index();
            resources.source_probe_dma_buf_fd = capture_image.planes.empty()
                ? -1 : capture_image.planes.front().fd;
            resources.source_probe_fourcc = capture_image.fourcc;
            resources.source_probe_modifier = capture_image.modifier;
            resources.source_probe_stride = capture_image.planes.empty()
                ? 0 : capture_image.planes.front().stride;
            resources.source_probe_offset = capture_image.planes.empty()
                ? 0 : capture_image.planes.front().offset;
            resources.source_probe_vk_image = input_vk->img[0];
        }
        const bool readback_frame = sequence <= 2U || sequence == 8U || sequence == 9U ||
                                    sequence == 10U || sequence == 17U || sequence == 18U ||
                                    sequence == 19U;
        if (debug_readback_nv12 && readback_frame) {
            auto readback = create_nv12_readback(resources);
            if (!readback) {
                if (output_vulkan_frames->unlock_frame)
                    output_vulkan_frames->unlock_frame(output_fc, output_vk);
                if (input_vulkan_frames->unlock_frame)
                    input_vulkan_frames->unlock_frame(input_fc, input_vk);
                release(resources, input, output);
                return std::unexpected(readback.error());
            }
            resources.nv12_readback_pending = true;
        }
        const bool trace_frame = trace_sync && (sequence <= 24 || sequence % 9U == 0);
        if (trace_frame) {
            std::fprintf(stderr,
                         "gpu sync trace: dispatch=%llu pts=%lld src{img=%p sem=%p val=%llu layout=%d access=0x%llx qf=%u} dst{img=%p sem=%p val=%llu layout=%d access=0x%llx qf=%u}\n",
                         static_cast<unsigned long long>(sequence),
                         static_cast<long long>(input->pts),
                         reinterpret_cast<void*>(input_vk->img[0]),
                         reinterpret_cast<void*>(input_vk->sem[0]),
                         static_cast<unsigned long long>(input_vk->sem_value[0]),
                         static_cast<int>(input_vk->layout[0]),
                         static_cast<unsigned long long>(input_vk->access[0]),
                         input_vk->queue_family[0],
                         reinterpret_cast<void*>(output_vk->img[0]),
                         reinterpret_cast<void*>(output_vk->sem[0]),
                         static_cast<unsigned long long>(output_vk->sem_value[0]),
                         static_cast<int>(output_vk->layout[0]),
                         static_cast<unsigned long long>(output_vk->access[0]),
                         output_vk->queue_family[0]);
        }

        auto source_probe = submit_source_sync_probe(resources, input_vk);
        if (!source_probe) {
            if (output_vulkan_frames->unlock_frame)
                output_vulkan_frames->unlock_frame(output_fc, output_vk);
            if (input_vulkan_frames->unlock_frame)
                input_vulkan_frames->unlock_frame(input_fc, input_vk);
            release(resources, input, output);
            return std::unexpected(source_probe.error());
        }
        if (trace_frame && source_sync_probe != SourceSyncProbe::none) {
            std::fprintf(stderr,
                         "gpu sync trace: source-probe=%s pts=%lld input-semaphore-value=%llu\n",
                         source_sync_probe == SourceSyncProbe::barrier_only
                             ? "barrier-only" : "wait-only",
                         static_cast<long long>(input->pts),
                         static_cast<unsigned long long>(input_vk->sem_value[0]));
        }

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(resources.command, &begin);
        if (result != VK_SUCCESS) {
            if (output_vulkan_frames->unlock_frame) output_vulkan_frames->unlock_frame(output_fc, output_vk);
            if (input_vulkan_frames->unlock_frame) input_vulkan_frames->unlock_frame(input_fc, input_vk);
            release(resources, input, output);
            return std::unexpected(vk_error("vkBeginCommandBuffer", result));
        }

        if (source_external_ownership != SourceExternalOwnership::none &&
            input_vk->layout[0] != VK_IMAGE_LAYOUT_GENERAL) {
            if (output_vulkan_frames->unlock_frame)
                output_vulkan_frames->unlock_frame(output_fc, output_vk);
            if (input_vulkan_frames->unlock_frame)
                input_vulkan_frames->unlock_frame(input_fc, input_vk);
            release(resources, input, output);
            return std::unexpected(GpuError{
                "external ownership experiment requires FFmpeg DRM import to leave the source in GENERAL layout", 0});
        }
        const VkImageMemoryBarrier2 input_acquire =
            source_external_ownership != SourceExternalOwnership::none
                ? external_acquire_barrier(input_vk->img[0], input_vk->layout[0],
                                           source_external_queue_family,
                                           compute_queue_family)
                : image_barrier(input_vk->img[0], input_vk->layout[0], input_vk->access[0],
                                input_vk->queue_family[0], compute_queue_family,
                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        std::array<VkImageMemoryBarrier2, 4> barriers{
            input_acquire,
            image_barrier(resources.temp_y, VK_IMAGE_LAYOUT_UNDEFINED, 0,
                          VK_QUEUE_FAMILY_IGNORED, compute_queue_family,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
            image_barrier(resources.temp_uv, VK_IMAGE_LAYOUT_UNDEFINED, 0,
                          VK_QUEUE_FAMILY_IGNORED, compute_queue_family,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
            image_barrier(output_vk->img[0], output_vk->layout[0], output_vk->access[0],
                          output_vk->queue_family[0], compute_queue_family,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_ASPECT_COLOR_BIT,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT),
        };
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(resources.command, &dependency);
        vkCmdBindPipeline(resources.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(resources.command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline_layout, 0, 1, &resources.descriptors, 0, nullptr);
        const std::array<std::uint32_t, 4> convert_params{
            width, height, source_probe_frame ? 1U : 0U,
            static_cast<std::uint32_t>(captured.sequence & 0xffff'ffffULL),
        };
        vkCmdPushConstants(resources.command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, static_cast<std::uint32_t>(sizeof(convert_params)),
                           convert_params.data());
        vkCmdDispatch(resources.command, (width + 31U) / 32U, (height + 31U) / 32U, 1);

        VkBufferMemoryBarrier2 probe_to_host{};
        if (source_probe_frame) {
            probe_to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            probe_to_host.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            probe_to_host.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            probe_to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            probe_to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            probe_to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            probe_to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            probe_to_host.buffer = resources.source_probe;
            probe_to_host.offset = 0;
            probe_to_host.size = resources.source_probe_size;
        }
        std::array<VkImageMemoryBarrier2, 3> temp_to_copy{};
        std::uint32_t temp_barrier_count = 0;
        if (source_external_ownership != SourceExternalOwnership::none) {
            temp_to_copy[temp_barrier_count++] = external_release_barrier(
                input_vk->img[0], compute_queue_family, source_external_queue_family);
        }
        temp_to_copy[temp_barrier_count++] = image_barrier(
            resources.temp_y, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, compute_queue_family,
            compute_queue_family, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        temp_to_copy[temp_barrier_count++] = image_barrier(
            resources.temp_uv, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, compute_queue_family,
            compute_queue_family, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        dependency.imageMemoryBarrierCount = temp_barrier_count;
        dependency.pImageMemoryBarriers = temp_to_copy.data();
        dependency.bufferMemoryBarrierCount = source_probe_frame ? 1U : 0U;
        dependency.pBufferMemoryBarriers = source_probe_frame ? &probe_to_host : nullptr;
        vkCmdPipelineBarrier2(resources.command, &dependency);
        dependency.bufferMemoryBarrierCount = 0;
        dependency.pBufferMemoryBarriers = nullptr;

        const VkDeviceSize nv12_size =
            static_cast<VkDeviceSize>(width) * height * 3U / 2U;
        if (resources.nv12_readback_pending) {
            const VkDeviceSize y_bytes = static_cast<VkDeviceSize>(width) * height;
            VkBufferImageCopy readback_copy{};
            readback_copy.imageSubresource = VkImageSubresourceLayers{
                VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            readback_copy.imageExtent = VkExtent3D{width, height, 1};
            vkCmdCopyImageToBuffer(resources.command, resources.temp_y,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   resources.nv12_readback, 1, &readback_copy);
            readback_copy.bufferOffset = y_bytes;
            readback_copy.imageExtent = VkExtent3D{width / 2U, height / 2U, 1};
            vkCmdCopyImageToBuffer(resources.command, resources.temp_uv,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   resources.nv12_readback, 1, &readback_copy);
        }

        VkImageCopy copy{};
        copy.srcSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
        copy.extent = VkExtent3D{width, height, 1};
        vkCmdCopyImage(resources.command, resources.temp_y, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       output_vk->img[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        copy.srcSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
        copy.extent = VkExtent3D{width / 2U, height / 2U, 1};
        vkCmdCopyImage(resources.command, resources.temp_uv, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       output_vk->img[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        VkImageMemoryBarrier2 output_to_encoder{};
        VkAccessFlags2 output_final_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        if (resources.nv12_readback_pending) {
            const VkImageMemoryBarrier2 output_to_readback = image_barrier(
                output_vk->img[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, compute_queue_family, compute_queue_family,
                VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &output_to_readback;
            vkCmdPipelineBarrier2(resources.command, &dependency);

            const VkDeviceSize y_bytes = static_cast<VkDeviceSize>(width) * height;
            VkBufferImageCopy readback_copy{};
            readback_copy.bufferOffset = nv12_size;
            readback_copy.imageSubresource = VkImageSubresourceLayers{
                VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
            readback_copy.imageExtent = VkExtent3D{width, height, 1};
            vkCmdCopyImageToBuffer(resources.command, output_vk->img[0],
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   resources.nv12_readback, 1, &readback_copy);
            readback_copy.bufferOffset = nv12_size + y_bytes;
            readback_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            readback_copy.imageExtent = VkExtent3D{width / 2U, height / 2U, 1};
            vkCmdCopyImageToBuffer(resources.command, output_vk->img[0],
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   resources.nv12_readback, 1, &readback_copy);

            VkBufferMemoryBarrier2 readback_to_host{};
            readback_to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            readback_to_host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            readback_to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            readback_to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            readback_to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            readback_to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            readback_to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            readback_to_host.buffer = resources.nv12_readback;
            readback_to_host.offset = 0;
            readback_to_host.size = VK_WHOLE_SIZE;
            VkDependencyInfo buffer_dependency{};
            buffer_dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            buffer_dependency.bufferMemoryBarrierCount = 1;
            buffer_dependency.pBufferMemoryBarriers = &readback_to_host;
            vkCmdPipelineBarrier2(resources.command, &buffer_dependency);

            output_final_access = VK_ACCESS_2_TRANSFER_READ_BIT;
            output_to_encoder = image_barrier(
                output_vk->img[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_2_TRANSFER_READ_BIT, compute_queue_family, compute_queue_family,
                output_final_access, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        } else {
            output_to_encoder = image_barrier(
                output_vk->img[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, compute_queue_family, compute_queue_family,
                output_final_access, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        }
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &output_to_encoder;
        vkCmdPipelineBarrier2(resources.command, &dependency);
        result = vkEndCommandBuffer(resources.command);
        if (result != VK_SUCCESS) {
            if (output_vulkan_frames->unlock_frame) output_vulkan_frames->unlock_frame(output_fc, output_vk);
            if (input_vulkan_frames->unlock_frame) input_vulkan_frames->unlock_frame(input_fc, input_vk);
            release(resources, input, output);
            return std::unexpected(vk_error("vkEndCommandBuffer", result));
        }

        struct SemaphoreState {
            VkSemaphore semaphore;
            std::uint64_t value;
            VkPipelineStageFlags2 wait_stage;
        };
        std::array<SemaphoreState, 2> frame_sync{};
        std::uint32_t frame_sync_count = 0;
        const auto add_frame_sync = [&](VkSemaphore semaphore, std::uint64_t value,
                                        VkPipelineStageFlags2 wait_stage) {
            if (!semaphore) return false;
            for (std::uint32_t i = 0; i < frame_sync_count; ++i) {
                if (frame_sync[i].semaphore == semaphore) {
                    frame_sync[i].value = std::max(frame_sync[i].value, value);
                    frame_sync[i].wait_stage |= wait_stage;
                    return true;
                }
            }
            frame_sync[frame_sync_count++] = {semaphore, value, wait_stage};
            return true;
        };
        if (!add_frame_sync(input_vk->sem[0], input_vk->sem_value[0],
                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) ||
            !add_frame_sync(output_vk->sem[0], output_vk->sem_value[0],
                            VK_PIPELINE_STAGE_2_TRANSFER_BIT)) {
            if (output_vulkan_frames->unlock_frame) output_vulkan_frames->unlock_frame(output_fc, output_vk);
            if (input_vulkan_frames->unlock_frame) input_vulkan_frames->unlock_frame(input_fc, input_vk);
            release(resources, input, output);
            return std::unexpected(GpuError{"AVVkFrame has no timeline semaphore for its image", 0});
        }
        std::array<VkSemaphoreSubmitInfo, 2> waits{};
        for (std::uint32_t i = 0; i < frame_sync_count; ++i) {
            waits[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            waits[i].semaphore = frame_sync[i].semaphore;
            waits[i].value = frame_sync[i].value;
            waits[i].stageMask = frame_sync[i].wait_stage;
        }
        std::array<VkSemaphoreSubmitInfo, 2> signals{};
        for (std::uint32_t i = 0; i < frame_sync_count; ++i) {
            signals[i].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            signals[i].semaphore = frame_sync[i].semaphore;
            signals[i].value = frame_sync[i].value + 1U;
            signals[i].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
        VkCommandBufferSubmitInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command_info.commandBuffer = resources.command;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = frame_sync_count;
        submit.pWaitSemaphoreInfos = waits.data();
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command_info;
        submit.signalSemaphoreInfoCount = frame_sync_count;
        submit.pSignalSemaphoreInfos = signals.data();

#if FF_API_VULKAN_SYNC_QUEUES
        if (vk_context->lock_queue) vk_context->lock_queue(device_context, compute_queue_family, 0);
#endif
        result = vkQueueSubmit2(compute_queue, 1, &submit, VK_NULL_HANDLE);
#if FF_API_VULKAN_SYNC_QUEUES
        if (vk_context->unlock_queue) vk_context->unlock_queue(device_context, compute_queue_family, 0);
#endif
        if (result == VK_SUCCESS) {
            resources.submitted = true;
            input_vk->layout[0] = VK_IMAGE_LAYOUT_GENERAL;
            input_vk->access[0] = source_external_ownership != SourceExternalOwnership::none
                ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            if (source_external_ownership != SourceExternalOwnership::none)
                input_vk->queue_family[0] = source_external_queue_family;
            else if (input_vk->queue_family[0] != VK_QUEUE_FAMILY_IGNORED)
                input_vk->queue_family[0] = compute_queue_family;
            output_vk->layout[0] = VK_IMAGE_LAYOUT_GENERAL;
            output_vk->access[0] = resources.nv12_readback_pending
                ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT;
            if (output_vk->queue_family[0] != VK_QUEUE_FAMILY_IGNORED)
                output_vk->queue_family[0] = compute_queue_family;
            const bool shared_semaphore = input_vk->sem[0] == output_vk->sem[0];
            if (shared_semaphore) {
                const std::uint64_t next_value = frame_sync[0].value + 1U;
                input_vk->sem_value[0] = next_value;
                output_vk->sem_value[0] = next_value;
            } else {
                ++input_vk->sem_value[0];
                ++output_vk->sem_value[0];
            }
            if (trace_frame) {
                std::fprintf(stderr,
                             "gpu sync trace: submitted=%llu src{val=%llu layout=%d access=0x%llx qf=%u} dst{val=%llu layout=%d access=0x%llx qf=%u}\n",
                             static_cast<unsigned long long>(sequence),
                             static_cast<unsigned long long>(input_vk->sem_value[0]),
                             static_cast<int>(input_vk->layout[0]),
                             static_cast<unsigned long long>(input_vk->access[0]),
                             input_vk->queue_family[0],
                             static_cast<unsigned long long>(output_vk->sem_value[0]),
                             static_cast<int>(output_vk->layout[0]),
                             static_cast<unsigned long long>(output_vk->access[0]),
                             output_vk->queue_family[0]);
            }
        }
        if (output_vulkan_frames->unlock_frame) output_vulkan_frames->unlock_frame(output_fc, output_vk);
        if (input_vulkan_frames->unlock_frame) input_vulkan_frames->unlock_frame(input_fc, input_vk);
        if (result != VK_SUCCESS) {
            release(resources, input, output);
            return std::unexpected(vk_error("vkQueueSubmit2", result));
        }
        if (wait_conversion_before_encode) {
            VkSemaphore semaphore = output_vk->sem[0];
            const std::uint64_t value = output_vk->sem_value[0];
            VkSemaphoreWaitInfo wait{};
            wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            wait.semaphoreCount = 1;
            wait.pSemaphores = &semaphore;
            wait.pValues = &value;
            result = vkWaitSemaphores(device, &wait,
                                      std::numeric_limits<std::uint64_t>::max());
            if (result != VK_SUCCESS) {
                release(resources, input, output);
                return std::unexpected(vk_error("waiting for direct NV12 conversion before encode", result));
            }
        }
        return resources;
    }

    void release(DirectFrameResources& resources, AVFrame* input, AVFrame* output) noexcept {
        if (device == VK_NULL_HANDLE) return;
        std::array<VkSemaphore, 3> semaphores{};
        std::array<std::uint64_t, 3> values{};
        std::uint32_t count = 0;
        const auto add_frame_semaphores = [&](AVFrame* frame) {
            AVVkFrame* vkf = vk_frame(frame);
            if (!vkf) return;
            AVHWFramesContext* fc = frames_context(frame);
            auto* vkfc = fc ? static_cast<AVVulkanFramesContext*>(fc->hwctx) : nullptr;
            if (vkfc && vkfc->lock_frame) vkfc->lock_frame(fc, vkf);
            if (frame == output && vkf->layout[0] == VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR) {
                // FFmpeg 9.0.2 stores frame access state in VkAccessFlagBits
                // (32-bit) even though ff_vk_frame_barrier supplies a 64-bit
                // VkAccessFlags2. The video-encode-read bit is above bit 31,
                // so the encoder's state update is truncated to zero. Restore
                // the last access after its output packet proves the encode
                // submission completed; the next transfer write then gets a
                // correct source access dependency.
                vkf->access[0] = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR;
            }
            if (trace_sync && frame == output &&
                (output->pts < 24 || output->pts % 9 == 0)) {
                std::fprintf(stderr,
                             "gpu sync trace: packet-release pts=%lld sem=%p val=%llu layout=%d access=0x%llx qf=%u\n",
                             static_cast<long long>(output->pts),
                             reinterpret_cast<void*>(vkf->sem[0]),
                             static_cast<unsigned long long>(vkf->sem_value[0]),
                             static_cast<int>(vkf->layout[0]),
                             static_cast<unsigned long long>(vkf->access[0]),
                             vkf->queue_family[0]);
            }
            for (std::size_t plane = 0; plane < 2; ++plane) {
                if (!vkf->sem[plane]) continue;
                bool duplicate = false;
                for (std::uint32_t i = 0; i < count; ++i) {
                    if (semaphores[i] == vkf->sem[plane]) {
                        values[i] = std::max(values[i], vkf->sem_value[plane]);
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate && count < semaphores.size()) {
                    semaphores[count] = vkf->sem[plane];
                    values[count] = vkf->sem_value[plane];
                    ++count;
                }
            }
            if (vkfc && vkfc->unlock_frame) vkfc->unlock_frame(fc, vkf);
        };
        add_frame_semaphores(input);
        add_frame_semaphores(output);
        if (count != 0) {
            VkSemaphoreWaitInfo wait{};
            wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            wait.semaphoreCount = count;
            wait.pSemaphores = semaphores.data();
            wait.pValues = values.data();
            (void)vkWaitSemaphores(device, &wait, std::numeric_limits<std::uint64_t>::max());
        }
        if (resources.submitted) {
            consume_nv12_readback(resources, output ? output->pts : AV_NOPTS_VALUE);
            consume_source_probe(resources, output ? output->pts : AV_NOPTS_VALUE);
        } else {
            if (resources.nv12_readback)
                vkDestroyBuffer(device, resources.nv12_readback, nullptr);
            if (resources.nv12_readback_memory)
                vkFreeMemory(device, resources.nv12_readback_memory, nullptr);
            resources.nv12_readback = VK_NULL_HANDLE;
            resources.nv12_readback_memory = VK_NULL_HANDLE;
            resources.nv12_readback_pending = false;
            resources.nv12_readback_host_coherent = false;
            resources.source_probe_pending = false;
        }
        resources.submitted = false;
        if (resources.command) (void)vkResetCommandBuffer(resources.command, 0);
        if (resources.descriptors) {
            (void)vkFreeDescriptorSets(device, descriptor_pool, 1, &resources.descriptors);
            resources.descriptors = VK_NULL_HANDLE;
        }
        if (resources.source_view) vkDestroyImageView(device, resources.source_view, nullptr);
        resources.source_view = VK_NULL_HANDLE;
        if (disable_resource_reuse) {
            destroy_resources(resources);
        } else {
            available_resources.push_back(resources);
        }
        resources = {};
    }
};

DirectNv12Converter::DirectNv12Converter(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
DirectNv12Converter::~DirectNv12Converter() = default;
DirectNv12Converter::DirectNv12Converter(DirectNv12Converter&&) noexcept = default;
DirectNv12Converter& DirectNv12Converter::operator=(DirectNv12Converter&&) noexcept = default;

std::expected<DirectNv12Converter, GpuError> DirectNv12Converter::create(
    AVBufferRef* input_frames, AVBufferRef* output_frames,
    std::uint32_t width, std::uint32_t height, std::uint32_t max_pending) {
    auto impl = std::make_unique<Impl>();
    auto initialized = impl->initialize(input_frames, output_frames, width, height, max_pending);
    if (!initialized) return std::unexpected(initialized.error());
    return DirectNv12Converter{std::move(impl)};
}

std::expected<DirectFrameResources, GpuError> DirectNv12Converter::dispatch(
    AVFrame* input, AVFrame* output,
    const capture::CapturedFrame& captured) {
    if (!impl_) return std::unexpected(GpuError{"direct converter is not initialized", 0});
    return impl_->dispatch(input, output, captured);
}

void DirectNv12Converter::release(DirectFrameResources& resources, AVFrame* input,
                                  AVFrame* output) noexcept {
    if (impl_) impl_->release(resources, input, output);
}

} // namespace fluxrec::gpu::detail
