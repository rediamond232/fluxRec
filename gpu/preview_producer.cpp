#include "gpu/preview_producer.hpp"

#include "preview/preview.hpp"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef FLUXREC_PREVIEW_SHADER_PATH
#define FLUXREC_PREVIEW_SHADER_PATH ""
#endif

namespace fluxrec::gpu {
namespace {

using Clock = std::chrono::steady_clock;

GpuError vk_error(std::string operation, VkResult result) {
    operation += " failed with VkResult ";
    operation += std::to_string(static_cast<int>(result));
    return GpuError{std::move(operation), static_cast<int>(result)};
}

GpuError preview_error(std::string message, int code = 0) {
    return GpuError{std::move(message), code};
}

GpuError source_unavailable(std::string message) {
    return preview_error(std::move(message), EAGAIN);
}

AVVkFrame* vk_frame(AVFrame* frame) {
    if (!frame || frame->format != AV_PIX_FMT_VULKAN || !frame->data[0]) return nullptr;
    return reinterpret_cast<AVVkFrame*>(frame->data[0]);
}

AVHWFramesContext* frames_context(AVFrame* frame) {
    return frame && frame->hw_frames_ctx
        ? reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data) : nullptr;
}

class VulkanFrameLock {
public:
    VulkanFrameLock(AVHWFramesContext* frames_context,
                    AVVulkanFramesContext* vulkan_frames,
                    AVVkFrame* frame) noexcept
        : frames_context_(frames_context), vulkan_frames_(vulkan_frames), frame_(frame) {
        if (frames_context_ && vulkan_frames_ && frame_ &&
            vulkan_frames_->lock_frame && vulkan_frames_->unlock_frame) {
            vulkan_frames_->lock_frame(frames_context_, frame_);
            locked_ = true;
        }
    }

    ~VulkanFrameLock() {
        unlock();
    }

    VulkanFrameLock(const VulkanFrameLock&) = delete;
    VulkanFrameLock& operator=(const VulkanFrameLock&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return locked_; }

    void unlock() noexcept {
        if (!locked_) return;
        vulkan_frames_->unlock_frame(frames_context_, frame_);
        locked_ = false;
    }

private:
    AVHWFramesContext* frames_context_ = nullptr;
    AVVulkanFramesContext* vulkan_frames_ = nullptr;
    AVVkFrame* frame_ = nullptr;
    bool locked_ = false;
};

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

std::uint64_t make_session_id() noexcept {
    const auto now = static_cast<std::uint64_t>(
        Clock::now().time_since_epoch().count());
    const auto process = static_cast<std::uint64_t>(::getpid());
    return (now << 17U) ^ (now >> 11U) ^ (process << 32U) ^ 0x46504c5558524543ULL;
}

preview::UniqueFd duplicate_fd(int fd) {
    if (fd < 0) return {};
    return preview::UniqueFd(::fcntl(fd, F_DUPFD_CLOEXEC, 0));
}

std::expected<preview::SeqPacketSocket, GpuError> connect_socket(
    const std::string& path) {
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) {
        return std::unexpected(preview_error("preview socket path is empty or too long"));
    }
    const int raw_socket = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (raw_socket < 0) {
        return std::unexpected(preview_error("creating preview SOCK_SEQPACKET socket failed",
                                             errno));
    }
    preview::UniqueFd fd(raw_socket);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    const socklen_t address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path.size() + 1U);
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), address_size) < 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) {
            return std::unexpected(preview_error("connecting preview socket failed", errno));
        }
        pollfd descriptor{fd.get(), POLLOUT, 0};
        int ready = 0;
        do {
            ready = ::poll(&descriptor, 1, 40);
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0) {
            return std::unexpected(preview_error(
                ready == 0 ? "preview socket connection timed out"
                           : "polling preview socket connection failed",
                ready == 0 ? ETIMEDOUT : errno));
        }
        int socket_error = 0;
        socklen_t error_size = sizeof(socket_error);
        if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &socket_error, &error_size) < 0) {
            return std::unexpected(preview_error("reading preview socket connection status failed",
                                                 errno));
        }
        if (socket_error != 0) {
            return std::unexpected(preview_error("connecting preview socket failed", socket_error));
        }
    }
    auto socket = preview::SeqPacketSocket::adopt(std::move(fd));
    if (!socket) {
        return std::unexpected(preview_error(socket.error().message, socket.error().system_error));
    }
    return std::move(*socket);
}

enum class FdReadiness {
    pending,
    signaled,
    error,
};

struct FdPollResult {
    FdReadiness readiness = FdReadiness::error;
    int system_error = 0;
    short events = 0;
};

FdPollResult poll_readiness(int fd) noexcept {
    if (fd < 0) return {FdReadiness::error, EBADF, POLLNVAL};
    pollfd descriptor{fd, POLLIN, 0};
    int result = 0;
    do {
        result = ::poll(&descriptor, 1, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0) return {FdReadiness::error, errno, descriptor.revents};
    if (result == 0) return {FdReadiness::pending, 0, 0};
    if ((descriptor.revents & (POLLERR | POLLNVAL | POLLHUP)) != 0 ||
        (descriptor.revents & POLLIN) == 0) {
        return {FdReadiness::error, 0, descriptor.revents};
    }
    return {FdReadiness::signaled, 0, descriptor.revents};
}

} // namespace

struct PreviewProducer::Impl {
    enum class SlotState : std::uint8_t {
        free,
        writing,
        ui_reading,
        wait_for_acquire,
        wait_for_release,
    };

    struct Slot {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImageView source_y_view = VK_NULL_HANDLE;
        VkImageView source_uv_view = VK_NULL_HANDLE;
        AVFrame* source_frame = nullptr;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        VkSemaphore export_semaphore = VK_NULL_HANDLE;
        VkFence completion_fence = VK_NULL_HANDLE;
        VkCommandBuffer command = VK_NULL_HANDLE;
        preview::UniqueFd buffer_fd;
        preview::UniqueFd fence_fd;
        VkDeviceSize allocation_size = 0;
        VkDeviceSize offset = 0;
        std::uint32_t stride = 0;
        std::uint64_t modifier = 0;
        bool initialized = false;
        bool completion_pending = false;
        std::atomic<SlotState> state{SlotState::free};
        std::uint64_t sequence = 0;
    };

    AVBufferRef* device_ref = nullptr;
    AVHWDeviceContext* av_device = nullptr;
    AVVulkanDeviceContext* vk_context = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t width = 960;
    std::uint32_t height = 540;
    std::uint32_t max_fps = 60;
    static constexpr std::size_t slot_capacity = 3;
    std::uint64_t generation = 1;
    std::uint64_t next_frame_deadline_ns = 0;
    std::uint64_t period_ns = 16'666'667;
    std::atomic<bool> connected{false};
    std::atomic<bool> enabled{false};
    std::atomic<bool> peer_error_sent{false};
    preview::SeqPacketSocket socket;
    PreviewProducerConfig config;
    std::jthread service_thread;
    std::atomic<std::uint64_t> frames_submitted{0};
    std::atomic<std::uint64_t> frames_rate_limited{0};
    std::atomic<std::uint64_t> frames_no_free_slot{0};
    std::atomic<std::uint64_t> frames_source_unavailable{0};
    std::atomic<std::uint64_t> frames_transport_dropped{0};
    std::atomic<std::uint64_t> frames_published{0};
    mutable std::mutex error_mutex;
    std::string error_text;
    std::array<Slot, slot_capacity> slots{};
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shader_module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    PFN_vkGetMemoryFdKHR get_memory_fd = nullptr;
    PFN_vkGetSemaphoreFdKHR get_semaphore_fd = nullptr;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_image_modifier_properties = nullptr;

