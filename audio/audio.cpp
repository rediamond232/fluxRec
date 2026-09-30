#include "audio/audio.hpp"
#include "audio/detail/aac_padding_tracker.hpp"
#include "audio/pipewire_clock_metrics.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <time.h>
#include <utility>

namespace fluxrec::audio {
namespace {

constexpr std::size_t kQueueCapacity = 12;
constexpr std::size_t kMaxFramesPerBuffer = 8192;
constexpr std::size_t kMaxChannels = 2;
// Playback backlog per monitored track. 8192 stereo frames is about 170 ms at
// 48 kHz, which absorbs a graph quantum change without adding audible delay.
constexpr std::size_t kMonitorRingSamples = kMaxFramesPerBuffer * kMaxChannels;

std::uint64_t monotonicNowNs() noexcept {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(now.tv_nsec);
}

std::string avErrorString(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> message{};
    av_strerror(code, message.data(), message.size());
    return message.data();
}

struct CaptureBlock {
    std::array<float, kMaxFramesPerBuffer * kMaxChannels> samples{};
    std::uint32_t frames{};
    std::uint64_t capture_time_ns{};
};

// One PipeWire stream produces into each source's ring. The PipeWire thread is
// the only producer and the audio worker is the only consumer.
class SourceQueue final {
public:
    bool push(const float* samples, std::uint32_t frames, std::uint32_t channels,
              std::uint64_t capture_time_ns) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        if (write - read >= kQueueCapacity || frames > kMaxFramesPerBuffer) {
            return false;
        }
        auto& block = blocks_[write % kQueueCapacity];
        const auto sample_count = static_cast<std::size_t>(frames) * channels;
        std::memcpy(block.samples.data(), samples, sample_count * sizeof(float));
        block.frames = frames;
        block.capture_time_ns = capture_time_ns;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    bool pop(CaptureBlock& destination, std::uint32_t channels) noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        if (read == write) {
            return false;
        }
        const auto& block = blocks_[read % kQueueCapacity];
        const auto sample_count = static_cast<std::size_t>(block.frames) * channels;
        std::memcpy(destination.samples.data(), block.samples.data(),
                    sample_count * sizeof(float));
        destination.frames = block.frames;
        destination.capture_time_ns = block.capture_time_ns;
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return read_.load(std::memory_order_acquire) ==
               write_.load(std::memory_order_acquire);
    }

private:
    std::array<CaptureBlock, kQueueCapacity> blocks_{};
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
};

// Gained samples travel from the audio worker to the track's playback stream
// through this ring. The worker is the only producer and the PipeWire playback
// callback is the only consumer, so a full ring discards the newest block
// instead of letting monitor latency grow without bound.
class MonitorRing final {
public:
    void push(const float* samples, std::size_t count) noexcept {
        if (count == 0 || count > kMonitorRingSamples) return;
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        if (kMonitorRingSamples - static_cast<std::size_t>(write - read) < count) return;
        for (std::size_t i = 0; i < count; ++i) {
            samples_[static_cast<std::size_t>((write + i) % kMonitorRingSamples)] = samples[i];
        }
        write_.store(write + count, std::memory_order_release);
    }

    // Fills `count` samples, padding with silence when the worker has not
    // produced enough yet. A playback buffer must always be fully written.
    void pull(float* destination, std::size_t count) noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        const auto available = std::min(static_cast<std::size_t>(write - read), count);
        for (std::size_t i = 0; i < available; ++i) {
            destination[i] = samples_[static_cast<std::size_t>((read + i) % kMonitorRingSamples)];
        }
        for (std::size_t i = available; i < count; ++i) {
            destination[i] = 0.0F;
        }
        read_.store(read + available, std::memory_order_release);
    }

    void clear() noexcept {
        read_.store(0, std::memory_order_relaxed);
        write_.store(0, std::memory_order_release);
    }

private:
    std::array<float, kMonitorRingSamples> samples_{};
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
};

struct SourceRuntime {
    std::atomic<SourceState> state{SourceState::stopped};
    std::atomic<float> volume{1.0F};
    std::atomic<bool> muted{false};
    std::atomic<float> meter_peak{0.0F};
    std::atomic<float> meter_rms{0.0F};
    std::atomic<std::uint64_t> meter_samples{0};
    std::atomic<std::uint64_t> buffers_received{0};
    std::atomic<std::uint64_t> buffers_dropped{0};
    std::atomic<std::uint64_t> samples_captured{0};
    PipeWireClockMetrics pipewire_clock_metrics;
    std::atomic<bool> error_pending{false};
    mutable std::mutex error_mutex;
    std::string error;
    std::string monitor_error;
    std::atomic<bool> monitor_streaming{false};
    SourceQueue queue;
    MonitorRing monitor_ring;
};

class Encoder final {
public:
    Encoder(std::size_t track_index, const Config& config,
            const std::function<void(Format)>& on_format,
            const std::function<void(Packet)>& on_packet,
            std::function<void(std::string)> report_error)
        : track_index_(track_index), config_(config), on_format_(on_format),
          on_packet_(on_packet), report_error_(std::move(report_error)) {}

    ~Encoder() { close(); }

