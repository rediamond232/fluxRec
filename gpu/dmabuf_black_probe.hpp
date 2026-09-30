#pragma once

#include "gpu/gpu.hpp"

#include <capture/capture.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

struct AVBufferRef;

namespace fluxrec::gpu::detail {

// Wait for currently published DMA-BUF writer dependencies without importing
// or sampling the image. The bound is 10 seconds; errors fail closed.
[[nodiscard]] std::expected<void, GpuError> wait_for_dma_buf_read_dependency(
    int dma_buf_fd, std::uint64_t sequence);

// A deliberately synchronous, opt-in observation path that imports the
// captured DMA-BUF directly into a second Vulkan image, counts near-black
// pixels in a compute shader, and reads back only that one aggregate count.
class DmabufBlackProbe final {
public:
    DmabufBlackProbe() noexcept = default;
    ~DmabufBlackProbe();

    DmabufBlackProbe(DmabufBlackProbe&&) noexcept;
    DmabufBlackProbe& operator=(DmabufBlackProbe&&) noexcept;
    DmabufBlackProbe(const DmabufBlackProbe&) = delete;
    DmabufBlackProbe& operator=(const DmabufBlackProbe&) = delete;

    [[nodiscard]] static std::expected<DmabufBlackProbe, GpuError> create(
        AVBufferRef* vulkan_device,
        std::span<const capture::DmabufImage> pool,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t fourcc,
        std::uint64_t modifier,
        std::string shader_path);

    [[nodiscard]] std::expected<void, GpuError> inspect(
        const capture::CapturedFrame& captured);
    void finish() noexcept;

private:
    struct Impl;
    explicit DmabufBlackProbe(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxrec::gpu::detail
