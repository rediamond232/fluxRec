#pragma once

#include "audio/track_config.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fluxrec::audio {

enum class Codec : std::uint8_t {
    aac,
    opus,
};

enum class SourceState : std::uint8_t {
    stopped,
    connecting,
    streaming,
    error,
};

struct Format;
struct Packet;

struct Config {
    // PipeWire converts into this format. Stereo 48 kHz works with AAC and
    // Opus and keeps every input on the same sample clock.
    std::uint32_t sample_rate{48'000};
    std::uint32_t channels{2};
    Codec codec{Codec::aac};
    std::int64_t bitrate{160'000};

    // One capture stream per track, in this order; that order is also the
    // audio stream order handed to the muxer. At most max_audio_tracks
    // entries. An empty list captures nothing.
    std::vector<AudioTrack> tracks{defaultAudioTracks()};

    // These callbacks run on the audio worker thread. Keep them short; packet
    // and format data are owned by the callback argument and may be moved.
    // Do not call stop() or destroy Capture from one of these callbacks.
    std::function<void(Format)> on_format;
    std::function<void(Packet)> on_packet;
    std::function<void(std::size_t, std::string)> on_error;
};

struct Format {
    // Index into Config::tracks.
    std::size_t track_index{};
    Codec codec{Codec::aac};
    std::uint32_t sample_rate{};
    std::uint32_t channels{};
    // Encoder priming delay in samples. Muxers use this to write container
    // delay metadata (Matroska CodecDelay) so decoders can skip AAC priming.
    int initial_padding{};
    // Encoded codec extradata (for AAC this contains AudioSpecificConfig).
    std::vector<std::uint8_t> extradata;
    // Packet PTS values use {time_base_num / time_base_den} seconds.
    std::int32_t time_base_num{1};
    std::int32_t time_base_den{48'000};
};

struct Packet {
    // Index into Config::tracks.
    std::size_t track_index{};
    Codec codec{Codec::aac};
    std::int64_t pts{};
    std::int64_t duration{};
    // Encoded packet PTS re-expressed in absolute CLOCK_MONOTONIC nanoseconds.
    // This value follows the frame PTS supplied to FFmpeg; it is not an
    // independent measurement of PipeWire or hardware clock accuracy.
    std::uint64_t capture_time_ns{};
    // Samples per channel at the end of this AAC packet that were added as
    // silence to complete a partial encoder frame. Matroska can preserve this
    // through DiscardPadding. Zero means the packet contains no tail padding.
    std::uint32_t discard_padding_samples{};
    std::vector<std::uint8_t> data;
    bool key_frame{};
};

struct Meter {
    // Linear amplitudes in [0, 1] after the source's software gain/mute.
    float peak{};
    float rms{};
    std::uint64_t samples{};
};

struct SourceSnapshot {
    SourceState state{SourceState::stopped};
    Meter meter{};
    float volume{1.0F};
    bool muted{};
    std::uint64_t buffers_received{};
    std::uint64_t buffers_dropped{};
    std::uint64_t samples_captured{};
    std::uint64_t pipewire_native_timestamp_buffers{};
    std::uint64_t monotonic_fallback_timestamp_buffers{};
    std::uint64_t invalid_pipewire_time_metadata_buffers{};
    std::uint64_t pipewire_graph_clock_samples{};
    std::uint64_t pipewire_graph_clock_unusable_samples{};
    std::uint64_t pipewire_rate_window_resets{};
    std::uint64_t pipewire_rate_window_samples{};
    std::uint64_t pipewire_graph_elapsed_ns{};
    std::uint64_t pipewire_monotonic_elapsed_ns{};
    // Relative rate observation, parts per million scaled by 1000. Valid only
    // after a ten-second window at an unchanged PipeWire tick rate.
    std::int64_t pipewire_graph_vs_monotonic_ppm_milli{};
    bool pipewire_graph_vs_monotonic_valid{};
    std::string error;
    // Set when this track asked for a monitor sink that could not be opened.
    // Capture keeps recording, so this never blocks a recording.
    std::string monitor_error;
};

// Captures one PipeWire input stream per configured track, plus an optional
// playback stream per track that lets a chosen sink hear the gained samples.
// A bounded, preallocated queue separates PipeWire callbacks from metering and
// FFmpeg encoding.
class Capture final {
public:
    explicit Capture(Config config = {});
    ~Capture();

    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    Capture(Capture&&) = delete;
    Capture& operator=(Capture&&) = delete;

    // Starts PipeWire capture and the audio worker. Returns false when the
    // track list is invalid or when the initial PipeWire connection or stream
    // setup fails. Inspect snapshot() for synchronous startup errors; later
    // stream errors also reach on_error.
    [[nodiscard]] bool start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;

    // Out-of-range indices are ignored; snapshot() then returns defaults.
    void setVolume(std::size_t track_index, float linear_gain) noexcept;
    void setMuted(std::size_t track_index, bool muted) noexcept;
    [[nodiscard]] SourceSnapshot snapshot(std::size_t track_index) const;
    [[nodiscard]] std::size_t trackCount() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxrec::audio
