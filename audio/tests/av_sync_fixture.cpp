#include "engine/audio_session.hpp"
#include "mux/mux_writer.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <time.h>

namespace {

using namespace std::chrono_literals;

constexpr int kSampleRate = 48'000;
constexpr int kChannels = 2;
constexpr int kVideoWidth = 160;
constexpr int kVideoHeight = 90;
constexpr int kVideoFps = 60;
constexpr std::uint64_t kWarmupNs = 2'000'000'000ULL;
constexpr std::uint64_t kPulseLeadNs = 1'000'000'000ULL;
constexpr std::uint64_t kPulseWidthNs = 250'000'000ULL;
constexpr std::uint64_t kMaxOutputFramesPerBuffer = 65'536;

[[nodiscard]] std::uint64_t monotonic_now_ns() {
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC) failed");
    }
    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(now.tv_nsec);
}

[[nodiscard]] std::uint64_t monotonic_now_ns_noexcept() noexcept {
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(now.tv_nsec);
}

void sleep_until_monotonic(std::uint64_t target_ns) {
    for (;;) {
        const auto now = monotonic_now_ns();
        if (now >= target_ns) return;
        std::this_thread::sleep_for(std::chrono::nanoseconds(target_ns - now));
    }
}

[[nodiscard]] std::string ffmpeg_error(int error) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(error, text.data(), text.size());
    return text.data();
}

void check_av(int result, std::string_view operation) {
    if (result < 0) {
        throw std::runtime_error(std::string(operation) + ": " + ffmpeg_error(result));
    }
}

template<class Integer>
[[nodiscard]] Integer parse_integer(std::string_view text, std::string_view option) {
    Integer value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument(std::string(option) + " requires an integer value");
    }
    return value;
}

struct Options {
    std::string desktop_sink;
    std::string microphone_sink;
    std::string microphone_source;
    std::string output_path;
    std::string events_path;
    std::uint32_t seconds{60};
    std::uint32_t pulse_period_ms{5'000};
};

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (index + 1 >= argc) {
            throw std::invalid_argument(std::string(argument) + " requires a value");
        }
        const std::string_view value{argv[++index]};
        if (argument == "--desktop-sink") options.desktop_sink = value;
        else if (argument == "--microphone-sink") options.microphone_sink = value;
        else if (argument == "--microphone-source") options.microphone_source = value;
        else if (argument == "--output") options.output_path = value;
        else if (argument == "--events") options.events_path = value;
        else if (argument == "--seconds") options.seconds = parse_integer<std::uint32_t>(value, argument);
        else if (argument == "--pulse-period-ms") {
            options.pulse_period_ms = parse_integer<std::uint32_t>(value, argument);
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }
    if (options.desktop_sink.empty() || options.microphone_sink.empty() ||
        options.microphone_source.empty() ||
        options.output_path.empty()) {
        throw std::invalid_argument(
            "usage: fluxrec-audio-av-sync-fixture --desktop-sink NODE "
            "--microphone-sink NODE --microphone-source NODE "
            "--output FILE.mkv [--events FILE.json] "
            "[--seconds 60] [--pulse-period-ms 5000]");
    }
    if (options.seconds < 15 || options.seconds > 3'600) {
        throw std::invalid_argument("--seconds must be between 15 and 3600");
    }
    if (options.pulse_period_ms < 1'000 || options.pulse_period_ms > 60'000) {
        throw std::invalid_argument("--pulse-period-ms must be between 1000 and 60000");
    }
    if (options.events_path.empty()) options.events_path = options.output_path + ".events.json";
    return options;
}

class PulseOutputs final {
public:
    PulseOutputs(std::string first_playback_target, std::string second_playback_target,
                 std::uint64_t first_pulse_ns, std::uint64_t period_ns,
                 std::uint64_t end_ns)
        : targets_{std::move(first_playback_target), std::move(second_playback_target)},
          first_pulse_ns_(first_pulse_ns), period_ns_(period_ns), end_ns_(end_ns) {}

    ~PulseOutputs() { stop(); }
    PulseOutputs(const PulseOutputs&) = delete;
    PulseOutputs& operator=(const PulseOutputs&) = delete;