    ~Impl() {
        if (service_thread.joinable()) {
            service_thread.request_stop();
            service_thread.join();
        }
        if (device != VK_NULL_HANDLE) {
            // This runs during optional-preview shutdown. No queue operation in
            // the recording loop waits here, and the output pool can remain
            // imported by Qt through its own DMA-BUF references.
            (void)vkDeviceWaitIdle(device);
            for (auto& slot : slots) {
                if (slot.command && command_pool)
                    (void)vkFreeCommandBuffers(device, command_pool, 1, &slot.command);
                if (slot.source_y_view)
                    vkDestroyImageView(device, slot.source_y_view, nullptr);
                if (slot.source_uv_view)
                    vkDestroyImageView(device, slot.source_uv_view, nullptr);
                av_frame_free(&slot.source_frame);
                if (slot.export_semaphore)
                    vkDestroySemaphore(device, slot.export_semaphore, nullptr);
                if (slot.completion_fence)
                    vkDestroyFence(device, slot.completion_fence, nullptr);
                if (slot.view) vkDestroyImageView(device, slot.view, nullptr);
                if (slot.image) vkDestroyImage(device, slot.image, nullptr);
                if (slot.memory) vkFreeMemory(device, slot.memory, nullptr);
            }
            if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
            if (sampler) vkDestroySampler(device, sampler, nullptr);
            if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
            if (shader_module) vkDestroyShaderModule(device, shader_module, nullptr);
            if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            if (descriptor_layout)
                vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
        }
        av_buffer_unref(&device_ref);
    }

    void disable(std::string message, bool report_to_peer = true) {
        enabled.store(false, std::memory_order_release);
        const bool was_connected = connected.exchange(false, std::memory_order_acq_rel);
        if (!message.empty()) {
            std::lock_guard lock(error_mutex);
            if (error_text.empty()) error_text = message;
        }
        const bool send_error = report_to_peer && was_connected && !message.empty() &&
            !peer_error_sent.exchange(true, std::memory_order_acq_rel);
        if (send_error) {
            try {
                preview::PeerError peer_error{};
                peer_error.code = 1;
                peer_error.message = message.substr(0, preview::max_error_text_bytes);
                (void)socket.send(preview::Message{std::move(peer_error)});
            } catch (...) {
                // Reporting is best effort. Shutting down below still tells
                // the UI to discard its optional preview state.
            }
        }
        if (was_connected && socket.fd() >= 0) (void)::shutdown(socket.fd(), SHUT_RDWR);
    }

    void record_error(std::string message) {
        if (message.empty()) return;
        std::lock_guard lock(error_mutex);
        if (error_text.empty()) error_text = std::move(message);
    }

