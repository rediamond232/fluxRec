#include "capture/capture.hpp"
#include "engine/audio_session.hpp"
#include "engine/control_service.hpp"
#include "engine/encoder_stats_telemetry.hpp"
#include "engine/packet_router.hpp"
#include "engine/record_options.hpp"
#include "gpu/gpu.hpp"
#include "mux/mux_writer.hpp"
#include "replay/replay_buffer.hpp"
#include "telemetry/system_metrics.hpp"
#include "telemetry/telemetry.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::mutex output_mutex;

void observe_vulkan_conversion_timing(void* opaque, const char* stage,
                                      std::int64_t frame_pts,
                                      std::uint64_t elapsed_ns) noexcept {
    if (!opaque || !stage) return;
    auto& bridge = *static_cast<fluxrec::telemetry::GpuConversionTimingBridge*>(opaque);
    if (std::strcmp(stage, "gpu_rgb_to_nv12") == 0) {
        bridge.observePass(fluxrec::telemetry::GpuConversionPass::first,
                           frame_pts, elapsed_ns);
    } else if (std::strcmp(stage, "gpu_output_resize") == 0) {
        bridge.observePass(fluxrec::telemetry::GpuConversionPass::second,
                           frame_pts, elapsed_ns);
    }
}

void observe_nvenc_api_timing(void* opaque, const char* stage,
                              std::int64_t frame_pts,
                              std::uint64_t elapsed_ns,
                              std::uint64_t event_monotonic_ns) noexcept {
    if (!opaque || !stage) return;
    auto& bridge = *static_cast<fluxrec::telemetry::NvencApiTimingBridge*>(opaque);
    if (std::strcmp(stage, "nvenc_encode_picture_api_wall") == 0) {
        bridge.observe(fluxrec::telemetry::NvencApiCall::encode_picture,
                       frame_pts, elapsed_ns, event_monotonic_ns);
    } else if (std::strcmp(stage, "nvenc_lock_bitstream_api_wall") == 0) {
        bridge.observe(fluxrec::telemetry::NvencApiCall::lock_bitstream,
                       frame_pts, elapsed_ns, event_monotonic_ns);
    }
}

void write_json_string(std::ostream& stream, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    stream.put('"');
    for (const unsigned char byte : value) {
        if (byte == '"' || byte == '\\') {
            stream.put('\\');
            stream.put(static_cast<char>(byte));
        } else if (byte < 0x20U) {
            stream << "\\u00" << hex[byte >> 4U] << hex[byte & 0x0fU];
        } else {
            stream.put(static_cast<char>(byte));
        }
    }
    stream.put('"');
}

const char* replay_snapshot_status_name(fluxrec::replay::SnapshotStatus status) noexcept {
    using Status = fluxrec::replay::SnapshotStatus;
    switch (status) {
    case Status::ready: return "ready";
    case Status::invalid_duration: return "invalid_duration";
    case Status::insufficient_history: return "insufficient_history";
    case Status::no_keyframe: return "no_keyframe";
    case Status::keyframe_preroll_exceeded: return "keyframe_preroll_exceeded";
    case Status::snapshot_memory_budget: return "snapshot_memory_budget";
    case Status::snapshot_capacity: return "snapshot_capacity";
    }
    return "unknown";
}

void report_control_event(const fluxrec::engine::ControlServiceEvent& event) {
    using EventType = fluxrec::engine::ControlServiceEventType;
    using CommandType = fluxrec::engine::ControlCommandType;
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(9);
    if (event.type == EventType::stop_requested) {
        output << "{\"type\":\"stop_requested\"}";
    } else if (event.type == EventType::replay_started ||
               event.type == EventType::replay_stopped) {
        output << "{\"type\":\""
               << (event.type == EventType::replay_started ? "replay_started" : "replay_stopped")
               << "\",\"command\":\""
               << (event.command == CommandType::start_replay ? "start_replay" : "stop_replay")
               << "\",\"ok\":" << (event.ok ? "true" : "false")
               << ",\"active\":" << (event.active ? "true" : "false")
               << ",\"state_changed\":" << (event.state_changed ? "true" : "false");
        if (!event.message.empty()) {
            output << ",\"message\":";
            write_json_string(output, event.message);
        }
        output << '}';
    } else if (event.type == EventType::replay_control_failed) {
        output << "{\"type\":\"replay_control_failed\",\"command\":\""
               << (event.command == CommandType::start_replay ? "start_replay" : "stop_replay")
               << "\",\"ok\":false,\"active\":"
               << (event.active ? "true" : "false")
               << ",\"state_changed\":" << (event.state_changed ? "true" : "false");
        if (!event.message.empty()) {
            output << ",\"message\":";
            write_json_string(output, event.message);
        }
        output << '}';
    } else if (event.type == EventType::replay_queued) {
        output << "{\"type\":\"replay_queued\",\"path\":";
        write_json_string(output, event.output_path);
        output << ",\"job_id\":" << event.job_id << '}';
    } else if (event.type == EventType::replay_succeeded ||
               event.type == EventType::replay_failed) {
        output << "{\"type\":\"replay_saved\",\"path\":";
        write_json_string(output, event.output_path);
        output << ",\"ok\":"
                  << (event.type == EventType::replay_succeeded ? "true" : "false")
                  << ",\"job_id\":" << event.job_id
                  << ",\"packets\":" << event.snapshot_packets
                  << ",\"bytes\":" << event.output_file_bytes;
        if (event.snapshot_status) {
            output << ",\"snapshot_status\":\""
                   << replay_snapshot_status_name(*event.snapshot_status) << '"';
        }
        if (!event.message.empty()) {
            output << ",\"message\":";
            write_json_string(output, event.message);
        }
        output << '}';
    } else if (event.type == EventType::audio_control_changed ||
               event.type == EventType::audio_control_failed) {
        const bool ok = event.type == EventType::audio_control_changed;
        output << "{\"type\":\"" << (ok ? "audio_control_changed" : "audio_control_failed")
               << "\",\"source\":";
        write_json_string(output, event.audio_track_id);
        if (event.audio_gain) output << ",\"gain\":" << *event.audio_gain;
        if (event.audio_muted.has_value()) {
            output << ",\"muted\":" << (*event.audio_muted ? "true" : "false");
        }
        if (!ok && !event.message.empty()) {
            output << ",\"message\":";
            write_json_string(output, event.message);
        }
        output << '}';
    }
    std::lock_guard lock(output_mutex);
    std::cout << output.str() << '\n' << std::flush;
}

