# Audio Track Devices Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the audio mixer hold up to eight tracks, each recording one chosen input and optionally playing that audio to one chosen output during a session.

**Architecture:** A header-only-free `fluxrec_audio_tracks` library owns track validation and PipeWire stream properties. The recorder receives those tracks through `--audio-track` and captures one stream plus an optional playback stream per track. The Qt mixer edits the same list in `RecorderSettings` and only sends it on the next launch.

**Tech Stack:** C++23, PipeWire, FFmpeg AAC, Matroska mux, Qt 6 widgets, QSettings, CTest.

## Global Constraints

- At most 8 tracks. A ninth is rejected by validation, settings load, argument parsing, and the mixer.
- Track id matches `^[A-Za-z][A-Za-z0-9_-]{0,63}$` and is unique. It contains no spaces.
- Track name is 1 to 64 characters after trimming. Blank is rejected. Spaces are allowed.
- `input_kind` is `source` or `sink-monitor`.
- An empty PipeWire target is the CLI argument `-`.
- Gain is linear from 0.00× to 4.00×. Gain and mute are not saved. New tracks start at 1.00× and unmuted.
- None (empty `monitor_target`) does not open a playback stream.
- A capture connection failure fails startup and the error includes that track’s name. The session does not start.
- A monitor connection failure does not fail the session. The track still records, `monitor_error` is set, and no playback stream is left running.
- A mid-session capture disconnect sets that track’s `state` to `error`, stops its packets, and leaves the session running.
- Telemetry `audio` keeps `enabled` and the session counters, drops `desktop` and `microphone`, and uses a `tracks` array in list order.
- `config::Settings` schema is not bumped and its desktop/microphone fields stay.
- Unsaved track list means the two defaults. A saved empty list means zero tracks.
- Device combos, add, and remove are disabled while a session is starting, active, or stopping.
- Audio control acknowledgement timeout stays 5 seconds.
- Do not run a live 360 FPS recording for this work.
- `UX-CONTRACT.md` matches the new behavior.

---

### Task 1: Track validation and stream properties

