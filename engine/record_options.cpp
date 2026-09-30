#include "engine/record_options.hpp"

#include <charconv>
#include <system_error>
#include <utility>

namespace fluxrec::engine {
namespace {

template<class Integer>
std::expected<Integer, RecordOptionsError> parseInteger(
    std::string_view value, std::string_view option) {
    Integer parsed{};
    if (value.empty()) {
        return std::unexpected(RecordOptionsError{
            std::string(option) + " requires a decimal integer"});
    }
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto [position, error] = std::from_chars(begin, end, parsed, 10);
    if (error == std::errc::result_out_of_range) {
        return std::unexpected(RecordOptionsError{
            std::string(option) + " value is outside the integer range"});
    }
    if (error != std::errc{} || position != end) {
        return std::unexpected(RecordOptionsError{
            std::string(option) + " requires a decimal integer"});
    }
    return parsed;
}

template<class Integer>
std::expected<Integer, RecordOptionsError> takeInteger(
    std::span<const std::string_view> arguments, std::size_t& index,
    std::string_view option) {
    if (index + 1 >= arguments.size()) {
        return std::unexpected(RecordOptionsError{
            std::string(option) + " requires a value"});
    }
    ++index;
    return parseInteger<Integer>(arguments[index], option);
}

std::expected<void, RecordOptionsError> parseResolution(
    std::string_view value, RecordOptions& options) {
    const auto separator = value.find('x');
    if (separator == std::string_view::npos ||
        value.find('x', separator + 1) != std::string_view::npos) {
        return std::unexpected(RecordOptionsError{
            "--resolution must have the form WIDTHxHEIGHT"});
    }
    auto width = parseInteger<std::uint32_t>(value.substr(0, separator), "--resolution width");
    if (!width) return std::unexpected(width.error());
    auto height = parseInteger<std::uint32_t>(value.substr(separator + 1), "--resolution height");
    if (!height) return std::unexpected(height.error());
    if (*width < 4 || *width > 16'384 || (*width & 1U) != 0 ||
        *height < 4 || *height > 16'384 || (*height & 1U) != 0) {
        return std::unexpected(RecordOptionsError{
            "--resolution dimensions must both be even values between 4 and 16384"});
    }
    options.output_width = *width;
    options.output_height = *height;
    return {};
}

std::expected<void, RecordOptionsError> parseEncoderTune(
    std::string_view value, RecordOptions& options) {
    if (value == "default") options.encoder_tune = gpu::EncoderTune::default_tuning;
    else if (value == "ll") options.encoder_tune = gpu::EncoderTune::ll;
    else if (value == "ull") options.encoder_tune = gpu::EncoderTune::ull;
    else if (value == "hq") options.encoder_tune = gpu::EncoderTune::hq;
    else {
        return std::unexpected(RecordOptionsError{
            "--encoder-tune must be default, ll, ull, or hq"});
    }
    return {};
}

std::expected<void, RecordOptionsError> parseH264Coder(
    std::string_view value, RecordOptions& options) {
    if (value == "cabac") options.h264_coder = gpu::H264Coder::cabac;
    else if (value == "vlc") options.h264_coder = gpu::H264Coder::vlc;
    else {
        return std::unexpected(RecordOptionsError{
            "--h264-coder must be cabac or vlc"});
    }
    return {};
}

std::expected<std::uint32_t, RecordOptionsError> parseQp(
    std::span<const std::string_view> arguments, std::size_t& index) {
    auto qp = takeInteger<std::uint32_t>(arguments, index, "--qp");
    if (!qp) return std::unexpected(qp.error());
    if (*qp > 51) {
        return std::unexpected(RecordOptionsError{
            "--qp must be between 0 and 51 for the current 8-bit video path"});
    }
    return *qp;
}

} // namespace

std::expected<VideoBackend, RecordOptionsError> parseVideoBackend(
    std::string_view value) {
    if (value == "vulkan") return VideoBackend::vulkan;
    if (value == "nvenc") return VideoBackend::nvenc;
    if (value == "experimental-cuda-egl") return VideoBackend::experimental_cuda_egl;
    return std::unexpected(RecordOptionsError{
        "must be vulkan, nvenc, or experimental-cuda-egl"});
}

std::expected<void, RecordOptionsError> configureRecordOptionsForBackend(
    RecordOptions& options, VideoBackend backend) {
    options.backend = backend;
    if (backend != VideoBackend::experimental_cuda_egl) return {};

    if (!options.pool_explicit) options.pool_size = 16;
    if (!options.async_depth_explicit) options.async_depth = 12;

    if (options.codec != gpu::Codec::h264 && options.codec != gpu::Codec::hevc) {
        return std::unexpected(RecordOptionsError{
            "--backend experimental-cuda-egl supports H.264 and HEVC"});
    }
    if (options.output_width != 0U && options.output_width != 2560U) {
        return std::unexpected(RecordOptionsError{
            "--backend experimental-cuda-egl requires native 2560x1440 output; "
            "omit --resolution or use --resolution 2560x1440"});
    }
    if (options.output_height != 0U && options.output_height != 1440U) {
        return std::unexpected(RecordOptionsError{
            "--backend experimental-cuda-egl requires native 2560x1440 output; "
            "omit --resolution or use --resolution 2560x1440"});
    }
    if (options.b_frames != 0U) {
        return std::unexpected(RecordOptionsError{
            "--backend experimental-cuda-egl does not support B-frames; use --b-frames 0"});
    }
    if (options.pool_size < 3U || options.pool_size > 16U) {
        return std::unexpected(RecordOptionsError{
            "--backend experimental-cuda-egl requires --pool between 3 and 16"});
    }
    if (options.async_depth < 1 || options.async_depth > 16) {
        return std::unexpected(RecordOptionsError{
            "--backend experimental-cuda-egl requires --async-depth between 1 and 16"});
    }
    if (options.conversion_explicit) {
        return std::unexpected(RecordOptionsError{
            "--conversion is not supported with --backend experimental-cuda-egl; "
            "this backend imports native-size frames without a conversion pass"});
    }
    if (options.verify_frame_ids) {
        return std::unexpected(RecordOptionsError{
            "frame-ID verification is not supported with --backend experimental-cuda-egl"});
    }
    if (!options.preview_socket_path.empty()) {
        return std::unexpected(RecordOptionsError{
            "--preview-socket is not supported with --backend experimental-cuda-egl"});
    }
    if (options.encoder_tune != gpu::EncoderTune::default_tuning &&
        options.encoder_tune != gpu::EncoderTune::ll) {
        return std::unexpected(RecordOptionsError{
            "--backend experimental-cuda-egl supports encoder tune default or ll only"});
    }
    if (options.h264_coder != gpu::H264Coder::vlc) {
        return std::unexpected(RecordOptionsError{
            "--backend experimental-cuda-egl supports H.264 CAVLC only; use --h264-coder vlc"});
    }
    if (options.nvenc_preset && *options.nvenc_preset != "p1") {
        return std::unexpected(RecordOptionsError{
            "--backend experimental-cuda-egl uses fixed NVENC preset p1"});
    }
    return {};
}

std::expected<RecordOptions, RecordOptionsError> parseRecordOptions(
    std::span<const std::string_view> arguments) {
    RecordOptions options;
    bool bitrate_was_explicit = false;
    bool audio_tracks_default = false;
    bool audio_track_explicit = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        if (argument == "--output" || argument == "--display") {
            if (index + 1 >= arguments.size()) {
                return std::unexpected(RecordOptionsError{
                    std::string(argument) + " requires a value"});
            }
            const std::string value{arguments[++index]};
            if (argument == "--output") options.output_path = value;
            else options.output_name = value;
        } else if (argument == "--preview-socket") {
            if (index + 1 >= arguments.size()) {
                return std::unexpected(RecordOptionsError{
                    "--preview-socket requires a Unix socket path"});
            }
            const auto path = arguments[++index];
            if (path.empty()) {
                return std::unexpected(RecordOptionsError{
                    "--preview-socket path must not be empty"});
            }
            options.preview_socket_path = path;
        } else if (argument == "--seconds") {
            auto value = takeInteger<int>(arguments, index, argument);
            if (!value) return std::unexpected(value.error());
            options.seconds = *value;
        } else if (argument == "--until-signal") {
            options.until_signal = true;
        } else if (argument == "--verify-frame-ids") {
            options.verify_frame_ids = true;
        } else if (argument == "--verify-frame-ids-source-only") {
            options.verify_frame_ids = true;
            options.verify_frame_ids_source_only = true;
            options.verify_frame_ids_unrelated = false;
        } else if (argument == "--verify-frame-ids-unrelated") {
            auto iterations = takeInteger<std::uint32_t>(arguments, index, argument);
            if (!iterations) return std::unexpected(iterations.error());
            if (*iterations < 1 || *iterations > 64) {
                return std::unexpected(RecordOptionsError{
                    "--verify-frame-ids-unrelated must be between 1 and 64"});
            }
            options.verify_frame_ids = true;
            options.verify_frame_ids_source_only = false;
            options.verify_frame_ids_unrelated = true;
            options.frame_identity_unrelated_iterations = *iterations;
        } else if (argument == "--audio") {
            if (index + 1 >= arguments.size()) {
                return std::unexpected(RecordOptionsError{"--audio requires on or off"});
            }
            const auto value = arguments[++index];
            if (value == "on") options.audio_enabled = true;
            else if (value == "off") {
                options.audio_enabled = false;
                options.audio_tracks.clear();
                audio_tracks_default = false;
                audio_track_explicit = false;
            } else return std::unexpected(RecordOptionsError{"--audio must be on or off"});
        } else if (argument == "--audio-tracks") {
            if (index + 1 >= arguments.size()) {
                return std::unexpected(RecordOptionsError{
                    "--audio-tracks requires default"});
            }
            const auto value = arguments[++index];
            if (value != "default") {
                return std::unexpected(RecordOptionsError{
                    "--audio-tracks must be default"});
            }
            if (audio_track_explicit) {
                return std::unexpected(RecordOptionsError{
                    "--audio-tracks default cannot be combined with --audio-track"});
            }
            audio_tracks_default = true;
            options.audio_tracks = fluxrec::audio::defaultAudioTracks();
        } else if (argument == "--audio-track") {
            if (audio_tracks_default) {
                return std::unexpected(RecordOptionsError{
                    "--audio-tracks default cannot be combined with --audio-track"});
            }
            if (index + 5 >= arguments.size()) {
                return std::unexpected(RecordOptionsError{
                    "--audio-track requires id, name, kind, input target, and monitor target"});
            }
            fluxrec::audio::AudioTrack track;
            track.id = std::string{arguments[++index]};
            track.name = std::string{arguments[++index]};
            const auto kind = arguments[++index];
            if (kind == "source") {
                track.input_kind = fluxrec::audio::InputKind::source;
            } else if (kind == "sink-monitor") {
                track.input_kind = fluxrec::audio::InputKind::sink_monitor;
            } else {
                return std::unexpected(RecordOptionsError{
                    "--audio-track kind must be source or sink-monitor"});
            }
            const auto input_target = arguments[++index];
            const auto monitor_target = arguments[++index];
            track.input_target = input_target == "-" ? std::string{} : std::string{input_target};
            track.monitor_target =
                monitor_target == "-" ? std::string{} : std::string{monitor_target};
            audio_track_explicit = true;
            options.audio_tracks.push_back(std::move(track));
        } else if (argument == "--codec") {
            if (index + 1 >= arguments.size()) {
                return std::unexpected(RecordOptionsError{"--codec requires h264 or hevc"});
            }
            const auto value = arguments[++index];
            if (value == "h264") options.codec = gpu::Codec::h264;
            else if (value == "hevc") options.codec = gpu::Codec::hevc;
            else return std::unexpected(RecordOptionsError{"--codec must be h264 or hevc"});
        } else if (argument == "--backend") {
            if (index + 1 >= arguments.size()) {
                return std::unexpected(RecordOptionsError{
                    "--backend requires vulkan, nvenc, or experimental-cuda-egl"});
            }
            auto backend = parseVideoBackend(arguments[++index]);
            if (!backend) return std::unexpected(RecordOptionsError{
                "--backend " + backend.error().message});
            options.backend = *backend;
            options.backend_explicit = true;
        } else if (argument == "--nvenc-preset") {
            if (index + 1 >= arguments.size()) {
                return std::unexpected(RecordOptionsError{
                    "--nvenc-preset requires p1 through p7"});
            }
            const auto value = arguments[++index];
            if (value.size() != 2 || value[0] != 'p' ||
                value[1] < '1' || value[1] > '7') {
                return std::unexpected(RecordOptionsError{
                    "--nvenc-preset must be p1 through p7"});
            }
            options.nvenc_preset = std::string(value);
        } else if (argument == "--conversion") {
            if (index + 1 >= arguments.size()) {
                return std::unexpected(RecordOptionsError{
                    "--conversion requires auto, direct, or two-pass"});
            }
            const auto value = arguments[++index];
            if (value == "auto") options.conversion_mode = gpu::ConversionMode::automatic;
            else if (value == "direct") options.conversion_mode = gpu::ConversionMode::direct_compute;
            else if (value == "two-pass") options.conversion_mode = gpu::ConversionMode::two_pass_vulkan;
            else return std::unexpected(RecordOptionsError{
                "--conversion must be auto, direct, or two-pass"});
            options.conversion_explicit = true;
        } else if (argument == "--encoder-tune") {
            if (index + 1 >= arguments.size()) {
                return std::unexpected(RecordOptionsError{
                    "--encoder-tune requires default, ll, ull, or hq"});
            }
            auto parsed = parseEncoderTune(arguments[++index], options);
            if (!parsed) return std::unexpected(parsed.error());
        } else if (argument == "--h264-coder") {
            if (index + 1 >= arguments.size()) {
                return std::unexpected(RecordOptionsError{
                    "--h264-coder requires cabac or vlc"});
            }
            auto parsed = parseH264Coder(arguments[++index], options);
            if (!parsed) return std::unexpected(parsed.error());
            options.h264_coder_explicit = true;
        } else if (argument == "--qp") {
            auto qp = parseQp(arguments, index);
            if (!qp) return std::unexpected(qp.error());
            options.qp = *qp;
            options.rate_control = VideoRateControl::constant_quantizer;
        } else if (argument == "--bitrate-bps") {
            auto bitrate = takeInteger<std::uint64_t>(arguments, index, argument);
            if (!bitrate) return std::unexpected(bitrate.error());
            if (*bitrate < 1'000'000 || *bitrate > 500'000'000) {
                return std::unexpected(RecordOptionsError{
                    "--bitrate-bps must be between 1000000 and 500000000"});
            }
            options.bitrate_bps = *bitrate;
            bitrate_was_explicit = true;
        } else if (argument == "--bitrate-mbps") {
            auto megabits = takeInteger<std::uint32_t>(arguments, index, argument);
            if (!megabits) return std::unexpected(megabits.error());
            if (*megabits < 1 || *megabits > 500) {
                return std::unexpected(RecordOptionsError{
                    "--bitrate-mbps must be between 1 and 500"});
            }
            options.bitrate_bps = static_cast<std::uint64_t>(*megabits) * 1'000'000ULL;
            bitrate_was_explicit = true;
        } else if (argument == "--gop") {
            auto size = takeInteger<std::uint32_t>(arguments, index, argument);
            if (!size) return std::unexpected(size.error());
            if (*size < 1 || *size > 3'600) {
                return std::unexpected(RecordOptionsError{"--gop must be between 1 and 3600"});
            }
            options.gop_size = *size;
        } else if (argument == "--b-frames") {
            auto count = takeInteger<std::uint32_t>(arguments, index, argument);
            if (!count) return std::unexpected(count.error());
            if (*count > 4) {
                return std::unexpected(RecordOptionsError{"--b-frames must be between 0 and 4"});
            }
            options.b_frames = *count;
        } else if (argument == "--resolution") {
            if (index + 1 >= arguments.size()) {
                return std::unexpected(RecordOptionsError{
                    "--resolution requires WIDTHxHEIGHT"});
            }
            auto result = parseResolution(arguments[++index], options);
            if (!result) return std::unexpected(result.error());
        } else if (argument == "--max-fps") {
            auto rate = takeInteger<std::uint32_t>(arguments, index, argument);
            if (!rate) return std::unexpected(rate.error());
            if (*rate != 0 && *rate != 60 && *rate != 120 && *rate != 240 && *rate != 360) {
                return std::unexpected(RecordOptionsError{
                    "--max-fps must be 0 (uncapped), 60, 120, 240, or 360"});
            }
            options.max_fps = *rate;
        } else if (argument == "--pool") {
            auto size = takeInteger<std::uint32_t>(arguments, index, argument);
            if (!size) return std::unexpected(size.error());
            if (*size < 2 || *size > 64) {
                return std::unexpected(RecordOptionsError{"--pool must be between 2 and 64"});
            }
            options.pool_size = *size;
            options.pool_explicit = true;
        } else if (argument == "--async-depth") {
            auto depth = takeInteger<int>(arguments, index, argument);
            if (!depth) return std::unexpected(depth.error());
            if (*depth < 1 || *depth > 64) {
                return std::unexpected(RecordOptionsError{
                    "--async-depth must be between 1 and 64"});
            }
            options.async_depth = *depth;
            options.async_depth_explicit = true;
        } else if (argument == "--replay-seconds") {
            auto duration = takeInteger<int>(arguments, index, argument);
            if (!duration) return std::unexpected(duration.error());
            if (*duration < 0 || *duration > 3600) {
                return std::unexpected(RecordOptionsError{
                    "--replay-seconds must be between 0 and 3600"});
            }
            options.replay_seconds = *duration;
        } else if (argument == "--replay-only") {
            options.replay_only = true;
        } else {
            return std::unexpected(RecordOptionsError{
                "unknown option or unexpected value: " + std::string(argument)});
        }
    }