    std::expected<void, GpuError> initialize(AVBufferRef* supplied_device,
                                              PreviewProducerConfig producer_config) {
        if (!supplied_device || !supplied_device->data) {
            return std::unexpected(preview_error("preview needs the encoder Vulkan device"));
        }
        if (producer_config.source_width == 0 || producer_config.source_height == 0 ||
            producer_config.width != 960 || producer_config.height != 540 ||
            producer_config.max_fps == 0 || producer_config.max_fps > 60 ||
            producer_config.slot_count != 3 ||
            producer_config.socket_path.empty()) {
            return std::unexpected(preview_error(
                "preview requires 960x540, a socket, 1..60 FPS, and three slots"));
        }
        if (producer_config.session_id == 0) producer_config.session_id = make_session_id();
        config = std::move(producer_config);
        source_width = config.source_width;
        source_height = config.source_height;
        width = config.width;
        height = config.height;
        max_fps = config.max_fps;
        period_ns = (1'000'000'000ULL + max_fps - 1U) / max_fps;

        device_ref = av_buffer_ref(supplied_device);
        if (!device_ref) {
            return std::unexpected(preview_error("retaining FFmpeg Vulkan device failed"));
        }
        av_device = reinterpret_cast<AVHWDeviceContext*>(device_ref->data);
        if (av_device->type != AV_HWDEVICE_TYPE_VULKAN || !av_device->hwctx) {
            return std::unexpected(preview_error("preview device reference is not Vulkan"));
        }
        vk_context = static_cast<AVVulkanDeviceContext*>(av_device->hwctx);
        device = vk_context->act_dev;
        physical_device = vk_context->phys_dev;
        if (device == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE) {
            return std::unexpected(preview_error("FFmpeg Vulkan device handles are incomplete"));
        }
        for (const char* required : {
                 VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                 VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                 VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
                 VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME}) {
            if (!extension_enabled(vk_context, required)) {
                return std::unexpected(preview_error(
                    std::string("preview requires the enabled Vulkan device extension ") + required));
            }
        }
        get_memory_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
            vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
        get_semaphore_fd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
            vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
        get_image_modifier_properties =
            reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
                vkGetDeviceProcAddr(device, "vkGetImageDrmFormatModifierPropertiesEXT"));
        if (!get_memory_fd || !get_semaphore_fd || !get_image_modifier_properties) {
            return std::unexpected(preview_error(
                "preview Vulkan DMA-BUF, modifier, or sync-file entry point is unavailable"));
        }
        if (vk_context->nb_qf <= 0) {
            return std::unexpected(preview_error("FFmpeg Vulkan device exposes no queue families"));
        }
        bool found_compute = false;
        for (int index = 0; index < vk_context->nb_qf; ++index) {
            if ((vk_context->qf[index].flags & VK_QUEUE_COMPUTE_BIT) == 0 ||
                vk_context->qf[index].num <= 0) {
                continue;
            }
            queue_family = static_cast<std::uint32_t>(vk_context->qf[index].idx);
            vkGetDeviceQueue(device, queue_family, 0, &queue);
            found_compute = queue != VK_NULL_HANDLE;
            if (found_compute) break;
        }
        if (!found_compute) {
            return std::unexpected(preview_error(
                "FFmpeg Vulkan device exposes no compute queue for preview"));
        }

        VkPhysicalDeviceDrmPropertiesEXT drm_properties{};
        drm_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceIDProperties id_properties{};
        id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        id_properties.pNext = &drm_properties;
        VkPhysicalDeviceProperties2 physical_properties{};
        physical_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        physical_properties.pNext = &id_properties;
        vkGetPhysicalDeviceProperties2(physical_device, &physical_properties);
        if (drm_properties.hasRender == VK_FALSE || drm_properties.renderMajor < 0 ||
            drm_properties.renderMinor < 0 ||
            static_cast<std::uint64_t>(drm_properties.renderMajor) >
                std::numeric_limits<std::uint32_t>::max() ||
            static_cast<std::uint64_t>(drm_properties.renderMinor) >
                std::numeric_limits<std::uint32_t>::max() ||
            (drm_properties.renderMajor == 0 && drm_properties.renderMinor == 0)) {
            return std::unexpected(preview_error(
                "Vulkan device does not report a DRM render-node identity"));
        }
        const bool nonzero_uuid = std::any_of(
            std::begin(id_properties.deviceUUID), std::end(id_properties.deviceUUID),
            [](std::uint8_t byte) { return byte != 0; });
        if (!nonzero_uuid) {
            return std::unexpected(preview_error("Vulkan device UUID is unavailable"));
        }

        VkPhysicalDeviceExternalSemaphoreInfo external_semaphore_info{};
        external_semaphore_info.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
        external_semaphore_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        VkExternalSemaphoreProperties external_semaphore_properties{};
        external_semaphore_properties.sType =
            VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
        vkGetPhysicalDeviceExternalSemaphoreProperties(
            physical_device, &external_semaphore_info, &external_semaphore_properties);
        if ((external_semaphore_properties.externalSemaphoreFeatures &
             VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) == 0) {
            return std::unexpected(preview_error(
                "Vulkan device cannot export preview acquire sync_file fences"));
        }

        auto socket_result = connect_socket(config.socket_path);
        if (!socket_result) return std::unexpected(socket_result.error());
        socket = std::move(*socket_result);

        auto pipeline_result = create_pipeline();
        if (!pipeline_result) return pipeline_result;
        auto pool_result = create_pool();
        if (!pool_result) return pool_result;

        preview::Hello hello{config.session_id};
        auto sent = socket.send(preview::Message{hello});
        if (!sent) {
            return std::unexpected(preview_error("sending preview HELLO failed: " +
                                                 sent.error().message,
                                                 sent.error().system_error));
        }
        preview::PoolBegin begin{};
        begin.generation = generation;
        begin.slot_count = static_cast<std::uint32_t>(slots.size());
        begin.width = width;
        begin.height = height;
        begin.fourcc = preview::drm_format_abgr8888;
        begin.modifier = slots.front().modifier;
        std::copy(std::begin(id_properties.deviceUUID), std::end(id_properties.deviceUUID),
                  begin.device_uuid.begin());
        begin.drm_major = static_cast<std::uint32_t>(drm_properties.renderMajor);
        begin.drm_minor = static_cast<std::uint32_t>(drm_properties.renderMinor);
        sent = socket.send(preview::Message{begin});
        if (!sent) {
            return std::unexpected(preview_error("sending preview pool description failed: " +
                                                 sent.error().message,
                                                 sent.error().system_error));
        }
        for (std::uint32_t index = 0; index < slots.size(); ++index) {
            auto& slot = slots[index];
            auto buffer_fd = duplicate_fd(slot.buffer_fd.get());
            if (!buffer_fd) {
                return std::unexpected(preview_error(
                    "duplicating preview DMA-BUF for pool description failed", errno));
            }
            preview::PoolSlot pool_slot{};
            pool_slot.generation = generation;
            pool_slot.slot_count = static_cast<std::uint32_t>(slots.size());
            pool_slot.slot_index = index;
            pool_slot.image.width = width;
            pool_slot.image.height = height;
            pool_slot.image.fourcc = preview::drm_format_abgr8888;
            pool_slot.image.modifier = slot.modifier;
            pool_slot.image.offset = slot.offset;
            pool_slot.image.stride = slot.stride;
            pool_slot.image.allocation_size = static_cast<std::uint64_t>(slot.allocation_size);
            pool_slot.image.buffer_fd = std::move(buffer_fd);
            sent = socket.send(preview::Message{std::move(pool_slot)});
            if (!sent) {
                return std::unexpected(preview_error("sending preview pool slot failed: " +
                                                     sent.error().message,
                                                     sent.error().system_error));
            }
        }
        sent = socket.send(preview::Message{preview::PoolReady{
            generation, static_cast<std::uint32_t>(slots.size())}});
        if (!sent) {
            return std::unexpected(preview_error("sending preview pool ready failed: " +
                                                 sent.error().message,
                                                 sent.error().system_error));
        }
        connected.store(true, std::memory_order_release);
        enabled.store(true, std::memory_order_release);
        service_thread = std::jthread([this](std::stop_token stop_token) {
            try {
                service_loop(stop_token);
            } catch (...) {
                enabled.store(false, std::memory_order_release);
                connected.store(false, std::memory_order_release);
            }
        });
        return {};
    }