**Files:**
- Create: `audio/track_config.hpp`
- Create: `audio/track_config.cpp`
- Create: `audio/tests/track_config_test.cpp`
- Modify: `audio/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing
- Produces:
  - `fluxrec::audio::max_audio_tracks` = `8`
  - `enum class InputKind : std::uint8_t { source, sink_monitor }`
  - `struct AudioTrack { std::string id, name; InputKind input_kind; std::string input_target, monitor_target; }`
  - `struct TrackStreamProperties { bool capture_sink; std::string target_object; bool open_monitor; std::string monitor_object; }`
  - `[[nodiscard]] bool validTrackId(std::string_view id) noexcept`
  - `[[nodiscard]] std::expected<AudioTrack, std::string> validateAudioTrack(AudioTrack track)`
  - `[[nodiscard]] std::expected<std::vector<AudioTrack>, std::string> validateAudioTracks(std::vector<AudioTrack> tracks)`
  - `[[nodiscard]] std::vector<AudioTrack> defaultAudioTracks()`
  - `[[nodiscard]] TrackStreamProperties streamProperties(const AudioTrack& track)`
  - `[[nodiscard]] std::string nextTrackId(std::span<const AudioTrack> tracks)`

- [ ] **Step 1: Write the failing test**

Create `audio/tests/track_config_test.cpp`:

```cpp
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
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build-root -j2 --target fluxrec_audio_track_config_test && ./build-root/audio/fluxrec_audio_track_config_test`

Expected: the target does not exist yet, or the include fails.

- [ ] **Step 3: Implement track config**

Create `audio/track_config.hpp`:

```cpp
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
```

Create `audio/track_config.cpp` with these rules:

- `validTrackId`: length 1..64, first byte is `A-Z` or `a-z`, every later byte is alphanumeric, `_`, or `-`.
- `validateAudioTrack`: trim `name` by removing leading and trailing ASCII spaces. Reject an empty result or a result longer than 64. Reject an invalid id. Copy the trimmed name into the returned track.
- `validateAudioTracks`: reject `tracks.size() > max_audio_tracks`. Validate each track. Reject a repeated id with `duplicate audio track id <id>`.
- `defaultAudioTracks`: `{ "desktop", "Desktop audio", sink_monitor, "", "" }` then `{ "microphone", "Microphone", source, "", "" }`.
- `streamProperties`: `capture_sink` is true for `sink_monitor`. If `input_target` ends in `.monitor`, `target_object` is the prefix before that suffix; otherwise it is `input_target`. `open_monitor` is true when `monitor_target` is non-empty. `monitor_object` is `monitor_target`.
- `nextTrackId`: return the first `track-N` for `N` from 1 upward whose id is not already present.

Add to `audio/CMakeLists.txt` before the `fluxrec_audio` target:

```cmake
add_library(fluxrec_audio_tracks STATIC track_config.cpp)
target_compile_features(fluxrec_audio_tracks PUBLIC cxx_std_23)
target_include_directories(fluxrec_audio_tracks PUBLIC "${CMAKE_CURRENT_LIST_DIR}/..")
target_compile_options(fluxrec_audio_tracks PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
```

Link it publicly from `fluxrec_audio`. Inside `if(BUILD_TESTING)` add:

```cmake
add_executable(fluxrec_audio_track_config_test tests/track_config_test.cpp)
target_compile_features(fluxrec_audio_track_config_test PRIVATE cxx_std_23)
target_link_libraries(fluxrec_audio_track_config_test PRIVATE fluxrec_audio_tracks)
target_compile_options(fluxrec_audio_track_config_test PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
add_test(NAME fluxrec_audio_track_config COMMAND fluxrec_audio_track_config_test)
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build-root -j2 --target fluxrec_audio_track_config_test && ./build-root/audio/fluxrec_audio_track_config_test`

Expected: exit 0 and no `FAIL` lines.

- [ ] **Step 5: Commit**

```bash
git add audio/track_config.hpp audio/track_config.cpp audio/tests/track_config_test.cpp audio/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Add validated audio track configuration.

Capture and the mixer need one description of a track's input kind, target, and optional monitor before either side grows past the fixed desktop and microphone pair.
EOF
)"
```

---

### Task 2: Parse `--audio-track`

**Files:**
- Modify: `engine/record_options.hpp`
- Modify: `engine/record_options.cpp`
- Modify: `engine/tests/record_options_test.cpp`
- Modify: `engine/tests/CMakeLists.txt` (add `../../audio/track_config.cpp` to `fluxrec_record_options_test` sources, and include the repo root, which that target already does)

**Interfaces:**
- Consumes: `fluxrec::audio::AudioTrack`, `validateAudioTracks`, `InputKind`
- Produces: `RecordOptions::audio_tracks` as `std::vector<fluxrec::audio::AudioTrack>`. Empty when `--audio on` is given without tracks; the session layer applies defaults later. `--audio off` clears `audio_tracks` even if tracks were parsed earlier.

- [ ] **Step 1: Write the failing test**

In `engine/tests/record_options_test.cpp`, before `return ok ? 0 : 1;`, add:

```cpp
const std::array<std::string_view, 12> track_args{
    "--output", "clip.mkv", "--audio", "on",
    "--audio-track", "desktop", "Desktop audio", "sink-monitor", "-", "-",
    "--audio-track", "mic-2", "Headset mic", "source", "alsa_input.usb", "alsa_output.speakers",
};
const auto tracks = parseRecordOptions(track_args);
ok &= tracks.has_value();
if (tracks) {
    ok &= tracks->audio_enabled;
    ok &= tracks->audio_tracks.size() == 2;
    ok &= tracks->audio_tracks[0].input_target.empty();
    ok &= tracks->audio_tracks[0].monitor_target.empty();
    ok &= tracks->audio_tracks[0].input_kind == fluxrec::audio::InputKind::sink_monitor;
    ok &= tracks->audio_tracks[1].id == "mic-2";
    ok &= tracks->audio_tracks[1].input_target == "alsa_input.usb";
    ok &= tracks->audio_tracks[1].monitor_target == "alsa_output.speakers";
}
const std::array<std::string_view, 8> audio_off_clears{
    "--output", "clip.mkv",
    "--audio-track", "desktop", "Desktop audio", "source", "-", "-",
    "--audio", "off",
};
const auto cleared = parseRecordOptions(audio_off_clears);
ok &= cleared.has_value() && cleared->audio_tracks.empty() && !cleared->audio_enabled;
ok &= expect_error({"--output", "clip.mkv", "--audio-track", "desktop", "Name", "source", "-"},
                   "audio track is missing its monitor target");
ok &= expect_error({"--output", "clip.mkv", "--audio-track", "1bad", "Name", "source", "-", "-"},
                   "bad track id");
ok &= expect_error({"--output", "clip.mkv", "--audio-track", "desk", "Name", "input", "-", "-"},
                   "unknown kind");
```

Then append eight more `--audio-track deskN Name source - -` groups and expect an error for nine tracks. Build the vector in the test rather than a giant array if that is clearer; the assertion is `!parseRecordOptions(arguments)`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build-root -j2 --target fluxrec_record_options_test && ./build-root/engine/tests/fluxrec_record_options_test`

Expected: compile failure because `audio_tracks` does not exist.

- [ ] **Step 3: Parse five arguments per track**

In `engine/record_options.hpp`, include `audio/track_config.hpp` and add `std::vector<fluxrec::audio::AudioTrack> audio_tracks;` next to `audio_enabled`.

In `engine/record_options.cpp`, on `--audio off`, set `audio_enabled` false and `audio_tracks.clear()`. On `--audio-track`, require five following arguments. `-` becomes an empty string. `source` and `sink-monitor` map to `InputKind`. Any other kind returns `--audio-track kind must be source or sink-monitor`. After the parse loop, if `audio_tracks` is non-empty, call `validateAudioTracks` and return its error text. If `audio_enabled` is false, clear `audio_tracks` again so a trailing `--audio off` wins.

Update the usage string in `engine/record.cpp` to include `--audio-track <id> <name> <source|sink-monitor> <input-target|-> <monitor-target|->`.

Add `../../audio/track_config.cpp` to the `fluxrec_record_options_test` sources.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build-root -j2 --target fluxrec_record_options_test && ./build-root/engine/tests/fluxrec_record_options_test`

Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git add engine/record_options.hpp engine/record_options.cpp engine/record.cpp engine/tests/record_options_test.cpp engine/tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Parse ordered audio track arguments.

The recorder needs the mixer’s track list at launch, including an explicit empty device target, without guessing devices inside the flag parser.
EOF
)"
```

---

### Task 3: Address audio controls by track id

**Files:**
- Modify: `engine/control_pipe.hpp`
- Modify: `engine/control_pipe.cpp`
- Modify: `engine/control_service.hpp`
- Modify: `engine/control_service.cpp`
- Modify: `engine/tests/audio_control_integration.cpp`
- Modify: `engine/record.cpp` acknowledgement JSON

**Interfaces:**
- Consumes: track id grammar from Task 1 (`validTrackId`)
- Produces: `ControlCommand::audio_track_id` (`std::string`) and `ControlServiceEvent::audio_track_id` (`std::string`). Remove `ControlAudioSource`.

- [ ] **Step 1: Write the failing test**

In `engine/tests/audio_control_integration.cpp`, replace expectations of `ControlAudioSource::desktop` and `microphone` with string ids. Send:

```text
set_audio_volume desktop 0.75
set_audio_volume track-2 2.0
set_audio_mute headset on
set_audio_volume has space 1.0
set_audio_volume desktop 4.01
```

Expect the first three commands to pop with ids `desktop`, `track-2`, and `headset`, gains `0.75` and `2.0`, and mute true. Expect the space id and the out-of-range gain to increment `invalid_lines` and not enqueue.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build-root -j2 --target fluxrec_audio_control_integration_test && ./build-root/engine/tests/fluxrec_audio_control_integration_test`

Expected: compile failure on `ControlAudioSource`.

- [ ] **Step 3: Store the id string**

Replace `parse_audio_source` with `validTrackId(source_text)`. Assign `command.audio_track_id = std::string(source_text)`.

Replace `ControlServiceEvent::audio_source` with `std::string audio_track_id`. Copy it from the command in `emit_audio_control`.

In `engine/record.cpp` `write_event_json`, write the track id with `write_json_string` under the key `source`. Delete the desktop/microphone ternary.

Until Task 6 replaces it, the apply site in `record.cpp` maps only the two ids the current session still exposes:

```cpp
const auto source = command.audio_track_id == "microphone"
    ? fluxrec::audio::Source::microphone
    : command.audio_track_id == "desktop"
        ? fluxrec::audio::Source::desktop
        : std::optional<fluxrec::audio::Source>{};
if (!source) {
    // emit audio_control_failed with message "Unknown audio track."
    return;
}
```

Do not leave `ControlAudioSource` in any header.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build-root -j2 --target fluxrec_audio_control_integration_test fluxrec_control_service_integration_test && ./build-root/engine/tests/fluxrec_audio_control_integration_test && ./build-root/engine/tests/fluxrec_control_service_integration_test`

Expected: both exit 0.

- [ ] **Step 5: Commit**

```bash
git add engine/control_pipe.hpp engine/control_pipe.cpp engine/control_service.hpp engine/control_service.cpp engine/record.cpp engine/tests/audio_control_integration.cpp
git commit -m "$(cat <<'EOF'
Address audio controls by track id.

Volume and mute have to name whichever track the mixer added, not only the original desktop and microphone pair.
EOF
)"
```

---

### Task 4: Mux one to eight audio streams

**Files:**
- Modify: `mux/mux_writer.cpp` around the `audio_streams.size() > 2U` check
- Modify: `mux/mux_writer.hpp` comment that says at most two tracks
- Modify: `mux/tests/mux_writer_integration.cpp`

**Interfaces:**
- Consumes: existing `MuxConfig::audio_streams`
- Produces: acceptance of 1..8 audio streams. Nine throws `std::invalid_argument` containing `at most eight`. An out-of-range `stream_index` still returns `EnqueueResult::invalid_packet`.

- [ ] **Step 1: Write the failing test**

In `mux/tests/mux_writer_integration.cpp`, add `test_audio_track_counts()` and call it from `main`. It loops `track_count` over `{1, 8}`:

- Build that many `AudioEncoder` AAC instances.
- Put their `config()` values in `MuxConfig::audio_streams` in that order.
- Encode one silent frame per stream with `stream_index` equal to the loop index and a shared `video_origin_time_ns`.
- Write them with the existing enqueue helper, close the writer, and open the file with `avformat_open_input`.
- Require `nb_streams == track_count + 1` and `streams[index + 1]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO` for each track.

Then construct a config with 9 copies of one AAC config and expect `MuxWriter` to throw. Enqueue one packet with `stream_index == track_count` on the 1-stream writer before close and expect `invalid_packet`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build-root -j2 --target fluxrec_mux_integration_test && ./build-root/mux/fluxrec_mux_integration_test`

Expected: the 8-stream case throws `mux supports at most two audio streams`.

- [ ] **Step 3: Raise the limit**

Change the check to `config.audio_streams.size() > 8U` and the message to `mux supports at most eight audio streams`. Update the header comment to say packet `stream_index` is the track-list order, from 0 through 7.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build-root -j2 --target fluxrec_mux_integration_test && ./build-root/mux/fluxrec_mux_integration_test`

Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git add mux/mux_writer.cpp mux/mux_writer.hpp mux/tests/mux_writer_integration.cpp
git commit -m "$(cat <<'EOF'
Allow up to eight muxed audio streams.

Track order in the file has to match the mixer list, including a recording that uses one track or all eight.
EOF
)"
```

---

### Task 5: Capture and monitor one stream per track

**Files:**
- Modify: `audio/audio.hpp`
- Modify: `audio/audio.cpp`
- Create: `audio/tests/track_stream_properties_test.cpp` only if Task 1’s property test is not enough; prefer extending nothing here and testing through `streamProperties` already covered. This task’s check is a compile-time fixture plus the existing property test.
- Modify: `audio/tests/av_sync_fixture.cpp` so it builds `Config::tracks` instead of `desktop_target` and `microphone_target`.

**Interfaces:**
- Consumes: `AudioTrack`, `streamProperties`
- Produces:
  - `audio::Config::tracks` (`std::vector<AudioTrack>`). Remove `desktop_target` and `microphone_target`.
  - `Format::track_index` and `Packet::track_index` (`std::size_t`). Remove `Format::source` and `Packet::source`.
  - `Capture::setVolume(std::size_t track_index, float)`
  - `Capture::setMuted(std::size_t track_index, bool)`
  - `Capture::snapshot(std::size_t track_index) const`
  - `Capture::trackCount() const`
  - `SourceSnapshot::monitor_error` (`std::string`)
  - Default `Config{}` uses `defaultAudioTracks()` so existing two-stream callers keep that behavior until they pass an explicit list.

- [ ] **Step 1: Write the failing compile check**

Create `audio/tests/capture_track_api_test.cpp`. It includes `audio/audio.hpp` and instantiates:

```cpp
fluxrec::audio::Config config;
config.tracks = fluxrec::audio::defaultAudioTracks();
config.tracks.push_back(
    {"track-1", "Track 1", fluxrec::audio::InputKind::source, {}, "speakers"});
