# Audio tracks with input and monitor output

## Purpose

The audio mixer can hold up to eight tracks. Each track records one chosen input and can play that captured audio to one chosen output while a recording or replay-only session is running.

Audio stays off until the existing Settings checkbox enables it. Device choices apply to the next session. Gain and mute stay live controls on a track that telemetry reports as streaming.

## Track

A track has:

- `id`: matches `^[A-Za-z][A-Za-z0-9_-]{0,63}$`. Unique within the list. The control line and telemetry use this id. It contains no spaces.
- `name`: one to 64 characters after trimming. Blank is rejected. Spaces are allowed. The launch argument carries the name as one argv element.
- `input_kind`: `source` or `sink-monitor`.
- `input_target`: PipeWire node name or object serial. Empty means PipeWire’s default for that kind.
- `monitor_target`: playback sink node name or object serial. Empty means None. None does not open a playback stream.

The input list contains PipeWire sources and sink monitors. A sink monitor is how a track records desktop audio from a speaker or headset. The output list contains None and playback sinks. The combo shows the node description and stores the node name, or the object serial when the name is empty.

Default ids, used only for the unsaved default list, are `desktop` and `microphone`.

## Saved list

The UI `RecorderSettings` store is the saved track list. `config::Settings` keeps its existing desktop and microphone fields. This change does not bump that schema, because the recorder does not read it at launch.

Until the user edits the list, nothing is saved and the effective list is:

1. Desktop audio, id `desktop`, kind `sink-monitor`, empty input, monitor None.
2. Microphone, id `microphone`, kind `source`, empty input, monitor None.

The mixer shows that effective list. The first add, remove, rename, or device change saves the list, including that edit. A saved empty list, from removing every track, is distinct from an unsaved list. `--audio on` with a saved empty list starts the session with no audio streams. `--audio on` with no saved list sends the two defaults above.

Add is disabled at eight tracks. A ninth track is rejected in the mixer, in settings load, and in argument parsing. Duplicate ids and a blank name are rejected. The same device may be selected on more than one track.

## When controls are available

Add, remove, and both device combos are enabled while the recorder is idle, whether or not audio capture is enabled. They are disabled while a session is starting, active, or stopping.

Gain and mute follow the current rule: they activate only while that track is reported as streaming during an active recording or replay-only session. The row label updates after acknowledgement. A five-second acknowledgement timeout restores the last reported value.

The output combo’s description states that the selected speakers play this track’s captured input for the life of the session.

## Launch

`--audio off` sends no `--audio-track` arguments.

`--audio on` sends one `--audio-track` per effective track, in list order. Each is followed by exactly five arguments:

`--audio-track <id> <name> <input-kind> <input-target> <monitor-target>`

An empty target is the single argument `-`. `input-kind` is `source` or `sink-monitor`. More than eight tracks, a bad id, a blank name, an unknown kind, or a missing argument fails parsing. Track order is the Matroska audio-stream order and the replay audio-stream order.

## Recorder path

The fixed desktop/microphone source pair becomes a track index, `0 .. n-1`.

For each track the recorder opens one PipeWire capture stream and one AAC encoder. A `sink-monitor` track sets sink-monitor capture. A target ending in `.monitor` is resolved to the sink name before it is used as `target.object`. A `source` track does not request sink-monitor capture. Sample rate, channel count, and codec stay the current shared AAC settings.

A non-empty monitor target also opens one playback stream. It receives the captured float PCM after that track’s gain and mute and before AAC encoding. Playback is not muxed and is not stored in replay history. The playback stream exists only while the session is running.

Startup waits until every track has published codec extradata, then the mux header and replay path use that ordered stream list. `MuxConfig::audio_streams` length is the track count, including one and eight. A packet whose `stream_index` is outside that list is invalid.

Volume and mute commands name a track id:

- `set_audio_volume <id> <gain>`
- `set_audio_mute <id> on|off`

Gain remains linear from 0.00× to 4.00×. An unknown id is rejected.

## Telemetry

The `audio` object keeps `enabled` and the existing session counters. It no longer contains `desktop` or `microphone` objects.

When audio is enabled it contains `tracks`, an array in track order. Each element has `id`, `name`, `monitor_error`, and the current per-source fields: `state`, `peak`, `rms`, `samples`, `gain`, `muted`, buffer counts, and PipeWire clock fields. It also includes `error`, the capture error string, empty when there is none.

`monitor_error` is empty when the monitor target is None or playback is connected. The mixer shows a non-empty `monitor_error` on that row. Absent or malformed meter values stay “Not reported.” The panel does not simulate levels. Freshness stays the existing three-second threshold. Engine logs and tests that read the old `desktop` and `microphone` objects read `tracks` instead. A startup failure includes the track name.

## Device list

The UI opens a short-lived PipeWire connection when the mixer is shown and when the user refreshes the lists. Enumeration is not part of the recorder’s capture path.

If enumeration fails, the combos keep the saved selection and the mixer says the device list is unavailable. Tracks are not cleared and audio is not turned off. A selected node that is no longer reported stays selected and is marked unavailable. Refresh does not substitute a different device.

## Errors

Every configured capture stream must connect before recording or replay-only capture begins. If one input is missing, busy, or rejected, startup fails and the error includes that track’s name. The session does not start.

A track that disconnects during the session reports `state` `error`, stops contributing packets, and leaves the session running. The muxed stream for that track ends when packets stop.

A missing or rejected monitor sink does not fail the session. That track still records. `monitor_error` is set, and no playback stream is left running for that track.

## Mixer

Each row is one track: name, input combo, output combo, the existing state, meters, gain, mute, and buffer line. Add appends a track. Remove deletes that row. New tracks use the next free id `track-1`, `track-2`, and so on, skipping ids already present. A new track’s kind is `source`, its input target is empty, its monitor target is None, and its name is `Track N` using that same number.

The Settings checkbox remains the opt-in for capture. Its label is “Capture audio tracks.” Its description says the mixer chooses each track’s input and monitor output, and that the choice applies to the next session. The mixer still explains that capture is off when the checkbox is off. Rows remain visible so the list can be edited while idle. New tracks start at gain 1.00× and unmuted. Gain and mute are not saved.

`UX-CONTRACT.md` is updated to match this behavior.

## Tests

- Settings round-trip the track list, including an empty monitor target. Nine tracks, a duplicate id, and a blank name are rejected.
- `--audio-track` preserves order, treats `-` as empty, and accepts both kinds. `--audio off` launches with no tracks. More than eight tracks fails parsing.
- Volume and mute accept a configured track id and reject an unknown id.
- The mux writer writes one, two, and eight audio streams and rejects an out-of-range stream index. Stream order matches the track list.
- Stream setup marks a source track as ordinary capture and a sink-monitor track as sink-monitor capture. A `.monitor` suffix is stripped. A monitor target requests a playback stream. None does not. These checks do not require a live PipeWire server.
- An offscreen mixer check adds a row with both combos, leaves the ninth add disabled, and disables add, remove, and both combos while a session is active.
- With audio on and no saved tracks, launch sends Desktop audio as a sink monitor, then Microphone as a source, both with monitor target `-`. A saved empty list sends no `--audio-track` arguments.

A live 360 FPS recording is out of scope.
