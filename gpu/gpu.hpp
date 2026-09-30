#pragma once

#include "capture/capture.hpp"
#include "gpu/frame_identity.hpp"
#include "mux/packet.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fluxrec::gpu {

using DeviceTimingCallback = void (*)(void* opaque, const char* stage,
                                      std::int64_t frame_pts,
                                      std::uint64_t elapsed_ns);
using NvencApiTimingCallback = void (*)(void* opaque, const char* stage,
                                        std::int64_t frame_pts,
                                        std::uint64_t elapsed_ns,
                                        std::uint64_t event_monotonic_ns);

struct GpuError {
    std::string message;
    int code = 0;
};

enum class Codec {
    h264,
    hevc,
};

enum class EncoderTune {
    default_tuning,
    ll,
    ull,
    hq,
};

enum class H264Coder {
    cabac,
    vlc,
};

enum class ConversionMode {
    automatic,
    direct_compute,
    two_pass_vulkan,
};

struct EncoderConfig {
    Codec codec = Codec::h264;
    EncoderTune encoder_tune = EncoderTune::default_tuning;
    H264Coder h264_coder = H264Coder::vlc;
    bool h264_coder_explicit = false;
    ConversionMode conversion_mode = ConversionMode::automatic;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint32_t fps_num = 360;
    std::uint32_t fps_den = 1;
    // Engaged selects constant QP instead of bitrate control: Vulkan
    // `rc_mode=cqp`, FFmpeg NVENC `rc=constqp`, or direct NVENC CONSTQP.
    // QP is bounded for the current 8-bit path; the selected codec, profile,
    // and driver must still accept it at initialization.
    std::optional<std::uint32_t> qp;
    std::int64_t bitrate = 50'000'000;
    std::uint32_t gop_size = 120;
    // NVENC only. Zero disables frame reordering and low-latency flags remain active.
    std::uint32_t b_frames = 0;
    int async_depth = 8;
    std::size_t capture_pool_size = 12;
    // Computes an asynchronous full-frame digest of each imported RGB capture.
    // This adds a GPU read pass and is intended for diagnostic runs only.
    bool verify_frame_ids = false;
    FrameIdentityMode frame_identity_mode = FrameIdentityMode::digest;
    std::uint32_t frame_identity_unrelated_iterations = 1;
    // Optional callback for completed device-side conversion timings. The
    // callback and opaque pointer must outlive this encoder. It is registered
    // only when FLUXREC_FF_VK_DEVICE_TIMING=1 is enabled.
    DeviceTimingCallback device_timing_callback = nullptr;
    void* device_timing_opaque = nullptr;
    // Optional NVENC API wall spans and return timestamps. The paired
    // submit-to-bitstream value is a host-observed availability proxy, not a
    // GPU/NVENC hardware execution timestamp.
    NvencApiTimingCallback nvenc_api_timing_callback = nullptr;
    void* nvenc_api_timing_opaque = nullptr;
    // Optional Unix seqpacket socket for the GPU-only 960x540 preview bridge.
    // An empty path leaves preview disabled.
    std::string preview_socket_path;
};

struct PreviewStats {
    bool requested = false;
    bool enabled = false;
    std::uint64_t frames_submitted = 0;
    std::uint64_t frames_rate_limited = 0;
    std::uint64_t frames_no_free_slot = 0;
    std::uint64_t frames_source_unavailable = 0;
    std::uint64_t frames_transport_dropped = 0;
    std::uint64_t frames_published = 0;
    std::string last_error;
};

struct FrameIdentityPoll {
    struct Sample {
        std::optional<std::int64_t> pts;
        FrameIdentityDigest digest;
    };
    std::vector<Sample> completed;
    // Cumulative captures skipped because the bounded digest ring was full.
    std::uint64_t skipped_frames = 0;
};

struct GpuInfo {
    std::string device_name;
    std::uint32_t capture_fourcc = 0;
    std::uint64_t capture_modifier = 0;
    std::uint32_t capture_plane_count = 0;
    bool drm_prime_import_proven = false;
    bool cpu_pixel_fallback_enabled = false;
    std::string conversion_path;
};