static_assert(std::is_same_v<decltype(config.tracks), std::vector<fluxrec::audio::AudioTrack>>);
```

That file need not call `start()`, because a live PipeWire server is out of scope. Link it to `fluxrec_audio` so the new signatures must compile. Name the test `fluxrec_audio_capture_track_api_test` and register it with CTest.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build-root -j2 --target fluxrec_audio_capture_track_api_test`

Expected: `config.tracks` is not a member.

- [ ] **Step 3: Replace the fixed pair with a track vector**

In `audio.hpp`, replace the `Source` enum uses on `Format` and `Packet` with `std::size_t track_index`. Keep `SourceState`. Add `monitor_error` to `SourceSnapshot`. Change `Config` to hold `std::vector<AudioTrack> tracks` default-initialized from `defaultAudioTracks()`. Change the three public methods to take `std::size_t track_index`. Add `[[nodiscard]] std::size_t trackCount() const noexcept`.

In `audio.cpp`:

- Replace `std::array<..., kSourceCount>` for runtimes, capture streams, hooks, and user data with `std::vector` sized from `config_.tracks.size()` in the constructor. Reject `tracks.size() > max_audio_tracks` or a failed `validateAudioTracks` in `start()` by setting every constructed runtime’s error and returning false. An empty `tracks` vector starts successfully and captures nothing.
- `createStream(std::size_t index)` calls `streamProperties(config_.tracks[index])`. Set `PW_KEY_STREAM_CAPTURE_SINK` from `capture_sink`. Set `PW_KEY_TARGET_OBJECT` only when `target_object` is non-empty. Node name is `fluxrec.audio.<id>`.
- Add `createMonitor(std::size_t index)` when `open_monitor` is true. It connects `PW_DIRECTION_OUTPUT` to `monitor_object` with `PW_KEY_MEDIA_CLASS` `Stream/Output/Audio`, node name `fluxrec.audio.<id>.monitor`, and the same float format as capture. Store the stream beside the capture stream. If connect fails, set `monitor_error`, destroy that playback stream, and return true so capture startup continues.
- In the capture process callback, after gain and mute, if that track’s monitor stream is streaming, queue the same float samples to a per-track monitor ring drained by the monitor stream’s process callback. Do not pass those samples to the AAC encoder twice; the encoder still consumes the capture queue only.
- `setVolume` / `setMuted` ignore an out-of-range index. `snapshot` returns a default snapshot for an out-of-range index.
- Startup failure on a capture stream stores the PipeWire error on that track and returns false from `start()`. `AudioSession` in Task 6 turns that into a named startup error.
- On capture stream error after start, set that runtime’s state to `error` and stop enqueueing its packets. Do not stop other tracks.

