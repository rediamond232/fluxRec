#include "mux_writer.hpp"
#include "detail/timestamp_rebaser.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}

namespace fluxrec::mux {
namespace {

[[nodiscard]] AVCodecID to_av_codec_id(media::VideoCodec codec) {
    switch (codec) {
    case media::VideoCodec::h264:
        return AV_CODEC_ID_H264;
    case media::VideoCodec::hevc:
        return AV_CODEC_ID_HEVC;
    }
    throw std::invalid_argument("unsupported video codec");
}

[[nodiscard]] AVCodecID to_av_audio_codec_id(media::AudioCodec codec) {
    switch (codec) {
    case media::AudioCodec::aac:
        return AV_CODEC_ID_AAC;
    case media::AudioCodec::opus:
        return AV_CODEC_ID_OPUS;
    }
    throw std::invalid_argument("unsupported audio codec");
}

[[nodiscard]] AVRational to_av_rational(media::Rational value) {
    return AVRational{value.num, value.den};
}

[[nodiscard]] std::string av_error_string(int error) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

void check_av(int result, const char* operation) {
    if (result < 0) {
        throw std::runtime_error(std::string(operation) + ": " + av_error_string(result));
    }
}

[[nodiscard]] std::uint64_t elapsed_ns(std::chrono::steady_clock::duration duration) {
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    return value < 0 ? 0U : static_cast<std::uint64_t>(value);
}

template <typename T>
void atomic_max(std::atomic<T>& target, T value) noexcept {
    auto current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

} // namespace

struct MuxWriter::Impl {
    struct QueuedPacket {
        media::EncodedPacket owned;
        media::EncodedAudioPacket audio;
        std::shared_ptr<const media::EncodedPacket> shared;
        std::shared_ptr<const media::EncodedAudioPacket> shared_audio;
        bool is_audio{false};
        std::chrono::steady_clock::time_point accepted_at{};
        std::chrono::steady_clock::time_point dequeued_at{};
        std::size_t bytes{0};

        [[nodiscard]] const media::EncodedPacket& packet() const noexcept {
            return shared ? *shared : owned;
        }

        [[nodiscard]] const media::EncodedAudioPacket& audio_packet() const noexcept {
            return shared_audio ? *shared_audio : audio;
        }

        void clear() noexcept {
            owned = {};
            audio = {};
            shared.reset();
            shared_audio.reset();
            is_audio = false;
            accepted_at = {};
            dequeued_at = {};
            bytes = 0;
        }
    };

    explicit Impl(MuxConfig value) : config(std::move(value)) {
        validate_config();
        slots.resize(config.queue_capacity_packets +
                     (config.audio_streams.empty() ? 0U : 1U));
        worker = std::thread([this] { run(); });
    }

    ~Impl() {
        close();
    }

    void validate_config() const {
        if (config.output_path.empty()) {
            throw std::invalid_argument("mux output path must not be empty");
        }
        if (config.width <= 0 || config.height <= 0) {
            throw std::invalid_argument("mux video dimensions must be positive");
        }
        if (!config.stream_time_base.valid()) {
            throw std::invalid_argument("mux stream timebase must be positive");
        }
        if (config.frame_rate.num != 0 && !config.frame_rate.valid()) {
            throw std::invalid_argument("mux frame rate must be positive when set");
        }
        if (config.queue_capacity_packets == 0 || config.queue_capacity_bytes == 0) {
            throw std::invalid_argument("mux queue limits must be positive");
        }
        if (!config.audio_streams.empty() &&
            config.queue_capacity_packets == std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("mux packet queue capacity is too large");
        }
        if (!config.audio_streams.empty() &&
            config.queue_capacity_bytes > std::numeric_limits<std::size_t>::max() / 2U) {
            throw std::invalid_argument("mux byte queue capacity is too large");
        }
        if (config.codec_extradata.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE)) {
            throw std::invalid_argument("codec extradata is too large for FFmpeg");
        }
        if (config.audio_streams.size() > 8U) {
            throw std::invalid_argument("mux supports at most eight audio streams");
        }
        if (!config.audio_streams.empty() && config.video_origin_time_ns == 0) {
            throw std::invalid_argument("audio muxing requires a monotonic video origin timestamp");
        }
        for (const auto& audio : config.audio_streams) {
            if (audio.sample_rate <= 0 || audio.channels <= 0 || audio.channels > 64) {
                throw std::invalid_argument("audio sample rate and channel count must be positive");
            }
            if (audio.initial_padding < 0) {
                throw std::invalid_argument("audio initial padding must not be negative");
            }
            if (audio.bit_rate < 0 || !audio.stream_time_base.valid()) {
                throw std::invalid_argument("audio bitrate or stream timebase is invalid");
            }
            if (audio.codec_extradata.size() >
                static_cast<std::size_t>(std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE)) {
                throw std::invalid_argument("audio codec extradata is too large for FFmpeg");
            }
            (void)to_av_audio_codec_id(audio.codec);
        }
        (void)to_av_codec_id(config.codec);
    }

