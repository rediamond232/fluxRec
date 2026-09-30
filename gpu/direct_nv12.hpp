#pragma once

#include "gpu/gpu.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <expected>
#include <memory>

extern "C" {
struct AVBufferRef;
struct AVFrame;
}

namespace fluxrec::gpu::detail {

struct DirectFrameResources {
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    VkImageView source_view = VK_NULL_HANDLE;
    VkImageView y_view = VK_NULL_HANDLE;
    VkImageView uv_view = VK_NULL_HANDLE;
    VkImage temp_y = VK_NULL_HANDLE;
    VkImage temp_uv = VK_NULL_HANDLE;
    VkDeviceMemory temp_y_memory = VK_NULL_HANDLE;
    VkDeviceMemory temp_uv_memory = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBuffer source_sync_probe_command = VK_NULL_HANDLE;
    VkBuffer nv12_readback = VK_NULL_HANDLE;
    VkDeviceMemory nv12_readback_memory = VK_NULL_HANDLE;
    bool nv12_readback_host_coherent = false;
    bool nv12_readback_pending = false;
    VkBuffer source_probe = VK_NULL_HANDLE;
    VkDeviceMemory source_probe_memory = VK_NULL_HANDLE;
    VkDeviceSize source_probe_size = 0;
    bool source_probe_host_coherent = false;
    bool source_probe_pending = false;
    std::uint64_t source_probe_sequence = 0;
    std::uint64_t source_probe_dispatch = 0;
    std::size_t source_probe_slot = 0;
    int source_probe_dma_buf_fd = -1;
    std::uint32_t source_probe_fourcc = 0;
    std::uint64_t source_probe_modifier = 0;
    std::uint32_t source_probe_stride = 0;
    std::uint32_t source_probe_offset = 0;
    VkImage source_probe_vk_image = VK_NULL_HANDLE;
    bool submitted = false;
};

class DirectNv12Converter final {
public:
    DirectNv12Converter() noexcept = default;
    ~DirectNv12Converter();
    DirectNv12Converter(DirectNv12Converter&&) noexcept;
    DirectNv12Converter& operator=(DirectNv12Converter&&) noexcept;
    DirectNv12Converter(const DirectNv12Converter&) = delete;
    DirectNv12Converter& operator=(const DirectNv12Converter&) = delete;

    [[nodiscard]] static std::expected<DirectNv12Converter, GpuError> create(
        AVBufferRef* input_frames, AVBufferRef* output_frames,
        std::uint32_t width, std::uint32_t height, std::uint32_t max_pending);

    [[nodiscard]] std::expected<DirectFrameResources, GpuError> dispatch(
        AVFrame* input, AVFrame* output,
        const capture::CapturedFrame& captured);

    // Waits for the compute timeline signals before deleting descriptor/image
    // resources. The AVFrames remain owned by the caller and must stay alive
    // until this call returns.
    void release(DirectFrameResources& resources, AVFrame* input,
                 AVFrame* output) noexcept;

private:
    struct Impl;
    explicit DirectNv12Converter(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

struct Nv12ColorCorrectionResources {
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    VkImageView source_y_view = VK_NULL_HANDLE;
    VkImageView source_uv_view = VK_NULL_HANDLE;
    VkImageView y_view = VK_NULL_HANDLE;
    VkImageView uv_view = VK_NULL_HANDLE;
    VkImage temp_y = VK_NULL_HANDLE;
    VkImage temp_uv = VK_NULL_HANDLE;
    VkDeviceMemory temp_y_memory = VK_NULL_HANDLE;
    VkDeviceMemory temp_uv_memory = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    bool submitted = false;
};

// Swaps red/blue in a GPU-produced limited-range BT.709 NV12 frame in place,
// retaining the FFmpeg filter sink image and Vulkan storage throughout. Input
// and output frame headers must reference the same multiplanar AVVkFrame.
class Nv12ColorCorrector final {
public:
    Nv12ColorCorrector() noexcept = default;
    ~Nv12ColorCorrector();
    Nv12ColorCorrector(Nv12ColorCorrector&&) noexcept;
    Nv12ColorCorrector& operator=(Nv12ColorCorrector&&) noexcept;
    Nv12ColorCorrector(const Nv12ColorCorrector&) = delete;
    Nv12ColorCorrector& operator=(const Nv12ColorCorrector&) = delete;

    [[nodiscard]] static std::expected<Nv12ColorCorrector, GpuError> create(
        AVBufferRef* input_frames, AVBufferRef* output_frames,
        std::uint32_t width, std::uint32_t height, std::uint32_t max_pending);

    [[nodiscard]] std::expected<Nv12ColorCorrectionResources, GpuError> dispatch(
        AVFrame* input, AVFrame* output);
    void release(Nv12ColorCorrectionResources& resources, AVFrame* input,
                 AVFrame* output) noexcept;

private:
    struct Impl;
    explicit Nv12ColorCorrector(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxrec::gpu::detail