Update `audio/tests/av_sync_fixture.cpp` assignments:

```cpp
audio_config.capture.tracks = fluxrec::audio::defaultAudioTracks();
audio_config.capture.tracks[0].input_kind = fluxrec::audio::InputKind::sink_monitor;
audio_config.capture.tracks[0].input_target = options.desktop_sink;
audio_config.capture.tracks[1].input_kind = fluxrec::audio::InputKind::source;
audio_config.capture.tracks[1].input_target = options.microphone_source;
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build-root -j2 --target fluxrec_audio_capture_track_api_test fluxrec_audio_track_config_test && ./build-root/audio/fluxrec_audio_capture_track_api_test && ./build-root/audio/fluxrec_audio_track_config_test`

Expected: both exit 0. `fluxrec_audio` itself builds.

- [ ] **Step 5: Commit**

```bash
git add audio/audio.hpp audio/audio.cpp audio/tests/capture_track_api_test.cpp audio/tests/av_sync_fixture.cpp audio/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Capture one PipeWire stream per audio track.

Each track records its selected input, and a chosen monitor sink hears the gained samples without those playback buffers becoming an encoded packet.
EOF
)"
```

---

### Task 6: Forward N tracks and publish track telemetry

**Files:**
- Modify: `engine/audio_session.hpp`
- Modify: `engine/audio_session.cpp`
- Modify: `engine/record.cpp` (`append_audio_telemetry`, startup, volume/mute dispatch, stderr meter line)

