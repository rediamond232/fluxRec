#pragma once

#include "capture/capture.hpp"
#include "gpu/gpu.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fluxrec::gpu {

struct DirectCudaEglConfig {
    Codec codec = Codec::h264;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps_num = 360;
    std::uint32_t fps_den = 1;
    std::uint32_t gop_size = 120;
    std::int64_t bitrate = 50'000'000;
    // When set, NVENC initializes in CONSTQP and uses this value for I and P
    // frames. The 8-bit path accepts 0..51. Absent keeps VBR at `bitrate`.
    std::optional<std::uint32_t> qp;
    std::size_t pool_size = 0;
    int async_depth = 8;
};

// Experimental desktop route: DMA-BUF -> EGLImage -> CUDA EGL array -> NVENC
// CUDAARRAY. NVIDIA documents the CUDA EGL interop used here for Tegra only;
// desktop support is empirical and depends on the exact driver/exporter tuple.
class DirectCudaEglEncoder final {
public:
    DirectCudaEglEncoder() noexcept = default;
    ~DirectCudaEglEncoder();

    DirectCudaEglEncoder(DirectCudaEglEncoder&&) noexcept;
    DirectCudaEglEncoder& operator=(DirectCudaEglEncoder&&) noexcept;
    DirectCudaEglEncoder(const DirectCudaEglEncoder&) = delete;
    DirectCudaEglEncoder& operator=(const DirectCudaEglEncoder&) = delete;

    [[nodiscard]] static std::expected<DirectCudaEglEncoder, GpuError> create(
        std::string render_node, std::span<const capture::DmabufImage> images,
        DirectCudaEglConfig config);

    // pts is the compositor-derived GpuEncoder timestamp in microseconds.
    // The capture lease is retained through bitstream lock/unmap completion.
    [[nodiscard]] std::expected<std::vector<media::EncodedPacket>, GpuError> submit(
        capture::CapturedFrame&& frame, std::int64_t pts,
        std::chrono::steady_clock::time_point submitted_at);
    [[nodiscard]] std::expected<std::vector<media::EncodedPacket>, GpuError> drain();

    [[nodiscard]] EncoderStats stats() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> codec_extradata() const noexcept;
    [[nodiscard]] std::string cuda_device_name() const;

private:
    struct Impl;
    explicit DirectCudaEglEncoder(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxrec::gpu