std::string append_audio_telemetry(std::string json,
                                   fluxrec::engine::AudioSession* audio_session) {
    if (json.empty() || json.back() != '}') return json;
    json.pop_back();
    std::ostringstream audio_json;
    if (audio_session == nullptr) {
        fluxrec::engine::writeAudioTelemetryObject(audio_json, {}, false);
    } else {
        fluxrec::engine::writeAudioTelemetryObject(audio_json, audio_session->snapshot(),
                                                   true);
    }
    json += ",\"audio\":";
    json += audio_json.str();
    json.push_back('}');
    return json;
}

std::string append_preview_telemetry(std::string json,
                                    const fluxrec::gpu::PreviewStats& preview) {
    if (json.empty() || json.back() != '}') return json;
    json.pop_back();
    json += ",\"preview\":{\"requested\":";
    json += preview.requested ? "true" : "false";
    json += ",\"enabled\":";
    json += preview.enabled ? "true" : "false";
    json += ",\"frames_submitted\":" + std::to_string(preview.frames_submitted);
    json += ",\"frames_rate_limited\":" + std::to_string(preview.frames_rate_limited);
    json += ",\"preview_skipped_pool_full\":" + std::to_string(preview.frames_no_free_slot);
    json += ",\"frames_source_unavailable\":" +
            std::to_string(preview.frames_source_unavailable);
    json += ",\"preview_transport_drops\":" +
            std::to_string(preview.frames_transport_dropped);
    json += ",\"frames_published\":" + std::to_string(preview.frames_published);
    if (!preview.last_error.empty()) {
        json += ",\"error\":";
        std::ostringstream error_json;
        write_json_string(error_json, preview.last_error);
        json += error_json.str();
    }
    json += "}}";
    return json;
}

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void request_stop_from_signal(int) noexcept {
    stop_requested = 1;
}

class PipelineStatsSampler {
public:
    PipelineStatsSampler(fluxrec::capture::WaylandCapture& capture,
                         fluxrec::mux::MuxWriter* writer,
                         fluxrec::replay::ReplayBuffer* replay,
                         const fluxrec::engine::PacketRouter& router,
                         fluxrec::telemetry::Telemetry& telemetry)
        : capture_(capture), writer_(writer), replay_(replay), router_(router),
          telemetry_(telemetry) {
        worker_ = std::thread([this] { run(); });
    }

    ~PipelineStatsSampler() { stop(); }

    PipelineStatsSampler(const PipelineStatsSampler&) = delete;
    PipelineStatsSampler& operator=(const PipelineStatsSampler&) = delete;

    void stop() noexcept {
        if (!stopping_.exchange(true, std::memory_order_acq_rel) && worker_.joinable()) {
            worker_.join();
        } else if (worker_.joinable()) {
            worker_.join();
        }
    }

    void sample_final() noexcept { sample_once(); }

private:
    void sample_once() noexcept {
        const auto capture_stats = capture_.stats();
        telemetry_.addCaptureWaylandCompleted(
            fluxrec::telemetry::monotonicCounterDelta(last_capture_completed_,
                                                       capture_stats.completed));
        telemetry_.addCaptureAdmitted(
            fluxrec::telemetry::monotonicCounterDelta(last_capture_admitted_,
                                                       capture_stats.admitted));
        telemetry_.addCaptureRateLimited(
            fluxrec::telemetry::monotonicCounterDelta(last_capture_rate_limited_,
                                                       capture_stats.rate_limited));
        last_capture_completed_ = capture_stats.completed;
        last_capture_admitted_ = capture_stats.admitted;
        last_capture_rate_limited_ = capture_stats.rate_limited;

        telemetry_.observeQueue(fluxrec::telemetry::Queue::capture,
                                capture_stats.frame_queue_occupancy);
        telemetry_.observeQueue(fluxrec::telemetry::Queue::gpu_frame_pool,
                                capture_stats.pool_busy_slots);
        telemetry_.addDroppedFrames(
            fluxrec::telemetry::DropStage::capture,
            fluxrec::telemetry::monotonicCounterDelta(last_capture_dropped_,
                                                       capture_stats.dropped));
        last_capture_dropped_ = capture_stats.dropped;

        if (writer_ != nullptr) {
            const auto mux_stats = writer_->stats();
            telemetry_.addMuxedFrames(
                fluxrec::telemetry::monotonicCounterDelta(last_muxed_video_packets_,
                                                           mux_stats.muxed_video_packets));
            last_muxed_video_packets_ = mux_stats.muxed_video_packets;
            telemetry_.observeQueue(fluxrec::telemetry::Queue::mux,
                                    mux_stats.queued_packets);
            telemetry_.observeOutputStats(mux_stats.output_file_bytes,
                                          mux_stats.packet_payload_bytes);
        }
        telemetry_.observeReplayMode(replay_ != nullptr,
                                     replay_ != nullptr && replay_->active(),
                                     router_.packet_queue_dropped(),
                                     replay_ != nullptr && writer_ == nullptr);

        if (replay_) {
            const auto replay_stats = replay_->stats();
            telemetry_.observeReplayStats(replay_stats.payload_bytes,
                                          replay_stats.snapshot_payload_bytes,
                                          replay_->config().max_bytes);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_system_sample_) {
            telemetry_.observeSystemMetrics(system_sampler_.sample());
            next_system_sample_ = now + std::chrono::seconds(1);
        }
    }