struct EncoderStats {
    std::uint64_t submitted_frames = 0;
    std::uint64_t encoded_frames = 0;
    std::size_t pending_capture_leases = 0;
    // CPU-side elapsed time around calls made by this process. Import, filter,
    // send, and receive values are sums for the most recent submit/drain call
    // and cumulative sums since encoder creation. They can include driver
    // overhead or waiting, and are not GPU execution timestamps.
    std::uint64_t last_dmabuf_import_cpu_ns = 0;
    std::uint64_t total_dmabuf_import_cpu_ns = 0;
    std::uint64_t last_cuda_transfer_cpu_ns = 0;
    std::uint64_t total_cuda_transfer_cpu_ns = 0;
    std::uint64_t last_filter_submit_cpu_ns = 0;
    std::uint64_t total_filter_submit_cpu_ns = 0;
    std::uint64_t last_filter_pull_cpu_ns = 0;
    std::uint64_t total_filter_pull_cpu_ns = 0;
    std::uint64_t last_encoder_send_cpu_ns = 0;
    std::uint64_t total_encoder_send_cpu_ns = 0;
    std::uint64_t last_encoder_receive_cpu_ns = 0;
    std::uint64_t total_encoder_receive_cpu_ns = 0;
    // CPU steady-clock wall latency from GpuEncoder::submit entry until the
    // matching packet is observed. This is end-to-end host-observed latency,
    // not GPU execution time. Totals/count/max cover matched packets.
    std::uint64_t last_submit_to_packet_wall_ns = 0;
    std::uint64_t total_submit_to_packet_wall_ns = 0;
    std::uint64_t submit_to_packet_samples = 0;
    std::uint64_t max_submit_to_packet_wall_ns = 0;

    // Legacy direct-path call durations, also measured on the CPU.
    std::uint64_t last_conversion_submit_ns = 0;
    std::uint64_t total_conversion_submit_ns = 0;
    std::uint64_t last_encoder_submit_ns = 0;
    std::uint64_t total_encoder_submit_ns = 0;
};

// Owns an exact-modifier GBM pool, maps each captured DMA-BUF directly into
// FFmpeg's Vulkan hardware-frame context, and converts on the GPU. The recorder
// CLI selects NVENC by default; this lower-level class still selects Vulkan
// Video when no backend is configured. NVENC transfers the converted Vulkan
// NV12 images into a separate CUDA pool without copying raw pixels through
// host memory.
class GpuEncoder final {
public:
    GpuEncoder() noexcept = default;
    ~GpuEncoder();

    GpuEncoder(GpuEncoder&&) noexcept;
    GpuEncoder& operator=(GpuEncoder&&) noexcept;

    GpuEncoder(const GpuEncoder&) = delete;
    GpuEncoder& operator=(const GpuEncoder&) = delete;

    [[nodiscard]] static std::expected<GpuEncoder, GpuError> create(
        const capture::CaptureRequirements& requirements,
        EncoderConfig config = {});

    // Pass these descriptors directly to WaylandCapture::set_buffer_pool().
    // The returned fds stay open for the lifetime of this encoder.
    [[nodiscard]] std::span<const capture::DmabufImage> capture_buffers() const noexcept;

    // Submission keeps the capture lease alive until the matching encoded
    // packet is returned by FFmpeg. Only compressed packet bytes are copied to
    // host memory.
    [[nodiscard]] std::expected<std::vector<media::EncodedPacket>, GpuError> submit(
        capture::CapturedFrame&& frame);
    [[nodiscard]] std::expected<std::vector<media::EncodedPacket>, GpuError> drain();
    // Collects completed 16-byte digests without waiting. The capture caller
    // can classify adjacent equal digests as duplicates and observe GPU time.
    [[nodiscard]] std::expected<FrameIdentityPoll, GpuError> poll_frame_identity();

    [[nodiscard]] const GpuInfo& info() const noexcept;
    [[nodiscard]] EncoderStats stats() const noexcept;
    [[nodiscard]] PreviewStats preview_stats() const;
    [[nodiscard]] media::Rational packet_time_base() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> codec_extradata() const noexcept;

private:
    struct Impl;
    explicit GpuEncoder(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxrec::gpu