**Interfaces:**
- Consumes: `Capture::trackCount`, `snapshot(index)`, `Format::track_index`, `Packet::track_index`, `AudioTrack`
- Produces:
  - `AudioSession` constructed from `AudioSessionConfig` whose `capture.tracks` is the validated list
  - `stream_configs()` returns one `AudioStreamConfig` per track, in order
  - `set_volume(std::string_view id, float)` and `set_muted(std::string_view id, bool)` return false for an unknown id
  - `AudioSessionSnapshot::tracks` is `std::vector<audio::SourceSnapshot>` plus parallel ids and names. Remove `desktop` and `microphone` fields.
  - JSON `audio.tracks[]` objects contain `id`, `name`, `error`, `monitor_error`, and the existing per-source numeric fields

- [ ] **Step 1: Write the failing observation**

There is no standalone audio-session binary. Extend `engine/tests/audio_control_integration.cpp` only for the pipe. For session JSON, add a small driver `engine/tests/audio_telemetry_shape_test.cpp` that calls a new free function declared in `engine/audio_session.hpp`:

```cpp
void writeAudioTelemetryObject(std::ostream& output, const AudioSessionSnapshot& snapshot, bool enabled);
```

The test builds a snapshot with two tracks, ids `desktop` and `track-1`, one `monitor_error` of `speakers missing`, and expects the serialized object to contain `"tracks":[` , both ids, `monitor_error`, and neither `"desktop":` nor `"microphone":`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build-root -j2 --target fluxrec_audio_telemetry_shape_test`

Expected: `writeAudioTelemetryObject` is not declared.

- [ ] **Step 3: Index the session by track**

Replace `source_count` arrays in `audio_session.cpp` with vectors sized from `config_.capture.tracks.size()`. `formats_ready` is true when every track has extradata, including the zero-track case. `handle_format` rejects a `track_index` outside the vector.

On `start()` failure, scan snapshots for the first non-empty `error` and call `set_error(tracks[index].name + " audio startup failed: " + error)`. Timeout text is `timed out waiting for audio track codec extradata`.

`set_volume` / `set_muted` find the id. Unknown ids return false and do not change gain.

`writeAudioTelemetryObject` writes `{"enabled":false}` when disabled. Otherwise it writes `enabled`, the `tracks` array, and the existing session counters (`captured_packets`, `callback_drops`, `unforwarded_drops`, `mux_drops`, `replay_stored_packets`, `replay_drops`, `sync_errors`, `audio_errors`, `queue_packets`, `queue_bytes`). Each track object starts with `id` and `name` as JSON strings, then the current source fields, then `"error"` and `"monitor_error"`.

`append_audio_telemetry` in `record.cpp` calls that function. The stderr line that prints `microphone_peak` prints `tracks=<count>` and the first track’s peak, or `tracks=0` when audio is off.

`record.cpp` startup deletes the `audio_streams.size() != 2` check. Pass `options.audio_tracks` into the session config when non-empty. When `audio_enabled` is true and `audio_tracks` is empty, assign `defaultAudioTracks()` before constructing the session. When `audio_enabled` is false, do not construct a session.

Wire volume and mute:

```cpp
const bool applied = command.type == ControlCommandType::set_audio_volume
    ? audio_session->set_volume(command.audio_track_id, command.audio_gain)
    : audio_session->set_muted(command.audio_track_id, command.audio_muted);
```

Unknown id emits `audio_control_failed` with message `Unknown audio track.`

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build-root -j2 --target fluxrec_audio_telemetry_shape_test fluxrec-record && ./build-root/engine/tests/fluxrec_audio_telemetry_shape_test`

Expected: the shape test exits 0 and `fluxrec-record` links.

- [ ] **Step 5: Commit**

```bash
git add engine/audio_session.hpp engine/audio_session.cpp engine/record.cpp engine/tests/audio_telemetry_shape_test.cpp engine/tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Report and forward every configured audio track.

Startup, mux stream order, and telemetry now follow the track list, and a missing monitor no longer hides a capture error for a different track.
EOF
)"
```

