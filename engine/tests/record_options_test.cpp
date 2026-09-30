#include "engine/record_options.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using fluxrec::engine::RecordOptions;
using fluxrec::engine::parseRecordOptions;

bool expect_error(std::initializer_list<std::string_view> values,
                  std::string_view label) {
    const std::vector<std::string_view> arguments{values};
    const auto result = parseRecordOptions(arguments);
    if (!result) return true;
    std::cerr << "expected parser error for " << label << '\n';
    return false;
}

} // namespace

int main() {
    bool ok = true;
    const std::array<std::string_view, 2> defaults_args{"--output", "default.mkv"};
    const auto defaults = parseRecordOptions(defaults_args);
    ok &= defaults.has_value();
    if (defaults) {
        ok &= defaults->preview_socket_path.empty();
        ok &= defaults->bitrate_bps == 50'000'000;
        ok &= defaults->gop_size == 120;
        ok &= defaults->b_frames == 0;
        ok &= defaults->pool_size == 48;
        ok &= defaults->async_depth == 8;
        ok &= !defaults->async_depth_explicit;
        ok &= defaults->backend == fluxrec::engine::VideoBackend::nvenc;
        ok &= !defaults->backend_explicit;
        ok &= !defaults->nvenc_preset.has_value();
        ok &= defaults->output_width == 0 && defaults->output_height == 0;
        ok &= defaults->max_fps == 0;
        ok &= defaults->nominal_fps_num == 359'979 && defaults->nominal_fps_den == 1'000;
        ok &= defaults->encoder_tune == fluxrec::gpu::EncoderTune::default_tuning;
        ok &= defaults->h264_coder == fluxrec::gpu::H264Coder::vlc;
        ok &= defaults->rate_control == fluxrec::engine::VideoRateControl::bitrate;
        ok &= !defaults->qp.has_value();
    }

    const std::array<std::string_view, 18> configured_args{
        "--output", "custom.mkv", "--bitrate-bps", "85000000",
        "--resolution", "1920x1080", "--max-fps", "120",
        "--encoder-tune", "ull", "--h264-coder", "vlc",
        "--gop", "90", "--pool", "56", "--preview-socket",
        "/run/user/1000/fluxrec-preview.sock"};
    const auto configured = parseRecordOptions(configured_args);
    ok &= configured.has_value();
    if (configured) {
        ok &= configured->bitrate_bps == 85'000'000;
        ok &= configured->output_width == 1920 && configured->output_height == 1080;
        ok &= configured->max_fps == 120;
        ok &= configured->nominal_fps_num == 120 && configured->nominal_fps_den == 1;
        ok &= configured->encoder_tune == fluxrec::gpu::EncoderTune::ull;
        ok &= configured->h264_coder == fluxrec::gpu::H264Coder::vlc;
        ok &= configured->gop_size == 90;
        ok &= configured->pool_size == 56;
        ok &= configured->preview_socket_path == "/run/user/1000/fluxrec-preview.sock";
    }

    const std::array<std::string_view, 10> nvenc_args{
        "--output", "nvenc.mkv", "--backend", "nvenc",
        "--nvenc-preset", "p4", "--async-depth", "4", "--b-frames", "2"};
    const auto nvenc = parseRecordOptions(nvenc_args);
    ok &= nvenc.has_value();
    if (nvenc) {
        ok &= nvenc->backend == fluxrec::engine::VideoBackend::nvenc;
        ok &= nvenc->backend_explicit;
        ok &= nvenc->nvenc_preset == "p4";
        ok &= nvenc->async_depth == 4 && nvenc->async_depth_explicit;
        ok &= nvenc->b_frames == 2;
    }

    const std::array<std::string_view, 4> direct_backend_args{
        "--output", "direct.mkv", "--backend", "experimental-cuda-egl"};
    const auto direct_backend = parseRecordOptions(direct_backend_args);
    ok &= direct_backend.has_value();
    if (direct_backend) {
        ok &= direct_backend->backend ==
              fluxrec::engine::VideoBackend::experimental_cuda_egl;
        ok &= direct_backend->backend_explicit;
        ok &= direct_backend->pool_size == 16 && !direct_backend->pool_explicit;
        ok &= direct_backend->async_depth == 12 && !direct_backend->async_depth_explicit;
        ok &= direct_backend->codec == fluxrec::gpu::Codec::h264;
        ok &= direct_backend->b_frames == 0;
    }

    // Exercise the same backend-default resolver used for an environment-selected
    // backend. Each explicit queue setting is preserved independently.
    const auto parsed_default_for_env = parseRecordOptions(defaults_args);
    ok &= parsed_default_for_env.has_value();
    if (parsed_default_for_env) {
        auto env_selected = *parsed_default_for_env;
        const auto selected = fluxrec::engine::parseVideoBackend("experimental-cuda-egl");
        ok &= selected.has_value();
        if (selected) {
            const auto configured_env =
                fluxrec::engine::configureRecordOptionsForBackend(env_selected, *selected);
            ok &= configured_env.has_value();
            ok &= env_selected.backend ==
                  fluxrec::engine::VideoBackend::experimental_cuda_egl;
            ok &= env_selected.pool_size == 16 && env_selected.async_depth == 12;
        }
    }
    const std::array<std::string_view, 8> direct_partial_explicit_args{
        "--output", "direct.mkv", "--backend", "experimental-cuda-egl",
        "--pool", "12", "--async-depth", "9"};
    const auto direct_partial_explicit = parseRecordOptions(direct_partial_explicit_args);
    ok &= direct_partial_explicit.has_value();
    if (direct_partial_explicit) {
        ok &= direct_partial_explicit->pool_size == 12;
        ok &= direct_partial_explicit->pool_explicit;
        ok &= direct_partial_explicit->async_depth == 9;
        ok &= direct_partial_explicit->async_depth_explicit;
    }
    const std::array<std::string_view, 6> direct_pool_only_args{
        "--output", "direct.mkv", "--backend", "experimental-cuda-egl",
        "--pool", "8"};
    const auto direct_pool_only = parseRecordOptions(direct_pool_only_args);
    ok &= direct_pool_only.has_value();
    if (direct_pool_only) {
        ok &= direct_pool_only->pool_size == 8;
        ok &= direct_pool_only->async_depth == 12;
    }
    const std::array<std::string_view, 6> direct_depth_only_args{
        "--output", "direct.mkv", "--backend", "experimental-cuda-egl",
        "--async-depth", "10"};
    const auto direct_depth_only = parseRecordOptions(direct_depth_only_args);
    ok &= direct_depth_only.has_value();
    if (direct_depth_only) {
        ok &= direct_depth_only->pool_size == 16;
        ok &= direct_depth_only->async_depth == 10;
    }

    const std::array<std::string_view, 4> vulkan_backend_args{
        "--output", "vulkan.mkv", "--backend", "vulkan"};
    const auto vulkan_backend = parseRecordOptions(vulkan_backend_args);
    ok &= vulkan_backend.has_value();
    if (vulkan_backend) {
        ok &= vulkan_backend->backend == fluxrec::engine::VideoBackend::vulkan;
        ok &= vulkan_backend->backend_explicit;
    }

    const std::array<std::string_view, 4> cqp_args{
        "--output", "cqp.mkv", "--qp", "28"};
    const auto cqp = parseRecordOptions(cqp_args);
    ok &= cqp.has_value();
    if (cqp) {
        ok &= cqp->rate_control == fluxrec::engine::VideoRateControl::constant_quantizer;
        ok &= cqp->qp == 28U;
        // The default bitrate value is retained as an inactive setting; the
        // explicit rate-control mode determines what the backend must use.
        ok &= cqp->bitrate_bps == 50'000'000;
    }
    const std::array<std::string_view, 4> minimum_cqp_args{
        "--output", "cqp.mkv", "--qp", "0"};
    const auto minimum_cqp = parseRecordOptions(minimum_cqp_args);
    ok &= minimum_cqp.has_value();
    if (minimum_cqp) ok &= minimum_cqp->qp == 0U;

    ok &= expect_error({"--output", "x.mkv", "--seconds", "999999999999999999999"},
                       "overflowing --seconds");
    ok &= expect_error({"--output", "x.mkv", "--pool", "65"}, "out-of-range --pool");
    ok &= expect_error({"--output", "x.mkv", "--async-depth", "999999999999999999999"},
                       "overflowing --async-depth");
    ok &= expect_error({"--output", "x.mkv", "--gop", "0"}, "zero --gop");
    ok &= expect_error({"--output", "x.mkv", "--bitrate-bps", "999999999999999999999"},
                       "overflowing --bitrate-bps");
    ok &= expect_error({"--output", "x.mkv", "--bitrate-mbps", "501"},
                       "out-of-range --bitrate-mbps");
    ok &= expect_error({"--output", "x.mkv", "--qp", "52"}, "QP above supported range");
    ok &= expect_error({"--output", "x.mkv", "--qp", "-1"}, "negative QP sentinel");
    ok &= expect_error({"--output", "x.mkv", "--qp", "999999999999999999999"},
                       "overflowing QP");
    ok &= expect_error({"--output", "x.mkv", "--qp"}, "missing QP");
    ok &= expect_error({"--output", "x.mkv", "--qp", "28", "--bitrate-bps", "85000000"},
                       "CQP with explicit bit rate");
    ok &= expect_error({"--output", "x.mkv", "--bitrate-mbps", "50", "--qp", "28"},
                       "CQP with explicit Mbps bit rate");
    ok &= expect_error({"--output", "x.mkv", "--resolution", "1921x1080"},
                       "odd --resolution");
    ok &= expect_error({"--output", "x.mkv", "--max-fps", "30"}, "unsupported --max-fps");
    ok &= expect_error({"--output", "x.mkv", "--replay-seconds", "-1"},
                       "negative --replay-seconds");
    ok &= expect_error({"--output", "x.mkv", "--verify-frame-ids-unrelated", "bad"},
                       "malformed identity iteration count");
    ok &= expect_error({"--output", "x.mkv", "--encoder-tune", "p7"},
                       "unsupported tune name");
    ok &= expect_error({"--output", "x.mkv", "--encoder-tune"}, "missing tune value");
    ok &= expect_error({"--output", "x.mkv", "--backend", "cuda"},
                       "unsupported backend");
    ok &= expect_error({"--output", "x.mkv", "--backend"},
                       "missing backend");
    ok &= expect_error({"--output", "x.mkv", "--b-frames"},
                       "missing B-frame count");
    ok &= expect_error({"--output", "x.mkv", "--b-frames", "5"},
                       "B-frame count above supported range");
    ok &= expect_error({"--output", "x.mkv", "--b-frames", "-1"},
                       "negative B-frame count");
    ok &= expect_error({"--output", "x.mkv", "--backend", "vulkan",
                        "--b-frames", "1"},
                       "B-frames on Vulkan backend");
    ok &= expect_error({"--output", "x.mkv", "--gop", "5",
                        "--b-frames", "4"},
                       "GOP too short for B-frame count");
    ok &= expect_error({"--output", "x.mkv", "--nvenc-preset", "p8"},
                       "unsupported NVENC preset");
    ok &= expect_error({"--output", "x.mkv", "--nvenc-preset"},
                       "missing NVENC preset");
    ok &= expect_error({"--output", "x.mkv", "--backend", "vulkan",
                        "--nvenc-preset", "p4"},
                       "NVENC preset on Vulkan backend");
    const std::array<std::string_view, 6> direct_hevc_args{
        "--output", "direct-hevc.mkv", "--backend", "experimental-cuda-egl",
        "--codec", "hevc"};
    const auto direct_hevc = parseRecordOptions(direct_hevc_args);
    ok &= direct_hevc.has_value();
    if (direct_hevc) {
        ok &= direct_hevc->codec == fluxrec::gpu::Codec::hevc;
        ok &= direct_hevc->b_frames == 0;
        ok &= direct_hevc->pool_size == 16;
        ok &= direct_hevc->async_depth == 12;
    }
    ok &= expect_error({"--output", "x.mkv", "--backend", "experimental-cuda-egl",
                        "--codec", "hevc", "--b-frames", "1"},
                       "B-frames on experimental CUDA/EGL HEVC");
    ok &= expect_error({"--output", "x.mkv", "--backend", "experimental-cuda-egl",
                        "--b-frames", "1"},
                       "B-frames on experimental CUDA/EGL backend");
    const std::array<std::string_view, 6> direct_cqp_args{
        "--output", "direct-cqp.mkv", "--backend", "experimental-cuda-egl",
        "--qp", "28"};
    const auto direct_cqp = parseRecordOptions(direct_cqp_args);
    ok &= direct_cqp.has_value();
    if (direct_cqp) {
        ok &= direct_cqp->backend == fluxrec::engine::VideoBackend::experimental_cuda_egl;
        ok &= direct_cqp->rate_control ==
            fluxrec::engine::VideoRateControl::constant_quantizer;
        ok &= direct_cqp->qp == 28U;
        ok &= direct_cqp->bitrate_bps == 50'000'000;
    }
    const std::array<std::string_view, 8> direct_hevc_cqp_args{
        "--output", "direct-hevc-cqp.mkv", "--backend", "experimental-cuda-egl",
        "--codec", "hevc", "--qp", "0"};
    const auto direct_hevc_cqp = parseRecordOptions(direct_hevc_cqp_args);
    ok &= direct_hevc_cqp.has_value();
    if (direct_hevc_cqp) {
        ok &= direct_hevc_cqp->codec == fluxrec::gpu::Codec::hevc;
        ok &= direct_hevc_cqp->qp == 0U;
    }
    ok &= expect_error({"--output", "x.mkv", "--backend", "experimental-cuda-egl",
                        "--resolution", "1920x1080"},
                       "scaled output on experimental CUDA/EGL backend");
    ok &= expect_error({"--output", "x.mkv", "--backend", "experimental-cuda-egl",
                        "--conversion", "two-pass"},
                       "conversion option on experimental CUDA/EGL backend");
    ok &= expect_error({"--output", "x.mkv", "--backend", "experimental-cuda-egl",
                        "--pool", "2"},
                       "undersized pool on experimental CUDA/EGL backend");
    ok &= expect_error({"--output", "x.mkv", "--backend", "experimental-cuda-egl",
                        "--pool", "17"},
                       "oversized pool on experimental CUDA/EGL backend");
    ok &= expect_error({"--output", "x.mkv", "--backend", "experimental-cuda-egl",
                        "--async-depth", "17"},
                       "oversized async depth on experimental CUDA/EGL backend");
    ok &= expect_error({"--output", "x.mkv", "--backend", "experimental-cuda-egl",
                        "--encoder-tune", "hq"},
                       "HQ tune on experimental CUDA/EGL backend");
    ok &= expect_error({"--output", "x.mkv", "--backend", "experimental-cuda-egl",
                        "--h264-coder", "cabac"},
                       "CABAC on experimental CUDA/EGL backend");
    ok &= expect_error({"--output", "x.mkv", "--backend", "experimental-cuda-egl",
                        "--nvenc-preset", "p4"},
                       "unsupported preset on experimental CUDA/EGL backend");
    ok &= expect_error({"--output", "x.mkv", "--backend", "experimental-cuda-egl",
                        "--preview-socket", "/tmp/preview.sock"},
                       "preview on experimental CUDA/EGL backend");
    ok &= expect_error({"--output", "x.mkv", "--backend", "experimental-cuda-egl",
                        "--verify-frame-ids"},
                       "frame-ID verification on experimental CUDA/EGL backend");
    ok &= expect_error({"--output", "x.mkv", "--h264-coder", "cavlc"},
                       "unsupported H.264 coder name");
    ok &= expect_error({"--output", "x.mkv", "--h264-coder"},
                       "missing H.264 coder value");
    ok &= expect_error({"--output", "x.mkv", "--preview-socket"},
                       "missing preview socket path");
    ok &= expect_error({"--output", "x.mkv", "--preview-socket", ""},
                       "empty preview socket path");
    const std::array<std::string_view, 4> hevc_default_coder_args{
        "--output", "hevc.mkv", "--codec", "hevc"};
    const auto hevc_default_coder = parseRecordOptions(hevc_default_coder_args);
    ok &= hevc_default_coder.has_value();
    ok &= expect_error({"--output", "x.mkv", "--codec", "hevc",
                        "--h264-coder", "cabac"},
                       "explicit H.264 coder with HEVC");
    const std::array<std::string_view, 5> replay_audio_args{
        "--replay-only", "--replay-seconds", "10", "--audio", "on"};
    const auto replay_audio = parseRecordOptions(replay_audio_args);
    ok &= replay_audio.has_value();
    if (replay_audio) {
        ok &= replay_audio->replay_only;
        ok &= replay_audio->audio_enabled;
        ok &= replay_audio->replay_seconds == 10;
    }
    ok &= expect_error({"--replay-only", "--audio", "on"},
                       "replay-only without a replay duration");
    ok &= expect_error({"--replay-only", "--replay-seconds", "10", "--output", "x.mkv"},
                       "replay-only with an output file");

    const std::array<std::string_view, 16> track_args{
        "--output", "clip.mkv", "--audio", "on",
        "--audio-track", "desktop", "Desktop audio", "sink-monitor", "-", "-",
        "--audio-track", "mic-2", "Headset mic", "source", "alsa_input.usb", "alsa_output.speakers",
    };
    const auto tracks = parseRecordOptions(track_args);
    ok &= tracks.has_value();
    if (tracks) {
        ok &= tracks->audio_enabled;
        ok &= tracks->audio_tracks.size() == 2;
        ok &= tracks->audio_tracks[0].input_target.empty();
        ok &= tracks->audio_tracks[0].monitor_target.empty();
        ok &= tracks->audio_tracks[0].input_kind == fluxrec::audio::InputKind::sink_monitor;
        ok &= tracks->audio_tracks[1].id == "mic-2";
        ok &= tracks->audio_tracks[1].input_target == "alsa_input.usb";
        ok &= tracks->audio_tracks[1].monitor_target == "alsa_output.speakers";
    }
    const std::array<std::string_view, 6> default_tracks_args{
        "--output", "clip.mkv", "--audio", "on", "--audio-tracks", "default",
    };
    const auto default_tracks = parseRecordOptions(default_tracks_args);
    ok &= default_tracks.has_value();
    if (default_tracks) {
        ok &= default_tracks->audio_enabled;
        ok &= default_tracks->audio_tracks.size() == 2;
        ok &= default_tracks->audio_tracks[0].id == "desktop";
        ok &= default_tracks->audio_tracks[0].name == "Desktop audio";
        ok &= default_tracks->audio_tracks[0].input_kind ==
              fluxrec::audio::InputKind::sink_monitor;
        ok &= default_tracks->audio_tracks[0].input_target.empty();
        ok &= default_tracks->audio_tracks[0].monitor_target.empty();
        ok &= default_tracks->audio_tracks[1].id == "microphone";
        ok &= default_tracks->audio_tracks[1].name == "Microphone";
        ok &= default_tracks->audio_tracks[1].input_kind ==
              fluxrec::audio::InputKind::source;
        ok &= default_tracks->audio_tracks[1].input_target.empty();
        ok &= default_tracks->audio_tracks[1].monitor_target.empty();
    }
    const std::array<std::string_view, 4> audio_on_empty_args{
        "--output", "clip.mkv", "--audio", "on",
    };
    const auto audio_on_empty = parseRecordOptions(audio_on_empty_args);
    ok &= audio_on_empty.has_value();
    if (audio_on_empty) {
        ok &= audio_on_empty->audio_enabled;
        ok &= audio_on_empty->audio_tracks.empty();
    }
    ok &= expect_error(
        {"--output", "clip.mkv", "--audio", "on", "--audio-tracks", "default",
         "--audio-track", "desktop", "Desktop audio", "sink-monitor", "-", "-"},
        "default audio tracks combined with an explicit track");
    const std::array<std::string_view, 6> default_then_off_args{
        "--output", "clip.mkv", "--audio-tracks", "default", "--audio", "off",
    };
    const auto default_then_off = parseRecordOptions(default_then_off_args);
    ok &= default_then_off.has_value() && !default_then_off->audio_enabled &&
          default_then_off->audio_tracks.empty();
    const std::array<std::string_view, 10> audio_off_clears{
        "--output", "clip.mkv",
        "--audio-track", "desktop", "Desktop audio", "source", "-", "-",
        "--audio", "off",
    };
    const auto cleared = parseRecordOptions(audio_off_clears);
    ok &= cleared.has_value() && cleared->audio_tracks.empty() && !cleared->audio_enabled;
    ok &= expect_error({"--output", "clip.mkv", "--audio-track", "desktop", "Name", "source", "-"},
                       "audio track is missing its monitor target");
    ok &= expect_error({"--output", "clip.mkv", "--audio-track", "1bad", "Name", "source", "-", "-"},
                       "bad track id");
    ok &= expect_error({"--output", "clip.mkv", "--audio-track", "desk", "Name", "input", "-", "-"},
                       "unknown kind");
    std::vector<std::string_view> nine_tracks{"--output", "clip.mkv"};
    const std::array<std::string_view, 9> nine_ids{
        "desk0", "desk1", "desk2", "desk3", "desk4", "desk5", "desk6", "desk7", "desk8",
    };
    for (const auto id : nine_ids) {
        nine_tracks.insert(nine_tracks.end(),
                           {"--audio-track", id, "Name", "source", "-", "-"});
    }
    ok &= !parseRecordOptions(nine_tracks);

    if (!ok) return 1;
    std::cout << "record option parser contracts passed\n";
    return 0;
}