    void start() {
        loop_ = pw_thread_loop_new("fluxrec-av-sync-source", nullptr);
        if (!loop_) throw std::runtime_error("could not create PipeWire test-source loop");
        context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        if (!context_) throw std::runtime_error("could not create PipeWire test-source context");
        core_ = pw_context_connect(context_, nullptr, 0);
        if (!core_) throw std::runtime_error("could not connect PipeWire test-source core");

        for (std::size_t index = 0; index < streams_.size(); ++index) {
            users_[index] = StreamUser{this, index};
            create_stream(index);
        }
        if (pw_thread_loop_start(loop_) < 0) {
            throw std::runtime_error("could not start PipeWire test-source loop");
        }
        loop_started_ = true;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!error().empty()) throw std::runtime_error(error());
            if (streaming_[0].load(std::memory_order_acquire) &&
                streaming_[1].load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error("PipeWire synthetic playback streams did not become streaming");
    }

    void stop() noexcept {
        if (stopping_.exchange(true, std::memory_order_acq_rel)) return;
        if (loop_) {
            if (loop_started_) pw_thread_loop_lock(loop_);
            for (auto*& stream : streams_) {
                if (stream) {
                    pw_stream_disconnect(stream);
                    pw_stream_destroy(stream);
                    stream = nullptr;
                }
            }
            if (core_) {
                pw_core_disconnect(core_);
                core_ = nullptr;
            }
            if (context_) {
                pw_context_destroy(context_);
                context_ = nullptr;
            }
            if (loop_started_) pw_thread_loop_unlock(loop_);
            if (loop_started_) pw_thread_loop_stop(loop_);
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            loop_started_ = false;
        }
    }