    bool open() {
        const AVCodec* codec = nullptr;
        if (config_.codec == Codec::aac) {
            codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        } else {
            codec = avcodec_find_encoder_by_name("libopus");
            if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
        }
        if (!codec) {
            report_error_(config_.codec == Codec::aac
                              ? "FFmpeg AAC encoder is unavailable"
                              : "FFmpeg Opus encoder is unavailable");
            return false;
        }

        context_ = avcodec_alloc_context3(codec);
        if (!context_) {
            report_error_("Could not allocate FFmpeg audio encoder context");
            return false;
        }

        context_->sample_rate = static_cast<int>(config_.sample_rate);
        context_->time_base = AVRational{1, static_cast<int>(config_.sample_rate)};
        context_->bit_rate = config_.bitrate;
        context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_channel_layout_default(&context_->ch_layout,
                                  static_cast<int>(config_.channels));

        context_->sample_fmt = chooseSampleFormat(codec);
        if (context_->sample_fmt == AV_SAMPLE_FMT_NONE) {
            report_error_("The selected FFmpeg audio encoder has no supported float sample format");
            close();
            return false;
        }
        const int result = avcodec_open2(context_, codec, nullptr);
        if (result < 0) {
            report_error_("Could not open FFmpeg audio encoder: " + avErrorString(result));
            close();
            return false;
        }

        frame_size_ = context_->frame_size > 0 ? context_->frame_size : 1024;
        pending_.reserve(static_cast<std::size_t>(frame_size_) * config_.channels * 2);

        Format format;
        format.track_index = track_index_;
        format.codec = config_.codec;
        format.sample_rate = config_.sample_rate;
        format.channels = config_.channels;
        format.initial_padding = context_->initial_padding;
        format.time_base_den = static_cast<std::int32_t>(config_.sample_rate);
        if (context_->extradata && context_->extradata_size > 0) {
            format.extradata.assign(context_->extradata,
                                    context_->extradata + context_->extradata_size);
        }
        if (on_format_) {
            try {
                on_format_(std::move(format));
            } catch (...) {
                report_error_("Audio format callback threw an exception");
            }
        }
        return true;
    }

    void consume(const CaptureBlock& block, float gain, bool muted) {
        if (!context_) return;
        const auto channels = config_.channels;
        const auto sample_count = static_cast<std::size_t>(block.frames) * channels;
        if (sample_count == 0) return;

        const auto frame_duration_ns = static_cast<std::uint64_t>(
            (static_cast<long double>(block.frames) * 1'000'000'000.0L) /
            config_.sample_rate);
        if (!pending_.empty() && block.capture_time_ns > expected_next_time_ns_ + 8'000'000ULL) {
            padAndSendPending();
        }
        if (pending_.empty()) {
            pending_time_ns_ = block.capture_time_ns;
        }

        for (std::size_t i = 0; i < sample_count; ++i) {
            float value = muted ? 0.0F : block.samples[i] * gain;
            value = std::clamp(value, -1.0F, 1.0F);
            pending_.push_back(value);
        }
        expected_next_time_ns_ = block.capture_time_ns + frame_duration_ns;

        const auto samples_per_frame = static_cast<std::size_t>(frame_size_) * channels;
        while (pending_.size() >= samples_per_frame) {
            sendFrame(pending_.data(), pending_time_ns_);
            pending_.erase(pending_.begin(), pending_.begin() +
                           static_cast<std::ptrdiff_t>(samples_per_frame));
            pending_time_ns_ += static_cast<std::uint64_t>(
                (static_cast<long double>(frame_size_) * 1'000'000'000.0L) /
                config_.sample_rate);
        }
    }

    void flush() {
        if (context_ && !pending_.empty()) padAndSendPending();
        if (!context_) return;
        const int result = avcodec_send_frame(context_, nullptr);
        if (result < 0 && result != AVERROR_EOF) {
            report_error_("Could not flush FFmpeg audio encoder: " + avErrorString(result));
        }
        receivePackets();
        if (tail_padding_tracker_.size() != 0) {
            report_error_("AAC encoder flush did not return packets for padded audio frames");
        }
    }

private:
    static AVSampleFormat chooseSampleFormat(const AVCodec* codec) noexcept {
        const void* supported = nullptr;
        if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                         0, &supported, nullptr) < 0 || !supported) {
            // A null list means that the codec accepts all sample formats.
            return AV_SAMPLE_FMT_FLTP;
        }
        const auto* formats = static_cast<const AVSampleFormat*>(supported);
        for (const auto* f = formats; *f != AV_SAMPLE_FMT_NONE; ++f) {
            if (*f == AV_SAMPLE_FMT_FLTP) return *f;
        }
        for (const auto* f = formats; *f != AV_SAMPLE_FMT_NONE; ++f) {
            if (*f == AV_SAMPLE_FMT_FLT) return *f;
        }
        return AV_SAMPLE_FMT_NONE;
    }

    void padAndSendPending() {
        if (pending_.empty()) return;
        const auto samples_per_frame = static_cast<std::size_t>(frame_size_) * config_.channels;
        const auto valid_sample_frames = pending_.size() / config_.channels;
        const auto padding_samples = static_cast<std::uint32_t>(
            static_cast<std::size_t>(frame_size_) - valid_sample_frames);
        pending_.resize(samples_per_frame, 0.0F);
        sendFrame(pending_.data(), pending_time_ns_,
                  config_.codec == Codec::aac ? padding_samples : 0U);
        pending_.clear();
        pending_time_ns_ = expected_next_time_ns_;
    }

