#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fluxrec::audio {

inline constexpr std::size_t max_audio_tracks = 8;

enum class InputKind : std::uint8_t { source, sink_monitor };

struct AudioTrack {
    std::string id;
    std::string name;
    InputKind input_kind{InputKind::source};
    std::string input_target;
    std::string monitor_target;
};

struct TrackStreamProperties {
    bool capture_sink{false};
    std::string target_object;
    bool open_monitor{false};
    std::string monitor_object;
};

[[nodiscard]] bool validTrackId(std::string_view id) noexcept;
[[nodiscard]] std::expected<AudioTrack, std::string> validateAudioTrack(AudioTrack track);
[[nodiscard]] std::expected<std::vector<AudioTrack>, std::string> validateAudioTracks(
    std::vector<AudioTrack> tracks);
[[nodiscard]] std::vector<AudioTrack> defaultAudioTracks();
[[nodiscard]] TrackStreamProperties streamProperties(const AudioTrack& track);
[[nodiscard]] std::string nextTrackId(std::span<const AudioTrack> tracks);

} // namespace fluxrec::audio
