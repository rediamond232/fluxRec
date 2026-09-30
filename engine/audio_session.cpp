#include "engine/audio_session.hpp"
#include "replay/replay_buffer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace fluxrec::engine {
namespace {

[[nodiscard]] constexpr media::AudioCodec to_mux_codec(audio::Codec codec) noexcept {
    return codec == audio::Codec::opus ? media::AudioCodec::opus
                                       : media::AudioCodec::aac;
}

template<class T>
void atomic_max(std::atomic<T>& destination, T value) noexcept {
    auto observed = destination.load(std::memory_order_relaxed);
    while (observed < value &&
           !destination.compare_exchange_weak(observed, value,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
    }
}

[[nodiscard]] bool has_audio_timebase(const audio::Format& format) noexcept {
    return format.time_base_num > 0 && format.time_base_den > 0 &&
           format.sample_rate > 0 && format.channels > 0;
}

// Validate that the encoded packet PTS can be represented in the signed
// nanosecond clock domain used by muxing. `capture_time_ns` is itself derived
// from this PTS in Encoder::receivePackets(), so comparing the two would be a
// tautology and says nothing about PipeWire capture-clock accuracy.
[[nodiscard]] bool packet_pts_fits_capture_clock(const audio::Packet& packet,
                                                 const audio::Format& format) noexcept {
    if (!has_audio_timebase(format) || packet.pts <= 0 || packet.capture_time_ns == 0 ||
        packet.capture_time_ns > static_cast<std::uint64_t>(
                                     std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    using Wide = __int128_t;
    const Wide numerator = static_cast<Wide>(packet.pts) * format.time_base_num * 1'000'000'000;
    const Wide denominator = format.time_base_den;
    const Wide pts_ns = (numerator + denominator / 2) / denominator;
    return pts_ns > 0 &&
           pts_ns <= static_cast<Wide>(std::numeric_limits<std::int64_t>::max());
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

[[nodiscard]] const char* audio_state_name(audio::SourceState state) noexcept {
    using State = audio::SourceState;
    switch (state) {
    case State::stopped: return "stopped";
    case State::connecting: return "connecting";
    case State::streaming: return "streaming";
    case State::error: return "error";
    }
    return "error";
}

void write_audio_source_fields(std::ostream& output, const audio::SourceSnapshot& source) {
    output << "\"state\":\"" << audio_state_name(source.state) << "\",\"peak\":";
    if (source.meter.samples == 0 || !std::isfinite(source.meter.peak)) {
        output << "null";
    } else {
        output << source.meter.peak;
    }
    output << ",\"rms\":";
    if (source.meter.samples == 0 || !std::isfinite(source.meter.rms)) {
        output << "null";
    } else {
        output << source.meter.rms;
    }
    output << ",\"samples\":" << source.meter.samples
           << ",\"gain\":" << source.volume
           << ",\"muted\":" << (source.muted ? "true" : "false")
           << ",\"buffers_received\":" << source.buffers_received
           << ",\"buffers_dropped\":" << source.buffers_dropped
           << ",\"pipewire_native_timestamp_buffers\":"
           << source.pipewire_native_timestamp_buffers
           << ",\"monotonic_fallback_timestamp_buffers\":"
           << source.monotonic_fallback_timestamp_buffers
           << ",\"invalid_pipewire_time_metadata_buffers\":"
           << source.invalid_pipewire_time_metadata_buffers
           << ",\"pipewire_graph_clock_samples\":"
           << source.pipewire_graph_clock_samples
           << ",\"pipewire_graph_clock_unusable_samples\":"
           << source.pipewire_graph_clock_unusable_samples
           << ",\"pipewire_rate_window_resets\":"
           << source.pipewire_rate_window_resets
           << ",\"pipewire_rate_window_samples\":"
           << source.pipewire_rate_window_samples
           << ",\"pipewire_graph_elapsed_ns\":"
           << source.pipewire_graph_elapsed_ns
           << ",\"pipewire_monotonic_elapsed_ns\":"
           << source.pipewire_monotonic_elapsed_ns
           << ",\"pipewire_graph_vs_monotonic_ppm\":";
    if (source.pipewire_graph_vs_monotonic_valid) {
        output << static_cast<long double>(
                      source.pipewire_graph_vs_monotonic_ppm_milli) / 1000.0L;
    } else {
        output << "null";
    }
}

} // namespace

void writeAudioTelemetryObject(std::ostream& output, const AudioSessionSnapshot& snapshot,
                               bool enabled) {
    output.imbue(std::locale::classic());
    const auto prior_precision = output.precision();
    output << std::setprecision(9);
    if (!enabled) {
        output << "{\"enabled\":false}";
        output << std::setprecision(prior_precision);
        return;
    }
    output << "{\"enabled\":true,\"tracks\":[";
    const auto count = std::min({snapshot.tracks.size(), snapshot.ids.size(),
                                 snapshot.names.size()});
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) output << ',';
        output << "{\"id\":";
        write_json_string(output, snapshot.ids[index]);
        output << ",\"name\":";
        write_json_string(output, snapshot.names[index]);
        output << ',';
        write_audio_source_fields(output, snapshot.tracks[index]);
        output << ",\"error\":";
        write_json_string(output, snapshot.tracks[index].error);
        output << ",\"monitor_error\":";
        write_json_string(output, snapshot.tracks[index].monitor_error);
        output << '}';
    }
    output << "],\"captured_packets\":" << snapshot.captured_packets
           << ",\"callback_drops\":" << snapshot.callback_queue_drops
           << ",\"unforwarded_drops\":" << snapshot.unforwarded_packets_dropped
           << ",\"mux_drops\":" << snapshot.mux_dropped_packets
           << ",\"replay_stored_packets\":" << snapshot.replay_stored_packets
           << ",\"replay_drops\":" << snapshot.replay_dropped_packets
           << ",\"sync_errors\":" << snapshot.sync_errors
           << ",\"audio_errors\":" << snapshot.audio_errors
           << ",\"queue_packets\":" << snapshot.queued_packets
           << ",\"queue_bytes\":" << snapshot.queued_bytes << '}';
    output << std::setprecision(prior_precision);
}

class AudioSession::Impl final {
public:
    explicit Impl(AudioSessionConfig config)
        : config_(std::move(config)),
          formats_(config_.capture.tracks.size()),
          slots_(config_.packet_queue_capacity) {
        if (config_.packet_queue_capacity == 0 ||
            config_.packet_queue_capacity_bytes == 0) {
            throw std::invalid_argument("audio session queue limits must be positive");
        }
        if (config_.mux_retry_delay.count() < 0) {
            throw std::invalid_argument("audio session mux retry delay cannot be negative");
        }

        auto capture_config = config_.capture;
        capture_config.on_format = [this](audio::Format format) {
            handle_format(std::move(format));
        };
        capture_config.on_packet = [this](audio::Packet packet) {
            handle_packet(std::move(packet));
        };
        capture_config.on_error = [this](std::size_t track_index, std::string error) {
            handle_capture_error(track_index, std::move(error));
        };
        capture_ = std::make_unique<audio::Capture>(std::move(capture_config));
    }

    ~Impl() { stop(); }

    bool start(std::chrono::milliseconds timeout) {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        const auto current = state_.load(std::memory_order_acquire);
        if (current == AudioSessionState::ready || current == AudioSessionState::forwarding) {
            return true;
        }
        if (current == AudioSessionState::starting || current == AudioSessionState::stopping) {
            set_error("audio session lifecycle operation is already in progress", false);
            return false;
        }
        if (current == AudioSessionState::failed) {
            set_error("stop the failed audio session before restarting it", false);
            return false;
        }

        reset_format_state();
        audio_errors_.store(0, std::memory_order_release);
        producer_closed_.store(false, std::memory_order_release);
        state_.store(AudioSessionState::starting, std::memory_order_release);
        if (!capture_->start()) {
            const auto& tracks = config_.capture.tracks;
            bool reported = false;
            for (std::size_t index = 0; index < tracks.size(); ++index) {
                const auto snap = capture_->snapshot(index);
                if (!snap.error.empty()) {
                    set_error(tracks[index].name + " audio startup failed: " + snap.error);
                    reported = true;
                    break;
                }
            }
            if (!reported) {
                set_error("PipeWire audio capture failed to start");
            }
            state_.store(AudioSessionState::failed, std::memory_order_release);
            return false;
        }

        std::unique_lock format_lock(format_mutex_);
        const bool finished = format_cv_.wait_for(format_lock, timeout, [this] {
            return formats_ready_locked() || audio_errors_.load(std::memory_order_acquire) != 0;
        });
        const bool ready = finished && formats_ready_locked() &&
                           audio_errors_.load(std::memory_order_acquire) == 0;
        format_lock.unlock();
        if (!ready) {
            if (!finished) {
                set_error("timed out waiting for audio track codec extradata");
            }
            capture_->stop();
            producer_closed_.store(true, std::memory_order_release);
            discard_queued_packets();
            state_.store(AudioSessionState::failed, std::memory_order_release);
            return false;
        }

        state_.store(AudioSessionState::ready, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::vector<mux::AudioStreamConfig> stream_configs() const {
        std::lock_guard format_lock(format_mutex_);
        if (!formats_ready_locked()) return {};
        std::vector<mux::AudioStreamConfig> result;
        result.reserve(formats_.size());
        for (std::size_t index = 0; index < formats_.size(); ++index) {
            const auto& format = *formats_[index];
            mux::AudioStreamConfig stream;
            stream.codec = to_mux_codec(format.codec);
            stream.sample_rate = static_cast<int>(format.sample_rate);
            stream.channels = static_cast<int>(format.channels);
            stream.initial_padding = format.initial_padding;
            stream.bit_rate = config_.capture.bitrate;
            stream.stream_time_base = media::Rational{
                format.time_base_num, format.time_base_den};
            stream.codec_extradata = format.extradata;
            result.push_back(std::move(stream));
        }
        return result;
    }

    bool begin_forwarding(mux::MuxWriter& writer,
                          std::uint64_t video_origin_time_ns,
                          std::chrono::milliseconds writer_timeout) {
        return begin_forwarding(&writer, nullptr, video_origin_time_ns, writer_timeout);
    }

    bool begin_forwarding(mux::MuxWriter* writer,
                          replay::ReplayBuffer* replay_buffer,
                          std::uint64_t video_origin_time_ns,
                          std::chrono::milliseconds writer_timeout) {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        if (state_.load(std::memory_order_acquire) != AudioSessionState::ready) {
            set_error("audio session is not ready to forward packets", false);
            return false;
        }
        if (!writer && !replay_buffer) {
            set_error("audio forwarding needs a mux writer or replay buffer", false);
            return false;
        }
        if (video_origin_time_ns == 0) {
            sync_errors_.fetch_add(1, std::memory_order_relaxed);
            set_error("video origin timestamp is zero", false);
            return false;
        }
        if (video_origin_time_ns > static_cast<std::uint64_t>(
                                       std::numeric_limits<std::int64_t>::max())) {
            sync_errors_.fetch_add(1, std::memory_order_relaxed);
            set_error("video origin timestamp exceeds the signed mux timestamp range", false);
            return false;
        }
        if (writer && (!writer->wait_until_ready(writer_timeout) ||
                       writer->state() != mux::WriterState::running)) {
            set_error("MuxWriter did not become ready: " + writer->error_message(), false);
            return false;
        }

        writer_ = writer;
        replay_buffer_ = replay_buffer;
        video_origin_time_ns_.store(video_origin_time_ns, std::memory_order_release);
        producer_closed_.store(false, std::memory_order_release);
        {
            std::lock_guard error_lock(error_mutex_);
            last_error_.clear();
        }
        state_.store(AudioSessionState::forwarding, std::memory_order_release);
        try {
            forward_worker_ = std::thread([this] { forward_main(); });
        } catch (const std::exception& exception) {
            writer_ = nullptr;
            set_error(std::string("could not start audio mux worker: ") + exception.what());
            state_.store(AudioSessionState::failed, std::memory_order_release);
            return false;
        } catch (...) {
            writer_ = nullptr;
            set_error("could not start audio mux worker");
            state_.store(AudioSessionState::failed, std::memory_order_release);
            return false;
        }
        wake_.notify_one();
        return true;
    }

    void stop() noexcept {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        const auto current = state_.load(std::memory_order_acquire);
        if (current == AudioSessionState::stopped && !forward_worker_.joinable()) return;
        state_.store(AudioSessionState::stopping, std::memory_order_release);

        // Capture::stop joins its encoder worker and delivers its final packets
        // before producer_closed_ is set, so the bridge worker sees the full
        // accepted tail of the compressed audio queue.
        capture_->stop();
        producer_closed_.store(true, std::memory_order_release);
        wake_.notify_all();
        if (forward_worker_.joinable()) {
            try {
                forward_worker_.join();
            } catch (...) {
                set_error("could not join audio mux worker");
            }
        } else {
            discard_queued_packets();
        }
        writer_ = nullptr;
        replay_buffer_ = nullptr;
        state_.store(AudioSessionState::stopped, std::memory_order_release);
    }

    [[nodiscard]] bool set_volume(std::string_view id, float gain) noexcept {
        const auto index = find_track_index(id);
        if (!index) return false;
        capture_->setVolume(*index, gain);
        return true;
    }

    [[nodiscard]] bool set_muted(std::string_view id, bool muted) noexcept {
        const auto index = find_track_index(id);
        if (!index) return false;
        capture_->setMuted(*index, muted);
        return true;
    }

    [[nodiscard]] AudioSessionSnapshot snapshot() const {
        AudioSessionSnapshot result;
        result.state = state_.load(std::memory_order_acquire);
        result.formats_ready = formats_ready();
        result.video_origin_time_ns = video_origin_time_ns_.load(std::memory_order_acquire);
        const auto head = queue_head_.load(std::memory_order_acquire);
        const auto tail = queue_tail_.load(std::memory_order_acquire);
        result.queued_packets = static_cast<std::size_t>(head - tail);
        result.queued_bytes = queued_bytes_.load(std::memory_order_relaxed);
        result.queue_high_water_packets = queue_high_water_packets_.load(std::memory_order_relaxed);
        result.queue_high_water_bytes = queue_high_water_bytes_.load(std::memory_order_relaxed);
        result.captured_packets = captured_packets_.load(std::memory_order_relaxed);
        result.callback_queue_drops = callback_queue_drops_.load(std::memory_order_relaxed);
        result.unforwarded_packets_dropped = unforwarded_packets_dropped_.load(std::memory_order_relaxed);
        result.sync_errors = sync_errors_.load(std::memory_order_relaxed);
        result.mux_accepted_packets = mux_accepted_packets_.load(std::memory_order_relaxed);
        result.mux_dropped_packets = mux_dropped_packets_.load(std::memory_order_relaxed);
        result.mux_busy_retries = mux_busy_retries_.load(std::memory_order_relaxed);
        result.mux_full_retries = mux_full_retries_.load(std::memory_order_relaxed);
        result.replay_stored_packets = replay_stored_packets_.load(std::memory_order_relaxed);
        result.replay_dropped_packets = replay_dropped_packets_.load(std::memory_order_relaxed);
        result.audio_errors = audio_errors_.load(std::memory_order_relaxed);
        const auto& tracks = config_.capture.tracks;
        result.tracks.reserve(tracks.size());
        result.ids.reserve(tracks.size());
        result.names.reserve(tracks.size());
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            result.ids.push_back(tracks[index].id);
            result.names.push_back(tracks[index].name);
            result.tracks.push_back(capture_->snapshot(index));
        }
        {
            std::lock_guard error_lock(error_mutex_);
            result.last_error = last_error_;
        }
        return result;
    }

private:
    [[nodiscard]] std::optional<std::size_t> find_track_index(std::string_view id) const noexcept {
        const auto& tracks = config_.capture.tracks;
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            if (tracks[index].id == id) return index;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string track_label(std::size_t index) const {
        if (index < config_.capture.tracks.size()) {
            return config_.capture.tracks[index].name;
        }
        return "audio";
    }

    void reset_format_state() {
        {
            std::lock_guard format_lock(format_mutex_);
            for (auto& format : formats_) format.reset();
        }
        {
            std::lock_guard error_lock(error_mutex_);
            last_error_.clear();
        }
        video_origin_time_ns_.store(0, std::memory_order_release);
    }

    void handle_format(audio::Format format) {
        if (format.track_index >= formats_.size()) {
            sync_errors_.fetch_add(1, std::memory_order_relaxed);
            set_error("audio encoder published an unknown track format");
            format_cv_.notify_all();
            return;
        }
        const auto index = format.track_index;
        if (!has_audio_timebase(format) || format.extradata.empty() ||
            format.sample_rate > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
            format.channels > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            audio_errors_.fetch_add(1, std::memory_order_relaxed);
            set_error(track_label(index) +
                      " encoder format is missing valid codec extradata or audio parameters");
            format_cv_.notify_all();
            return;
        }
        {
            std::lock_guard format_lock(format_mutex_);
            formats_[index] = std::move(format);
        }
        format_cv_.notify_all();
    }

    void handle_packet(audio::Packet packet) {
        captured_packets_.fetch_add(1, std::memory_order_relaxed);
        if (packet.track_index >= formats_.size()) {
            note_sync_error("encoded audio packet references an unknown track");
            return;
        }
        const auto index = packet.track_index;
        audio::Format format;
        format.track_index = packet.track_index;
        format.codec = config_.capture.codec;
        format.sample_rate = config_.capture.sample_rate;
        format.channels = config_.capture.channels;
        format.time_base_num = 1;
        format.time_base_den = static_cast<std::int32_t>(config_.capture.sample_rate);
        if (packet.codec != format.codec ||
            !packet_pts_fits_capture_clock(packet, format)) {
            note_sync_error("encoded audio packet has invalid PTS or capture-clock metadata");
            return;
        }

        media::EncodedAudioPacket output;
        output.stream_index = index;
        output.data = std::move(packet.data);
        output.codec = to_mux_codec(packet.codec);
        output.pts = packet.pts;
        output.duration = packet.duration;
        output.capture_time_ns = packet.capture_time_ns;
        output.discard_padding_samples = packet.discard_padding_samples;
        output.keyframe = packet.key_frame;
        output.time_base = media::Rational{format.time_base_num, format.time_base_den};
        const auto bytes = output.data.size();
        if (bytes == 0 || bytes > config_.packet_queue_capacity_bytes ||
            !enqueue_packet(std::move(output), bytes)) {
            callback_queue_drops_.fetch_add(1, std::memory_order_relaxed);
        }
        wake_.notify_one();
    }

    void handle_capture_error(std::size_t track_index, std::string error) {
        audio_errors_.fetch_add(1, std::memory_order_relaxed);
        set_error(track_label(track_index) + " audio: " + std::move(error));
        format_cv_.notify_all();
        wake_.notify_one();
    }

    void note_sync_error(std::string message) {
        sync_errors_.fetch_add(1, std::memory_order_relaxed);
        set_error(std::move(message));
    }

    bool enqueue_packet(media::EncodedAudioPacket&& packet, std::size_t bytes) noexcept {
        const auto head = queue_head_.load(std::memory_order_relaxed);
        const auto tail = queue_tail_.load(std::memory_order_acquire);
        if (producer_closed_.load(std::memory_order_acquire) ||
            head - tail >= slots_.size()) {
            return false;
        }

        const auto current_bytes = queued_bytes_.load(std::memory_order_relaxed);
        if (bytes > config_.packet_queue_capacity_bytes ||
            current_bytes > config_.packet_queue_capacity_bytes - bytes) {
            return false;
        }
        // There is one producer and the consumer can only lower this count, so
        // the preceding capacity check cannot race into an over-limit value.
        const auto bytes_before_add = queued_bytes_.fetch_add(bytes, std::memory_order_acq_rel);

        slots_[head % slots_.size()].emplace(std::move(packet));
        queue_head_.store(head + 1U, std::memory_order_release);
        const auto queued_packets = static_cast<std::size_t>(head + 1U - tail);
        atomic_max(queue_high_water_packets_, queued_packets);
        atomic_max(queue_high_water_bytes_, bytes_before_add + bytes);
        return true;
    }

    bool dequeue_packet(media::EncodedAudioPacket& packet) noexcept {
        const auto tail = queue_tail_.load(std::memory_order_relaxed);
        const auto head = queue_head_.load(std::memory_order_acquire);
        if (tail == head) return false;
        auto& slot = slots_[tail % slots_.size()];
        const auto bytes = slot->data.size();
        packet = std::move(*slot);
        slot.reset();
        queue_tail_.store(tail + 1U, std::memory_order_release);
        queued_bytes_.fetch_sub(bytes, std::memory_order_acq_rel);
        return true;
    }

    [[nodiscard]] bool queue_empty() const noexcept {
        return queue_tail_.load(std::memory_order_acquire) ==
               queue_head_.load(std::memory_order_acquire);
    }

    void forward_main() noexcept {
        while (true) {
            media::EncodedAudioPacket packet;
            if (dequeue_packet(packet)) {
                forward_packet(std::move(packet));
                continue;
            }
            if (producer_closed_.load(std::memory_order_acquire)) return;
            std::unique_lock wake_lock(wake_mutex_);
            wake_.wait_for(wake_lock, std::chrono::milliseconds(50), [this] {
                return !queue_empty() || producer_closed_.load(std::memory_order_acquire);
            });
        }
    }

    void forward_packet(media::EncodedAudioPacket packet) noexcept {
        auto* writer = writer_;
        auto* replay_buffer = replay_buffer_;
        std::shared_ptr<const media::EncodedAudioPacket> shared_packet;
        if (replay_buffer) {
            try {
                shared_packet = std::make_shared<const media::EncodedAudioPacket>(
                    std::move(packet));
                const auto origin = video_origin_time_ns_.load(std::memory_order_acquire);
                const auto capture_time = static_cast<std::int64_t>(
                    shared_packet->capture_time_ns);
                const auto origin_time = static_cast<std::int64_t>(origin);
                const auto relative_time = capture_time - origin_time;
                const auto appended = replay_buffer->append_audio(
                    shared_packet, replay::Nanoseconds{relative_time});
                if (appended == replay::AppendResult::stored) {
                    replay_stored_packets_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    replay_dropped_packets_.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (const std::exception& exception) {
                set_error(std::string("could not retain audio packet for replay: ") +
                          exception.what(), false);
                replay_dropped_packets_.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                set_error("could not retain audio packet for replay", false);
                replay_dropped_packets_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!writer) {
            return;
        }
        mux::EnqueueResult result = mux::EnqueueResult::closed;
        try {
            if (shared_packet) {
                result = writer->try_enqueue(shared_packet);
            } else {
                result = writer->try_enqueue(std::move(packet));
            }
            for (unsigned int attempt = 0;
                 (result == mux::EnqueueResult::queue_busy ||
                  result == mux::EnqueueResult::queue_full) &&
                 attempt < config_.mux_retry_count;
                 ++attempt) {
                if (result == mux::EnqueueResult::queue_busy) {
                    mux_busy_retries_.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                } else {
                    mux_full_retries_.fetch_add(1, std::memory_order_relaxed);
                    if (config_.mux_retry_delay.count() > 0) {
                        std::this_thread::sleep_for(config_.mux_retry_delay);
                    }
                }
                if (shared_packet) {
                    result = writer->try_enqueue(shared_packet);
                } else {
                    result = writer->try_enqueue(std::move(packet));
                }
            }
        } catch (const std::exception& exception) {
            set_error(std::string("MuxWriter rejected audio packet: ") + exception.what());
            mux_dropped_packets_.fetch_add(1, std::memory_order_relaxed);
            return;
        } catch (...) {
            set_error("MuxWriter rejected audio packet with an unknown exception");
            mux_dropped_packets_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (result == mux::EnqueueResult::accepted) {
            mux_accepted_packets_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (result == mux::EnqueueResult::invalid_packet) {
            note_sync_error("MuxWriter rejected an audio packet's codec, timebase, or sync metadata");
        }
        mux_dropped_packets_.fetch_add(1, std::memory_order_relaxed);
    }

    void discard_queued_packets() noexcept {
        media::EncodedAudioPacket discarded;
        while (dequeue_packet(discarded)) {
            discarded = {};
            unforwarded_packets_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool formats_ready_locked() const noexcept {
        return std::all_of(formats_.begin(), formats_.end(), [](const auto& format) {
            return format.has_value() && !format->extradata.empty();
        });
    }

    [[nodiscard]] bool formats_ready() const {
        std::lock_guard format_lock(format_mutex_);
        return formats_ready_locked();
    }

    void set_error(std::string message, bool mark_failed = true) noexcept {
        {
            std::lock_guard error_lock(error_mutex_);
            last_error_ = std::move(message);
        }
        if (mark_failed) {
            const auto current = state_.load(std::memory_order_acquire);
            if (current != AudioSessionState::stopping && current != AudioSessionState::stopped) {
                state_.store(AudioSessionState::failed, std::memory_order_release);
            }
        }
    }

    AudioSessionConfig config_;
    std::unique_ptr<audio::Capture> capture_;
    std::vector<std::optional<audio::Format>> formats_;
    std::vector<std::optional<media::EncodedAudioPacket>> slots_;

    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex format_mutex_;
    std::condition_variable format_cv_;

    mutable std::mutex error_mutex_;
    std::string last_error_;

    std::atomic<AudioSessionState> state_{AudioSessionState::stopped};
    std::atomic<bool> producer_closed_{true};
    std::atomic<std::uint64_t> video_origin_time_ns_{0};
    std::atomic<std::uint64_t> queue_head_{0};
    std::atomic<std::uint64_t> queue_tail_{0};
    std::atomic<std::size_t> queued_bytes_{0};
    std::atomic<std::size_t> queue_high_water_packets_{0};
    std::atomic<std::size_t> queue_high_water_bytes_{0};
    std::atomic<std::uint64_t> captured_packets_{0};
    std::atomic<std::uint64_t> callback_queue_drops_{0};
    std::atomic<std::uint64_t> unforwarded_packets_dropped_{0};
    std::atomic<std::uint64_t> sync_errors_{0};
    std::atomic<std::uint64_t> mux_accepted_packets_{0};
    std::atomic<std::uint64_t> mux_dropped_packets_{0};
    std::atomic<std::uint64_t> mux_busy_retries_{0};
    std::atomic<std::uint64_t> mux_full_retries_{0};
    std::atomic<std::uint64_t> replay_stored_packets_{0};
    std::atomic<std::uint64_t> replay_dropped_packets_{0};
    std::atomic<std::uint64_t> audio_errors_{0};
    std::mutex wake_mutex_;
    std::condition_variable wake_;
    mux::MuxWriter* writer_{};
    replay::ReplayBuffer* replay_buffer_{};
    std::thread forward_worker_;
};

AudioSession::AudioSession(AudioSessionConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

AudioSession::~AudioSession() = default;

bool AudioSession::start(std::chrono::milliseconds format_timeout) {
    return impl_->start(format_timeout);
}

std::vector<mux::AudioStreamConfig> AudioSession::stream_configs() const {
    return impl_->stream_configs();
}

bool AudioSession::begin_forwarding(mux::MuxWriter& writer,
                                    std::uint64_t video_origin_time_ns,
                                    std::chrono::milliseconds writer_timeout) {
    return impl_->begin_forwarding(writer, video_origin_time_ns, writer_timeout);
}

bool AudioSession::begin_forwarding(mux::MuxWriter* writer,
                                    replay::ReplayBuffer* replay_buffer,
                                    std::uint64_t video_origin_time_ns,
                                    std::chrono::milliseconds writer_timeout) {
    return impl_->begin_forwarding(writer, replay_buffer, video_origin_time_ns,
                                   writer_timeout);
}

void AudioSession::stop() noexcept { impl_->stop(); }

bool AudioSession::set_volume(std::string_view id, float linear_gain) noexcept {
    return impl_->set_volume(id, linear_gain);
}

bool AudioSession::set_muted(std::string_view id, bool muted) noexcept {
    return impl_->set_muted(id, muted);
}

AudioSessionSnapshot AudioSession::snapshot() const { return impl_->snapshot(); }

} // namespace fluxrec::engine