    EnqueueResult enqueue_owned(media::EncodedPacket&& packet) {
        if (!validate_packet(packet)) {
            invalid_packets.fetch_add(1, std::memory_order_relaxed);
            return EnqueueResult::invalid_packet;
        }
        if (packet.data.size() > config.queue_capacity_bytes) {
            return EnqueueResult::packet_too_large;
        }

        std::unique_lock lock(queue_mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            queue_busy_drops.fetch_add(1, std::memory_order_relaxed);
            return EnqueueResult::queue_busy;
        }
        const auto current_state = writer_state.load(std::memory_order_acquire);
        if (current_state == WriterState::starting) {
            return EnqueueResult::not_ready;
        }
        if (current_state != WriterState::running || close_requested) {
            return EnqueueResult::closed;
        }
        const bool reserved_first_video = !config.audio_streams.empty() &&
                                          !first_video_enqueued;
        const auto packet_limit = config.queue_capacity_packets +
                                  (reserved_first_video ? 1U : 0U);
        if (slot_count >= packet_limit ||
            (!reserved_first_video &&
             (queued_bytes > config.queue_capacity_bytes ||
              packet.data.size() > config.queue_capacity_bytes - queued_bytes)) ||
            (reserved_first_video && queued_bytes > config.queue_capacity_bytes)) {
            queue_full_drops.fetch_add(1, std::memory_order_relaxed);
            return EnqueueResult::queue_full;
        }

        auto& slot = slots[write_index];
        slot.owned = std::move(packet);
        slot.audio = {};
        slot.shared.reset();
        slot.shared_audio.reset();
        slot.is_audio = false;
        slot.bytes = slot.owned.data.size();
        slot.accepted_at = std::chrono::steady_clock::now();
        queued_bytes += slot.bytes;
        ++slot_count;
        write_index = (write_index + 1U) % slots.size();
        if (!first_video_enqueued) first_video_enqueued = true;
        record_enqueue(slot.bytes);
        lock.unlock();
        queue_cv.notify_one();
        return EnqueueResult::accepted;
    }

    EnqueueResult enqueue_shared(const std::shared_ptr<const media::EncodedPacket>& packet) {
        if (!packet || !validate_packet(*packet)) {
            invalid_packets.fetch_add(1, std::memory_order_relaxed);
            return EnqueueResult::invalid_packet;
        }
        if (packet->data.size() > config.queue_capacity_bytes) {
            return EnqueueResult::packet_too_large;
        }

        std::unique_lock lock(queue_mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            queue_busy_drops.fetch_add(1, std::memory_order_relaxed);
            return EnqueueResult::queue_busy;
        }
        const auto current_state = writer_state.load(std::memory_order_acquire);
        if (current_state == WriterState::starting) {
            return EnqueueResult::not_ready;
        }
        if (current_state != WriterState::running || close_requested) {
            return EnqueueResult::closed;
        }
        const bool reserved_first_video = !config.audio_streams.empty() &&
                                          !first_video_enqueued;
        const auto packet_limit = config.queue_capacity_packets +
                                  (reserved_first_video ? 1U : 0U);
        if (slot_count >= packet_limit ||
            (!reserved_first_video &&
             (queued_bytes > config.queue_capacity_bytes ||
              packet->data.size() > config.queue_capacity_bytes - queued_bytes)) ||
            (reserved_first_video && queued_bytes > config.queue_capacity_bytes)) {
            queue_full_drops.fetch_add(1, std::memory_order_relaxed);
            return EnqueueResult::queue_full;
        }

        auto& slot = slots[write_index];
        slot.owned = {};
        slot.audio = {};
        slot.shared = packet;
        slot.shared_audio.reset();
        slot.is_audio = false;
        slot.bytes = slot.shared->data.size();
        slot.accepted_at = std::chrono::steady_clock::now();
        queued_bytes += slot.bytes;
        ++slot_count;
        write_index = (write_index + 1U) % slots.size();
        if (!first_video_enqueued) first_video_enqueued = true;
        record_enqueue(slot.bytes);
        lock.unlock();
        queue_cv.notify_one();
        return EnqueueResult::accepted;
    }

    EnqueueResult enqueue_audio(media::EncodedAudioPacket&& packet) {
        if (!validate_audio_packet(packet)) {
            invalid_packets.fetch_add(1, std::memory_order_relaxed);
            return EnqueueResult::invalid_packet;
        }
        if (packet.data.size() > config.queue_capacity_bytes) {
            return EnqueueResult::packet_too_large;
        }

        std::unique_lock lock(queue_mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            queue_busy_drops.fetch_add(1, std::memory_order_relaxed);
            return EnqueueResult::queue_busy;
        }
        const auto current_state = writer_state.load(std::memory_order_acquire);
        if (current_state == WriterState::starting) {
            return EnqueueResult::not_ready;
        }
        if (current_state != WriterState::running || close_requested) {
            return EnqueueResult::closed;
        }
        if (slot_count >= config.queue_capacity_packets ||
            queued_bytes > config.queue_capacity_bytes ||
            packet.data.size() > config.queue_capacity_bytes - queued_bytes) {
            queue_full_drops.fetch_add(1, std::memory_order_relaxed);
            return EnqueueResult::queue_full;
        }

        auto& slot = slots[write_index];
        slot.owned = {};
        slot.audio = std::move(packet);
        slot.shared.reset();
        slot.shared_audio.reset();
        slot.is_audio = true;
        slot.bytes = slot.audio.data.size();
        slot.accepted_at = std::chrono::steady_clock::now();
        queued_bytes += slot.bytes;
        ++slot_count;
        write_index = (write_index + 1U) % slots.size();
        record_enqueue(slot.bytes);
        lock.unlock();
        queue_cv.notify_one();
        return EnqueueResult::accepted;
    }