---

### Task 7: Persist tracks and launch them

**Files:**
- Modify: `ui/src/recorder_settings.hpp`
- Modify: `ui/src/recorder_settings.cpp`
- Modify: `ui/src/process_recorder_backend.cpp`
- Modify: `ui/src/main.cpp` smoke section that already round-trips settings
- Modify: `ui/CMakeLists.txt` so the UI links `fluxrec_audio_tracks`

**Interfaces:**
- Consumes: `AudioTrack`, `defaultAudioTracks`, `validateAudioTracks`, `nextTrackId`
- Produces:
  - `struct AudioTrackSettings { QString id, name, input_kind, input_target, monitor_target; }`
  - `RecorderSettings::audio_tracks`
  - `RecorderSettings::audio_tracks_custom`
  - `QList<AudioTrackSettings> effectiveAudioTracks(const RecorderSettings&)`
  - `void appendAudioTrackArguments(QStringList& arguments, const RecorderSettings& settings)`

- [ ] **Step 1: Write the failing smoke assertions**

In the `--smoke` path in `ui/src/main.cpp`, after the scene settings round-trip, set:

```cpp
scene_round_trip_settings.audio_tracks_custom = true;
scene_round_trip_settings.audio_tracks = {
    {QStringLiteral("desktop"), QStringLiteral("Desktop audio"),
     QStringLiteral("sink-monitor"), QString(), QString()},
    {QStringLiteral("track-1"), QStringLiteral("Headset"),
     QStringLiteral("source"), QStringLiteral("alsa_input.usb"),
     QStringLiteral("alsa_output.speakers")},
};
scene_round_trip_settings.audio_enabled = true;
fluxrec::ui::saveSettings(scene_round_trip_settings);
const auto audio_round_trip = fluxrec::ui::loadSettings();
```

Fail the smoke process if the loaded list differs, if `effectiveAudioTracks` on a default-constructed settings value is not the two defaults, or if a custom empty list does not make `effectiveAudioTracks` empty. Also save a custom list with a duplicate id and a custom list with a blank name; both must load back as non-custom defaults. A ninth array entry must load the same way. Call `appendAudioTrackArguments` for the enabled custom settings and require the argument sequence:

```text
--audio on
--audio-track desktop "Desktop audio" sink-monitor - -
--audio-track track-1 Headset source alsa_input.usb alsa_output.speakers
```

Call it again with `audio_enabled == false` and require no `--audio-track` and `--audio off`.

- [ ] **Step 2: Run smoke to verify it fails**

Run: `cmake --build build-root -j2 --target fluxrec-ui && QT_QPA_PLATFORM=offscreen ./build-root/ui/fluxrec-ui --smoke`

Expected: compile failure or `audio tracks did not round-trip`.

- [ ] **Step 3: Save the list and append arguments**

Store `audio/tracksCustom` as a bool. Store the list with `beginWriteArray("audio/tracks")` using keys `id`, `name`, `inputKind`, `inputTarget`, `monitorTarget`. On load, drop the saved list and set `audio_tracks_custom` false when `validateAudioTracks` would reject it, so a corrupt ini falls back to the defaults.

`effectiveAudioTracks` returns `defaultAudioTracks()` converted to `AudioTrackSettings` when `audio_tracks_custom` is false. Otherwise it returns `audio_tracks`, which may be empty.

`appendAudioTrackArguments` always appends `--audio` `on` or `off`. When audio is enabled, it appends one `--audio-track` group per effective track. Empty targets become `-`. `input_kind` `sink_monitor` is written `sink-monitor`.

Both `start_recording` and the replay-only launch in `process_recorder_backend.cpp` call `appendAudioTrackArguments` instead of appending `--audio` themselves.

- [ ] **Step 4: Run smoke to verify it passes**

Run: `cmake --build build-root -j2 --target fluxrec-ui && QT_QPA_PLATFORM=offscreen ./build-root/ui/fluxrec-ui --smoke`

Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git add ui/src/recorder_settings.hpp ui/src/recorder_settings.cpp ui/src/process_recorder_backend.cpp ui/src/main.cpp ui/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Launch the saved audio track list.

