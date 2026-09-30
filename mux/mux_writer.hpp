#pragma once

#include "packet.hpp"
#include "telemetry/telemetry.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fluxrec::mux {

struct AudioStreamConfig {
    media::AudioCodec codec{media::AudioCodec::aac};
    int sample_rate{48'000};
    int channels{2};
    // Encoder priming delay in samples; copied to AVCodecParameters before
    // the container header is written.
    int initial_padding{};
    std::int64_t bit_rate{0};
    // Expected packet timebase; current audio capture emits sample ticks
    // (1 / sample_rate) on a CLOCK_MONOTONIC-derived PTS clock.
    media::Rational stream_time_base{1, 48'000};
    // Encoder-provided codec private data, including AAC AudioSpecificConfig.
    std::vector<std::uint8_t> codec_extradata;
};

struct MuxConfig {
    // A local filesystem path. Fluxrec's custom AVIO writer captures the
    // duration of each write(2) call without forcing an fsync per packet.
    std::string output_path;
    media::VideoCodec codec{media::VideoCodec::h264};
    int width{0};
    int height{0};
    media::Rational stream_time_base{1, 1000};
    media::Rational frame_rate{0, 1};
    std::int64_t bit_rate{0};
    // Encoder-provided codec private data, when available. This is copied
    // into the Matroska video track's CodecPrivate field.
    std::vector<std::uint8_t> codec_extradata;
    // Packet stream_index is the track-list order, from 0 through 7.
    std::vector<AudioStreamConfig> audio_streams;
    // CLOCK_MONOTONIC nanoseconds for the first captured video frame. Required
    // when audio streams are configured. Audio packet PTS must still be on its
    // absolute monotonic-derived sample clock. MuxWriter adjusts this origin by
    // the first video packet's DTS before rebasing audio, keeping presentation
    // time aligned when the encoder reorders frames. Until that packet arrives,
    // the bounded queue may temporarily hold one extra first-video packet and
    // its payload (at most 2x the configured byte limit); shutdown without any
    // video packet drains audio with a zero DTS adjustment.
    std::uint64_t video_origin_time_ns{0};
    std::size_t queue_capacity_packets{512};
    std::size_t queue_capacity_bytes{64U * 1024U * 1024U};
    // Invoked on the mux I/O worker. Exceptions are swallowed so telemetry
    // cannot fail recording. Map Timing::mux_queue_wait, Timing::mux and
    // Timing::disk_write directly to Telemetry::observe when desired.
    std::function<void(telemetry::Timing, std::chrono::nanoseconds)> timing_observer;
};

enum class EnqueueResult {
    accepted,
    queue_full,
    queue_busy,
    packet_too_large,
    invalid_packet,
    not_ready,
    closed,
};

enum class WriterState {
    starting,
    running,
    closing,
    closed,
    failed,
};

struct MuxStats {
    std::uint64_t enqueued_packets{0};
    // Total successfully muxed packets across video and all configured audio streams.
    std::uint64_t muxed_packets{0};
    // Successfully muxed video packets only; audio packets are excluded.
    std::uint64_t muxed_video_packets{0};
    std::uint64_t queue_full_drops{0};
    std::uint64_t queue_busy_drops{0};
    std::uint64_t invalid_packets{0};
    std::uint64_t packet_payload_bytes{0};
    std::size_t queued_packets{0};
    std::size_t queued_bytes{0};
    std::size_t queue_high_water_packets{0};
    std::size_t queue_high_water_bytes{0};
    std::uint64_t output_file_bytes{0};
    // Enqueue acceptance through completion of av_interleaved_write_frame.
    std::uint64_t total_mux_latency_ns{0};
    std::uint64_t max_mux_latency_ns{0};
    // Duration of av_interleaved_write_frame itself (legacy metric).
    std::uint64_t total_write_latency_ns{0};
    std::uint64_t max_write_latency_ns{0};
    // Time spent queued before the I/O worker dequeues each packet.
    std::uint64_t queue_wait_samples{0};
    std::uint64_t total_queue_wait_latency_ns{0};
    std::uint64_t max_queue_wait_latency_ns{0};
    // Actual write(2) calls made by the custom local-file AVIO callback.
    std::uint64_t disk_write_calls{0};
    std::uint64_t total_disk_write_latency_ns{0};
    std::uint64_t max_disk_write_latency_ns{0};
};

class MuxWriter final {
public:
    explicit MuxWriter(MuxConfig config);
    ~MuxWriter();

    MuxWriter(const MuxWriter&) = delete;
    MuxWriter& operator=(const MuxWriter&) = delete;
    MuxWriter(MuxWriter&&) = delete;
    MuxWriter& operator=(MuxWriter&&) = delete;

    // The worker owns all FFmpeg objects and file I/O. Wait for header setup
    // from a control thread before sending packets from a real-time producer.
    [[nodiscard]] bool wait_until_ready(std::chrono::milliseconds timeout);

    // Never waits for queue space. `queue_busy` means the brief queue lock was
    // contended; callers may count/drop or retry outside a latency-sensitive
    // thread. Shared packets support zero-copy handoff from replay snapshots;
    // FFmpeg's padded packet buffer is built on the I/O thread.
    [[nodiscard]] EnqueueResult try_enqueue(media::EncodedPacket&& packet);
    [[nodiscard]] EnqueueResult try_enqueue(media::EncodedAudioPacket&& packet);
    [[nodiscard]] EnqueueResult try_enqueue(
        const std::shared_ptr<const media::EncodedPacket>& packet);
    [[nodiscard]] EnqueueResult try_enqueue(
        const std::shared_ptr<const media::EncodedAudioPacket>& packet);

    // Stops accepting packets, drains accepted packets, writes the trailer,
    // and joins the I/O worker. Safe to call more than once.
    void close() noexcept;

    [[nodiscard]] WriterState state() const noexcept;
    [[nodiscard]] std::string error_message() const;
    [[nodiscard]] MuxStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxrec::mux
