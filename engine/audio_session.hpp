#pragma once

#include "audio/audio.hpp"
#include "mux/mux_writer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fluxrec::replay {
class ReplayBuffer;
}

namespace fluxrec::engine {

struct AudioSessionConfig {
    audio::Config capture{};
    std::size_t packet_queue_capacity{512};
    std::size_t packet_queue_capacity_bytes{16U * 1024U * 1024U};
    unsigned int mux_retry_count{8};
    std::chrono::microseconds mux_retry_delay{200};
};

enum class AudioSessionState : std::uint8_t {
    stopped,
    starting,
    ready,
    forwarding,
    stopping,
    failed,
};

struct AudioSessionSnapshot {
    AudioSessionState state{AudioSessionState::stopped};
    bool formats_ready{};
    std::uint64_t video_origin_time_ns{};
    std::size_t queued_packets{};
    std::size_t queued_bytes{};
    std::size_t queue_high_water_packets{};
    std::size_t queue_high_water_bytes{};
    std::uint64_t captured_packets{};
    std::uint64_t callback_queue_drops{};
    std::uint64_t unforwarded_packets_dropped{};
    std::uint64_t sync_errors{};
    std::uint64_t mux_accepted_packets{};
    std::uint64_t mux_dropped_packets{};
    std::uint64_t mux_busy_retries{};
    std::uint64_t mux_full_retries{};
    std::uint64_t replay_stored_packets{};
    std::uint64_t replay_dropped_packets{};
    std::uint64_t audio_errors{};
    std::vector<audio::SourceSnapshot> tracks;
    std::vector<std::string> ids;
    std::vector<std::string> names;
    std::string last_error;
};

void writeAudioTelemetryObject(std::ostream& output, const AudioSessionSnapshot& snapshot,
                               bool enabled);

// Bridges the PipeWire/FFmpeg audio worker to MuxWriter and/or ReplayBuffer.
// Audio packets remain on their absolute CLOCK_MONOTONIC-derived sample clock
// until MuxWriter subtracts the shared video origin. The replay buffer receives
// the same packets with a separate video-relative capture timestamp. Destinations
// must outlive forwarding; stop this session before closing or destroying them.
class AudioSession final {
public:
    explicit AudioSession(AudioSessionConfig config = {});
    ~AudioSession();

    AudioSession(const AudioSession&) = delete;
    AudioSession& operator=(const AudioSession&) = delete;
    AudioSession(AudioSession&&) = delete;
    AudioSession& operator=(AudioSession&&) = delete;

    // Starts configured capture tracks and waits until every encoder publishes
    // non-empty codec extradata. No MuxWriter is required yet; encoded packets
    // are held in the bounded queue while the video origin/writer is prepared.
    [[nodiscard]] bool start(
        std::chrono::milliseconds format_timeout = std::chrono::seconds(5));

    // Returns one AudioStreamConfig per configured track, in track order, for
    // MuxConfig::audio_streams.
    [[nodiscard]] std::vector<mux::AudioStreamConfig> stream_configs() const;

    // Waits for the mux header to become ready, then starts the independent
    // forwarding worker. Pass the exact origin also supplied to MuxConfig.
    [[nodiscard]] bool begin_forwarding(
        mux::MuxWriter& writer, std::uint64_t video_origin_time_ns,
        std::chrono::milliseconds writer_timeout = std::chrono::seconds(5));

    // Starts the same independent worker with optional destinations. At least
    // one destination must be non-null. Passing a null writer enables a future
    // replay-only path without coupling replay storage to the capture callback.
    [[nodiscard]] bool begin_forwarding(
        mux::MuxWriter* writer, replay::ReplayBuffer* replay_buffer,
        std::uint64_t video_origin_time_ns,
        std::chrono::milliseconds writer_timeout = std::chrono::seconds(5));

    // Stops PipeWire, drains accepted bridge-queue packets through the
    // forwarding worker, then joins it. It never closes MuxWriter.
    void stop() noexcept;

    // Returns false when id is not present in the configured track list.
    [[nodiscard]] bool set_volume(std::string_view id, float linear_gain) noexcept;
    [[nodiscard]] bool set_muted(std::string_view id, bool muted) noexcept;
    [[nodiscard]] AudioSessionSnapshot snapshot() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxrec::engine