An untouched settings file still records the default desktop monitor and microphone, while a saved empty list records none.
EOF
)"
```

---

### Task 8: List PipeWire inputs and outputs

**Files:**
- Create: `ui/src/audio_device_catalog.hpp`
- Create: `ui/src/audio_device_catalog.cpp`
- Modify: `ui/CMakeLists.txt` to compile the catalog and link `PkgConfig::PIPEWIRE` privately on `fluxrec-ui`
- Modify: `ui/src/main.cpp` smoke path

**Interfaces:**
- Consumes: nothing from earlier tasks except Qt
- Produces:
  - `struct AudioDeviceEntry { QString target; QString description; QString input_kind; }`
  - `[[nodiscard]] bool isAudioSourceNode(QStringView media_class, QStringView node_name)`
  - `[[nodiscard]] bool isAudioSinkNode(QStringView media_class)`
  - `class AudioDeviceCatalog` with `refresh()`, `QString error()`, `QList<AudioDeviceEntry> inputs()`, `QList<AudioDeviceEntry> outputs()`, and `available() const`

- [ ] **Step 1: Write the failing classification check**

In `--smoke`, fail unless all of these hold:

- `isAudioSourceNode("Audio/Source", "alsa_input.usb")` is true
- `isAudioSourceNode("Audio/Source", "speakers.monitor")` is false
- `isAudioSinkNode("Audio/Sink")` is true
- `isAudioSourceNode("Video/Source", "camera")` is false
- `isAudioSinkNode("Audio/Source")` is false

Use two functions so one sink node can be both an input monitor and a playback device:

```cpp
[[nodiscard]] bool isAudioSourceNode(QStringView media_class, QStringView node_name);
[[nodiscard]] bool isAudioSinkNode(QStringView media_class);
```

`isAudioSourceNode` is true only for media class `Audio/Source` whose node name does not end in `.monitor`. `isAudioSinkNode` is true only for media class `Audio/Sink`.

- [ ] **Step 2: Run smoke to verify it fails**

Run: `QT_QPA_PLATFORM=offscreen ./build-root/ui/fluxrec-ui --smoke`

Expected: the new functions are undeclared.

- [ ] **Step 3: Enumerate nodes**

`refresh()` creates a short-lived `pw_thread_loop`, connects, and round-trips a registry sync. For each node, read `node.name`, falling back to `object.serial` as a decimal string when the name is empty. Description is `node.description`, then `node.name`, then the serial.

Inputs: source nodes as `input_kind = "source"`, sink nodes as `input_kind = "sink-monitor"`. Outputs: sink nodes only. The mixer adds None itself. On any PipeWire failure, leave the previous lists intact, set `error()` to `Audio devices are unavailable.`, and return false from `refresh()`.

- [ ] **Step 4: Run smoke to verify it passes**

Run: `cmake --build build-root -j2 --target fluxrec-ui && QT_QPA_PLATFORM=offscreen ./build-root/ui/fluxrec-ui --smoke`

Expected: exit 0. Classification does not connect to PipeWire.

- [ ] **Step 5: Commit**

```bash
git add ui/src/audio_device_catalog.hpp ui/src/audio_device_catalog.cpp ui/src/main.cpp ui/CMakeLists.txt
git commit -m "$(cat <<'EOF'
List PipeWire inputs and playback sinks for audio tracks.

The mixer can offer microphones, desktop monitors, and speaker outputs without asking the recorder process to be running.
EOF
)"
```

---

### Task 9: Mixer rows with device combos

**Files:**
- Modify: `ui/src/audio_mixer_panel.hpp`
- Modify: `ui/src/audio_mixer_panel.cpp`
- Modify: `ui/src/main_window.cpp`
- Modify: `ui/src/process_recorder_backend.cpp` so volume/mute accept any track id matching `validTrackId` rather than only `desktop` and `microphone`
- Modify: `ui/src/settings_dialog.cpp` checkbox copy
- Modify: `ui/src/main.cpp` smoke path

**Interfaces:**
- Consumes: `effectiveAudioTracks`, `nextTrackId`, `AudioDeviceCatalog`, `validateAudioTrack`
- Produces: `AudioMixerPanel::setTracks(QList<AudioTrackSettings>, bool custom)`, `setDevices(const AudioDeviceCatalog&)`, `setSessionBusy(bool)`, and signal `tracksEdited(QList<AudioTrackSettings>)`. Object names: `addAudioTrackButton`, `audioTrackRow`, `audioTrackInput`, `audioTrackOutput`, `removeAudioTrackButton`.

- [ ] **Step 1: Write the failing smoke check**

Construct an `AudioMixerPanel`, call `setTracks` with the two defaults and `custom == false`, then:

- Click `addAudioTrackButton` six times and require eight rows. The new ids are `track-1` through `track-6`. The last row’s input kind is `source`, both targets are empty, and its name is `Track 6`.
- Require `addAudioTrackButton` disabled at eight rows.
- Call `setSessionBusy(true)` and require add, every remove button, and every combo disabled.
- Call `setSessionBusy(false)` and require them enabled.
- Feed a catalog error and a device list that omits the saved target `missing-mic`. Require the input combo still contains that target, marked `missing-mic (unavailable)`, and a label whose text is `Audio devices are unavailable.`

- [ ] **Step 2: Run smoke to verify it fails**

Run: `QT_QPA_PLATFORM=offscreen ./build-root/ui/fluxrec-ui --smoke`

Expected: `addAudioTrackButton` is not found.

- [ ] **Step 3: Build one row per track**

Replace the fixed desktop and microphone members with a `QVBoxLayout` of rows. Each row has a name field, input combo, output combo, remove button, and the existing meter, gain, mute, and buffer widgets. Gain and mute stay hidden until that row’s telemetry `state` is `streaming` during an active session.

The input combo lists catalog inputs. The output combo lists `None` with empty target, then catalog outputs. Selecting a missing catalog does not clear the current item. A selected target absent from the new list remains, with the suffix ` (unavailable)`.

Add appends a track from `nextTrackId` and emits `tracksEdited` with the full list. Remove emits the list without that row. The first edit is what makes the caller set `audio_tracks_custom`. `MainWindow` saves settings on `tracksEdited`.

`setSnapshot` reads `audio.tracks` and matches rows by `id`. It shows `monitor_error` in the row status when non-empty. It does not invent meter values.

`process_recorder_backend.cpp` accepts any id for which `validTrackId` is true.

Settings checkbox label: `Capture audio tracks`. Description: `The audio mixer chooses each track’s input and monitor output. Applies to the next capture session.`

The empty-state sentence when audio is off: `Audio capture is off. Enable audio tracks in Settings. The mixer keeps the track list for the next session.`

Output combo accessible description: `Play this track’s captured input through the selected speakers while the session is running.`

- [ ] **Step 4: Run smoke to verify it passes**

Run: `cmake --build build-root -j2 --target fluxrec-ui && QT_QPA_PLATFORM=offscreen ./build-root/ui/fluxrec-ui --smoke`

Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git add ui/src/audio_mixer_panel.hpp ui/src/audio_mixer_panel.cpp ui/src/main_window.cpp ui/src/process_recorder_backend.cpp ui/src/settings_dialog.cpp ui/src/main.cpp
git commit -m "$(cat <<'EOF'
Edit audio tracks and their devices in the mixer.

Each row chooses the recorded input and the optional monitor output, and those choices stay put while a capture session is running.
EOF
)"
```