    void run() noexcept {
        while (!stopping_.load(std::memory_order_acquire)) {
            sample_once();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    fluxrec::capture::WaylandCapture& capture_;
    fluxrec::mux::MuxWriter* writer_{};
    fluxrec::replay::ReplayBuffer* replay_;
    const fluxrec::engine::PacketRouter& router_;
    fluxrec::telemetry::Telemetry& telemetry_;
    fluxrec::telemetry::SystemMetricsSampler system_sampler_;
    std::chrono::steady_clock::time_point next_system_sample_{};
    std::atomic<bool> stopping_{false};
    std::uint64_t last_capture_completed_{};
    std::uint64_t last_capture_admitted_{};
    std::uint64_t last_capture_rate_limited_{};
    std::uint64_t last_capture_dropped_{};
    std::uint64_t last_muxed_video_packets_{};
    std::thread worker_;
};

} // namespace

int fluxrec_record_main(int argc, char** argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    auto parsed_options = fluxrec::engine::parseRecordOptions(arguments);
    if (!parsed_options) {
        std::cerr << "options: " << parsed_options.error().message << '\n';
        std::cerr << "usage: fluxrec-record [--output FILE.mkv | --replay-only] [--display DP-1] "
                     "[--seconds 10 | --until-signal] [--codec h264|hevc] "
                     "[--backend nvenc|vulkan|experimental-cuda-egl (default nvenc)] "
                     "[--nvenc-preset p1..p7] "
                     "[--conversion two-pass|direct|auto (default two-pass)] "
                     "[--encoder-tune default|ll|ull|hq (default default)] "
                     "[--h264-coder cabac|vlc (default vlc; H.264 only)] "
                     "[--audio on|off (default off)] "
                     "[--audio-tracks default] "
                     "[--audio-track <id> <name> <source|sink-monitor> <input-target|-> <monitor-target|->] "
                     "[--preview-socket PATH] "
                     "[--verify-frame-ids|--verify-frame-ids-source-only|"
                     "--verify-frame-ids-unrelated N] "
                     "[--bitrate-bps 50000000 | --qp 0..51 (experimental CQP)] "
                     "[--gop 120] [--b-frames 0..4 (NVENC only)] "
                     "[--resolution WIDTHxHEIGHT] "
                     "[--max-fps 0|60|120|240|360 (0 uncapped)] [--pool 48] "
                     "[--async-depth 8 (NVENC default 4)] [--replay-seconds 0] "
                     "(--replay-only requires --replay-seconds > 0 and no --output)\n";
        return 2;
    }
    auto options = std::move(*parsed_options);

    const char* backend_env = std::getenv("FLUXREC_GPU_ENCODER_BACKEND");
    auto backend_name = [](fluxrec::engine::VideoBackend backend) -> const char* {
        switch (backend) {
        case fluxrec::engine::VideoBackend::vulkan: return "vulkan";
        case fluxrec::engine::VideoBackend::nvenc: return "nvenc";
        case fluxrec::engine::VideoBackend::experimental_cuda_egl:
            return "experimental-cuda-egl";
        }
        return "nvenc";
    };
    fluxrec::engine::VideoBackend selected_backend = options.backend;
    if (options.backend_explicit) {
        selected_backend = options.backend;
    } else if (backend_env && *backend_env) {
        auto parsed_backend = fluxrec::engine::parseVideoBackend(backend_env);
        if (!parsed_backend) {
            std::cerr << "encoder backend: FLUXREC_GPU_ENCODER_BACKEND "
                      << parsed_backend.error().message << '\n';
            return 2;
        }
        selected_backend = *parsed_backend;
    }
    auto configured_backend = fluxrec::engine::configureRecordOptionsForBackend(
        options, selected_backend);
    if (!configured_backend) {
        std::cerr << "encoder backend: " << configured_backend.error().message << '\n';
        return 2;
    }
    // Native 1440p constant-QP on the FFmpeg NVENC route waits out two Vulkan
    // rescales per frame. At QP 17 that fell to about 120 FPS. The direct
    // encoder takes the same DMA-BUF and does not run that converter.
    const bool native_output = (options.output_width == 0U || options.output_width == 2560U) &&
        (options.output_height == 0U || options.output_height == 1440U);
    const bool preset_is_p1 = !options.nvenc_preset || *options.nvenc_preset == "p1";
    const bool low_latency_tune = options.encoder_tune == fluxrec::gpu::EncoderTune::default_tuning ||
        options.encoder_tune == fluxrec::gpu::EncoderTune::ll;
    if (selected_backend == fluxrec::engine::VideoBackend::nvenc && options.qp &&
        options.b_frames == 0U && preset_is_p1 && low_latency_tune && native_output &&
        !options.verify_frame_ids && !options.verify_frame_ids_source_only &&
        !options.verify_frame_ids_unrelated &&
        options.h264_coder == fluxrec::gpu::H264Coder::vlc &&
        options.conversion_mode != fluxrec::gpu::ConversionMode::direct_compute &&
        (options.codec == fluxrec::gpu::Codec::h264 ||
         options.codec == fluxrec::gpu::Codec::hevc)) {
        std::cerr << "Constant-QP native 2560x1440 is using the direct CUDA EGL encoder "
                     "(pool 16, async depth 12, one reference, ultra-low-latency). "
                     "The two-pass Vulkan rescale and GPU preview are not on this path.\n";
        options.preview_socket_path.clear();
        options.conversion_explicit = false;
        options.pool_size = 16;
        options.pool_explicit = true;
        options.async_depth = 12;
        options.async_depth_explicit = true;
        selected_backend = fluxrec::engine::VideoBackend::experimental_cuda_egl;
        configured_backend = fluxrec::engine::configureRecordOptionsForBackend(
            options, selected_backend);
        if (!configured_backend) {
            std::cerr << "encoder backend: " << configured_backend.error().message << '\n';
            return 2;
        }
    }
    if (options.backend_explicit || !backend_env || !*backend_env) {
        const char* selected_backend_name = backend_name(selected_backend);
        if (::setenv("FLUXREC_GPU_ENCODER_BACKEND", selected_backend_name, 1) != 0) {
            std::cerr << "encoder backend: setenv failed: "
                      << std::strerror(errno) << '\n';
            return 1;
        }
    }
    const bool nvenc_backend =
        selected_backend == fluxrec::engine::VideoBackend::nvenc;
    const bool direct_cuda_egl_backend =
        selected_backend == fluxrec::engine::VideoBackend::experimental_cuda_egl;
    if (options.b_frames > 0 && !nvenc_backend) {
        std::cerr << "--b-frames currently requires the NVENC backend\n";
        return 2;
    }
    if (options.nvenc_preset && !nvenc_backend && !direct_cuda_egl_backend) {
        std::cerr << "--nvenc-preset requires the NVENC backend\n";
        return 2;
    }
    if (options.nvenc_preset &&
        ::setenv("FLUXREC_NVENC_PRESET", options.nvenc_preset->c_str(), 1) != 0) {
        std::cerr << "NVENC preset: setenv failed: "
                  << std::strerror(errno) << '\n';
        return 1;
    }
    if (nvenc_backend && !options.async_depth_explicit) {
        options.async_depth = 4;
        std::cerr << "NVENC selected: default async depth set to 4 validated frames\n";
    } else if (nvenc_backend && options.async_depth != 4) {
        std::cerr << "WARNING: NVENC async depth " << options.async_depth
                  << " is experimental. Controlled depth-8 and depth-16 recordings produced black frames; use depth 4 for normal recording.\n";
    }
    if (direct_cuda_egl_backend && options.pool_size < 16U) {
        std::cerr << "WARNING: direct CUDA EGL pool " << options.pool_size
                  << " is below the 16-buffer size that kept source IDs exact. "
                  << "Five-second motion-1 checks with 4 and 8 buffers skipped source IDs "
                  << "while capture_dropped stayed 0.\n";
    }

    // The Vulkan scale wait prevents the measured periodic black outputs for
    // HEVC, the H.264 ULL tune, and the NVENC interop route. Set it before
    // FFmpeg filter setup; ordinary Vulkan Video H.264 stays asynchronous.
    const bool needs_synchronized_scale = !direct_cuda_egl_backend &&
        (nvenc_backend ||
         options.codec == fluxrec::gpu::Codec::hevc ||
         (options.codec == fluxrec::gpu::Codec::h264 &&
          options.encoder_tune == fluxrec::gpu::EncoderTune::ull));
    if (needs_synchronized_scale) {
        if (::setenv("FLUXREC_FF_VK_WAIT_SIMPLE", "1", 1) != 0) {
            std::cerr << "Vulkan scale wait: setenv failed: "
                      << std::strerror(errno) << '\n';
            return 1;
        }
        std::cerr << (nvenc_backend
                          ? "NVENC selected: synchronized Vulkan scale enabled\n"
                          : options.codec == fluxrec::gpu::Codec::hevc
                              ? "HEVC selected: synchronized Vulkan scale enabled\n"
                              : "H.264 ULL selected: synchronized Vulkan scale enabled\n");
    }

    if (options.until_signal) {
        std::signal(SIGINT, request_stop_from_signal);
        std::signal(SIGTERM, request_stop_from_signal);
    }

    auto connected = fluxrec::capture::WaylandCapture::connect(
        {.output_name = options.output_name,
         .paint_cursors = true,
         .max_fps = options.max_fps});
    if (!connected) {
        std::cerr << "capture connect: " << connected.error().message << '\n';
        return 1;
    }
    auto capture = std::move(*connected);
    std::cerr << "capture: Hyprland will paint the pointer into captured frames\n";
    const auto& requirements = capture.requirements();

    fluxrec::telemetry::Telemetry telemetry;
    std::optional<fluxrec::telemetry::GpuConversionTimingBridge>
        gpu_conversion_timing_bridge;
    std::optional<fluxrec::telemetry::NvencApiTimingBridge>
        nvenc_api_timing_bridge;
    fluxrec::gpu::EncoderConfig encoder_config;
    encoder_config.codec = options.codec;
    encoder_config.encoder_tune = options.encoder_tune;
    encoder_config.h264_coder = options.h264_coder;
    encoder_config.h264_coder_explicit = options.h264_coder_explicit;
    encoder_config.conversion_mode = options.conversion_mode;
    encoder_config.output_width = options.output_width == 0
        ? requirements.width : options.output_width;
    encoder_config.output_height = options.output_height == 0
        ? requirements.height : options.output_height;
    encoder_config.fps_num = options.nominal_fps_num;
    encoder_config.fps_den = options.nominal_fps_den;
    encoder_config.qp = options.qp;
    encoder_config.bitrate = options.qp
        ? 0 : static_cast<std::int64_t>(options.bitrate_bps);
    encoder_config.gop_size = options.gop_size;
    encoder_config.b_frames = options.b_frames;
    encoder_config.capture_pool_size = options.pool_size;
    encoder_config.async_depth = options.async_depth;
    encoder_config.verify_frame_ids = options.verify_frame_ids;
    encoder_config.preview_socket_path = options.preview_socket_path;
    const char* device_timing_env = std::getenv("FLUXREC_FF_VK_DEVICE_TIMING");
    if (device_timing_env && std::strcmp(device_timing_env, "1") == 0) {
        gpu_conversion_timing_bridge.emplace(telemetry);
        encoder_config.device_timing_callback = &observe_vulkan_conversion_timing;
        encoder_config.device_timing_opaque = &*gpu_conversion_timing_bridge;
    }
    const char* nvenc_api_timing_env = std::getenv("FLUXREC_FF_NVENC_API_TIMING");
    if (nvenc_api_timing_env && std::strcmp(nvenc_api_timing_env, "1") == 0) {
        nvenc_api_timing_bridge.emplace(telemetry);
        encoder_config.nvenc_api_timing_callback = &observe_nvenc_api_timing;
        encoder_config.nvenc_api_timing_opaque = &*nvenc_api_timing_bridge;
    }
    if (options.verify_frame_ids_source_only) {
        encoder_config.frame_identity_mode = fluxrec::gpu::FrameIdentityMode::source_pass_only;
    } else if (options.verify_frame_ids_unrelated) {
        encoder_config.frame_identity_mode = fluxrec::gpu::FrameIdentityMode::unrelated_workload;
        encoder_config.frame_identity_unrelated_iterations =
            options.frame_identity_unrelated_iterations;
    }
    auto opened_encoder = fluxrec::gpu::GpuEncoder::create(requirements, encoder_config);
    if (!opened_encoder) {
        std::cerr << "GPU encoder: " << opened_encoder.error().message << '\n';
        return 1;
    }
    auto encoder = std::move(*opened_encoder);
    if (options.verify_frame_ids && !options.verify_frame_ids_source_only &&
        !options.verify_frame_ids_unrelated) {
        // A successful digest setup makes zero duplicates/skips meaningful.
        // Leave all identity fields unknown for ordinary and workload-only runs.
        telemetry.addUniqueFrames(0);
        telemetry.addDuplicatedFrames(0);
        telemetry.addIdentityCheckedFrames(0);
        telemetry.addIdentitySkippedFrames(0);
        telemetry.addIdentityUnclassifiedFrames(0);
    }
    if (auto set = capture.set_buffer_pool(encoder.capture_buffers()); !set) {
        std::cerr << "capture pool: " << set.error().message << '\n';
        return 1;
    }
    fluxrec::engine::EncoderStatsTelemetryObserver encoder_stats_telemetry{
        telemetry, encoder.stats()};

    constexpr std::size_t mux_queue_capacity_packets = 512;
    telemetry.setQueueCapacity(fluxrec::telemetry::Queue::gpu_frame_pool,
                               options.pool_size);
    telemetry.setQueueCapacity(fluxrec::telemetry::Queue::capture,
                               options.pool_size);
    telemetry.setQueueCapacity(fluxrec::telemetry::Queue::encode,
                               options.pool_size);
    if (!options.replay_only) {
        telemetry.setQueueCapacity(fluxrec::telemetry::Queue::mux,
                                   mux_queue_capacity_packets);
    }
    std::unique_ptr<fluxrec::mux::MuxWriter> writer;
    std::unique_ptr<fluxrec::engine::AudioSession> audio_session;
    std::vector<fluxrec::mux::AudioStreamConfig> audio_streams;
    if (options.audio_enabled && !options.audio_tracks.empty()) {
        fluxrec::engine::AudioSessionConfig audio_config;
        audio_config.capture.tracks = options.audio_tracks;
        audio_session =
            std::make_unique<fluxrec::engine::AudioSession>(std::move(audio_config));
        if (!audio_session->start(std::chrono::seconds(5))) {
            const auto snapshot = audio_session->snapshot();
            std::cerr << "audio startup: " << snapshot.last_error << '\n';
            audio_session->stop();
            return 1;
        }
        audio_streams = audio_session->stream_configs();
    }

    if (auto started = capture.start(); !started) {
        std::cerr << "capture start: " << started.error().message << '\n';
        if (audio_session) audio_session->stop();
        return 1;
    }
    std::optional<fluxrec::capture::CapturedFrame> first_frame;
    const auto first_frame_deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (!first_frame && !stop_requested &&
           std::chrono::steady_clock::now() < first_frame_deadline) {
        first_frame = capture.wait_pop_frame(std::chrono::milliseconds(20));
        if (!first_frame && capture.status() == fluxrec::capture::CaptureStatus::failed) {
            const auto error = capture.runtime_error();
            std::cerr << "capture: "
                      << (error ? error->message : "failed before the first frame") << '\n';
            capture.stop();
            if (audio_session) audio_session->stop();
            return 1;
        }
    }
    if (!first_frame) {
        std::cerr << "capture: timed out waiting for the first frame\n";
        capture.stop();
        if (audio_session) audio_session->stop();
        return 1;
    }
    if (audio_session && !first_frame->has_presentation_time) {
        std::cerr << "audio sync: the first captured frame has no compositor presentation timestamp; "
                     "audio requires a CLOCK_MONOTONIC video origin\n";
        capture.stop();
        audio_session->stop();
        return 1;
    }
    // Wayland's compositor presentation timestamp shares CLOCK_MONOTONIC with
    // PipeWire. The first captured frame establishes the common A/V epoch; in
    // audio mode a missing timestamp is a hard startup error, never a guessed offset.
    const std::uint64_t video_origin_time_ns = first_frame->has_presentation_time
        ? first_frame->presentation_time_ns : 0;

    std::unique_ptr<fluxrec::replay::ReplayBuffer> replay;
    if (options.replay_seconds > 0) {
        const auto replay_target_bitrate_bps = encoder_config.bitrate > 0
            ? static_cast<std::uint64_t>(encoder_config.bitrate) : 0;
        auto replay_config = fluxrec::replay::make_duration_aware_replay_buffer_config(
            std::chrono::seconds(options.replay_seconds), replay_target_bitrate_bps);
        constexpr std::uint64_t mib = 1024U * 1024U;
        std::cerr << "replay buffer: history=" << options.replay_seconds
                  << "s ring_budget_mib=" << replay_config.max_bytes / mib
                  << " snapshot_budget_mib=" << replay_config.max_snapshot_bytes / mib
                  << " (lazy allocation; saves report insufficient_history when the byte cap "
                     "cannot retain the full requested window)\n";
        replay = std::make_unique<fluxrec::replay::ReplayBuffer>(replay_config);
    }

    fluxrec::mux::MuxConfig mux_config;
    mux_config.output_path = options.replay_only ? std::string{} : options.output_path;
    mux_config.codec = options.codec == fluxrec::gpu::Codec::h264
        ? fluxrec::media::VideoCodec::h264 : fluxrec::media::VideoCodec::hevc;
    mux_config.width = static_cast<int>(encoder_config.output_width);
    mux_config.height = static_cast<int>(encoder_config.output_height);
    mux_config.stream_time_base = encoder.packet_time_base();
    mux_config.queue_capacity_packets = mux_queue_capacity_packets;
    mux_config.frame_rate = {
        static_cast<int>(encoder_config.fps_num), static_cast<int>(encoder_config.fps_den)};
    mux_config.bit_rate = encoder_config.bitrate;
    mux_config.audio_streams = audio_streams;
    mux_config.video_origin_time_ns = video_origin_time_ns;
    const auto extra = encoder.codec_extradata();
    std::cerr << "encoder codec extradata bytes=" << extra.size() << '\n';
    mux_config.codec_extradata.assign(extra.begin(), extra.end());
    fluxrec::mux::MuxConfig replay_mux_template = mux_config;
    // Replay snapshots share the same stream descriptions and monotonic origin
    // as the live recording. ReplayExporter rebases its private origin to the
    // first selected video DTS before it writes a keyframe-aligned clip.
    replay_mux_template.timing_observer = {};
    if (!options.replay_only) {
        mux_config.timing_observer = [&telemetry](fluxrec::telemetry::Timing timing,
                                                  std::chrono::nanoseconds duration) {
            telemetry.observe(timing, duration);
        };
        writer = std::make_unique<fluxrec::mux::MuxWriter>(std::move(mux_config));
        if (!writer->wait_until_ready(std::chrono::seconds(5))) {
            std::cerr << "mux writer: " << writer->error_message() << '\n';
            capture.stop();
            if (audio_session) audio_session->stop();
            return 1;
        }
    }
    if (audio_session &&
        !audio_session->begin_forwarding(writer.get(), replay.get(), video_origin_time_ns,
                                         std::chrono::seconds(5))) {
        const auto snapshot = audio_session->snapshot();
        std::cerr << "audio forwarding startup: " << snapshot.last_error << '\n';
        audio_session->stop();
        if (writer) writer->close();
        capture.stop();
        return 1;
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(options.seconds);
    std::unique_ptr<fluxrec::engine::ControlService> control;
    if (replay || audio_session) {
        fluxrec::engine::ControlServiceConfig control_config;
        if (replay) {
            control_config.replay_buffer = replay.get();
            control_config.mux_config_template = std::move(replay_mux_template);
            control_config.replay_duration = std::chrono::seconds(options.replay_seconds);
        }
        if (audio_session) {
            control_config.audio_command_callback =
                [session = audio_session.get()](const fluxrec::engine::ControlCommand& command) {
                    const bool applied = command.type ==
                                                fluxrec::engine::ControlCommandType::set_audio_volume
                        ? session->set_volume(command.audio_track_id, command.audio_gain)
                        : session->set_muted(command.audio_track_id, command.audio_muted);
                    if (!applied) {
                        throw std::runtime_error("Unknown audio track.");
                    }
                };
        }
        control_config.event_callback = report_control_event;
        control = std::make_unique<fluxrec::engine::ControlService>(
            std::move(control_config));
    }
    telemetry.observeQueue(fluxrec::telemetry::Queue::encode,
                           encoder.stats().pending_capture_leases);
    if (writer) {
        telemetry.observeQueue(fluxrec::telemetry::Queue::mux,
                               writer->stats().queued_packets);
    }
    fluxrec::telemetry::PeriodicJsonReporter reporter{
        telemetry, std::chrono::seconds(1),
        [audio = audio_session.get(), encoder_ptr = &encoder](std::string json) {
            json = append_audio_telemetry(std::move(json), audio);
            json = append_preview_telemetry(std::move(json), encoder_ptr->preview_stats());
            std::lock_guard lock(output_mutex);
            std::cout << json << '\n' << std::flush;
        }};
    fluxrec::engine::PacketRouter router{writer.get(), replay.get(), telemetry};
    PipelineStatsSampler sampler{capture, writer.get(), replay.get(), router, telemetry};
    std::uint64_t distinct_presentation_times = 0;
    std::optional<std::uint64_t> previous_presentation_time;
    bool failed = false;
    std::uint64_t last_identity_skipped = 0;
    auto collect_frame_identity = [&]() -> bool {
        if (!options.verify_frame_ids) return true;
        auto collected = encoder.poll_frame_identity();
        if (!collected) {
            std::cerr << "frame identity poll: " << collected.error().message << '\n';
            return false;
        }
        if (collected->skipped_frames >= last_identity_skipped) {
            telemetry.addIdentitySkippedFrames(collected->skipped_frames -
                                               last_identity_skipped);
        }
        last_identity_skipped = collected->skipped_frames;

        for (const auto& sample : collected->completed) {
            const auto& digest = sample.digest;
            if (!digest.digest_available) {
                if (digest.gpu_time_ns) {
                    telemetry.observe(fluxrec::telemetry::Timing::frame_identity_gpu,
                        std::chrono::nanoseconds(*digest.gpu_time_ns));
                }
                const char* workload_mode = options.verify_frame_ids_source_only
                    ? "source_pass_only" : "unrelated_workload";
                std::lock_guard lock(output_mutex);
                std::cerr << "{\"type\":\"frame_identity_workload\",\"mode\":\""
                          << workload_mode << "\",\"sequence\":"
                          << digest.sequence << ",\"pts\":";
                if (sample.pts) std::cerr << *sample.pts;
                else std::cerr << "null";
                if (digest.gpu_time_ns)
                    std::cerr << ",\"gpu_ns\":" << *digest.gpu_time_ns;
                std::cerr << "}\n" << std::flush;
                continue;
            }
            telemetry.addIdentityCheckedFrames();
            enum class Classification { unique, duplicate, unclassified };
            Classification classification = Classification::unclassified;
            if (digest.comparison_is_adjacent_capture) {
                classification = digest.same_as_compared_digest
                    ? Classification::duplicate : Classification::unique;
            } else if (!digest.compared_to_sequence && digest.sequence == 1) {
                classification = Classification::unique;
            }
            if (classification == Classification::unique) {
                telemetry.addUniqueFrames();
            } else if (classification == Classification::duplicate) {
                telemetry.addDuplicatedFrames();
            } else {
                telemetry.addIdentityUnclassifiedFrames();
            }
            if (digest.gpu_time_ns) {
                telemetry.observe(fluxrec::telemetry::Timing::frame_identity_gpu,
                    std::chrono::nanoseconds(*digest.gpu_time_ns));
            }

            const char* classification_name = classification == Classification::unique
                ? "unique" : classification == Classification::duplicate
                ? "duplicate" : "unclassified";
            std::lock_guard lock(output_mutex);
            std::cerr << "{\"type\":\"frame_identity\",\"sequence\":"
                      << digest.sequence << ",\"pts\":";
            if (sample.pts) std::cerr << *sample.pts;
            else std::cerr << "null";
            std::cerr << ",\"digest\":\"" << std::hex << std::setfill('0');
            for (const std::uint32_t word : digest.words)
                std::cerr << std::setw(8) << word;
            std::cerr << "\",\"classification\":\"" << classification_name << '\"';
            std::cerr << std::dec;
            if (digest.gpu_time_ns) std::cerr << ",\"gpu_ns\":" << *digest.gpu_time_ns;
            std::cerr << "}\n" << std::setfill(' ') << std::flush;
        }
        return true;
    };
    auto process_frame = [&](fluxrec::capture::CapturedFrame&& frame) -> bool {
        telemetry.addCaptured();
        telemetry.observe(fluxrec::telemetry::Timing::capture,
                          std::chrono::nanoseconds(frame.request_to_ready_ns));
        if (frame.has_presentation_time &&
            (!previous_presentation_time ||
             *previous_presentation_time != frame.presentation_time_ns)) {
            ++distinct_presentation_times;
            telemetry.addCompositorFrames();
        }
        if (frame.has_presentation_time) {
            if (previous_presentation_time &&
                frame.presentation_time_ns > *previous_presentation_time) {
                telemetry.observe(fluxrec::telemetry::Timing::frame_time,
                    std::chrono::nanoseconds(frame.presentation_time_ns -
                                             *previous_presentation_time));
            }
            previous_presentation_time = frame.presentation_time_ns;
        }

        std::optional<std::chrono::nanoseconds> capture_ready_to_submit_wall;
        if (frame.ready_at) {
            capture_ready_to_submit_wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - *frame.ready_at);
        }
        auto packets = encoder.submit(std::move(frame));
        if (capture_ready_to_submit_wall) {
            telemetry.observe(fluxrec::telemetry::Timing::capture_ready_to_submit_wall,
                              *capture_ready_to_submit_wall);
        }
        const auto after = encoder.stats();
        encoder_stats_telemetry.observe(after);
        telemetry.observeQueue(fluxrec::telemetry::Queue::encode,
                               after.pending_capture_leases);
        if (!collect_frame_identity()) return false;
        if (!packets) {
            std::cerr << "GPU encode: " << packets.error().message << '\n';
            telemetry.addDroppedFrames(fluxrec::telemetry::DropStage::encoder);
            return false;
        }
        for (auto& packet : *packets) {
            (void)router.try_submit(std::move(packet));
        }
        return true;
    };

    failed = !process_frame(std::move(*first_frame));
    first_frame.reset();

    while (!failed && !stop_requested && !(control && control->stop_requested()) &&
           (options.until_signal || std::chrono::steady_clock::now() < deadline)) {
        if (!router.error().empty()) {
            failed = true;
            break;
        }
        if (audio_session) {
            const auto audio_stats = audio_session->snapshot();
            if (audio_stats.audio_errors != 0 || audio_stats.sync_errors != 0) {
                std::cerr << "audio runtime: " << audio_stats.last_error << '\n';
                failed = true;
                break;
            }
        }
        auto frame = capture.wait_pop_frame(std::chrono::milliseconds(20));
        if (!frame) {
            if (capture.status() == fluxrec::capture::CaptureStatus::failed) {
                std::cerr << "capture: " << capture.runtime_error()->message << '\n';
                failed = true;
                break;
            }
            continue;
        }
        if (!process_frame(std::move(*frame))) {
            failed = true;
            break;
        }
        if (!router.error().empty()) {
            failed = true;
            break;
        }
    }
    capture.stop();
    while (auto queued = capture.try_pop_frame()) {
        if (!router.error().empty()) {
            failed = true;
            break;
        }
        if (!process_frame(std::move(*queued))) {
            failed = true;
            break;
        }
    }
    auto drained = encoder.drain();
    const auto after_drain = encoder.stats();
    encoder_stats_telemetry.observe(after_drain);
    telemetry.observeQueue(fluxrec::telemetry::Queue::encode,
                           after_drain.pending_capture_leases);
    if (drained) {
        for (auto& packet : *drained) {
            (void)router.try_submit(std::move(packet));
        }
    } else {
        std::cerr << "encoder drain: " << drained.error().message << '\n';
        failed = true;
    }
    if (!collect_frame_identity()) failed = true;
    router.close();
    if (control) control->close();
    if (audio_session) audio_session->stop();
    if (writer) writer->close();
    sampler.stop();
    sampler.sample_final();

    if (const auto router_error = router.error(); !router_error.empty()) {
        std::cerr << router_error << '\n';
        failed = true;
    }

    const auto capture_stats = capture.stats();
    const auto encode_stats = encoder.stats();
    std::cerr << "captured=" << capture_stats.completed
              << " capture_admitted=" << capture_stats.admitted
              << " capture_rate_limited=" << capture_stats.rate_limited
              << " distinct_presentation_times=" << distinct_presentation_times
              << " submitted=" << encode_stats.submitted_frames
              << " encoded=" << encode_stats.encoded_frames
              << " capture_dropped=" << capture_stats.dropped
              << " capture_unarmed=" << capture_stats.overlap_arm_failures
              << " capture_cancelled_at_stop=" << capture_stats.cancelled_at_stop
              << " pool_starvation_periods=" << capture_stats.pool_starvation_periods
              << " pool_starvation_ms="
              << (capture_stats.pool_starvation_total_ns / 1'000'000.0)
              << " packet_dropped=" << router.dropped()
              << " packet_queue_dropped=" << router.packet_queue_dropped();
    if (writer) {
        const auto mux_stats = writer->stats();
        std::cerr << " muxed_video_frames=" << mux_stats.muxed_video_packets
                  << " muxed_packets_total=" << mux_stats.muxed_packets
                  << " output_bytes=" << mux_stats.output_file_bytes;
    } else {
        std::cerr << " primary_mux=disabled output_file=disabled";
    }
    if (replay) {
        const auto replay_stats = replay->stats();
        std::cerr << " replay_retained_packets=" << replay_stats.packet_count
                  << " replay_video_packets=" << replay_stats.video_packet_count
                  << " replay_audio_packets=" << replay_stats.audio_packet_count
                  << " replay_payload_bytes=" << replay_stats.payload_bytes
                  << " replay_rejected_packets=" << replay_stats.rejected_packets
                  << " replay_audio_rejected_packets=" << replay_stats.audio_rejected_packets
                  << " replay_audio_evicted_packets=" << replay_stats.audio_evicted_packets
                  << " replay_inactive_packets=" << replay_stats.inactive_packets
                  << " replay_append_not_stored=" << router.replay_dropped();
    }
    std::cerr << '\n';
    if (audio_session) {
        const auto audio_stats = audio_session->snapshot();
        std::cerr << "audio_captured_packets=" << audio_stats.captured_packets
                  << " audio_mux_accepted_packets=" << audio_stats.mux_accepted_packets
                  << " audio_callback_drops=" << audio_stats.callback_queue_drops
                  << " audio_unforwarded_drops=" << audio_stats.unforwarded_packets_dropped
                  << " audio_mux_drops=" << audio_stats.mux_dropped_packets
                  << " audio_replay_stored=" << audio_stats.replay_stored_packets
                  << " audio_replay_drops=" << audio_stats.replay_dropped_packets
                  << " audio_sync_errors=" << audio_stats.sync_errors;
        if (audio_stats.tracks.empty()) {
            std::cerr << " tracks=0\n";
        } else {
            std::cerr << " tracks=" << audio_stats.tracks.size()
                      << " track0_peak=" << audio_stats.tracks.front().meter.peak
                      << '\n';
        }
        if (audio_stats.audio_errors != 0 || audio_stats.sync_errors != 0) {
            if (!audio_stats.last_error.empty()) {
                std::cerr << "audio error: " << audio_stats.last_error << '\n';
            }
            failed = true;
        }
    }
    if (writer && writer->state() == fluxrec::mux::WriterState::failed) {
        std::cerr << "mux writer: " << writer->error_message() << '\n';
        failed = true;
    }
    return failed ? 1 : 0;
}