    void sendFrame(const float* interleaved, std::uint64_t time_ns,
                   std::uint32_t discard_padding_samples = 0) {
        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            report_error_("Could not allocate FFmpeg audio frame");
            return;
        }
        frame->format = context_->sample_fmt;
        frame->sample_rate = context_->sample_rate;
        frame->nb_samples = frame_size_;
        frame->pts = av_rescale_q(static_cast<std::int64_t>(time_ns),
                                  AVRational{1, 1'000'000'000}, context_->time_base);
        av_channel_layout_copy(&frame->ch_layout, &context_->ch_layout);

        int result = av_frame_get_buffer(frame, 0);
        if (result >= 0) result = av_frame_make_writable(frame);
        if (result < 0) {
            report_error_("Could not allocate FFmpeg audio frame samples: " + avErrorString(result));
            av_frame_free(&frame);
            return;
        }

        if (context_->sample_fmt == AV_SAMPLE_FMT_FLTP) {
            for (std::uint32_t channel = 0; channel < config_.channels; ++channel) {
                auto* output = reinterpret_cast<float*>(frame->extended_data[channel]);
                for (int sample = 0; sample < frame_size_; ++sample) {
                    output[sample] = interleaved[
                        static_cast<std::size_t>(sample) * config_.channels + channel];
                }
            }
        } else {
            std::memcpy(frame->data[0], interleaved,
                        static_cast<std::size_t>(frame_size_) * config_.channels * sizeof(float));
        }

        if (discard_padding_samples != 0) {
            // The AAC encoder delays packet output by its priming frame. Keep
            // only the small tail markers keyed by input PTS so delayed output
            // can carry padding on the packet for this exact input frame.
            // Normal full frames allocate no marker and retain the existing
            // callback path.
            if (!tail_padding_tracker_.remember(frame->pts, discard_padding_samples)) {
                // Drain once before treating an unexpectedly long AAC delay
                // as an encoder failure. The fixed tracker keeps memory
                // bounded even if an encoder stops returning matching PTS.
                receivePackets();
                if (!tail_padding_tracker_.remember(frame->pts,
                                                    discard_padding_samples)) {
                    report_error_("AAC encoder did not resolve earlier tail-padding markers");
                    av_frame_free(&frame);
                    return;
                }
            }
        }
        const auto frame_pts = frame->pts;
        result = avcodec_send_frame(context_, frame);
        av_frame_free(&frame);
        if (result < 0) {
            if (discard_padding_samples != 0) {
                (void)tail_padding_tracker_.take(frame_pts);
            }
            report_error_("FFmpeg rejected an audio frame: " + avErrorString(result));
            return;
        }
        receivePackets();
    }

    void receivePackets() {
        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            report_error_("Could not allocate FFmpeg audio packet");
            return;
        }
        while (true) {
            const int result = avcodec_receive_packet(context_, packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            if (result < 0) {
                report_error_("FFmpeg failed to encode audio: " + avErrorString(result));
                break;
            }
            Packet output;
            output.track_index = track_index_;
            output.codec = config_.codec;
            output.pts = packet->pts;
            output.duration = packet->duration;
            output.key_frame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
            if (packet->pts != AV_NOPTS_VALUE) {
                output.discard_padding_samples = tail_padding_tracker_.take(packet->pts);
            }
            if (packet->pts != AV_NOPTS_VALUE) {
                const auto ns = av_rescale_q(packet->pts, context_->time_base,
                                             AVRational{1, 1'000'000'000});
                output.capture_time_ns = ns < 0 ? 0 : static_cast<std::uint64_t>(ns);
            }
            if (packet->size > 0) {
                output.data.assign(packet->data, packet->data + packet->size);
            }
            if (on_packet_) {
                try {
                    on_packet_(std::move(output));
                } catch (...) {
                    report_error_("Audio packet callback threw an exception");
                }
            }
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }

    void close() noexcept {
        if (context_) avcodec_free_context(&context_);
    }

    std::size_t track_index_;
    const Config& config_;
    const std::function<void(Format)>& on_format_;
    const std::function<void(Packet)>& on_packet_;
    std::function<void(std::string)> report_error_;
    AVCodecContext* context_{};
    int frame_size_{1024};
    std::vector<float> pending_;
    detail::AacPaddingTracker tail_padding_tracker_;
    std::uint64_t pending_time_ns_{};
    std::uint64_t expected_next_time_ns_{};
};

} // namespace

class Capture::Impl final {
public:
    explicit Impl(Config config) : config_(std::move(config)) {
        // An over-long track list is rejected by start(), so only bound the
        // per-track allocation here; every constructed runtime still reports
        // that rejection through its snapshot.
        const auto count = std::min(config_.tracks.size(), max_audio_tracks);
        runtimes_.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            runtimes_.push_back(std::make_unique<SourceRuntime>());
        }
        streams_.assign(count, nullptr);
        monitor_streams_.assign(count, nullptr);
        stream_hooks_.resize(count);
        monitor_hooks_.resize(count);
        stream_data_.resize(count);
        monitor_data_.resize(count);
    }

    ~Impl() { stop(); }

    bool start() {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (running_.load(std::memory_order_acquire)) return true;
        for (auto& runtime : runtimes_) {
            runtime->state.store(SourceState::stopped, std::memory_order_release);
            runtime->error_pending.store(false, std::memory_order_release);
            runtime->monitor_streaming.store(false, std::memory_order_release);
            runtime->pipewire_clock_metrics.reset();
            runtime->monitor_ring.clear();
            std::lock_guard error_lock(runtime->error_mutex);
            runtime->error.clear();
            runtime->monitor_error.clear();
        }
        if (config_.tracks.size() > max_audio_tracks) {
            setErrorAll("Audio capture supports at most " +
                        std::to_string(max_audio_tracks) + " tracks");
            return false;
        }
        auto validated = validateAudioTracks(config_.tracks);
        if (!validated) {
            setErrorAll("Invalid audio track list: " + validated.error());
            return false;
        }
        config_.tracks = std::move(*validated);
        if (config_.sample_rate < 8'000 || config_.sample_rate > 192'000 ||
            (config_.channels != 1 && config_.channels != 2)) {
            setErrorAll("Audio configuration requires 8-192 kHz and one or two channels");
            return false;
        }

        // With no tracks there is nothing to connect, so skip PipeWire and let
        // the worker idle. Callers that add a track restart the capture.
        if (!config_.tracks.empty()) {
            static std::once_flag pipewire_initialized;
            std::call_once(pipewire_initialized, [] { pw_init(nullptr, nullptr); });

            loop_ = pw_thread_loop_new("fluxrec-audio", nullptr);
            if (!loop_) {
                setErrorAll("Could not create PipeWire thread loop");
                return false;
            }
            context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
            if (!context_) {
                setErrorAll("Could not create PipeWire context");
                releasePipeWire();
                return false;
            }
            core_ = pw_context_connect(context_, nullptr, 0);
            if (!core_) {
                setErrorAll("Could not connect to the PipeWire server");
                releasePipeWire();
                return false;
            }

            for (std::size_t index = 0; index < config_.tracks.size(); ++index) {
                if (!createStream(index)) {
                    releasePipeWire();
                    return false;
                }
                if (streamProperties(config_.tracks[index]).open_monitor) {
                    createMonitor(index);
                }
            }
        }

        worker_ = std::jthread([this](std::stop_token token) { workerMain(token); });
        if (loop_ && pw_thread_loop_start(loop_) < 0) {
            setErrorAll("Could not start PipeWire thread loop");
            worker_.request_stop();
            wake_.notify_all();
            if (worker_.joinable()) worker_.join();
            releasePipeWire();
            return false;
        }
        running_.store(true, std::memory_order_release);
        return true;
    }

    void stop() noexcept {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (!loop_ && !worker_.joinable()) {
            running_.store(false, std::memory_order_release);
            for (auto& runtime : runtimes_) {
                runtime->state.store(SourceState::stopped, std::memory_order_release);
            }
            return;
        }
        running_.store(false, std::memory_order_release);
        if (loop_) {
            pw_thread_loop_lock(loop_);
            destroyStreams();
            if (core_) {
                pw_core_disconnect(core_);
                core_ = nullptr;
            }
            if (context_) {
                pw_context_destroy(context_);
                context_ = nullptr;
            }
            pw_thread_loop_unlock(loop_);
            pw_thread_loop_stop(loop_);
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
        }
        if (worker_.joinable()) {
            worker_.request_stop();
            wake_.notify_all();
            worker_.join();
        }
        for (auto& runtime : runtimes_) {
            runtime->state.store(SourceState::stopped, std::memory_order_release);
            runtime->monitor_streaming.store(false, std::memory_order_release);
        }
    }

    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t trackCount() const noexcept { return runtimes_.size(); }

    void setVolume(std::size_t track_index, float gain) noexcept {
        if (track_index >= runtimes_.size()) return;
        if (std::isnan(gain)) gain = 1.0F;
        runtimes_[track_index]->volume.store(std::clamp(gain, 0.0F, 4.0F),
                                             std::memory_order_release);
    }

    void setMuted(std::size_t track_index, bool muted) noexcept {
        if (track_index >= runtimes_.size()) return;
        runtimes_[track_index]->muted.store(muted, std::memory_order_release);
    }

    SourceSnapshot snapshot(std::size_t track_index) const {
        if (track_index >= runtimes_.size()) return {};
        const auto& runtime = *runtimes_[track_index];
        SourceSnapshot result;
        result.state = runtime.state.load(std::memory_order_acquire);
        result.meter.peak = runtime.meter_peak.load(std::memory_order_relaxed);
        result.meter.rms = runtime.meter_rms.load(std::memory_order_relaxed);
        result.meter.samples = runtime.meter_samples.load(std::memory_order_relaxed);
        result.volume = runtime.volume.load(std::memory_order_acquire);
        result.muted = runtime.muted.load(std::memory_order_acquire);
        result.buffers_received = runtime.buffers_received.load(std::memory_order_relaxed);
        result.buffers_dropped = runtime.buffers_dropped.load(std::memory_order_relaxed);
        result.samples_captured = runtime.samples_captured.load(std::memory_order_relaxed);
        const auto clock = runtime.pipewire_clock_metrics.snapshot();
        result.pipewire_native_timestamp_buffers = clock.native_timestamp_buffers;
        result.monotonic_fallback_timestamp_buffers = clock.fallback_timestamp_buffers;
        result.invalid_pipewire_time_metadata_buffers = clock.invalid_time_metadata_buffers;
        result.pipewire_graph_clock_samples = clock.usable_graph_clock_samples;
        result.pipewire_graph_clock_unusable_samples =
            clock.unusable_graph_clock_samples;
        result.pipewire_rate_window_resets = clock.rate_window_resets;
        result.pipewire_rate_window_samples = clock.rate_window_samples;
        result.pipewire_graph_elapsed_ns = clock.graph_elapsed_ns;
        result.pipewire_monotonic_elapsed_ns = clock.monotonic_elapsed_ns;
        result.pipewire_graph_vs_monotonic_ppm_milli =
            clock.graph_vs_monotonic_ppm_milli;
        result.pipewire_graph_vs_monotonic_valid = clock.graph_vs_monotonic_valid;
        {
            std::lock_guard error_lock(runtime.error_mutex);
            result.error = runtime.error;
            result.monitor_error = runtime.monitor_error;
        }
        return result;
    }

private:
    struct StreamUserData {
        Impl* owner{};
        std::size_t track_index{};
    };

    static void onStateChanged(void* data, pw_stream_state old_state,
                               pw_stream_state state, const char* error) noexcept {
        (void)old_state;
        auto& user = *static_cast<StreamUserData*>(data);
        auto& runtime = *user.owner->runtimes_[user.track_index];
        if (state == PW_STREAM_STATE_STREAMING) {
            if (runtime.state.load(std::memory_order_acquire) != SourceState::error) {
                runtime.state.store(SourceState::streaming, std::memory_order_release);
            }
        } else if (state == PW_STREAM_STATE_ERROR) {
            runtime.state.store(SourceState::error, std::memory_order_release);
            user.owner->setError(user.track_index,
                                 error ? error : "PipeWire stream entered an error state");
            runtime.error_pending.store(true, std::memory_order_release);
            user.owner->wake_.notify_one();
        } else if (state == PW_STREAM_STATE_CONNECTING || state == PW_STREAM_STATE_PAUSED) {
            if (runtime.state.load(std::memory_order_acquire) != SourceState::error) {
                runtime.state.store(SourceState::connecting, std::memory_order_release);
            }
        }
    }

    static void onProcess(void* data) noexcept {
        auto& user = *static_cast<StreamUserData*>(data);
        user.owner->process(user.track_index);
    }

    static const pw_stream_events& streamEvents() {
        static const pw_stream_events events = [] {
            pw_stream_events value{};
            value.version = PW_VERSION_STREAM_EVENTS;
            value.state_changed = onStateChanged;
            value.process = onProcess;
            return value;
        }();
        return events;
    }

    // A monitor stream only plays back what the worker already gained. It
    // never changes the track's capture state, so losing it degrades to a
    // silent monitor rather than a failed recording.
    static void onMonitorStateChanged(void* data, pw_stream_state old_state,
                                      pw_stream_state state, const char* error) noexcept {
        (void)old_state;
        auto& user = *static_cast<StreamUserData*>(data);
        auto& runtime = *user.owner->runtimes_[user.track_index];
        runtime.monitor_streaming.store(state == PW_STREAM_STATE_STREAMING,
                                        std::memory_order_release);
        if (state == PW_STREAM_STATE_ERROR) {
            user.owner->setMonitorError(
                user.track_index,
                error ? error : "PipeWire monitor stream entered an error state");
        }
    }

    static void onMonitorProcess(void* data) noexcept {
        auto& user = *static_cast<StreamUserData*>(data);
        user.owner->monitorProcess(user.track_index);
    }

    static const pw_stream_events& monitorEvents() {
        static const pw_stream_events events = [] {
            pw_stream_events value{};
            value.version = PW_VERSION_STREAM_EVENTS;
            value.state_changed = onMonitorStateChanged;
            value.process = onMonitorProcess;
            return value;
        }();
        return events;
    }

    [[nodiscard]] const spa_pod* buildFormat(spa_pod_builder& builder,
                                             spa_audio_info_raw& info) const noexcept {
        info.format = SPA_AUDIO_FORMAT_F32_LE;
        info.rate = config_.sample_rate;
        info.channels = config_.channels;
        if (config_.channels == 1) {
            info.position[0] = SPA_AUDIO_CHANNEL_MONO;
        } else {
            info.position[0] = SPA_AUDIO_CHANNEL_FL;
            info.position[1] = SPA_AUDIO_CHANNEL_FR;
        }
        return spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
    }

    // PulseAudio exposes a sink monitor as `<sink-name>.monitor`, but the
    // native PipeWire graph targets the sink node and selects its monitor
    // output with stream.capture.sink. streamProperties() strips that alias,
    // because passing it as a native target.object can fail to resolve, after
    // which AUTOCONNECT may link this input stream to the default microphone.
    bool createStream(std::size_t index) {
        const auto& track = config_.tracks[index];
        const auto track_properties = streamProperties(track);
        stream_data_[index] = StreamUserData{this, index};
        const bool sink_monitor = track.input_kind == InputKind::sink_monitor;
        const auto node_name = "fluxrec.audio." + track.id;
        const auto description = "Fluxrec " + track.name;
        auto* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, sink_monitor ? "Game" : "Communication",
            PW_KEY_MEDIA_CLASS, "Stream/Input/Audio",
            PW_KEY_NODE_NAME, node_name.c_str(),
            PW_KEY_NODE_DESCRIPTION, description.c_str(),
            PW_KEY_STREAM_CAPTURE_SINK, track_properties.capture_sink ? "true" : "false",
            PW_KEY_STREAM_MONITOR, "true",
            nullptr);
        if (!properties) {
            setError(index, "Could not allocate PipeWire stream properties");
            return false;
        }
        if (!track_properties.target_object.empty()) {
            pw_properties_set(properties, PW_KEY_TARGET_OBJECT,
                              track_properties.target_object.c_str());
        }
        streams_[index] = pw_stream_new(core_, description.c_str(), properties);
        if (!streams_[index]) {
            setError(index, "Could not create PipeWire audio stream");
            return false;
        }
        pw_stream_add_listener(streams_[index], &stream_hooks_[index], &streamEvents(),
                               &stream_data_[index]);

        spa_audio_info_raw info{};
        std::array<std::uint8_t, 1024> pod_buffer{};
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer.data(), pod_buffer.size());
        const spa_pod* params[] = {buildFormat(builder, info)};
        const auto result = pw_stream_connect(streams_[index], PW_DIRECTION_INPUT, PW_ID_ANY,
                                              static_cast<pw_stream_flags>(
                                                  PW_STREAM_FLAG_AUTOCONNECT |
                                                  PW_STREAM_FLAG_MAP_BUFFERS),
                                              params, 1);
        if (result < 0) {
            setError(index, "Could not connect PipeWire audio stream: " +
                            std::string(spa_strerror(result)));
            return false;
        }
        runtimes_[index]->state.store(SourceState::connecting, std::memory_order_release);
        return true;
    }