---

### Task 10: Update the UX contract

**Files:**
- Modify: `UX-CONTRACT.md` audio paragraph
- Modify: `ui/README.md` sentence that says the adapter only passes `--audio on|off`

**Interfaces:**
- Consumes: the behavior from Tasks 7 and 9
- Produces: contract text that matches the spec

- [ ] **Step 1: Replace the audio paragraph**

Replace the paragraph beginning `Audio capture is opt-in` with text that states:

- Capture is off until Settings “Capture audio tracks” is enabled.
- The mixer holds up to eight tracks. Each chooses an input and an optional monitor output.
- An unsaved list captures the default desktop monitor and the default microphone, with no monitor playback.
- A saved empty list captures no audio.
- Device edits apply to the next session and are disabled while a session is busy.
- Gain and mute still wait for a streaming track and an acknowledgement.
- Monitor playback is not a recorded track. A failed monitor does not fail the session.
- A failed input fails startup, and the error includes the track name.
- Telemetry is `audio.tracks`, and missing values stay “Not reported.”

In `ui/README.md`, state that the adapter also passes one `--audio-track` group per effective track when audio is on.

- [ ] **Step 2: Check the wording**

Run: `rg -n "desktop and microphone audio|at most two" UX-CONTRACT.md ui/README.md`

Expected: no remaining claim that audio is only the default pair or that the mux holds only two audio streams. The measurements document may still describe historical two-track runs; leave it.

- [ ] **Step 3: Commit**

```bash
git add UX-CONTRACT.md ui/README.md
git commit -m "$(cat <<'EOF'
Document per-track audio input and monitor choices.

The contract was still describing a fixed desktop and microphone pair after the mixer gained an editable track list.
EOF
)"
```

---

### Task 11: Regression pass

**Files:**
- Modify: any remaining call site that still names `audio::Source`, `desktop_target`, `microphone_target`, `ControlAudioSource`, or JSON `"desktop"` / `"microphone"` audio objects.

**Interfaces:**
- Consumes: all previous tasks
- Produces: a built tree whose audio and UI smoke tests pass

- [ ] **Step 1: Find leftovers**

Run:

```bash
rg -n "desktop_target|microphone_target|ControlAudioSource|audio::Source|\"desktop\":|\"microphone\":" \
  audio engine ui mux replay --glob '!docs/**' --glob '!.validation/**'
```

Update every remaining production or test use. Replay code that stores `EncodedAudioPacket::stream_index` already follows the mux list; do not give it a new track type.

- [ ] **Step 2: Run the focused tests**

Run:

```bash
cmake --build build-root -j2 --target \
  fluxrec_audio_track_config_test \
  fluxrec_record_options_test \
  fluxrec_audio_control_integration_test \
  fluxrec_audio_telemetry_shape_test \
  fluxrec_mux_integration_test \
  fluxrec-ui \
  fluxrec-record
./build-root/audio/fluxrec_audio_track_config_test
./build-root/engine/tests/fluxrec_record_options_test
./build-root/engine/tests/fluxrec_audio_control_integration_test
./build-root/engine/tests/fluxrec_audio_telemetry_shape_test
./build-root/mux/fluxrec_mux_integration_test
QT_QPA_PLATFORM=offscreen ./build-root/ui/fluxrec-ui --smoke
```

Expected: every command exits 0.

- [ ] **Step 3: Commit leftovers only if Step 1 changed files**

```bash
git add audio engine ui mux replay
git commit -m "$(cat <<'EOF'
Finish the audio track id migration.

Call sites that still assumed exactly two named sources could not follow the mixer list.
EOF
)"
```

If Step 1 finds nothing, do not create an empty commit.
