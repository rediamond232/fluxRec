# CPU-only A/V clock fixture

`fluxrec-audio-av-sync-fixture` is a manual live test for PipeWire capture
timestamps and mux timing. It does not use the physical desktop, microphone,
screen, or GPU. It writes a software H.264 test track that flashes at scheduled
`CLOCK_MONOTONIC` times and plays different audio tones to two temporary
PipeWire null sinks at the same scheduled times. `AudioSession` then captures
each sink's monitor source and encodes the two audio tracks.

The runner creates two temporary PipeWire Pulse null sinks and their monitor
sources. Test playback is routed only to those sinks; it does not change the
default sink/source or send the pulses to the user's speakers. The runner
checks that both newly created capture streams link to those exact monitor
source IDs, and fails if either stream falls back to another source. It unloads
both null-sink modules on exit. For a `.monitor` target, `AudioSession` targets
the native PipeWire sink node and marks the stream as sink capture; passing the
PulseAudio monitor alias as native `target.object` can otherwise leave an
unresolved target and autoconnect to a physical input.

Build and run from the repository root:

```sh
cmake --build build-root --target fluxrec-audio-av-sync-fixture -j2
audio/tests/run_av_sync_fixture.sh build-root /tmp/fluxrec-av-sync-60s.mkv 60 5000
```

The 60-second run places a 250 ms marker every 5 seconds. It emits an MKV and
a JSON sidecar listing the clock domain, video origin, pulse period, width,
frequencies, and intended marker timestamps. The analyzer verifies one H.264
track and two AAC tracks, checks strictly increasing DTS per track, decodes the
video flash transitions and audio pulse onsets, then matches those independent
observations by marker order.

For each source it reports the initial audio-minus-video offset, the linear
offset slope in parts per million, and the maximum residual after fitting that
slope. The intercept includes fixed PipeWire/codec path latency. The slope
shows relative drift after that fixed offset is removed. The decoded audio
onsets and video transitions are separate observations of the encoded tracks,
but their schedules use the same host monotonic clock. As a result, this is a
software A/V timestamp and mux calibration, not a measurement of drift between
independent hardware clocks. The measured offset also includes software
capture, AAC, and video-frame quantization delay. A 60-second run is a quick
sanity check; a longer run can reveal software scheduling or timestamp drift,
but independent-device drift requires an externally timed acoustic input or
other hardware reference.

This fixture checks PipeWire monitor capture timestamps, audio packet
encoding, shared-origin mux rebasing, and alignment against a synthetic video
timeline. It does not measure the Wayland compositor's presentation timestamp
accuracy, physical speaker/microphone latency, or relative drift between
hardware audio and video clocks. `AudioSession.sync_errors` reports invalid
shared video origins, unexpected stream identities, or unrepresentable encoded
audio timestamps. It does not compare PTS to `capture_time_ns`: that field is
derived from the encoded PTS, so such a comparison would be tautological.

Per-source recorder telemetry counts buffers stamped from PipeWire timing
metadata and buffers that used the `CLOCK_MONOTONIC` fallback. When PipeWire
provides usable graph ticks and a stable tick rate, telemetry also reports the
rate difference between `ticks × rate` and independent `CLOCK_MONOTONIC`
samples taken in the PipeWire callback. The estimate is a graph-clock versus
host-clock observation over the current stable-rate window; it can include
callback scheduling variation and PipeWire clock conversion. It does not
establish drift of the raw audio-device oscillator. The estimate stays null
until the window reaches ten seconds and resets on rate changes, tick
regressions, or fallback timing.