    // Opens the track's playback stream toward its chosen monitor sink. Every
    // failure here is recorded on the track and left behind, so capture
    // startup continues with a silent monitor.
    void createMonitor(std::size_t index) {
        const auto& track = config_.tracks[index];
        const auto track_properties = streamProperties(track);
        monitor_data_[index] = StreamUserData{this, index};
        const auto node_name = "fluxrec.audio." + track.id + ".monitor";
        const auto description = "Fluxrec " + track.name + " monitor";
        auto* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Production",
            PW_KEY_MEDIA_CLASS, "Stream/Output/Audio",
            PW_KEY_NODE_NAME, node_name.c_str(),
            PW_KEY_NODE_DESCRIPTION, description.c_str(),
            PW_KEY_TARGET_OBJECT, track_properties.monitor_object.c_str(),
            PW_KEY_STREAM_IS_LIVE, "true",
            nullptr);
        if (!properties) {
            setMonitorError(index, "Could not allocate PipeWire monitor properties");
            return;
        }
        monitor_streams_[index] = pw_stream_new(core_, description.c_str(), properties);
        if (!monitor_streams_[index]) {
            setMonitorError(index, "Could not create PipeWire monitor stream");
            return;
        }
        pw_stream_add_listener(monitor_streams_[index], &monitor_hooks_[index],
                               &monitorEvents(), &monitor_data_[index]);