    EnqueueResult enqueue_audio_shared(
        const std::shared_ptr<const media::EncodedAudioPacket>& packet) {
        if (!packet || !validate_audio_packet(*packet)) {
            invalid_packets.fetch_add(1, std::memory_order_relaxed);
            return EnqueueResult::invalid_packet;
        }
        if (packet->data.size() > config.queue_capacity_bytes) {
            return EnqueueResult::packet_too_large;
        }

        std::unique_lock lock(queue_mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            queue_busy_drops.fetch_add(1, std::memory_order_relaxed);
            return EnqueueResult::queue_busy;
        }
        const auto current_state = writer_state.load(std::memory_order_acquire);
        if (current_state == WriterState::starting) {
            return EnqueueResult::not_ready;
        }
        if (current_state != WriterState::running || close_requested) {
            return EnqueueResult::closed;
        }
        if (slot_count >= config.queue_capacity_packets ||
            queued_bytes > config.queue_capacity_bytes ||
            packet->data.size() > config.queue_capacity_bytes - queued_bytes) {
            queue_full_drops.fetch_add(1, std::memory_order_relaxed);
            return EnqueueResult::queue_full;
        }

        auto& slot = slots[write_index];
        slot.owned = {};
        slot.audio = {};
        slot.shared.reset();
        slot.shared_audio = packet;
        slot.is_audio = true;
        slot.bytes = packet->data.size();
        slot.accepted_at = std::chrono::steady_clock::now();
        queued_bytes += slot.bytes;
        ++slot_count;
        write_index = (write_index + 1U) % slots.size();
        record_enqueue(slot.bytes);
        lock.unlock();
        queue_cv.notify_one();
        return EnqueueResult::accepted;
    }

    [[nodiscard]] bool validate_packet(const media::EncodedPacket& packet) const noexcept {
        return packet.codec == config.codec && !packet.data.empty() &&
               packet.time_base.valid() && packet.duration >= 0 &&
               packet.pts != AV_NOPTS_VALUE && packet.dts != AV_NOPTS_VALUE;
    }