    [[nodiscard]] std::uint64_t fallback_time_estimates() const noexcept {
        return fallback_time_estimates_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t callback_errors() const noexcept {
        return callback_errors_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t last_requested_frames() const noexcept {
        return last_requested_frames_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t last_capacity_frames() const noexcept {
        return last_capacity_frames_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t last_frame_count() const noexcept {
        return last_frame_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string error() const {
        std::lock_guard lock(error_mutex_);
        return error_;
    }

private:
    struct StreamUser {
        PulseOutputs* owner{};
        std::size_t index{};
    };

    static void state_changed(void* data, pw_stream_state, pw_stream_state state,
                              const char* error) noexcept {
        auto& user = *static_cast<StreamUser*>(data);
        if (state == PW_STREAM_STATE_STREAMING) {
            user.owner->streaming_[user.index].store(true, std::memory_order_release);
        } else if (state == PW_STREAM_STATE_ERROR) {
            user.owner->streaming_[user.index].store(false, std::memory_order_release);
            user.owner->set_error(error ? error : "PipeWire test-source stream failed");
        }
    }

    static void process(void* data) noexcept {
        auto& user = *static_cast<StreamUser*>(data);
        user.owner->process_stream(user.index);
    }

    static const pw_stream_events& events() {
        static const pw_stream_events value = [] {
            pw_stream_events result{};
            result.version = PW_VERSION_STREAM_EVENTS;
            result.state_changed = state_changed;
            result.process = process;
            return result;
        }();
        return value;
    }

    void create_stream(std::size_t index) {
        auto* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Test",
            PW_KEY_NODE_NAME, index == 0 ? "fluxrec.sync.desktop.source"
                                         : "fluxrec.sync.microphone.source",
            PW_KEY_NODE_DESCRIPTION, index == 0 ? "Fluxrec sync fixture desktop pulse"
                                                : "Fluxrec sync fixture microphone pulse",
            PW_KEY_TARGET_OBJECT, targets_[index].c_str(),
            PW_KEY_STREAM_IS_LIVE, "true",
            nullptr);
        if (!properties) throw std::runtime_error("could not allocate PipeWire playback properties");
        streams_[index] = pw_stream_new(core_, index == 0 ? "Fluxrec sync desktop pulse"
                                                          : "Fluxrec sync microphone pulse",
                                        properties);
        if (!streams_[index]) throw std::runtime_error("could not create PipeWire playback stream");
        pw_stream_add_listener(streams_[index], &hooks_[index], &events(), &users_[index]);

        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32_LE;
        info.rate = kSampleRate;
        info.channels = kChannels;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        std::array<std::uint8_t, 1024> storage{};
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
        const spa_pod* params[] = {spa_format_audio_raw_build(
            &builder, SPA_PARAM_EnumFormat, &info)};
        const auto result = pw_stream_connect(
            streams_[index], PW_DIRECTION_OUTPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                         PW_STREAM_FLAG_MAP_BUFFERS),
            params, 1);
        if (result < 0) {
            throw std::runtime_error("could not connect PipeWire playback stream: " +
                                     std::string(spa_strerror(result)));
        }
    }

    void set_error(const char* message) noexcept {
        try {
            std::lock_guard lock(error_mutex_);
            if (error_.empty()) error_ = message ? message : "PipeWire test source failed";
        } catch (...) {
        }
    }

    [[nodiscard]] std::uint64_t buffer_output_time_ns(pw_stream* stream,
                                                       std::uint32_t frames) noexcept {
        pw_time time{};
        if (pw_stream_get_time_n(stream, &time, sizeof(time)) == 0 &&
            time.now > 0 && time.rate.denom != 0) {
            const long double graph_tick_ns =
                static_cast<long double>(time.rate.num) * 1'000'000'000.0L /
                static_cast<long double>(time.rate.denom);
            const long double stream_sample_ns =
                1'000'000'000.0L / static_cast<long double>(kSampleRate);
            const long double queued_samples = static_cast<long double>(time.queued) +
                static_cast<long double>(time.buffered);
            const long double start = static_cast<long double>(time.now) +
                static_cast<long double>(time.delay) * graph_tick_ns +
                queued_samples * stream_sample_ns;
            if (start > 0.0L &&
                start < static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
                return static_cast<std::uint64_t>(start);
            }
        }
        fallback_time_estimates_.fetch_add(1, std::memory_order_relaxed);
        const auto buffer_ns = static_cast<std::uint64_t>(
            static_cast<long double>(frames) * 1'000'000'000.0L / kSampleRate);
        const auto now = monotonic_now_ns_noexcept();
        return now > std::numeric_limits<std::uint64_t>::max() - buffer_ns
            ? now : now + buffer_ns;
    }

    void process_stream(std::size_t index) noexcept {
        if (stopping_.load(std::memory_order_acquire)) return;
        auto* stream = streams_[index];
        if (!stream) return;
        while (auto* buffer = pw_stream_dequeue_buffer(stream)) {
            auto* spa_buffer = buffer->buffer;
            if (!spa_buffer || spa_buffer->n_datas == 0 || !spa_buffer->datas) {
                pw_stream_queue_buffer(stream, buffer);
                continue;
            }
            auto& data = spa_buffer->datas[0];
            constexpr std::uint32_t frame_bytes = kChannels * sizeof(float);
            if (!data.data || !data.chunk || data.maxsize < frame_bytes) {
                pw_stream_queue_buffer(stream, buffer);
                continue;
            }
            const auto capacity_frames = data.maxsize / frame_bytes;
            const auto requested_frames = buffer->requested > 0
                ? std::min<std::uint64_t>(buffer->requested, capacity_frames)
                : capacity_frames;
            last_requested_frames_.store(buffer->requested, std::memory_order_relaxed);
            last_capacity_frames_.store(capacity_frames, std::memory_order_relaxed);
            last_frame_count_.store(requested_frames, std::memory_order_relaxed);
            if (requested_frames == 0 || requested_frames > kMaxOutputFramesPerBuffer ||
                (buffer->requested > 0 && buffer->requested > capacity_frames)) {
                callback_errors_.fetch_add(1, std::memory_order_relaxed);
                pw_stream_queue_buffer(stream, buffer);
                continue;
            }
            const auto frames = static_cast<std::uint32_t>(requested_frames);
            auto* samples = static_cast<float*>(data.data);
            const auto buffer_start = buffer_output_time_ns(stream, frames);
            const double frequency = index == 0 ? 997.0 : 1'417.0;
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                const auto sample_time = buffer_start + static_cast<std::uint64_t>(
                    (static_cast<long double>(frame) * 1'000'000'000.0L) / kSampleRate);
                double value = 0.0;
                if (sample_time >= first_pulse_ns_ && sample_time < end_ns_) {
                    const auto pulse_index = (sample_time - first_pulse_ns_) / period_ns_;
                    const auto pulse_start = first_pulse_ns_ + pulse_index * period_ns_;
                    if (sample_time - pulse_start < kPulseWidthNs) {
                        const double seconds = static_cast<double>(sample_time - pulse_start) /
                                               1'000'000'000.0;
                        value = 0.65 * std::sin(6.283185307179586 * frequency * seconds);
                    }
                }
                const auto offset = static_cast<std::size_t>(frame) * kChannels;
                samples[offset] = static_cast<float>(value);
                samples[offset + 1] = static_cast<float>(value);
            }
            data.chunk->offset = 0;
            data.chunk->stride = static_cast<int32_t>(frame_bytes);
            data.chunk->size = frames * frame_bytes;
            // PipeWire reports queued audio in the units stored here. For raw
            // audio, the stream API recommends frame counts, not byte counts.
            buffer->size = frames;
            pw_stream_queue_buffer(stream, buffer);
        }
    }

    std::array<std::string, 2> targets_;
    std::uint64_t first_pulse_ns_{};
    std::uint64_t period_ns_{};
    std::uint64_t end_ns_{};
    pw_thread_loop* loop_{};
    pw_context* context_{};
    pw_core* core_{};
    std::array<pw_stream*, 2> streams_{};
    std::array<spa_hook, 2> hooks_{};
    std::array<StreamUser, 2> users_{};
    std::array<std::atomic<bool>, 2> streaming_{};
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> fallback_time_estimates_{0};
    std::atomic<std::uint64_t> callback_errors_{0};
    std::atomic<std::uint64_t> last_requested_frames_{0};
    std::atomic<std::uint64_t> last_capacity_frames_{0};
    std::atomic<std::uint64_t> last_frame_count_{0};
    mutable std::mutex error_mutex_;
    std::string error_;
    bool loop_started_{};
};

class VideoEncoder final {
public:
    VideoEncoder() {
        const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) throw std::runtime_error("CPU-only fixture requires FFmpeg libx264");
        context_ = avcodec_alloc_context3(codec);
        if (!context_) throw std::runtime_error("could not allocate CPU video encoder");
        context_->width = kVideoWidth;
        context_->height = kVideoHeight;
        context_->pix_fmt = AV_PIX_FMT_YUV420P;
        context_->time_base = AVRational{1, 1'000'000};
        context_->framerate = AVRational{kVideoFps, 1};
        context_->gop_size = kVideoFps;
        context_->max_b_frames = 0;
        context_->bit_rate = 300'000;
        context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        check_av(av_opt_set(context_->priv_data, "preset", "ultrafast", 0),
                 "set libx264 ultrafast preset");
        check_av(av_opt_set(context_->priv_data, "tune", "zerolatency", 0),
                 "set libx264 zerolatency tune");
        check_av(avcodec_open2(context_, codec, nullptr), "open CPU H.264 encoder");
        if (!context_->extradata || context_->extradata_size <= 0) {
            throw std::runtime_error("CPU H.264 encoder did not publish codec extradata");
        }
        frame_ = av_frame_alloc();
        if (!frame_) throw std::runtime_error("could not allocate CPU video frame");
        frame_->format = context_->pix_fmt;
        frame_->width = context_->width;
        frame_->height = context_->height;
        check_av(av_frame_get_buffer(frame_, 32), "allocate CPU video frame pixels");
    }

    ~VideoEncoder() {
        av_frame_free(&frame_);
        avcodec_free_context(&context_);
    }
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    [[nodiscard]] AVCodecContext* context() const noexcept { return context_; }

    [[nodiscard]] std::vector<fluxrec::media::EncodedPacket> encode(
        bool marker, std::uint64_t pts_ns) {
        check_av(av_frame_make_writable(frame_), "prepare CPU video frame");
        const auto y_value = static_cast<std::uint8_t>(marker ? 235 : 16);
        for (int y = 0; y < kVideoHeight; ++y) {
            std::fill_n(frame_->data[0] + y * frame_->linesize[0], kVideoWidth, y_value);
        }
        for (int plane = 1; plane < 3; ++plane) {
            for (int y = 0; y < kVideoHeight / 2; ++y) {
                std::fill_n(frame_->data[plane] + y * frame_->linesize[plane],
                            kVideoWidth / 2, static_cast<std::uint8_t>(128));
            }
        }
        frame_->pts = av_rescale_q(static_cast<std::int64_t>(pts_ns),
                                   AVRational{1, 1'000'000'000}, context_->time_base);
        check_av(avcodec_send_frame(context_, frame_), "submit CPU video marker frame");
        return receive_packets();
    }

    [[nodiscard]] std::vector<fluxrec::media::EncodedPacket> flush() {
        check_av(avcodec_send_frame(context_, nullptr), "flush CPU video marker encoder");
        return receive_packets();
    }

private:
    [[nodiscard]] std::vector<fluxrec::media::EncodedPacket> receive_packets() {
        std::vector<fluxrec::media::EncodedPacket> result;
        AVPacket* packet = av_packet_alloc();
        if (!packet) throw std::runtime_error("could not allocate CPU video packet");
        for (;;) {
            const int status = avcodec_receive_packet(context_, packet);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) break;
            check_av(status, "receive CPU video marker packet");
            fluxrec::media::EncodedPacket output;
            output.data.assign(packet->data, packet->data + packet->size);
            output.codec = fluxrec::media::VideoCodec::h264;
            output.pts = packet->pts == AV_NOPTS_VALUE ? 0 : packet->pts;
            output.dts = packet->dts == AV_NOPTS_VALUE ? output.pts : packet->dts;
            output.duration = packet->duration > 0 ? packet->duration : 1;
            output.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            output.time_base = {context_->time_base.num, context_->time_base.den};
            result.push_back(std::move(output));
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
        return result;
    }

    AVCodecContext* context_{};
    AVFrame* frame_{};
};

void enqueue_video(fluxrec::mux::MuxWriter& writer,
                   std::vector<fluxrec::media::EncodedPacket> packets) {
    for (auto& packet : packets) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        for (;;) {
            const auto status = writer.try_enqueue(std::move(packet));
            if (status == fluxrec::mux::EnqueueResult::accepted) break;
            if (status != fluxrec::mux::EnqueueResult::queue_busy &&
                status != fluxrec::mux::EnqueueResult::queue_full) {
                throw std::runtime_error("video fixture packet enqueue failed: " +
                    std::to_string(static_cast<int>(status)));
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("video fixture packet queue remained full");
            }
            std::this_thread::sleep_for(100us);
        }
    }
}

void write_event_file(const Options& options, std::uint64_t origin_ns,
                      std::uint64_t period_ns) {
    std::ofstream output(options.events_path, std::ios::trunc);
    if (!output) throw std::runtime_error("could not create marker timing sidecar");
    const auto first = origin_ns + kPulseLeadNs;
    const auto end = origin_ns + static_cast<std::uint64_t>(options.seconds) * 1'000'000'000ULL;
    output << "{\"clock\":\"CLOCK_MONOTONIC\",\"video_origin_ns\":" << origin_ns
           << ",\"duration_seconds\":" << options.seconds
           << ",\"pulse_period_ns\":" << period_ns
           << ",\"pulse_width_ns\":" << kPulseWidthNs
           << ",\"desktop_frequency_hz\":997,\"microphone_frequency_hz\":1417"
           << ",\"markers_ns\":[";
    bool comma = false;
    for (auto marker = first; marker < end; marker += period_ns) {
        if (comma) output << ',';
        output << marker;
        comma = true;
    }
    output << "]}\n";
    if (!output) throw std::runtime_error("could not write marker timing sidecar");
}

void run(const Options& options) {
    fluxrec::engine::AudioSessionConfig audio_config;
    audio_config.capture.codec = fluxrec::audio::Codec::aac;
    audio_config.capture.sample_rate = kSampleRate;
    audio_config.capture.channels = kChannels;
    audio_config.capture.bitrate = 128'000;
    audio_config.capture.tracks = fluxrec::audio::defaultAudioTracks();
    audio_config.capture.tracks[0].input_kind = fluxrec::audio::InputKind::sink_monitor;
    audio_config.capture.tracks[0].input_target = options.desktop_sink;
    audio_config.capture.tracks[1].input_kind = fluxrec::audio::InputKind::source;
    audio_config.capture.tracks[1].input_target = options.microphone_source;
    fluxrec::engine::AudioSession audio_session(std::move(audio_config));
    if (!audio_session.start(5s)) {
        throw std::runtime_error("audio session startup: " + audio_session.snapshot().last_error);
    }
    const auto audio_tracks = audio_session.stream_configs();
    if (audio_tracks.size() != 2) {
        audio_session.stop();
        throw std::runtime_error("audio session did not publish both test track formats");
    }
    const auto input_deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < input_deadline) {
        const auto snapshot = audio_session.snapshot();
        if (snapshot.audio_errors != 0) {
            audio_session.stop();
            throw std::runtime_error("PipeWire capture input: " + snapshot.last_error);
        }
        if (snapshot.tracks.size() >= 2 &&
            snapshot.tracks[0].state == fluxrec::audio::SourceState::streaming &&
            snapshot.tracks[1].state == fluxrec::audio::SourceState::streaming) break;
        std::this_thread::sleep_for(10ms);
    }
    const auto input_snapshot = audio_session.snapshot();
    if (input_snapshot.tracks.size() < 2 ||
        input_snapshot.tracks[0].state != fluxrec::audio::SourceState::streaming ||
        input_snapshot.tracks[1].state != fluxrec::audio::SourceState::streaming) {
        audio_session.stop();
        throw std::runtime_error("both PipeWire capture inputs must reach streaming state");
    }

    VideoEncoder video_encoder;
    const auto origin_ns = monotonic_now_ns() + kWarmupNs;
    const auto period_ns = static_cast<std::uint64_t>(options.pulse_period_ms) * 1'000'000ULL;
    const auto first_pulse_ns = origin_ns + kPulseLeadNs;
    const auto end_ns = origin_ns + static_cast<std::uint64_t>(options.seconds) * 1'000'000'000ULL;
    PulseOutputs pulse_outputs{options.desktop_sink, options.microphone_sink,
                               first_pulse_ns, period_ns, end_ns};
    pulse_outputs.start();

    fluxrec::mux::MuxConfig mux_config;
    mux_config.output_path = options.output_path;
    mux_config.codec = fluxrec::media::VideoCodec::h264;
    mux_config.width = kVideoWidth;
    mux_config.height = kVideoHeight;
    mux_config.stream_time_base = {video_encoder.context()->time_base.num,
                                   video_encoder.context()->time_base.den};
    mux_config.frame_rate = {kVideoFps, 1};
    mux_config.bit_rate = video_encoder.context()->bit_rate;
    mux_config.codec_extradata.assign(video_encoder.context()->extradata,
        video_encoder.context()->extradata + video_encoder.context()->extradata_size);
    mux_config.audio_streams = audio_tracks;
    mux_config.video_origin_time_ns = origin_ns;
    mux_config.queue_capacity_packets = 2048;
    mux_config.queue_capacity_bytes = 32U * 1024U * 1024U;
    fluxrec::mux::MuxWriter writer(std::move(mux_config));
    if (!writer.wait_until_ready(5s)) {
        pulse_outputs.stop();
        audio_session.stop();
        throw std::runtime_error("mux startup: " + writer.error_message());
    }
    if (!audio_session.begin_forwarding(writer, origin_ns, 5s)) {
        const auto message = audio_session.snapshot().last_error;
        pulse_outputs.stop();
        audio_session.stop();
        writer.close();
        throw std::runtime_error("audio forwarding startup: " + message);
    }

    write_event_file(options, origin_ns, period_ns);
    const auto frame_period_ns = 1'000'000'000ULL / kVideoFps;
    const auto frame_count = static_cast<std::uint64_t>(options.seconds) * kVideoFps;
    const auto markers_count = (end_ns > first_pulse_ns)
        ? (end_ns - 1 - first_pulse_ns) / period_ns + 1U : 0U;
    std::uint64_t video_packets = 0;
    float track0_max_meter_peak = 0.0F;
    float track1_max_meter_peak = 0.0F;
    for (std::uint64_t frame_index = 0; frame_index < frame_count; ++frame_index) {
        if (const auto pulse_error = pulse_outputs.error(); !pulse_error.empty()) {
            throw std::runtime_error("synthetic PipeWire playback: " + pulse_error);
        }
        const auto live_audio_stats = audio_session.snapshot();
        if (live_audio_stats.tracks.size() >= 2) {
            track0_max_meter_peak = std::max(track0_max_meter_peak,
                                             live_audio_stats.tracks[0].meter.peak);
            track1_max_meter_peak = std::max(track1_max_meter_peak,
                                             live_audio_stats.tracks[1].meter.peak);
        }
        const auto target_ns = origin_ns + frame_index * frame_period_ns;
        sleep_until_monotonic(target_ns);
        const bool marker = target_ns >= first_pulse_ns &&
            ((target_ns - first_pulse_ns) % period_ns) < kPulseWidthNs;
        auto packets = video_encoder.encode(marker, target_ns - origin_ns);
        video_packets += packets.size();
        enqueue_video(writer, std::move(packets));
    }
    enqueue_video(writer, video_encoder.flush());
    pulse_outputs.stop();
    audio_session.stop();
    writer.close();

    const auto audio_stats = audio_session.snapshot();
    const auto mux_stats = writer.stats();
    if (audio_stats.audio_errors != 0 || audio_stats.sync_errors != 0 ||
        audio_stats.callback_queue_drops != 0 ||
        audio_stats.unforwarded_packets_dropped != 0 ||
        audio_stats.mux_dropped_packets != 0 || pulse_outputs.fallback_time_estimates() != 0 ||
        pulse_outputs.callback_errors() != 0) {
        throw std::runtime_error(std::string(
            "fixture reported audio errors, queue drops, or fallback clock estimates; ") +
            "audio_errors=" + std::to_string(audio_stats.audio_errors) +
            " sync_errors=" + std::to_string(audio_stats.sync_errors) +
            " callback_queue_drops=" + std::to_string(audio_stats.callback_queue_drops) +
            " unforwarded_packets_dropped=" + std::to_string(audio_stats.unforwarded_packets_dropped) +
            " mux_dropped_packets=" + std::to_string(audio_stats.mux_dropped_packets) +
            " track0_buffers=" +
            std::to_string(audio_stats.tracks.size() > 0
                               ? audio_stats.tracks[0].buffers_received
                               : 0) +
            " track0_drops=" +
            std::to_string(audio_stats.tracks.size() > 0
                               ? audio_stats.tracks[0].buffers_dropped
                               : 0) +
            " track0_last_buffer_peak=" +
            std::to_string(audio_stats.tracks.size() > 0 ? audio_stats.tracks[0].meter.peak
                                                        : 0.0F) +
            " track1_buffers=" +
            std::to_string(audio_stats.tracks.size() > 1
                               ? audio_stats.tracks[1].buffers_received
                               : 0) +
            " track1_drops=" +
            std::to_string(audio_stats.tracks.size() > 1
                               ? audio_stats.tracks[1].buffers_dropped
                               : 0) +
            " track1_last_buffer_peak=" +
            std::to_string(audio_stats.tracks.size() > 1 ? audio_stats.tracks[1].meter.peak
                                                        : 0.0F) +
            " clock_fallbacks=" + std::to_string(pulse_outputs.fallback_time_estimates()) +
            " fixture_callback_errors=" + std::to_string(pulse_outputs.callback_errors()) +
            " last_requested_frames=" + std::to_string(pulse_outputs.last_requested_frames()) +
            " last_capacity_frames=" + std::to_string(pulse_outputs.last_capacity_frames()) +
            " last_frame_count=" + std::to_string(pulse_outputs.last_frame_count()) +
            " last_error=" + audio_stats.last_error);
    }
    if (writer.state() == fluxrec::mux::WriterState::failed) {
        throw std::runtime_error("mux writer failed: " + writer.error_message());
    }
    if (monotonic_now_ns() < origin_ns || mux_stats.muxed_packets == 0 || video_packets == 0) {
        throw std::runtime_error("fixture did not write expected video/audio packets");
    }

    std::cout << "{\"type\":\"audio_av_fixture\",\"output\":\""
              << options.output_path << "\",\"events\":\"" << options.events_path
              << "\",\"video_origin_ns\":" << origin_ns
              << ",\"duration_seconds\":" << options.seconds
              << ",\"markers\":" << markers_count
              << ",\"video_packets\":" << video_packets
              << ",\"muxed_packets\":" << mux_stats.muxed_packets
              << ",\"track0_audio_packets\":"
              << (audio_stats.tracks.size() > 0 ? audio_stats.tracks[0].buffers_received : 0)
              << ",\"track1_audio_packets\":"
              << (audio_stats.tracks.size() > 1 ? audio_stats.tracks[1].buffers_received : 0)
              << ",\"track0_max_meter_peak\":" << track0_max_meter_peak
              << ",\"track1_max_meter_peak\":" << track1_max_meter_peak
              << ",\"clock_fallbacks\":" << pulse_outputs.fallback_time_estimates()
              << ",\"audio_callback_drops\":" << audio_stats.callback_queue_drops
              << ",\"audio_mux_drops\":" << audio_stats.mux_dropped_packets << "}\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        run(parse_options(argc, argv));
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "audio A/V sync fixture: " << exception.what() << '\n';
        return 1;
    }
}
