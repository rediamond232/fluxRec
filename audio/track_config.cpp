#include "audio/track_config.hpp"

#include <unordered_set>

namespace fluxrec::audio {

namespace {

[[nodiscard]] bool isAsciiUpper(unsigned char byte) noexcept {
    return byte >= 'A' && byte <= 'Z';
}

[[nodiscard]] bool isAsciiLower(unsigned char byte) noexcept {
    return byte >= 'a' && byte <= 'z';
}

[[nodiscard]] bool isAsciiLetter(unsigned char byte) noexcept {
    return isAsciiUpper(byte) || isAsciiLower(byte);
}

[[nodiscard]] bool isAsciiDigit(unsigned char byte) noexcept {
    return byte >= '0' && byte <= '9';
}

[[nodiscard]] bool isIdChar(unsigned char byte) noexcept {
    return isAsciiLetter(byte) || isAsciiDigit(byte) || byte == '_' || byte == '-';
}

[[nodiscard]] std::string trimAsciiSpaces(std::string_view value) {
    const auto begin = value.find_first_not_of(' ');
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(' ');
    return std::string{value.substr(begin, end - begin + 1)};
}

[[nodiscard]] bool endsWithMonitorSuffix(std::string_view value) {
    static constexpr std::string_view suffix{".monitor"};
    if (value.size() < suffix.size()) {
        return false;
    }
    return value.substr(value.size() - suffix.size()) == suffix;
}

} // namespace

bool validTrackId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(id.front());
    if (!isAsciiLetter(first)) {
        return false;
    }
    for (std::size_t index = 1; index < id.size(); ++index) {
        if (!isIdChar(static_cast<unsigned char>(id[index]))) {
            return false;
        }
    }
    return true;
}

std::expected<AudioTrack, std::string> validateAudioTrack(AudioTrack track) {
    if (!validTrackId(track.id)) {
        return std::unexpected{"invalid audio track id"};
    }
    track.name = trimAsciiSpaces(track.name);
    if (track.name.empty()) {
        return std::unexpected{"audio track name must not be empty"};
    }
    if (track.name.size() > 64) {
        return std::unexpected{"audio track name is too long"};
    }
    return track;
}

std::expected<std::vector<AudioTrack>, std::string> validateAudioTracks(
    std::vector<AudioTrack> tracks) {
    if (tracks.size() > max_audio_tracks) {
        return std::unexpected{"too many audio tracks"};
    }
    std::unordered_set<std::string_view> seen_ids;
    seen_ids.reserve(tracks.size());
    for (auto& track : tracks) {
        const auto validated = validateAudioTrack(std::move(track));
        if (!validated) {
            return std::unexpected{validated.error()};
        }
        track = *validated;
        if (!seen_ids.insert(track.id).second) {
            return std::unexpected{"duplicate audio track id " + track.id};
        }
    }
    return tracks;
}

std::vector<AudioTrack> defaultAudioTracks() {
    return {
        AudioTrack{"desktop", "Desktop audio", InputKind::sink_monitor, {}, {}},
        AudioTrack{"microphone", "Microphone", InputKind::source, {}, {}},
    };
}

TrackStreamProperties streamProperties(const AudioTrack& track) {
    TrackStreamProperties properties;
    properties.capture_sink = track.input_kind == InputKind::sink_monitor;
    if (endsWithMonitorSuffix(track.input_target)) {
        properties.target_object =
            track.input_target.substr(0, track.input_target.size() - std::string_view{".monitor"}.size());
    } else {
        properties.target_object = track.input_target;
    }
    properties.open_monitor = !track.monitor_target.empty();
    properties.monitor_object = track.monitor_target;
    return properties;
}

std::string nextTrackId(std::span<const AudioTrack> tracks) {
    std::unordered_set<std::string_view> existing_ids;
    existing_ids.reserve(tracks.size());
    for (const AudioTrack& track : tracks) {
        existing_ids.insert(track.id);
    }
    for (int index = 1; index <= 1000000; ++index) {
        const std::string candidate = "track-" + std::to_string(index);
        if (!existing_ids.contains(candidate)) {
            return candidate;
        }
    }
    return "track-1";
}

} // namespace fluxrec::audio