    [[nodiscard]] bool validate_audio_packet(const media::EncodedAudioPacket& packet) const noexcept {
        if (packet.stream_index >= config.audio_streams.size() ||
            packet.codec != config.audio_streams[packet.stream_index].codec ||
            packet.data.empty() || !packet.time_base.valid() || packet.duration < 0 ||
            packet.pts == AV_NOPTS_VALUE || packet.capture_time_ns == 0) {
            return false;
        }
        const auto& stream = config.audio_streams[packet.stream_index];
        if (packet.time_base.num != stream.stream_time_base.num ||
            packet.time_base.den != stream.stream_time_base.den ||
            packet.capture_time_ns >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        if (packet.discard_padding_samples != 0) {
            if (packet.codec != media::AudioCodec::aac || stream.sample_rate <= 0) {
                return false;
            }
            const auto packet_samples = av_rescale_q(
                packet.duration, to_av_rational(packet.time_base),
                AVRational{1, stream.sample_rate});
            if (packet_samples <= 0 ||
                packet.discard_padding_samples >
                    static_cast<std::uint64_t>(packet_samples)) {
                return false;
            }
        }
        const auto pts_time_ns = av_rescale_q(packet.pts, to_av_rational(packet.time_base),
                                              AVRational{1, 1'000'000'000});
        if (pts_time_ns < 0) {
            return false;
        }
        const auto pts_ns = static_cast<std::uint64_t>(pts_time_ns);
        const auto time_difference = pts_ns > packet.capture_time_ns
            ? pts_ns - packet.capture_time_ns
            : packet.capture_time_ns - pts_ns;
        const auto tick_ns = static_cast<std::uint64_t>(av_rescale_q(
            1, to_av_rational(packet.time_base), AVRational{1, 1'000'000'000}));
        return time_difference <= tick_ns + 1U;
    }

    void record_enqueue(std::size_t bytes) noexcept {
        enqueued_packets.fetch_add(1, std::memory_order_relaxed);
        current_packets.store(slot_count, std::memory_order_relaxed);
        current_bytes.store(queued_bytes, std::memory_order_relaxed);
        atomic_max(queue_high_water_packets, slot_count);
        atomic_max(queue_high_water_bytes, queued_bytes);
        (void)bytes;
    }

    [[nodiscard]] std::size_t first_queued_video_offset_locked() const noexcept {
        for (std::size_t offset = 0; offset < slot_count; ++offset) {
            const auto index = (read_index + offset) % slots.size();
            if (!slots[index].is_audio) return offset;
        }
        return slot_count;
    }

    [[nodiscard]] QueuedPacket dequeue_at_locked(std::size_t offset) noexcept {
        if (offset == 0) {
            QueuedPacket result = std::move(slots[read_index]);
            queued_bytes -= result.bytes;
            slots[read_index].clear();
            read_index = (read_index + 1U) % slots.size();
            --slot_count;
            if (slot_count == 0) write_index = read_index;
            result.dequeued_at = std::chrono::steady_clock::now();
            current_packets.store(slot_count, std::memory_order_relaxed);
            current_bytes.store(queued_bytes, std::memory_order_relaxed);
            return result;
        }

        QueuedPacket result = std::move(slots[(read_index + offset) % slots.size()]);
        queued_bytes -= result.bytes;
        for (std::size_t current = offset; current + 1U < slot_count; ++current) {
            const auto destination = (read_index + current) % slots.size();
            const auto source = (read_index + current + 1U) % slots.size();
            slots[destination] = std::move(slots[source]);
        }
        --slot_count;
        const auto empty_slot = (read_index + slot_count) % slots.size();
        slots[empty_slot].clear();
        write_index = empty_slot;
        result.dequeued_at = std::chrono::steady_clock::now();
        current_packets.store(slot_count, std::memory_order_relaxed);
        current_bytes.store(queued_bytes, std::memory_order_relaxed);
        return result;
    }

    void notify_timing(telemetry::Timing timing, std::uint64_t duration_ns) noexcept {
        if (!config.timing_observer) {
            return;
        }
        try {
            config.timing_observer(
                timing, std::chrono::nanoseconds{static_cast<std::int64_t>(duration_ns)});
        } catch (...) {
            // Instrumentation callbacks cannot affect the recording path.
        }
    }

    void record_queue_wait(const QueuedPacket& queued) noexcept {
        const auto duration_ns = elapsed_ns(queued.dequeued_at - queued.accepted_at);
        queue_wait_samples.fetch_add(1, std::memory_order_relaxed);
        total_queue_wait_latency_ns.fetch_add(duration_ns, std::memory_order_relaxed);
        atomic_max(max_queue_wait_latency_ns, duration_ns);
        notify_timing(telemetry::Timing::mux_queue_wait, duration_ns);
    }

    void record_disk_write(std::uint64_t duration_ns) noexcept {
        disk_write_calls.fetch_add(1, std::memory_order_relaxed);
        total_disk_write_latency_ns.fetch_add(duration_ns, std::memory_order_relaxed);
        atomic_max(max_disk_write_latency_ns, duration_ns);
        notify_timing(telemetry::Timing::disk_write, duration_ns);
    }

    static int write_file(void* opaque, const std::uint8_t* buffer, int buffer_size) noexcept {
        auto* self = static_cast<Impl*>(opaque);
        if (self == nullptr || self->output_fd < 0 || buffer_size < 0) {
            return AVERROR(EINVAL);
        }

        int written = 0;
        while (written < buffer_size) {
            const auto started_at = std::chrono::steady_clock::now();
            const auto result = ::write(self->output_fd, buffer + written,
                                        static_cast<std::size_t>(buffer_size - written));
            const int write_error = result < 0 ? errno : 0;
            const auto finished_at = std::chrono::steady_clock::now();
            self->record_disk_write(elapsed_ns(finished_at - started_at));
            if (result < 0) {
                if (write_error == EINTR) {
                    continue;
                }
                return AVERROR(write_error);
            }
            if (result == 0) {
                return AVERROR(EIO);
            }
            written += static_cast<int>(result);
        }
        return buffer_size;
    }

    static std::int64_t seek_file(void* opaque, std::int64_t offset, int whence) noexcept {
        auto* self = static_cast<Impl*>(opaque);
        if (self == nullptr || self->output_fd < 0) {
            return AVERROR(EINVAL);
        }
        if ((whence & AVSEEK_SIZE) != 0) {
            struct stat status {};
            if (::fstat(self->output_fd, &status) != 0) {
                return AVERROR(errno);
            }
            return static_cast<std::int64_t>(status.st_size);
        }
        whence &= ~AVSEEK_FORCE;
        const auto position = ::lseek(self->output_fd, static_cast<off_t>(offset), whence);
        return position < 0 ? AVERROR(errno) : static_cast<std::int64_t>(position);
    }

    void open_file_output(AVFormatContext* format) {
        output_fd = ::open(config.output_path.c_str(),
                           O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (output_fd < 0) {
            throw std::runtime_error("open Matroska output file: " +
                                     std::string(std::strerror(errno)));
        }

        constexpr int io_buffer_size = 64 * 1024;
        auto* buffer = static_cast<unsigned char*>(av_malloc(io_buffer_size));
        if (buffer == nullptr) {
            (void)::close(output_fd);
            output_fd = -1;
            throw std::runtime_error("allocate Matroska output buffer: out of memory");
        }
        AVIOContext* io = avio_alloc_context(buffer, io_buffer_size, 1, this,
                                             nullptr, &Impl::write_file, &Impl::seek_file);
        if (io == nullptr) {
            av_free(buffer);
            (void)::close(output_fd);
            output_fd = -1;
            throw std::runtime_error("allocate Matroska AVIO context: out of memory");
        }
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
    }

    void close_file_output(AVFormatContext* format) noexcept {
        if (format != nullptr && format->pb != nullptr &&
            (format->flags & AVFMT_FLAG_CUSTOM_IO) != 0) {
            av_freep(&format->pb->buffer);
            avio_context_free(&format->pb);
        }
        if (output_fd >= 0) {
            if (::close(output_fd) != 0) {
                const int error = errno;
                try {
                    publish_failure("close Matroska output file: " +
                                    std::string(std::strerror(error)));
                } catch (...) {
                    publish_failure("close Matroska output file failed");
                }
            }
            output_fd = -1;
        }
    }

    void close() noexcept {
        {
            std::lock_guard lock(queue_mutex);
            close_requested = true;
            if (writer_state.load(std::memory_order_relaxed) == WriterState::running) {
                writer_state.store(WriterState::closing, std::memory_order_release);
            }
        }
        queue_cv.notify_all();
        ready_cv.notify_all();
        try {
            if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
                worker.join();
            }
        } catch (...) {
            // Destructors and explicit shutdown are noexcept. The worker
            // publishes FFmpeg failures through `error` and `writer_state`.
        }
    }

    void run() noexcept {
        AVFormatContext* format = nullptr;
        std::vector<AVStream*> streams;
        bool header_written = false;
        bool trailer_written = false;
        try {
            check_av(avformat_alloc_output_context2(&format, nullptr, "matroska",
                                                     config.output_path.c_str()),
                     "allocate Matroska output context");
            if (format == nullptr) {
                throw std::runtime_error("FFmpeg did not create a Matroska output context");
            }
            // A stalled source must not hold packets from active tracks in the
            // interleaver indefinitely. A tenth of a second is comfortably
            // above normal AAC/Opus packet spacing while bounding mux latency.
            format->max_interleave_delta = 100'000;

            AVStream* video_stream = avformat_new_stream(format, nullptr);
            if (video_stream == nullptr) {
                throw std::runtime_error("create Matroska video stream: out of memory");
            }
            video_stream->time_base = to_av_rational(config.stream_time_base);
            if (config.frame_rate.valid()) {
                video_stream->avg_frame_rate = to_av_rational(config.frame_rate);
                video_stream->r_frame_rate = video_stream->avg_frame_rate;
            }

            AVCodecParameters* parameters = video_stream->codecpar;
            parameters->codec_type = AVMEDIA_TYPE_VIDEO;
            parameters->codec_id = to_av_codec_id(config.codec);
            parameters->codec_tag = 0;
            parameters->width = config.width;
            parameters->height = config.height;
            parameters->bit_rate = config.bit_rate;
            if (!config.codec_extradata.empty()) {
                const auto allocation_size = config.codec_extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE;
                parameters->extradata = static_cast<std::uint8_t*>(av_mallocz(allocation_size));
                if (parameters->extradata == nullptr) {
                    throw std::runtime_error("allocate codec extradata: out of memory");
                }
                std::memcpy(parameters->extradata, config.codec_extradata.data(),
                            config.codec_extradata.size());
                parameters->extradata_size = static_cast<int>(config.codec_extradata.size());
            }
            streams.push_back(video_stream);
            timestamp_rebasers.emplace_back(true);

            for (const auto& audio_config : config.audio_streams) {
                AVStream* audio_stream = avformat_new_stream(format, nullptr);
                if (audio_stream == nullptr) {
                    throw std::runtime_error("create Matroska audio stream: out of memory");
                }
                audio_stream->time_base = to_av_rational(audio_config.stream_time_base);
                AVCodecParameters* audio_parameters = audio_stream->codecpar;
                audio_parameters->codec_type = AVMEDIA_TYPE_AUDIO;
                audio_parameters->codec_id = to_av_audio_codec_id(audio_config.codec);
                audio_parameters->codec_tag = 0;
                audio_parameters->sample_rate = audio_config.sample_rate;
                audio_parameters->initial_padding = audio_config.initial_padding;
                av_channel_layout_default(&audio_parameters->ch_layout, audio_config.channels);
                audio_parameters->bit_rate = audio_config.bit_rate;
                if (!audio_config.codec_extradata.empty()) {
                    const auto allocation_size = audio_config.codec_extradata.size() +
                                                 AV_INPUT_BUFFER_PADDING_SIZE;
                    audio_parameters->extradata = static_cast<std::uint8_t*>(av_mallocz(allocation_size));
                    if (audio_parameters->extradata == nullptr) {
                        throw std::runtime_error("allocate audio codec extradata: out of memory");
                    }
                    std::memcpy(audio_parameters->extradata, audio_config.codec_extradata.data(),
                                audio_config.codec_extradata.size());
                    audio_parameters->extradata_size =
                        static_cast<int>(audio_config.codec_extradata.size());
                }
                streams.push_back(audio_stream);
                // Audio timestamps are already relative to the shared video
                // origin. Keep that offset while repairing only DTS collisions.
                timestamp_rebasers.emplace_back(false);
            }

            if ((format->oformat->flags & AVFMT_NOFILE) == 0) {
                open_file_output(format);
            }
            check_av(avformat_write_header(format, nullptr), "write Matroska header");
            header_written = true;
            refresh_file_size(format);
            publish_ready(true, {});

            for (;;) {
                QueuedPacket queued;
                {
                    std::unique_lock lock(queue_mutex);
                    queue_cv.wait(lock, [this] {
                        if (slot_count == 0) return close_requested;
                        if (config.audio_streams.empty() || first_video_dts_offset_known) {
                            return true;
                        }
                        return close_requested || first_queued_video_offset_locked() < slot_count;
                    });
                    if (slot_count == 0 && close_requested) {
                        break;
                    }
                    if (!config.audio_streams.empty() && !first_video_dts_offset_known) {
                        const auto video_offset = first_queued_video_offset_locked();
                        if (video_offset < slot_count) {
                            // Audio timestamps share the live capture epoch, but the
                            // video stream is rebased to its first DTS. Process that
                            // first video packet before audio so it can establish the
                            // corresponding audio-origin adjustment.
                            queued = dequeue_at_locked(video_offset);
                        } else {
                            // A recording may be closed before the encoder returns a
                            // video packet. In that case there is no reorder offset to
                            // apply; drain the bounded audio queue on the original epoch.
                            first_video_dts_offset_ns = 0;
                            first_video_dts_offset_known = true;
                            queued = dequeue_at_locked(0);
                        }
                    } else {
                        queued = dequeue_at_locked(0);
                    }
                }

                try {
                    record_queue_wait(queued);
                    write_packet(format, streams, queued);
                } catch (const std::exception& exception) {
                    publish_failure(exception.what());
                    break;
                }
            }

            if (header_written && writer_state.load(std::memory_order_acquire) != WriterState::failed) {
                check_av(av_write_trailer(format), "write Matroska trailer");
                if (format->pb != nullptr) {
                    avio_flush(format->pb);
                    if (format->pb->error < 0) {
                        check_av(format->pb->error, "flush Matroska output");
                    }
                    refresh_file_size(format);
                }
                trailer_written = true;
            }
        } catch (const std::exception& exception) {
            publish_failure(exception.what());
        } catch (...) {
            publish_failure("unknown error in Matroska mux worker");
        }

        bool ready_now = false;
        {
            std::lock_guard lock(queue_mutex);
            ready_now = ready;
        }
        if (!ready_now) {
            publish_ready(false, get_error());
        }
        if (format != nullptr) {
            if (format->pb != nullptr && (format->flags & AVFMT_FLAG_CUSTOM_IO) != 0) {
                avio_flush(format->pb);
                if (format->pb->error < 0) {
                    publish_failure("flush Matroska output: " + av_error_string(format->pb->error));
                }
                refresh_file_size(format);
                close_file_output(format);
            } else if (format->pb != nullptr && (format->oformat->flags & AVFMT_NOFILE) == 0) {
                const int close_result = avio_closep(&format->pb);
                if (close_result < 0) {
                    publish_failure("close Matroska output: " + av_error_string(close_result));
                }
            }
            avformat_free_context(format);
        }
        if (trailer_written && writer_state.load(std::memory_order_acquire) != WriterState::failed) {
            writer_state.store(WriterState::closed, std::memory_order_release);
        }
        queue_cv.notify_all();
        ready_cv.notify_all();
    }

    void publish_ready(bool success, std::string message) {
        {
            std::lock_guard lock(queue_mutex);
            ready = true;
            if (success) {
                writer_state.store(close_requested ? WriterState::closing : WriterState::running,
                                   std::memory_order_release);
            } else {
                error = std::move(message);
                writer_state.store(WriterState::failed, std::memory_order_release);
            }
        }
        ready_cv.notify_all();
    }

    void publish_failure(std::string message) noexcept {
        {
            std::lock_guard lock(queue_mutex);
            if (error.empty()) {
                error = std::move(message);
            }
            writer_state.store(WriterState::failed, std::memory_order_release);
            close_requested = true;
            while (slot_count > 0) {
                auto& slot = slots[read_index];
                slot.clear();
                read_index = (read_index + 1U) % slots.size();
                --slot_count;
            }
            queued_bytes = 0;
            current_packets.store(0, std::memory_order_relaxed);
            current_bytes.store(0, std::memory_order_relaxed);
            ready = true;
        }
        ready_cv.notify_all();
        queue_cv.notify_all();
    }

    [[nodiscard]] std::string get_error() const {
        std::lock_guard lock(queue_mutex);
        return error;
    }

    [[nodiscard]] std::int64_t audio_timestamp_from_video_origin(
        const media::EncodedAudioPacket& source) const {
        const __int128_t adjusted_origin =
            static_cast<__int128_t>(config.video_origin_time_ns) +
            first_video_dts_offset_ns;
        if (adjusted_origin <= 0 ||
            adjusted_origin > std::numeric_limits<std::int64_t>::max()) {
            throw std::runtime_error("video origin exceeds the signed audio timestamp range");
        }
        const auto origin_pts = av_rescale_q(
            static_cast<std::int64_t>(adjusted_origin),
            AVRational{1, 1'000'000'000}, to_av_rational(source.time_base));
        std::int64_t relative_pts = 0;
        if (__builtin_sub_overflow(source.pts, origin_pts, &relative_pts) ||
            relative_pts == AV_NOPTS_VALUE) {
            throw std::runtime_error("rebasing audio PTS overflowed the timestamp range");
        }
        return relative_pts;
    }

    void write_packet(AVFormatContext* format, const std::vector<AVStream*>& streams,
                      const QueuedPacket& queued) {
        const auto& video_source = queued.packet();
        const auto& audio_source = queued.audio_packet();
        const std::vector<std::uint8_t>& payload =
            queued.is_audio ? audio_source.data : video_source.data;
        if (payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("compressed packet exceeds FFmpeg AVPacket size limit");
        }

        std::size_t selected_stream = 0;
        AVStream* stream = nullptr;
        AVRational source_time_base{};
        std::int64_t pts = 0;
        std::int64_t dts = 0;
        std::int64_t duration = 0;
        bool keyframe = false;
        if (queued.is_audio) {
            selected_stream = audio_source.stream_index + 1U;
            stream = streams[selected_stream];
            source_time_base = to_av_rational(audio_source.time_base);
            pts = audio_timestamp_from_video_origin(audio_source);
            // AAC and Opus packets are presented in decode order.
            dts = pts;
            duration = audio_source.duration;
            keyframe = audio_source.keyframe;
        } else {
            stream = streams[0];
            source_time_base = to_av_rational(video_source.time_base);
            pts = video_source.pts;
            dts = video_source.dts;
            duration = video_source.duration;
            keyframe = video_source.keyframe;
            if (!config.audio_streams.empty() && !first_video_dts_offset_known) {
                const __int128_t offset_ns =
                    static_cast<__int128_t>(video_source.dts) *
                    video_source.time_base.num * 1'000'000'000 /
                    video_source.time_base.den;
                if (offset_ns < std::numeric_limits<std::int64_t>::min() ||
                    offset_ns > std::numeric_limits<std::int64_t>::max()) {
                    throw std::runtime_error("first video DTS offset exceeds the nanosecond range");
                }
                first_video_dts_offset_ns = static_cast<std::int64_t>(offset_ns);
                first_video_dts_offset_known = true;
            }
        }

        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr) {
            throw std::runtime_error("allocate FFmpeg packet: out of memory");
        }
        const int allocation_result = av_new_packet(packet, static_cast<int>(payload.size()));
        if (allocation_result < 0) {
            av_packet_free(&packet);
            check_av(allocation_result, "allocate padded FFmpeg packet data");
        }
        std::memcpy(packet->data, payload.data(), payload.size());
        packet->stream_index = stream->index;
        packet->pts = pts;
        packet->dts = dts;
        packet->duration = duration;
        packet->pos = -1;
        if (keyframe) {
            packet->flags |= AV_PKT_FLAG_KEY;
        }
        if (queued.is_audio && audio_source.discard_padding_samples != 0) {
            auto* skip_samples = av_packet_new_side_data(
                packet, AV_PKT_DATA_SKIP_SAMPLES, 10);
            if (skip_samples == nullptr) {
                av_packet_free(&packet);
                throw std::runtime_error("allocate AAC skip-samples packet metadata");
            }
            std::memset(skip_samples, 0, 10);
            const auto discard = audio_source.discard_padding_samples;
            skip_samples[4] = static_cast<std::uint8_t>(discard & 0xffU);
            skip_samples[5] = static_cast<std::uint8_t>((discard >> 8U) & 0xffU);
            skip_samples[6] = static_cast<std::uint8_t>((discard >> 16U) & 0xffU);
            skip_samples[7] = static_cast<std::uint8_t>((discard >> 24U) & 0xffU);
        }
        av_packet_rescale_ts(packet, source_time_base, stream->time_base);
        timestamp_rebasers[selected_stream].apply(packet);
        packet->time_base = stream->time_base;

        const auto write_started_at = std::chrono::steady_clock::now();
        const int result = av_interleaved_write_frame(format, packet);
        const auto finished_at = std::chrono::steady_clock::now();
        av_packet_free(&packet);
        check_av(result, queued.is_audio ? "mux compressed audio packet"
                                         : "mux compressed video packet");

        const auto write_ns = elapsed_ns(finished_at - write_started_at);
        const auto mux_ns = elapsed_ns(finished_at - queued.accepted_at);
        total_write_latency_ns.fetch_add(write_ns, std::memory_order_relaxed);
        atomic_max(max_write_latency_ns, write_ns);
        total_mux_latency_ns.fetch_add(mux_ns, std::memory_order_relaxed);
        atomic_max(max_mux_latency_ns, mux_ns);
        notify_timing(telemetry::Timing::mux, mux_ns);
        muxed_packets.fetch_add(1, std::memory_order_relaxed);
        if (!queued.is_audio) {
            muxed_video_packets.fetch_add(1, std::memory_order_relaxed);
        }
        packet_payload_bytes.fetch_add(payload.size(), std::memory_order_relaxed);
        refresh_file_size(format);
    }

    void refresh_file_size(AVFormatContext* format) noexcept {
        if (format->pb == nullptr) {
            return;
        }
        const auto position = avio_tell(format->pb);
        if (position >= 0) {
            const auto value = static_cast<std::uint64_t>(position);
            atomic_max(output_file_bytes, value);
        }
    }

    MuxConfig config;
    std::vector<QueuedPacket> slots;
    std::thread worker;
    int output_fd{-1};

    mutable std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::condition_variable ready_cv;
    std::string error;
    bool ready{false};
    bool close_requested{false};
    std::size_t read_index{0};
    std::size_t write_index{0};
    std::size_t slot_count{0};
    std::size_t queued_bytes{0};
    // The first live video packet establishes how audio's CLOCK_MONOTONIC
    // timestamps map to the DTS-rebased video timeline. Audio admission is
    // held within the configured bounds until that packet is queued. One
    // extra ring slot and at most one packet's bytes are reserved for it.
    bool first_video_enqueued{false}; // guarded by queue_mutex
    bool first_video_dts_offset_known{false}; // worker-only
    std::int64_t first_video_dts_offset_ns{0}; // worker-only
    // Worker-only per-track timestamp state, using Matroska stream timebases.
    std::vector<detail::TimestampRebaser> timestamp_rebasers;

    std::atomic<WriterState> writer_state{WriterState::starting};
    std::atomic<std::uint64_t> enqueued_packets{0};
    std::atomic<std::uint64_t> muxed_packets{0};
    std::atomic<std::uint64_t> muxed_video_packets{0};
    std::atomic<std::uint64_t> queue_full_drops{0};
    std::atomic<std::uint64_t> queue_busy_drops{0};
    std::atomic<std::uint64_t> invalid_packets{0};
    std::atomic<std::uint64_t> packet_payload_bytes{0};
    std::atomic<std::size_t> current_packets{0};
    std::atomic<std::size_t> current_bytes{0};
    std::atomic<std::size_t> queue_high_water_packets{0};
    std::atomic<std::size_t> queue_high_water_bytes{0};
    std::atomic<std::uint64_t> output_file_bytes{0};
    std::atomic<std::uint64_t> total_mux_latency_ns{0};
    std::atomic<std::uint64_t> max_mux_latency_ns{0};
    std::atomic<std::uint64_t> total_write_latency_ns{0};
    std::atomic<std::uint64_t> max_write_latency_ns{0};
    std::atomic<std::uint64_t> queue_wait_samples{0};
    std::atomic<std::uint64_t> total_queue_wait_latency_ns{0};
    std::atomic<std::uint64_t> max_queue_wait_latency_ns{0};
    std::atomic<std::uint64_t> disk_write_calls{0};
    std::atomic<std::uint64_t> total_disk_write_latency_ns{0};
    std::atomic<std::uint64_t> max_disk_write_latency_ns{0};
};

MuxWriter::MuxWriter(MuxConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

MuxWriter::~MuxWriter() = default;

bool MuxWriter::wait_until_ready(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->queue_mutex);
    return impl_->ready_cv.wait_for(lock, timeout, [this] { return impl_->ready; }) &&
           impl_->writer_state.load(std::memory_order_acquire) != WriterState::failed;
}

EnqueueResult MuxWriter::try_enqueue(media::EncodedPacket&& packet) {
    return impl_->enqueue_owned(std::move(packet));
}

EnqueueResult MuxWriter::try_enqueue(media::EncodedAudioPacket&& packet) {
    return impl_->enqueue_audio(std::move(packet));
}

EnqueueResult MuxWriter::try_enqueue(
    const std::shared_ptr<const media::EncodedAudioPacket>& packet) {
    return impl_->enqueue_audio_shared(packet);
}

EnqueueResult MuxWriter::try_enqueue(
    const std::shared_ptr<const media::EncodedPacket>& packet) {
    return impl_->enqueue_shared(packet);
}

void MuxWriter::close() noexcept {
    if (impl_) {
        impl_->close();
    }
}

WriterState MuxWriter::state() const noexcept {
    return impl_->writer_state.load(std::memory_order_acquire);
}

std::string MuxWriter::error_message() const {
    return impl_->get_error();
}

MuxStats MuxWriter::stats() const noexcept {
    MuxStats result;
    result.enqueued_packets = impl_->enqueued_packets.load(std::memory_order_relaxed);
    result.muxed_packets = impl_->muxed_packets.load(std::memory_order_relaxed);
    result.muxed_video_packets = impl_->muxed_video_packets.load(std::memory_order_relaxed);
    result.queue_full_drops = impl_->queue_full_drops.load(std::memory_order_relaxed);
    result.queue_busy_drops = impl_->queue_busy_drops.load(std::memory_order_relaxed);
    result.invalid_packets = impl_->invalid_packets.load(std::memory_order_relaxed);
    result.packet_payload_bytes = impl_->packet_payload_bytes.load(std::memory_order_relaxed);
    result.queued_packets = impl_->current_packets.load(std::memory_order_relaxed);
    result.queued_bytes = impl_->current_bytes.load(std::memory_order_relaxed);
    result.queue_high_water_packets = impl_->queue_high_water_packets.load(std::memory_order_relaxed);
    result.queue_high_water_bytes = impl_->queue_high_water_bytes.load(std::memory_order_relaxed);
    result.output_file_bytes = impl_->output_file_bytes.load(std::memory_order_relaxed);
    result.total_mux_latency_ns = impl_->total_mux_latency_ns.load(std::memory_order_relaxed);
    result.max_mux_latency_ns = impl_->max_mux_latency_ns.load(std::memory_order_relaxed);
    result.total_write_latency_ns = impl_->total_write_latency_ns.load(std::memory_order_relaxed);
    result.max_write_latency_ns = impl_->max_write_latency_ns.load(std::memory_order_relaxed);
    result.queue_wait_samples = impl_->queue_wait_samples.load(std::memory_order_relaxed);
    result.total_queue_wait_latency_ns = impl_->total_queue_wait_latency_ns.load(std::memory_order_relaxed);
    result.max_queue_wait_latency_ns = impl_->max_queue_wait_latency_ns.load(std::memory_order_relaxed);
    result.disk_write_calls = impl_->disk_write_calls.load(std::memory_order_relaxed);
    result.total_disk_write_latency_ns = impl_->total_disk_write_latency_ns.load(std::memory_order_relaxed);
    result.max_disk_write_latency_ns = impl_->max_disk_write_latency_ns.load(std::memory_order_relaxed);
    return result;
}

} // namespace fluxrec::mux
