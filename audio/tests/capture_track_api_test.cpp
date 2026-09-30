#include "audio/audio.hpp"

#include <cstddef>
#include <cstdio>
#include <type_traits>
#include <vector>

// A live PipeWire server is out of scope here, so this fixture never calls
// start(). It pins the per-track capture API: the configuration carries a
// track list, and volume, mute, and snapshots address a track by index.
namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "capture track API: %s\n", what);
        ++failures;
    }
}

} // namespace

int main() {
    fluxrec::audio::Config config;
    config.tracks = fluxrec::audio::defaultAudioTracks();
    config.tracks.push_back(
        {"track-1", "Track 1", fluxrec::audio::InputKind::source, {}, "speakers"});
    static_assert(std::is_same_v<decltype(config.tracks),
                                 std::vector<fluxrec::audio::AudioTrack>>);
    static_assert(std::is_same_v<decltype(fluxrec::audio::Format{}.track_index),
                                 std::size_t>);
    static_assert(std::is_same_v<decltype(fluxrec::audio::Packet{}.track_index),
                                 std::size_t>);

    expect(config.tracks.size() == 3, "the default pair plus one monitored track");

    fluxrec::audio::Capture capture(config);
    expect(capture.trackCount() == 3, "trackCount reports one stream per track");
    expect(!capture.running(), "an unstarted capture is not running");

    constexpr std::size_t monitored_track = 2;
    capture.setVolume(monitored_track, 0.5F);
    capture.setMuted(monitored_track, true);
    const auto monitored = capture.snapshot(monitored_track);
    expect(monitored.volume == 0.5F, "setVolume addresses a track by index");
    expect(monitored.muted, "setMuted addresses a track by index");
    expect(monitored.monitor_error.empty(), "an unstarted monitor reports no error");
    expect(monitored.state == fluxrec::audio::SourceState::stopped,
           "an unstarted track is stopped");

    constexpr std::size_t missing_track = 99;
    capture.setVolume(missing_track, 0.25F);
    capture.setMuted(missing_track, true);
    const auto missing = capture.snapshot(missing_track);
    expect(missing.volume == 1.0F, "an out-of-range snapshot is the default snapshot");
    expect(!missing.muted, "an out-of-range mute is ignored");
    expect(capture.snapshot(0).volume == 1.0F,
           "an out-of-range index does not disturb another track");

    return failures == 0 ? 0 : 1;
}