    if (options.seconds < 1 || options.seconds > 3600) {
        return std::unexpected(RecordOptionsError{"--seconds must be between 1 and 3600"});
    }
    if (options.qp && bitrate_was_explicit) {
        return std::unexpected(RecordOptionsError{
            "--qp selects constant-QP rate control and cannot be combined with "
            "--bitrate-bps or --bitrate-mbps; omit bitrate to use QP mode"});
    }
    if (options.max_fps != 0) {
        options.nominal_fps_num = options.max_fps;
        options.nominal_fps_den = 1;
    }
    if (options.codec == gpu::Codec::hevc && options.h264_coder_explicit) {
        return std::unexpected(RecordOptionsError{
            "--h264-coder only applies to --codec h264"});
    }
    if (options.b_frames > 0 && options.gop_size <= options.b_frames + 1) {
        return std::unexpected(RecordOptionsError{
            "--gop must be at least --b-frames + 2 when B-frames are enabled"});
    }
    if (options.b_frames > 0 && options.backend == VideoBackend::vulkan) {
        return std::unexpected(RecordOptionsError{
            "--b-frames is currently supported only with --backend nvenc"});
    }
    if (options.backend == VideoBackend::vulkan && options.nvenc_preset) {
        return std::unexpected(RecordOptionsError{
            "--nvenc-preset requires the NVENC backend"});
    }
    if (options.backend_explicit &&
        options.backend == VideoBackend::experimental_cuda_egl) {
        auto configured = configureRecordOptionsForBackend(options, options.backend);
        if (!configured) return std::unexpected(configured.error());
    }
    if ((!options.replay_only && options.output_path.empty()) ||
        (options.replay_only &&
         (!options.output_path.empty() || options.replay_seconds <= 0))) {
        return std::unexpected(RecordOptionsError{
            "--replay-only requires --replay-seconds > 0 and no --output"});
    }
    if (!options.audio_tracks.empty()) {
        auto validated = fluxrec::audio::validateAudioTracks(std::move(options.audio_tracks));
        if (!validated) {
            return std::unexpected(RecordOptionsError{std::move(validated.error())});
        }
        options.audio_tracks = std::move(*validated);
    }
    if (!options.audio_enabled) {
        options.audio_tracks.clear();
    }
    return options;
}

} // namespace fluxrec::engine
