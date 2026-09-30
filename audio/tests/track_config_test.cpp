#include "audio/track_config.hpp"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char* label) {
    if (!condition) {
        std::cerr << "FAIL " << label << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    using fluxrec::audio::AudioTrack;
    using fluxrec::audio::InputKind;
    expect(fluxrec::audio::validTrackId("desktop"), "default id");
    expect(fluxrec::audio::validTrackId("track-1"), "generated id");
    expect(!fluxrec::audio::validTrackId(""), "empty id");
    expect(!fluxrec::audio::validTrackId("1track"), "id must start with a letter");
    expect(!fluxrec::audio::validTrackId("has space"), "id cannot contain a space");

    const std::string id64 = std::string(1, 'a') + std::string(63, 'z');
    expect(id64.size() == 64 && fluxrec::audio::validTrackId(id64), "64 character id");
    const std::string id65 = id64 + 'z';
    expect(id65.size() == 65 && !fluxrec::audio::validTrackId(id65), "65 character id");
    expect(fluxrec::audio::validTrackId("Track_1"), "underscore and uppercase");
    expect(!fluxrec::audio::validTrackId("bad.id"), "punctuation rejected");
    expect(!fluxrec::audio::validTrackId("a\xc3\xa9"), "non-ASCII byte rejected");

    AudioTrack blank_name{"desktop", "  ", InputKind::sink_monitor, {}, {}};
    expect(!fluxrec::audio::validateAudioTrack(blank_name), "blank name");

    std::vector<AudioTrack> nine;
    for (int index = 0; index < 9; ++index) {
        nine.push_back(AudioTrack{
            "track" + std::to_string(index), "Track", InputKind::source, {}, {}});
    }
    expect(!fluxrec::audio::validateAudioTracks(nine), "nine tracks");

    AudioTrack duplicate = fluxrec::audio::defaultAudioTracks().front();
    expect(!fluxrec::audio::validateAudioTracks({duplicate, duplicate}), "duplicate id");

    const auto defaults = fluxrec::audio::defaultAudioTracks();
    expect(defaults.size() == 2, "two defaults");
    expect(defaults[0].id == "desktop" && defaults[0].input_kind == InputKind::sink_monitor &&
               defaults[0].input_target.empty() && defaults[0].monitor_target.empty(),
           "desktop default");
    expect(defaults[1].id == "microphone" && defaults[1].input_kind == InputKind::source,
           "microphone default");

    AudioTrack monitor{"mic", "Microphone", InputKind::source, "alsa_input.mic", "alsa_output.speakers"};
    const auto monitored = fluxrec::audio::streamProperties(monitor);
    expect(!monitored.capture_sink && monitored.target_object == "alsa_input.mic" &&
               monitored.open_monitor && monitored.monitor_object == "alsa_output.speakers",
           "source with monitor");

    AudioTrack sink{"desk", "Desktop", InputKind::sink_monitor, "speakers.monitor", {}};
    const auto captured = fluxrec::audio::streamProperties(sink);
    expect(captured.capture_sink && captured.target_object == "speakers" && !captured.open_monitor,
           "strip .monitor and do not open playback");

    std::vector<AudioTrack> existing{
        {"track-1", "A", InputKind::source, {}, {}},
        {"track-3", "B", InputKind::source, {}, {}},
    };
    expect(fluxrec::audio::nextTrackId(existing) == "track-2", "fill the first free id");

    AudioTrack same_device{"other", "Other", InputKind::source, "alsa_input.mic", {}};
    expect(static_cast<bool>(fluxrec::audio::validateAudioTracks({monitor, same_device})),
           "the same device may be selected twice");

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
