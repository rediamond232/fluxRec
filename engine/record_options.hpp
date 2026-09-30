#pragma once

#include "audio/track_config.hpp"
#include "gpu/gpu.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fluxrec::engine {

enum class VideoRateControl {
    bitrate,
    constant_quantizer,
};

enum class VideoBackend {
    vulkan,
    nvenc,
    experimental_cuda_egl,
};

struct RecordOptions {
    std::string output_path;
    std::string output_name{"DP-1"};
    std::string preview_socket_path;
    int seconds{10};
    bool until_signal{false};
    bool verify_frame_ids{false};
    bool verify_frame_ids_source_only{false};
    bool verify_frame_ids_unrelated{false};
    std::uint32_t frame_identity_unrelated_iterations{1};
    gpu::Codec codec{gpu::Codec::h264};
    VideoBackend backend{VideoBackend::nvenc};
    bool backend_explicit{false};
    std::optional<std::string> nvenc_preset;
    gpu::ConversionMode conversion_mode{gpu::ConversionMode::two_pass_vulkan};
    bool conversion_explicit{false};
    gpu::EncoderTune encoder_tune{gpu::EncoderTune::default_tuning};
    gpu::H264Coder h264_coder{gpu::H264Coder::vlc};
    bool h264_coder_explicit{false};
    bool audio_enabled{false};
    std::vector<fluxrec::audio::AudioTrack> audio_tracks;
    VideoRateControl rate_control{VideoRateControl::bitrate};
    std::uint64_t bitrate_bps{50'000'000};
    // Engages constant-QP mode; valid for the current 8-bit path is 0..51.
    std::optional<std::uint32_t> qp;
    std::uint32_t gop_size{120};
    // NVENC only. Zero keeps the low-latency I/P-only stream.
    std::uint32_t b_frames{0};
    std::uint32_t output_width{0};
    std::uint32_t output_height{0};
    std::uint32_t max_fps{0};
    // Nominal stream metadata: preserve the display mode rate when uncapped,
    // otherwise report the requested capture ceiling. Per-frame PTS remains
    // compositor-derived in either case.
    std::uint32_t nominal_fps_num{359'979};
    std::uint32_t nominal_fps_den{1'000};
    std::size_t pool_size{48};
    bool pool_explicit{false};
    int async_depth{8};
    bool async_depth_explicit{false};
    // Requested history duration. Replay storage is duration/bitrate sized but
    // capped; saves fail with insufficient_history when the full window is not retained.
    int replay_seconds{0};
    bool replay_only{false};
};

struct RecordOptionsError {
    std::string message;
};

// Parses all arguments after argv[0]. Numeric values are decimal and must
// consume the complete argument; failures return an error and never throw for
// malformed or overflowing integers.
[[nodiscard]] std::expected<RecordOptions, RecordOptionsError> parseRecordOptions(
    std::span<const std::string_view> arguments);

// Resolve an encoder backend name used by --backend or
// FLUXREC_GPU_ENCODER_BACKEND.
[[nodiscard]] std::expected<VideoBackend, RecordOptionsError> parseVideoBackend(
    std::string_view value);

// Apply backend-specific defaults and reject options that the selected backend
// cannot honor. The direct CUDA/EGL route is deliberately opt-in and narrowly
// constrained to its measured native-size H.264 configuration.
[[nodiscard]] std::expected<void, RecordOptionsError> configureRecordOptionsForBackend(
    RecordOptions& options, VideoBackend backend);

} // namespace fluxrec::engine