        spa_audio_info_raw info{};
        std::array<std::uint8_t, 1024> pod_buffer{};
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer.data(), pod_buffer.size());
        const spa_pod* params[] = {buildFormat(builder, info)};
        const auto result = pw_stream_connect(monitor_streams_[index], PW_DIRECTION_OUTPUT,
                                              PW_ID_ANY,
                                              static_cast<pw_stream_flags>(
                                                  PW_STREAM_FLAG_AUTOCONNECT |
                                                  PW_STREAM_FLAG_MAP_BUFFERS),
                                              params, 1);
        if (result < 0) {
            setMonitorError(index, "Could not connect PipeWire monitor stream: " +
                                   std::string(spa_strerror(result)));
            pw_stream_destroy(monitor_streams_[index]);
            monitor_streams_[index] = nullptr;
        }
    }

    void process(std::size_t index) noexcept {
        auto* stream = streams_[index];
        if (!stream) return;
        auto& runtime = *runtimes_[index];
        // A stream that has failed still has to recycle its buffers, but its
        // samples no longer reach the encoder. Other tracks keep recording.
        const bool failed = runtime.state.load(std::memory_order_acquire) == SourceState::error;
        while (auto* pw_buffer = pw_stream_dequeue_buffer(stream)) {
            if (failed) {
                pw_stream_queue_buffer(stream, pw_buffer);
                continue;
            }
            auto* spa_buffer = pw_buffer->buffer;
            if (!spa_buffer || spa_buffer->n_datas == 0 || !spa_buffer->datas) {
                pw_stream_queue_buffer(stream, pw_buffer);
                continue;
            }
            auto& data = spa_buffer->datas[0];
            const auto frame_bytes = config_.channels * sizeof(float);
            if (!data.data || !data.chunk || data.chunk->size == 0 || frame_bytes == 0) {
                pw_stream_queue_buffer(stream, pw_buffer);
                continue;
            }
            const auto* bytes = static_cast<const std::uint8_t*>(data.data) + data.chunk->offset;
            const auto frames = data.chunk->size / frame_bytes;
            if (frames == 0 || frames > kMaxFramesPerBuffer) {
                runtime.buffers_dropped.fetch_add(1, std::memory_order_relaxed);
                pw_stream_queue_buffer(stream, pw_buffer);
                continue;
            }
            std::array<float, kMaxFramesPerBuffer * kMaxChannels> converted{};
            const auto sample_count = static_cast<std::size_t>(frames) * config_.channels;
            for (std::size_t sample = 0; sample < sample_count; ++sample) {
                std::uint32_t raw{};
                std::memcpy(&raw, bytes + sample * sizeof(float), sizeof(raw));
                if constexpr (std::endian::native == std::endian::big) raw = std::byteswap(raw);
                converted[sample] = std::bit_cast<float>(raw);
            }

            pw_time time{};
            const int time_result = pw_stream_get_time_n(stream, &time, sizeof(time));
            const auto callback_monotonic_ns = monotonicNowNs();
            auto timestamp = callback_monotonic_ns;
            bool used_pipewire_time = false;
            if (time_result == 0 && pipewire_time_can_stamp(
                    time.now, time.rate.num, time.rate.denom)) {
                const long double tick_ns = static_cast<long double>(time.rate.num) * 1'000'000'000.0L /
                                            static_cast<long double>(time.rate.denom);
                const long double delay_ns = static_cast<long double>(time.delay) * tick_ns;
                const long double buffer_ns = static_cast<long double>(frames) * 1'000'000'000.0L /
                                              static_cast<long double>(config_.sample_rate);
                const long double start = static_cast<long double>(time.now) - delay_ns - buffer_ns;
                if (start >= 0 && start <= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
                    timestamp = static_cast<std::uint64_t>(start);
                    runtime.pipewire_clock_metrics.record_native_timestamp();
                    runtime.pipewire_clock_metrics.record_graph_clock_sample(
                        time.ticks, time.rate.num, time.rate.denom,
                        callback_monotonic_ns);
                    used_pipewire_time = true;
                }
            }
            if (!used_pipewire_time) {
                runtime.pipewire_clock_metrics.record_fallback(time_result == 0);
                const auto duration = static_cast<std::uint64_t>(
                    (static_cast<long double>(frames) * 1'000'000'000.0L) / config_.sample_rate);
                timestamp = timestamp > duration ? timestamp - duration : 0;
            }


            runtime.buffers_received.fetch_add(1, std::memory_order_relaxed);
            runtime.samples_captured.fetch_add(frames, std::memory_order_relaxed);
            if (!runtime.queue.push(converted.data(), static_cast<std::uint32_t>(frames),
                                    config_.channels, timestamp)) {
                runtime.buffers_dropped.fetch_add(1, std::memory_order_relaxed);
            } else {
                wake_.notify_one();
            }
            pw_stream_queue_buffer(stream, pw_buffer);
        }
    }

    // Plays whatever the worker has already gained for this track. It never
    // touches the capture queue, so a monitored track is not encoded twice.
    void monitorProcess(std::size_t index) noexcept {
        auto* stream = monitor_streams_[index];
        if (!stream) return;
        auto& runtime = *runtimes_[index];
        while (auto* pw_buffer = pw_stream_dequeue_buffer(stream)) {
            auto* spa_buffer = pw_buffer->buffer;
            if (!spa_buffer || spa_buffer->n_datas == 0 || !spa_buffer->datas) {
                pw_stream_queue_buffer(stream, pw_buffer);
                continue;
            }
            auto& data = spa_buffer->datas[0];
            const auto frame_bytes = config_.channels * sizeof(float);
            if (!data.data || !data.chunk || frame_bytes == 0 || data.maxsize < frame_bytes) {
                pw_stream_queue_buffer(stream, pw_buffer);
                continue;
            }
            const auto capacity_frames = data.maxsize / frame_bytes;
            auto frames = pw_buffer->requested > 0
                ? std::min<std::uint64_t>(pw_buffer->requested, capacity_frames)
                : capacity_frames;
            frames = std::min<std::uint64_t>(frames, kMaxFramesPerBuffer);
            if (frames == 0) {
                pw_stream_queue_buffer(stream, pw_buffer);
                continue;
            }
            const auto sample_count = static_cast<std::size_t>(frames) * config_.channels;
            std::array<float, kMaxFramesPerBuffer * kMaxChannels> gained{};
            runtime.monitor_ring.pull(gained.data(), sample_count);
            auto* bytes = static_cast<std::uint8_t*>(data.data);
            for (std::size_t sample = 0; sample < sample_count; ++sample) {
                auto raw = std::bit_cast<std::uint32_t>(gained[sample]);
                if constexpr (std::endian::native == std::endian::big) raw = std::byteswap(raw);
                std::memcpy(bytes + sample * sizeof(float), &raw, sizeof(raw));
            }
            data.chunk->offset = 0;
            data.chunk->stride = static_cast<std::int32_t>(frame_bytes);
            data.chunk->size = static_cast<std::uint32_t>(sample_count * sizeof(float));
            // PipeWire reports queued audio in the units stored here. For raw
            // audio, the stream API recommends frame counts, not byte counts.
            pw_buffer->size = static_cast<std::uint32_t>(frames);
            pw_stream_queue_buffer(stream, pw_buffer);
        }
    }

    void workerMain(std::stop_token token) {
        const auto track_count = runtimes_.size();
        std::vector<std::unique_ptr<Encoder>> encoders;
        encoders.reserve(track_count);
        for (std::size_t i = 0; i < track_count; ++i) {
            encoders.push_back(std::make_unique<Encoder>(
                i, config_, config_.on_format, config_.on_packet,
                [this, i](std::string message) { setError(i, std::move(message)); }));
            if (!encoders[i]->open()) {
                runtimes_[i]->state.store(SourceState::error, std::memory_order_release);
            }
        }

        CaptureBlock block;
        while (true) {
            bool did_work = false;
            for (std::size_t i = 0; i < track_count; ++i) {
                auto& runtime = *runtimes_[i];
                while (runtime.queue.pop(block, config_.channels)) {
                    did_work = true;
                    const auto gain = runtime.volume.load(std::memory_order_acquire);
                    const auto muted = runtime.muted.load(std::memory_order_acquire);
                    const auto count = static_cast<std::size_t>(block.frames) * config_.channels;
                    double square_sum = 0.0;
                    float peak = 0.0F;
                    for (std::size_t sample = 0; sample < count; ++sample) {
                        const float value = std::clamp(muted ? 0.0F : block.samples[sample] * gain,
                                                       -1.0F, 1.0F);
                        block.samples[sample] = value;
                        peak = std::max(peak, std::abs(value));
                        square_sum += static_cast<double>(value) * value;
                    }
                    runtime.meter_peak.store(peak, std::memory_order_relaxed);
                    runtime.meter_rms.store(count == 0 ? 0.0F :
                                                static_cast<float>(std::sqrt(
                                                    square_sum / static_cast<double>(count))),
                                            std::memory_order_relaxed);
                    runtime.meter_samples.fetch_add(count, std::memory_order_relaxed);
                    // The monitor hears exactly these gained samples. The
                    // encoder still consumes only the capture queue, so
                    // monitoring never produces a second encoded packet.
                    if (runtime.monitor_streaming.load(std::memory_order_acquire)) {
                        runtime.monitor_ring.push(block.samples.data(), count);
                    }
                    if (encoders[i]) encoders[i]->consume(block, 1.0F, false);
                }
                if (runtime.error_pending.exchange(false, std::memory_order_acq_rel) && config_.on_error) {
                    std::string message;
                    {
                        std::lock_guard error_lock(runtime.error_mutex);
                        message = runtime.error;
                    }
                    try {
                        config_.on_error(i, std::move(message));
                    } catch (...) {
                        // User callbacks are isolated from the audio worker.
                    }
                }
            }
            if (token.stop_requested() && queuesEmpty()) break;
            if (!did_work) {
                std::unique_lock lock(wake_mutex_);
                wake_.wait_for(lock, token, std::chrono::milliseconds(20), [this] {
                    return !queuesEmpty() || hasPendingErrors();
                });
            }
        }
        for (auto& encoder : encoders) {
            if (encoder) encoder->flush();
        }
    }

    [[nodiscard]] bool queuesEmpty() const noexcept {
        return std::ranges::all_of(runtimes_, [](const auto& runtime) {
            return runtime->queue.empty();
        });
    }

    [[nodiscard]] bool hasPendingErrors() const noexcept {
        return std::ranges::any_of(runtimes_, [](const auto& runtime) {
            return runtime->error_pending.load(std::memory_order_acquire);
        });
    }

    void setError(std::size_t track_index, std::string message) noexcept {
        if (track_index >= runtimes_.size()) return;
        auto& runtime = *runtimes_[track_index];
        runtime.state.store(SourceState::error, std::memory_order_release);
        {
            std::lock_guard error_lock(runtime.error_mutex);
            runtime.error = std::move(message);
        }
        runtime.error_pending.store(true, std::memory_order_release);
        wake_.notify_one();
    }

    void setMonitorError(std::size_t track_index, std::string message) noexcept {
        if (track_index >= runtimes_.size()) return;
        auto& runtime = *runtimes_[track_index];
        std::lock_guard error_lock(runtime.error_mutex);
        runtime.monitor_error = std::move(message);
    }

    void setErrorAll(const std::string& message) noexcept {
        for (std::size_t index = 0; index < runtimes_.size(); ++index) {
            setError(index, message);
        }
    }

    void destroyStreams() noexcept {
        for (auto& stream : monitor_streams_) {
            if (stream) {
                pw_stream_disconnect(stream);
                pw_stream_destroy(stream);
                stream = nullptr;
            }
        }
        for (auto& stream : streams_) {
            if (stream) {
                pw_stream_disconnect(stream);
                pw_stream_destroy(stream);
                stream = nullptr;
            }
        }
    }

    void releasePipeWire() noexcept {
        if (loop_) {
            destroyStreams();
            if (core_) {
                pw_core_disconnect(core_);
                core_ = nullptr;
            }
            if (context_) {
                pw_context_destroy(context_);
                context_ = nullptr;
            }
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
        }
    }

    Config config_;
    // Sized once in the constructor. PipeWire listeners hold pointers into the
    // hook and user-data vectors, so these must never be resized afterwards.
    std::vector<std::unique_ptr<SourceRuntime>> runtimes_;
    std::vector<pw_stream*> streams_;
    std::vector<pw_stream*> monitor_streams_;
    std::vector<spa_hook> stream_hooks_;
    std::vector<spa_hook> monitor_hooks_;
    std::vector<StreamUserData> stream_data_;
    std::vector<StreamUserData> monitor_data_;
    pw_thread_loop* loop_{};
    pw_context* context_{};
    pw_core* core_{};
    std::atomic<bool> running_{false};
    std::mutex lifecycle_mutex_;
    std::jthread worker_;
    std::mutex wake_mutex_;
    std::condition_variable_any wake_;
};

Capture::Capture(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Capture::~Capture() = default;

bool Capture::start() { return impl_->start(); }
void Capture::stop() noexcept { impl_->stop(); }
bool Capture::running() const noexcept { return impl_->running(); }
void Capture::setVolume(std::size_t track_index, float linear_gain) noexcept {
    impl_->setVolume(track_index, linear_gain);
}
void Capture::setMuted(std::size_t track_index, bool muted) noexcept {
    impl_->setMuted(track_index, muted);
}
SourceSnapshot Capture::snapshot(std::size_t track_index) const {
    return impl_->snapshot(track_index);
}
std::size_t Capture::trackCount() const noexcept { return impl_->trackCount(); }

} // namespace fluxrec::audio