    std::expected<void, GpuError> create_pipeline() {
        VkFormatProperties format_properties{};
        vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R8G8B8A8_UNORM,
                                            &format_properties);
        if ((format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0 ||
            (format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
            return std::unexpected(preview_error(
                "Vulkan device lacks sampled/storage support for RGBA8 preview images"));
        }

        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo descriptor_layout_info{};
        descriptor_layout_info.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptor_layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        descriptor_layout_info.pBindings = bindings.data();
        VkResult result = vkCreateDescriptorSetLayout(device, &descriptor_layout_info,
                                                       nullptr, &descriptor_layout);
        if (result != VK_SUCCESS) return std::unexpected(vk_error(
            "vkCreateDescriptorSetLayout(preview)", result));

        const auto set_count = config.slot_count;
        const std::array<VkDescriptorPoolSize, 2> pool_sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, set_count * 2U},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, set_count},
        };
        VkDescriptorPoolCreateInfo descriptor_pool_info{};
        descriptor_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        descriptor_pool_info.maxSets = set_count;
        descriptor_pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
        descriptor_pool_info.pPoolSizes = pool_sizes.data();
        result = vkCreateDescriptorPool(device, &descriptor_pool_info,
                                        nullptr, &descriptor_pool);
        if (result != VK_SUCCESS) return std::unexpected(vk_error(
            "vkCreateDescriptorPool(preview)", result));

        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(std::uint32_t) * 2U;
        VkPipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &descriptor_layout;
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push_range;
        result = vkCreatePipelineLayout(device, &pipeline_layout_info,
                                        nullptr, &pipeline_layout);
        if (result != VK_SUCCESS) return std::unexpected(vk_error(
            "vkCreatePipelineLayout(preview)", result));

        auto shader = read_shader(FLUXREC_PREVIEW_SHADER_PATH);
        if (!shader) return std::unexpected(shader.error());
        VkShaderModuleCreateInfo shader_info{};
        shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize = shader->size() * sizeof(std::uint32_t);
        shader_info.pCode = shader->data();
        result = vkCreateShaderModule(device, &shader_info, nullptr, &shader_module);
        if (result != VK_SUCCESS) return std::unexpected(vk_error(
            "vkCreateShaderModule(preview)", result));
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader_module;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage = stage;
        pipeline_info.layout = pipeline_layout;
        result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                          nullptr, &pipeline);
        if (result != VK_SUCCESS) return std::unexpected(vk_error(
            "vkCreateComputePipelines(preview)", result));

        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = 0.0F;
        result = vkCreateSampler(device, &sampler_info, nullptr, &sampler);
        if (result != VK_SUCCESS) return std::unexpected(vk_error(
            "vkCreateSampler(preview)", result));

        VkCommandPoolCreateInfo command_pool_info{};
        command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                                  VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        command_pool_info.queueFamilyIndex = queue_family;
        result = vkCreateCommandPool(device, &command_pool_info, nullptr, &command_pool);
        if (result != VK_SUCCESS) return std::unexpected(vk_error(
            "vkCreateCommandPool(preview)", result));
        return {};
    }

    std::expected<std::vector<std::uint32_t>, GpuError> read_shader(
        const std::string& path) {
        if (path.empty()) {
            return std::unexpected(preview_error("preview SPIR-V shader path was not configured"));
        }
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return std::unexpected(preview_error("opening preview shader failed", errno));
        }
        struct stat file_status {};
        if (::fstat(fd, &file_status) < 0 || file_status.st_size < 20 ||
            (static_cast<std::uint64_t>(file_status.st_size) % sizeof(std::uint32_t)) != 0) {
            const int saved_errno = errno;
            (void)::close(fd);
            return std::unexpected(preview_error("preview shader has invalid SPIR-V size",
                                                 saved_errno));
        }
        const auto bytes = static_cast<std::size_t>(file_status.st_size);
        std::vector<std::uint32_t> code(bytes / sizeof(std::uint32_t));
        std::size_t offset = 0;
        while (offset < bytes) {
            const ssize_t count = ::read(fd, reinterpret_cast<std::byte*>(code.data()) + offset,
                                         bytes - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                const int saved_errno = count < 0 ? errno : EIO;
                (void)::close(fd);
                return std::unexpected(preview_error("reading preview shader failed",
                                                     saved_errno));
            }
            offset += static_cast<std::size_t>(count);
        }
        (void)::close(fd);
        if (code.front() != 0x07230203U) {
            return std::unexpected(preview_error("preview shader does not have a SPIR-V header"));
        }
        return code;
    }

    std::expected<std::uint64_t, GpuError> select_modifier() {
        VkDrmFormatModifierPropertiesListEXT modifier_list{};
        modifier_list.sType =
            VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
        VkFormatProperties2 format_properties{};
        format_properties.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        format_properties.pNext = &modifier_list;
        vkGetPhysicalDeviceFormatProperties2(physical_device,
                                             VK_FORMAT_R8G8B8A8_UNORM,
                                             &format_properties);
        if (modifier_list.drmFormatModifierCount == 0) {
            return std::unexpected(preview_error(
                "Vulkan exposes no DRM modifiers for RGBA8 preview images"));
        }
        std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(
            modifier_list.drmFormatModifierCount);
        modifier_list.drmFormatModifierCount =
            static_cast<std::uint32_t>(modifiers.size());
        modifier_list.pDrmFormatModifierProperties = modifiers.data();
        vkGetPhysicalDeviceFormatProperties2(physical_device,
                                             VK_FORMAT_R8G8B8A8_UNORM,
                                             &format_properties);

        const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT;
        for (const auto& modifier : modifiers) {
            if (modifier.drmFormatModifierPlaneCount != 1 ||
                (modifier.drmFormatModifierTilingFeatures &
                 (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) !=
                 (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
                continue;
            }
            VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info{};
            modifier_info.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
            modifier_info.drmFormatModifier = modifier.drmFormatModifier;
            modifier_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkPhysicalDeviceExternalImageFormatInfo external_info{};
            external_info.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
            external_info.pNext = &modifier_info;
            external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            VkPhysicalDeviceImageFormatInfo2 image_info{};
            image_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
            image_info.pNext = &external_info;
            image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
            image_info.type = VK_IMAGE_TYPE_2D;
            image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            image_info.usage = usage;
            VkExternalImageFormatProperties external_properties{};
            external_properties.sType =
                VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
            VkImageFormatProperties2 image_properties{};
            image_properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
            image_properties.pNext = &external_properties;
            const VkResult result = vkGetPhysicalDeviceImageFormatProperties2(
                physical_device, &image_info, &image_properties);
            if (result != VK_SUCCESS) continue;
            if (image_properties.imageFormatProperties.maxExtent.width < width ||
                image_properties.imageFormatProperties.maxExtent.height < height) {
                continue;
            }
            const auto& memory = external_properties.externalMemoryProperties;
            constexpr VkExternalMemoryFeatureFlags needed =
                VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
            if ((memory.externalMemoryFeatures & needed) != needed ||
                (memory.compatibleHandleTypes &
                 VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) == 0) {
                continue;
            }
            return modifier.drmFormatModifier;
        }
        return std::unexpected(preview_error(
            "no single-plane RGBA8 DRM modifier supports sampled/storage use and DMA-BUF import/export"));
    }

    std::expected<void, GpuError> create_pool() {
        auto modifier_result = select_modifier();
        if (!modifier_result) return std::unexpected(modifier_result.error());
        const auto modifier = *modifier_result;
        VkImageDrmFormatModifierListCreateInfoEXT modifier_list{};
        modifier_list.sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
        modifier_list.drmFormatModifierCount = 1;
        modifier_list.pDrmFormatModifiers = &modifier;
        VkExternalMemoryImageCreateInfo external_image{};
        external_image.sType =
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external_image.pNext = &modifier_list;
        external_image.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.pNext = &external_image;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = VkExtent3D{width, height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        for (auto& slot : slots) {
            VkResult result = vkCreateImage(device, &image_info, nullptr, &slot.image);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error("vkCreateImage(preview DMA-BUF)", result));
            }
            VkImageDrmFormatModifierPropertiesEXT actual_modifier{};
            actual_modifier.sType =
                VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
            result = get_image_modifier_properties(device, slot.image, &actual_modifier);
            if (result != VK_SUCCESS || actual_modifier.drmFormatModifier != modifier) {
                return std::unexpected(result == VK_SUCCESS
                    ? preview_error("Vulkan created preview image with an unexpected DRM modifier")
                    : vk_error("vkGetImageDrmFormatModifierPropertiesEXT(preview)", result));
            }
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, slot.image, &requirements);
            std::uint32_t memory_type = std::numeric_limits<std::uint32_t>::max();
            VkPhysicalDeviceMemoryProperties memory_properties{};
            vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
            for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
                if ((requirements.memoryTypeBits & (1U << index)) != 0 &&
                    (memory_properties.memoryTypes[index].propertyFlags &
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                    memory_type = index;
                    break;
                }
            }
            if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected(preview_error(
                    "no device-local memory type can back an exportable preview image"));
            }
            VkMemoryDedicatedAllocateInfo dedicated{};
            dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
            dedicated.image = slot.image;
            VkExportMemoryAllocateInfo export_info{};
            export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
            export_info.pNext = &dedicated;
            export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            VkMemoryAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocate.pNext = &export_info;
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = memory_type;
            result = vkAllocateMemory(device, &allocate, nullptr, &slot.memory);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error("vkAllocateMemory(preview DMA-BUF)", result));
            }
            result = vkBindImageMemory(device, slot.image, slot.memory, 0);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error("vkBindImageMemory(preview DMA-BUF)", result));
            }
            const VkImageSubresource subresource{
                VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0};
            VkSubresourceLayout layout{};
            vkGetImageSubresourceLayout(device, slot.image, &subresource, &layout);
            if (layout.rowPitch > std::numeric_limits<std::uint32_t>::max() ||
                layout.offset > requirements.size || layout.size == 0 ||
                layout.size > requirements.size - layout.offset) {
                return std::unexpected(preview_error(
                    "exported preview modifier did not provide a valid one-plane layout"));
            }
            if (layout.rowPitch < static_cast<VkDeviceSize>(width) * 4U) {
                return std::unexpected(preview_error(
                    "exported preview modifier row pitch is shorter than RGBA pixels"));
            }
            VkMemoryGetFdInfoKHR memory_fd_info{};
            memory_fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
            memory_fd_info.memory = slot.memory;
            memory_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            int raw_buffer_fd = -1;
            result = get_memory_fd(device, &memory_fd_info, &raw_buffer_fd);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error("vkGetMemoryFdKHR(preview DMA-BUF)", result));
            }
            slot.buffer_fd.reset(raw_buffer_fd);
            slot.allocation_size = requirements.size;
            slot.offset = layout.offset;
            slot.stride = static_cast<std::uint32_t>(layout.rowPitch);
            slot.modifier = modifier;

            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = slot.image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_info.subresourceRange.levelCount = 1;
            view_info.subresourceRange.layerCount = 1;
            result = vkCreateImageView(device, &view_info, nullptr, &slot.view);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error("vkCreateImageView(preview output)", result));
            }

            VkExportSemaphoreCreateInfo semaphore_export{};
            semaphore_export.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
            semaphore_export.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            semaphore_info.pNext = &semaphore_export;
            result = vkCreateSemaphore(device, &semaphore_info, nullptr,
                                       &slot.export_semaphore);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error("vkCreateSemaphore(preview sync_file)", result));
            }
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            result = vkCreateFence(device, &fence_info, nullptr, &slot.completion_fence);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error("vkCreateFence(preview)", result));
            }
        }
        std::vector<VkCommandBuffer> commands(slots.size());
        VkCommandBufferAllocateInfo command_allocate{};
        command_allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_allocate.commandPool = command_pool;
        command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocate.commandBufferCount = static_cast<std::uint32_t>(commands.size());
        VkResult result = vkAllocateCommandBuffers(device, &command_allocate,
                                                   commands.data());
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("vkAllocateCommandBuffers(preview)", result));
        }
        for (std::size_t index = 0; index < slots.size(); ++index) {
            slots[index].command = commands[index];
        }

        std::array<VkDescriptorSetLayout, slot_capacity> layouts{};
        std::array<VkDescriptorSet, slot_capacity> descriptor_sets{};
        layouts.fill(descriptor_layout);
        VkDescriptorSetAllocateInfo descriptor_allocate{};
        descriptor_allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descriptor_allocate.descriptorPool = descriptor_pool;
        descriptor_allocate.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
        descriptor_allocate.pSetLayouts = layouts.data();
        result = vkAllocateDescriptorSets(device, &descriptor_allocate,
                                          descriptor_sets.data());
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("vkAllocateDescriptorSets(preview pool)", result));
        }
        for (std::size_t index = 0; index < slots.size(); ++index) {
            auto& slot = slots[index];
            slot.descriptor = descriptor_sets[index];
            const VkDescriptorImageInfo output_info{
                VK_NULL_HANDLE, slot.view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet output_write{};
            output_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            output_write.dstSet = slot.descriptor;
            output_write.dstBinding = 2;
            output_write.descriptorCount = 1;
            output_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            output_write.pImageInfo = &output_info;
            vkUpdateDescriptorSets(device, 1, &output_write, 0, nullptr);
        }
        return {};
    }

    std::expected<VkImageView, GpuError> create_source_view(
        VkImage image, VkFormat format, VkImageAspectFlags aspect) {
        if (image == VK_NULL_HANDLE || format == VK_FORMAT_UNDEFINED) {
            return std::unexpected(preview_error("preview source Vulkan image or format is undefined"));
        }
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange.aspectMask = aspect;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        const VkResult result = vkCreateImageView(device, &view_info, nullptr, &view);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("vkCreateImageView(preview source)", result));
        }
        return view;
    }

    std::expected<std::pair<VkImageView, VkImageView>, GpuError> create_source_views(
        AVVulkanFramesContext* frames, AVVkFrame* frame) {
        if (!frames || !frame) {
            return std::unexpected(source_unavailable("preview source Vulkan metadata is missing"));
        }
        VkImageView y_view = VK_NULL_HANDLE;
        VkImageView uv_view = VK_NULL_HANDLE;
        if (frames->format[1] == VK_FORMAT_UNDEFINED) {
            if (frames->format[0] != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ||
                !frame->img[0] || frame->img[1]) {
                return std::unexpected(source_unavailable(
                    "preview source is not a single-image Vulkan NV12 frame"));
            }
            auto y = create_source_view(frame->img[0], VK_FORMAT_R8_UNORM,
                                        VK_IMAGE_ASPECT_PLANE_0_BIT);
            if (!y) return std::unexpected(y.error());
            y_view = *y;
            auto uv = create_source_view(frame->img[0], VK_FORMAT_R8G8_UNORM,
                                         VK_IMAGE_ASPECT_PLANE_1_BIT);
            if (!uv) {
                vkDestroyImageView(device, y_view, nullptr);
                return std::unexpected(uv.error());
            }
            uv_view = *uv;
        } else {
            if (frames->format[0] != VK_FORMAT_R8_UNORM ||
                frames->format[1] != VK_FORMAT_R8G8_UNORM ||
                !frame->img[0] || !frame->img[1]) {
                return std::unexpected(source_unavailable(
                    "preview source separate NV12 planes must be R8 and RG8 images"));
            }
            auto y = create_source_view(frame->img[0], frames->format[0],
                                        VK_IMAGE_ASPECT_COLOR_BIT);
            if (!y) return std::unexpected(y.error());
            y_view = *y;
            auto uv = create_source_view(frame->img[1], frames->format[1],
                                         VK_IMAGE_ASPECT_COLOR_BIT);
            if (!uv) {
                vkDestroyImageView(device, y_view, nullptr);
                return std::unexpected(uv.error());
            }
            uv_view = *uv;
        }
        return std::pair{y_view, uv_view};
    }

    void retire_source_views(Slot& slot) noexcept {
        if (slot.source_y_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, slot.source_y_view, nullptr);
            slot.source_y_view = VK_NULL_HANDLE;
        }
        if (slot.source_uv_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, slot.source_uv_view, nullptr);
            slot.source_uv_view = VK_NULL_HANDLE;
        }
        av_frame_free(&slot.source_frame);
    }

    bool poll_gpu_completion(Slot& slot) noexcept {
        if (!slot.completion_pending) return true;
        const VkResult status = vkGetFenceStatus(device, slot.completion_fence);
        if (status == VK_NOT_READY) return false;
        if (status != VK_SUCCESS) {
            disable("checking preview completion fence failed: " +
                    std::to_string(static_cast<int>(status)));
            return false;
        }
        slot.completion_pending = false;
        retire_source_views(slot);
        return true;
    }

    void service() {
        if (connected.load(std::memory_order_acquire)) {
            for (;;) {
                auto message = socket.receive();
                if (!message) {
                    if (message.error().code == preview::ErrorCode::would_block) break;
                    disable("preview control channel failed: " + message.error().message);
                    break;
                }
                if (auto* release = std::get_if<preview::FrameRelease>(&*message)) {
                    if (release->generation != generation || release->slot_count != slots.size() ||
                        release->slot_index >= slots.size()) {
                        disable("preview release referred to an unknown pool generation or slot");
                        break;
                    }
                    auto& slot = slots[release->slot_index];
                    if (slot.state.load(std::memory_order_acquire) != SlotState::ui_reading ||
                        slot.sequence != release->sequence || !release->release_fence_fd) {
                        disable("preview release did not match an in-use slot and sequence");
                        break;
                    }
                    slot.fence_fd = std::move(release->release_fence_fd);
                    slot.state.store(SlotState::wait_for_release,
                                     std::memory_order_release);
                    continue;
                }
                if (const auto* stop = std::get_if<preview::Stop>(&*message)) {
                    if (stop->generation != generation) {
                        disable("preview STOP used an unknown pool generation");
                    } else {
                        disable("preview peer requested shutdown", false);
                    }
                    break;
                }
                if (const auto* peer_error = std::get_if<preview::PeerError>(&*message)) {
                    disable("preview peer error: " + peer_error->message, false);
                    break;
                }
                disable("preview peer sent a message invalid for the producer role");
                break;
            }
        }
        for (auto& slot : slots) {
            const SlotState state = slot.state.load(std::memory_order_acquire);
            if (state == SlotState::writing || state == SlotState::free) continue;
            const bool gpu_complete = poll_gpu_completion(slot);
            if (state == SlotState::wait_for_acquire && gpu_complete) {
                slot.fence_fd.reset();
                slot.state.store(SlotState::free, std::memory_order_release);
            } else if (state == SlotState::wait_for_release) {
                const auto release_readiness = poll_readiness(slot.fence_fd.get());
                if (release_readiness.readiness == FdReadiness::error) {
                    std::string message = "polling preview RELEASE fence failed";
                    if (release_readiness.system_error != 0) {
                        message += ": ";
                        message += std::strerror(release_readiness.system_error);
                    } else {
                        message += ": poll revents=";
                        message += std::to_string(release_readiness.events);
                    }
                    disable(std::move(message));
                    break;
                }
                if (gpu_complete && release_readiness.readiness == FdReadiness::signaled) {
                    slot.fence_fd.reset();
                    slot.state.store(SlotState::free, std::memory_order_release);
                }
            }
        }
    }

    bool has_local_work() const noexcept {
        for (const auto& slot : slots) {
            const SlotState state = slot.state.load(std::memory_order_acquire);
            if (state == SlotState::writing) return true;
            if (state != SlotState::free && slot.completion_pending) return true;
        }
        return false;
    }

    void service_loop(std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            if (!connected.load(std::memory_order_acquire)) {
                service();
                if (!has_local_work()) break;
                (void)::poll(nullptr, 0, 20);
                continue;
            }
            std::array<pollfd, slot_capacity + 1U> descriptors{};
            nfds_t count = 0;
            descriptors[count++] = pollfd{socket.fd(), POLLIN, 0};
            for (const auto& slot : slots) {
                const SlotState state = slot.state.load(std::memory_order_acquire);
                if ((state == SlotState::wait_for_acquire ||
                     state == SlotState::wait_for_release) && slot.fence_fd) {
                    descriptors[count++] = pollfd{slot.fence_fd.get(), POLLIN, 0};
                }
            }
            int result = 0;
            do {
                result = ::poll(descriptors.data(), count, 20);
            } while (result < 0 && errno == EINTR && !stop_token.stop_requested());
            if (stop_token.stop_requested()) break;
            if (result < 0) {
                disable("polling preview control/fence descriptors failed: " +
                        std::string(std::strerror(errno)));
                break;
            }
            service();
        }
    }

    static VkImageMemoryBarrier2 barrier(VkImage image, VkImageLayout old_layout,
                                         VkImageLayout new_layout,
                                         VkPipelineStageFlags2 source_stage,
                                         VkAccessFlags2 source_access,
                                         VkPipelineStageFlags2 destination_stage,
                                         VkAccessFlags2 destination_access,
                                         std::uint32_t source_queue,
                                         std::uint32_t destination_queue) {
        const bool transfer = source_queue != VK_QUEUE_FAMILY_IGNORED &&
                              destination_queue != VK_QUEUE_FAMILY_IGNORED &&
                              source_queue != destination_queue;
        VkImageMemoryBarrier2 result{};
        result.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        result.srcStageMask = source_stage;
        result.srcAccessMask = source_access;
        result.dstStageMask = destination_stage;
        result.dstAccessMask = destination_access;
        result.oldLayout = old_layout;
        result.newLayout = new_layout;
        result.srcQueueFamilyIndex = transfer ? source_queue : VK_QUEUE_FAMILY_IGNORED;
        result.dstQueueFamilyIndex = transfer ? destination_queue : VK_QUEUE_FAMILY_IGNORED;
        result.image = image;
        result.subresourceRange = VkImageSubresourceRange{
            VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return result;
    }

    std::expected<void, GpuError> dispatch(AVFrame* source,
                                            std::uint64_t sequence,
                                            std::uint64_t presentation_time_ns,
                                            bool has_presentation_time,
                                            Slot& slot,
                                            std::size_t slot_index) {
        AVVkFrame* source_vk = vk_frame(source);
        AVHWFramesContext* source_frames = frames_context(source);
        if (!source_vk || !source_frames) {
            return std::unexpected(source_unavailable(
                "preview source is not a Vulkan NV12 frame"));
        }
        if (source->width != static_cast<int>(source_width) ||
            source->height != static_cast<int>(source_height) ||
            source_frames->width != static_cast<int>(source_width) ||
            source_frames->height != static_cast<int>(source_height)) {
            return std::unexpected(source_unavailable(
                "preview source dimensions differ from the negotiated capture size"));
        }
        auto* source_vk_frames = static_cast<AVVulkanFramesContext*>(source_frames->hwctx);
        if (!source_vk_frames || source_frames->sw_format != AV_PIX_FMT_NV12) {
            return std::unexpected(source_unavailable(
                "preview source does not expose Vulkan NV12 storage"));
        }
        if ((source_vk_frames->usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0) {
            return std::unexpected(source_unavailable(
                "preview source NV12 images were not created for sampling"));
        }
        VulkanFrameLock frame_lock(source_frames, source_vk_frames, source_vk);
        if (!frame_lock) {
            return std::unexpected(source_unavailable(
                "preview source does not provide FFmpeg Vulkan frame lock callbacks"));
        }
        const bool separate_source_planes =
            source_vk_frames->format[1] != VK_FORMAT_UNDEFINED;
        const std::uint32_t source_count = separate_source_planes ? 2U : 1U;
        if ((!separate_source_planes &&
             source_vk_frames->format[0] != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) ||
            (separate_source_planes &&
             (source_vk_frames->format[0] != VK_FORMAT_R8_UNORM ||
              source_vk_frames->format[1] != VK_FORMAT_R8G8_UNORM))) {
            return std::unexpected(source_unavailable(
                "preview source Vulkan NV12 plane formats are unsupported"));
        }
        for (std::uint32_t plane = 0; plane < source_count; ++plane) {
            if (!source_vk->img[plane] || source_vk->layout[plane] != VK_IMAGE_LAYOUT_GENERAL ||
                !source_vk->sem[plane] ||
                source_vk->sem_value[plane] == std::numeric_limits<std::uint64_t>::max()) {
                return std::unexpected(source_unavailable(
                    "preview source NV12 plane has no GENERAL layout or timeline semaphore"));
            }
        }
        for (const VkFormat format : {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM}) {
            VkFormatProperties source_format_properties{};
            vkGetPhysicalDeviceFormatProperties(physical_device, format,
                                                &source_format_properties);
            if ((source_format_properties.optimalTilingFeatures &
                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0) {
                return std::unexpected(source_unavailable(
                    "preview Vulkan NV12 plane format does not support linear sampling"));
            }
        }

        // The retained clone keeps the final NV12 VkImage alive until this
        // preview submission's local fence signals.
        slot.source_frame = av_frame_clone(source);
        if (!slot.source_frame) {
            return std::unexpected(source_unavailable(
                "retaining the Vulkan NV12 preview source frame failed"));
        }
        auto source_views = create_source_views(source_vk_frames, source_vk);
        if (!source_views) return std::unexpected(source_views.error());
        slot.source_y_view = source_views->first;
        slot.source_uv_view = source_views->second;
        const std::array<VkDescriptorImageInfo, 2> source_infos{
            VkDescriptorImageInfo{sampler, slot.source_y_view, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{sampler, slot.source_uv_view, VK_IMAGE_LAYOUT_GENERAL},
        };
        std::array<VkWriteDescriptorSet, 2> source_writes{};
        for (std::uint32_t binding = 0; binding < source_writes.size(); ++binding) {
            source_writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            source_writes[binding].dstSet = slot.descriptor;
            source_writes[binding].dstBinding = binding;
            source_writes[binding].descriptorCount = 1;
            source_writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            source_writes[binding].pImageInfo = &source_infos[binding];
        }
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(source_writes.size()),
                               source_writes.data(), 0, nullptr);

        VkResult result = vkResetCommandBuffer(slot.command, 0);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("vkResetCommandBuffer(preview)", result));
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(slot.command, &begin);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("vkBeginCommandBuffer(preview)", result));
        }

        std::array<VkImageMemoryBarrier2, 3> acquire_barriers{};
        for (std::uint32_t plane = 0; plane < source_count; ++plane) {
            const std::uint32_t source_family = source_vk->queue_family[plane];
            const bool source_is_external = source_family == VK_QUEUE_FAMILY_EXTERNAL;
            acquire_barriers[plane] = barrier(
                source_vk->img[plane], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                source_is_external ? VK_PIPELINE_STAGE_2_NONE
                                   : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                source_is_external ? 0 : source_vk->access[plane],
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                source_family, queue_family);
        }
        const VkImageMemoryBarrier2 output_acquire = barrier(
            slot.image,
            slot.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_NONE, 0,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            slot.initialized ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED,
            slot.initialized ? queue_family : VK_QUEUE_FAMILY_IGNORED);
        acquire_barriers[source_count] = output_acquire;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = source_count + 1U;
        dependency.pImageMemoryBarriers = acquire_barriers.data();
        vkCmdPipelineBarrier2(slot.command, &dependency);
        vkCmdBindPipeline(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(slot.command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                pipeline_layout, 0, 1, &slot.descriptor, 0, nullptr);
        const std::array<std::uint32_t, 2> extent{width, height};
        vkCmdPushConstants(slot.command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(extent), extent.data());
        vkCmdDispatch(slot.command, (width + 15U) / 16U,
                      (height + 15U) / 16U, 1);

        std::array<VkImageMemoryBarrier2, 3> release_barriers{};
        for (std::uint32_t plane = 0; plane < source_count; ++plane) {
            release_barriers[plane] = barrier(
                source_vk->img[plane], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                VK_PIPELINE_STAGE_2_NONE, 0, queue_family,
                source_vk->queue_family[plane]);
        }
        const VkImageMemoryBarrier2 output_release = barrier(
            slot.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_NONE, 0, queue_family, VK_QUEUE_FAMILY_EXTERNAL);
        release_barriers[source_count] = output_release;
        dependency.imageMemoryBarrierCount = source_count + 1U;
        dependency.pImageMemoryBarriers = release_barriers.data();
        vkCmdPipelineBarrier2(slot.command, &dependency);
        result = vkEndCommandBuffer(slot.command);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error("vkEndCommandBuffer(preview)", result));
        }

        struct SourceTimeline {
            VkSemaphore semaphore = VK_NULL_HANDLE;
            std::uint64_t wait_value = 0;
            std::uint64_t signal_value = 0;
        };
        std::array<SourceTimeline, 2> source_timelines{};
        std::uint32_t timeline_count = 0;
        for (std::uint32_t plane = 0; plane < source_count; ++plane) {
            std::uint32_t timeline_index = 0;
            while (timeline_index < timeline_count &&
                   source_timelines[timeline_index].semaphore != source_vk->sem[plane]) {
                ++timeline_index;
            }
            if (timeline_index == timeline_count) {
                source_timelines[timeline_index] = SourceTimeline{
                    source_vk->sem[plane], source_vk->sem_value[plane], 0};
                ++timeline_count;
            } else {
                source_timelines[timeline_index].wait_value = std::max(
                    source_timelines[timeline_index].wait_value,
                    source_vk->sem_value[plane]);
            }
        }
        std::array<VkSemaphoreSubmitInfo, 2> waits{};
        std::array<VkSemaphoreSubmitInfo, 3> signals{};
        for (std::uint32_t index = 0; index < timeline_count; ++index) {
            auto& timeline = source_timelines[index];
            if (timeline.wait_value == std::numeric_limits<std::uint64_t>::max()) {
                return std::unexpected(source_unavailable(
                    "preview source NV12 timeline semaphore value cannot be incremented"));
            }
            timeline.signal_value = timeline.wait_value + 1U;
            waits[index].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            waits[index].semaphore = timeline.semaphore;
            waits[index].value = timeline.wait_value;
            waits[index].stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            signals[index].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            signals[index].semaphore = timeline.semaphore;
            signals[index].value = timeline.signal_value;
            signals[index].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
        signals[timeline_count].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[timeline_count].semaphore = slot.export_semaphore;
        signals[timeline_count].value = 0;
        signals[timeline_count].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo command{};
        command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command.commandBuffer = slot.command;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = timeline_count;
        submit.pWaitSemaphoreInfos = waits.data();
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command;
        submit.signalSemaphoreInfoCount = timeline_count + 1U;
        submit.pSignalSemaphoreInfos = signals.data();

#if FF_API_VULKAN_SYNC_QUEUES
        if (vk_context->lock_queue) vk_context->lock_queue(av_device, queue_family, 0);
#endif
        result = vkResetFences(device, 1, &slot.completion_fence);
        if (result == VK_SUCCESS)
            result = vkQueueSubmit2(queue, 1, &submit, slot.completion_fence);
#if FF_API_VULKAN_SYNC_QUEUES
        if (vk_context->unlock_queue) vk_context->unlock_queue(av_device, queue_family, 0);
#endif
        if (result == VK_SUCCESS) {
            for (std::uint32_t plane = 0; plane < source_count; ++plane) {
                source_vk->layout[plane] = VK_IMAGE_LAYOUT_GENERAL;
                source_vk->access[plane] = 0;
                const auto timeline = std::find_if(
                    source_timelines.begin(), source_timelines.begin() + timeline_count,
                    [&](const SourceTimeline& candidate) {
                        return candidate.semaphore == source_vk->sem[plane];
                    });
                if (timeline != source_timelines.begin() + timeline_count) {
                    source_vk->sem_value[plane] = timeline->signal_value;
                }
            }
            frame_lock.unlock();
        }
        if (result != VK_SUCCESS)
            return std::unexpected(vk_error("vkQueueSubmit2(preview)", result));

        slot.initialized = true;
        slot.completion_pending = true;
        frames_submitted.fetch_add(1, std::memory_order_relaxed);
        slot.sequence = sequence;
        slot.fence_fd.reset();
        slot.state.store(SlotState::ui_reading, std::memory_order_release);
        VkSemaphoreGetFdInfoKHR semaphore_fd_info{};
        semaphore_fd_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
        semaphore_fd_info.semaphore = slot.export_semaphore;
        semaphore_fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        int raw_fence = -1;
        result = get_semaphore_fd(device, &semaphore_fd_info, &raw_fence);
        if (result != VK_SUCCESS) {
            slot.state.store(SlotState::wait_for_acquire, std::memory_order_release);
            return std::unexpected(vk_error("vkGetSemaphoreFdKHR(preview acquire)", result));
        }
        // Vulkan permits a signaled SYNC_FD export to return -1. The protocol
        // requires a pollable sync_file, so drop this preview publication and
        // recycle the slot after its nonblocking Vulkan fence poll succeeds.
        if (raw_fence < 0) {
            slot.state.store(SlotState::wait_for_acquire, std::memory_order_release);
            frames_transport_dropped.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        preview::FrameReady ready{};
        ready.generation = generation;
        ready.slot_count = static_cast<std::uint32_t>(slots.size());
        ready.slot_index = static_cast<std::uint32_t>(slot_index);
        ready.sequence = sequence;
        ready.presentation_time_ns = has_presentation_time ? presentation_time_ns : 0;
        ready.has_presentation_time = has_presentation_time && presentation_time_ns != 0;
        if (!ready.has_presentation_time) ready.presentation_time_ns = 0;
        ready.acquire_fence_fd = preview::UniqueFd(raw_fence);
        preview::Message message{std::move(ready)};
        auto sent = socket.send(message);
        if (!sent) {
            auto* retained = std::get_if<preview::FrameReady>(&message);
            if (retained) slot.fence_fd = std::move(retained->acquire_fence_fd);
            slot.state.store(SlotState::wait_for_acquire, std::memory_order_release);
            frames_transport_dropped.fetch_add(1, std::memory_order_relaxed);
            if (sent.error().code != preview::ErrorCode::would_block) {
                disable("sending preview FRAME failed: " + sent.error().message);
            }
            return {};
        }
        frames_published.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    void submit_frame(AVFrame* source, std::uint64_t sequence,
                      std::uint64_t presentation_time_ns,
                      bool has_presentation_time) {
        if (!enabled.load(std::memory_order_acquire) ||
            !connected.load(std::memory_order_acquire)) return;
        const auto now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now().time_since_epoch()).count());
        if (now < next_frame_deadline_ns) {
            frames_rate_limited.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        next_frame_deadline_ns = now + period_ns;

        std::size_t slot_index = slots.size();
        for (std::size_t index = 0; index < slots.size(); ++index) {
            auto expected = SlotState::free;
            if (slots[index].state.compare_exchange_strong(
                    expected, SlotState::writing, std::memory_order_acquire,
                    std::memory_order_relaxed)) {
                slot_index = index;
                break;
            }
        }
        if (slot_index == slots.size()) {
            frames_no_free_slot.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto& slot = slots[slot_index];
        std::expected<void, GpuError> result;
        try {
            result = dispatch(source, sequence, presentation_time_ns,
                              has_presentation_time, slot, slot_index);
        } catch (...) {
            if (slot.state.load(std::memory_order_acquire) == SlotState::writing) {
                retire_source_views(slot);
                slot.fence_fd.reset();
                slot.state.store(SlotState::free, std::memory_order_release);
            }
            throw;
        }
        if (!result) {
            // dispatch leaves a pre-submit failure in `writing`. Once a queue
            // submit succeeds it publishes an in-flight state, and the
            // service thread alone retires its fence, source views, and frame.
            if (slot.state.load(std::memory_order_acquire) == SlotState::writing) {
                retire_source_views(slot);
                slot.fence_fd.reset();
                slot.state.store(SlotState::free, std::memory_order_release);
            }
            if (result.error().code == EAGAIN) {
                frames_source_unavailable.fetch_add(1, std::memory_order_relaxed);
                record_error(result.error().message);
                return;
            }
            disable("preview submission disabled: " + result.error().message);
            return;
        }
    }
};

PreviewProducer::PreviewProducer(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

PreviewProducer::~PreviewProducer() = default;
PreviewProducer::PreviewProducer(PreviewProducer&&) noexcept = default;
PreviewProducer& PreviewProducer::operator=(PreviewProducer&&) noexcept = default;

std::expected<PreviewProducer, GpuError> PreviewProducer::connect(
    AVBufferRef* vulkan_device_ref, PreviewProducerConfig config) {
    try {
        auto implementation = std::make_unique<Impl>();
        auto initialized = implementation->initialize(vulkan_device_ref, std::move(config));
        if (!initialized) return std::unexpected(initialized.error());
        return PreviewProducer(std::move(implementation));
    } catch (const std::exception& exception) {
        return std::unexpected(preview_error(
            std::string("initializing optional preview failed: ") + exception.what()));
    } catch (...) {
        return std::unexpected(preview_error(
            "initializing optional preview failed with an unknown exception"));
    }
}

void PreviewProducer::submit(AVFrame* source, std::uint64_t sequence,
                             std::uint64_t presentation_time_ns,
                             bool has_presentation_time) noexcept {
    if (!impl_) return;
    try {
        impl_->submit_frame(source, sequence, presentation_time_ns,
                            has_presentation_time);
    } catch (const std::exception& exception) {
        try {
            impl_->disable(std::string("preview producer exception: ") + exception.what());
        } catch (...) {
            impl_->enabled.store(false, std::memory_order_release);
            impl_->connected.store(false, std::memory_order_release);
        }
    } catch (...) {
        impl_->enabled.store(false, std::memory_order_release);
        impl_->connected.store(false, std::memory_order_release);
    }
}

PreviewProducerStats PreviewProducer::stats() const {
    PreviewProducerStats result;
    if (!impl_) return result;
    result.enabled = impl_->enabled.load(std::memory_order_acquire);
    result.frames_submitted = impl_->frames_submitted.load(std::memory_order_relaxed);
    result.frames_rate_limited = impl_->frames_rate_limited.load(std::memory_order_relaxed);
    result.frames_no_free_slot = impl_->frames_no_free_slot.load(std::memory_order_relaxed);
    result.frames_source_unavailable =
        impl_->frames_source_unavailable.load(std::memory_order_relaxed);
    result.frames_transport_dropped =
        impl_->frames_transport_dropped.load(std::memory_order_relaxed);
    result.frames_published = impl_->frames_published.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(impl_->error_mutex);
        result.last_error = impl_->error_text;
    }
    return result;
}

} // namespace fluxrec::gpu
