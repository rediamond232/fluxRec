# Controlled stimulus and recording benchmark

`controlled_benchmark.py` serializes a stimulus/recorder run, waits for the
stimulus's first actual presentation feedback, starts `fluxrec-record`, and
then decodes the GPU-drawn frame IDs from the finished MKV. Offline decoding
is a separate CPU analysis step; it is not part of normal recording.

Build the three executables first. From the repository root:

```sh
cmake --build build-root --target fluxrec-record fluxrec-decode-stimulus-ids
cmake --build stimulus/build --target fluxrec-stimulus
```

Example controlled run with a longer stimulus and a ten-second two-pass H.264
recording:

```sh
python3 tools/controlled_benchmark.py \
  --display DP-1 \
  --stimulus-seconds 14 \
  --record-seconds 10 \
  --codec h264 \
  --encoder-tune default \
  --conversion two-pass \
  --pool 32 \
  --bitrate-mbps 50 \
  --output /tmp/fluxrec-controlled-pool32.mkv
```

Bitrate mode defaults to 50 Mbps when `--bitrate-mbps` is omitted. To measure
experimental constant-QP rate control, pass `--qp 0..51`; the harness sends
only `--qp N` to the recorder and does not send a bitrate argument. For example:

```sh
python3 tools/controlled_benchmark.py \
  --display DP-1 \
  --stimulus-seconds 14 \
  --record-seconds 10 \
  --codec h264 \
  --conversion two-pass \
  --pool 48 \
  --qp 28 \
  --output /tmp/fluxrec-controlled-cqp28.mkv
```

QP 0–51 applies to the current 8-bit NV12 path. `--qp` and an explicitly
provided `--bitrate-mbps` are mutually exclusive; the harness rejects runs
that specify both. Each summary records the rate-control mode and its active
value under both `conditions.rate_control` and `recording`.

The recorder defaults to NVENC and selects its validated queue depth when
`--async-depth` is omitted. To make the backend choice explicit:

```sh
python3 tools/controlled_benchmark.py \
  --display DP-1 \
  --stimulus-seconds 14 \
  --record-seconds 10 \
  --codec h264 \
  --backend nvenc \
  --nvenc-preset p1 \
  --conversion two-pass \
  --pool 48 \
  --bitrate-mbps 50 \
  --output /tmp/fluxrec-controlled-nvenc.mkv
```

`--backend` accepts `vulkan` or `nvenc`; omission defaults to NVENC unless
`FLUXREC_GPU_ENCODER_BACKEND` explicitly selects a backend. An explicit CLI
choice takes precedence over that environment setting. `--nvenc-preset p1..p7`
passes a preset through to NVENC and is rejected with an explicitly selected
Vulkan backend. When depth is omitted, the recorder selects depth four for
NVENC and depth eight for Vulkan. An explicit `--async-depth 8` (or another
value from 1 to 64) is still forwarded unchanged for matched comparisons and
diagnostics. The summary records the requested value as `async_depth_requested`
and the selected value as `async_depth`; if a run ends before startup identifies
an automatic depth, the latter is `null`.

In `conditions` and `recording`, the existing `backend` field continues to
mean the CLI request (`vulkan` or `nvenc`), or `inherited` when `--backend` was
omitted. `backend_observed` is parsed from the recorder's successful `fluxrec
GPU path` startup line and reports the encoder actually opened (`vulkan`,
`nvenc`, `mixed`, or `null` when startup did not identify it). An explicit CLI
request that contradicts the opened backend invalidates the benchmark.

## Mid-recording replay export

To export one replay while the primary recording is still running, configure a
replay ring and a save time measured from recorder process start:

```sh
python3 tools/controlled_benchmark.py \
  --display DP-1 \
  --stimulus-seconds 34 \
  --record-seconds 30 \
  --replay-seconds 8 \
  --save-replay-after 16 \
  --replay-output /tmp/fluxrec-controlled-midrun-replay.mkv \
  --codec h264 \
  --backend nvenc \
  --nvenc-preset p1 \
  --conversion two-pass \
  --pool 48 \
  --output /tmp/fluxrec-controlled-midrun.mkv
```

`--save-replay-after` sends one `save_replay /absolute/path.mkv` command through
the recorder's stdin. It requires `--replay-seconds` greater than zero and a
save time strictly between zero and the requested recording duration. If
`--replay-output` is omitted, the harness writes `<primary-stem>-replay.mkv`
beside the primary MKV. Both paths and their sidecar logs must be new files.
With no timed save requested, the harness keeps its existing stdin and artifact
behavior.

After recording, the harness checks that the recorder queued and completed
exactly one save to the requested path. It then reads the replay with `ffprobe`
to verify packet counts and strictly increasing DTS per stream, decodes all
streams with `ffmpeg`, and runs the stimulus-ID decoder. Readback status and
logs appear under `replay_export` and `artifacts` in the summary. The replay's
`decoded_identity.first_id`, `last_id`, and `id_span` describe only IDs inside
the saved replay. They remain separate from the primary recording's
`decoded_identity` and `comparison` fields. Replay-local missing-ID counts,
black frames, invalid frames, and source-range checks are reported there too.
This opt-in check requires `ffprobe` and `ffmpeg` on `PATH`; failed packet,
decode, acknowledgement, or identity checks produce a nonzero harness exit.

To capture only into the replay ring and export a mid-run clip, add
`--replay-only`. This mode still requires `--replay-seconds` and
`--save-replay-after`; it does not create a primary MKV. The optional
`--output` argument names the run's sidecars in this mode, and its path appears
as `recording.run_prefix`. The primary `recording.output_path`,
`decoded_identity`, and `comparison` fields are `null`; decoded source IDs for
the saved clip remain under `replay_export.decoded_identity`.

```sh
python3 tools/controlled_benchmark.py \
  --display DP-1 \
  --stimulus-seconds 24 \
  --record-seconds 20 \
  --replay-only \
  --replay-seconds 8 \
  --save-replay-after 10 \
  --codec h264 \
  --backend nvenc \
  --nvenc-preset p1 \
  --audio off \
  --output /tmp/fluxrec-replay-only-run.mkv \
  --replay-output /tmp/fluxrec-replay-only-saved.mkv
```

The stimulus must outlast the recording by at least one second. The harness
holds an exclusive lock and refuses to start if another `fluxrec-stimulus` or
`fluxrec-record` process is already running. It starts the recorder only
after it reads `stimulus-ready: first-presented` from the stimulus process.
It then waits 200 ms by default for the compositor's full-screen transition to
settle; adjust this with `--settle-seconds`. `--skip-initial-frames N` can
exclude known transition frames from offline identity counts while still
decoding the complete file. ID glyphs are decoded independently per frame with
a default luma threshold of 100. Override it with `--decode-threshold` when
calibrating another output path or display setup; compression can shift the
sampled segment luma near the decision boundary. The selected threshold is
recorded in the summary JSON.
If presentation feedback never arrives, it times out and stops the stimulus.
On interruption or error, it terminates both process groups.

During setup and recording, the harness reads the stimulus's per-second
presentation statistics. A zero presentation rate, a non-advancing presented
total, or a missing statistics heartbeat for 2.5 seconds aborts the run. It
also polls Hyprland's read-only `clients` and `monitors` JSON every 250 ms
during recording. The stimulus must remain mapped, fullscreen, on the requested
output, and on that output's active workspace. The harness never switches
workspaces. If an environment check fails, it stops both processes, exits with
an explicit `benchmark invalid environment` message, and writes a summary with
`environmental_validity.valid=false` and the reason. Successful summaries
record `environmental_validity.valid=true` plus the observed cadence and
workspace-check counts. The harness launches the stimulus with
`--lock-pattern-motion`; Space, 1–5, and +/- cannot change the workload, while
Esc/Q can still quit. It also checks every logged `pattern=` or `motion=` value
against the requested conditions and invalidates any mismatch.

The final result reports presented source IDs and cadence, capture/encode/mux
counts, per-second FPS ranges, encoder utilization, pool occupancy/starvation,
and decoded ID gaps, duplicates, backwards transitions, black frames, and
invalid frames. Sequence gaps count only between adjacent valid decoded IDs;
transitions spanning invalid or black frames are reported as unknown. The
reported missing-ID percentage uses the overall observed ID span as its
denominator, while its numerator only counts adjacent-valid gaps. Recorded
skips are evidence of sampling cadence or dropped frames; they do not by
themselves make the run fail. `identity_validity_clean` checks for black or
invalid frames, duplicates, backward transitions, and IDs outside the source's
presented range. The console result keeps image quality separate from cadence:
`frame-content` reports decode/content anomalies. `source-continuity` reports
`unknown` when black or invalid spans make transitions unobservable, and fails
when the remaining source or recorded IDs show unexplained gaps, duplicates, or
backward steps. For `--max-fps 60`, `120`, or `240`, clean ordered IDs that skip
source IDs by design are labeled `source-continuity=expected-subsampling`.
These labels do not change JSON validity fields or measured missing-ID counts.
`encoded` and `recording.muxed_video_frames` count video frames.
`muxed-total-packets` and `recording.muxed_packets_total` count packets across
all streams, so with `--audio on` that total includes the video packets and both
AAC tracks. The older `recording.muxed` field remains an alias for the total
packet count.

Use `--pattern colorchart` for a static GPU-rendered color chart while the
frame-ID overlay continues to change; analyze its recorded patch and ramp
values with `build-root/tools/fluxrec-decode-colorchart`.

Recording runs keep the primary MKV, stimulus stderr, recorder stdout/stderr,
decoder output, and a machine-readable `.summary.json` beside the output path.
Replay-only runs keep the replay MKV, stimulus stderr, recorder stdout/stderr,
replay readback logs, and summary beside the run prefix. Default outputs and
lock locations are under `/tmp` or `XDG_RUNTIME_DIR`.

Common recording options include `--codec`, `--backend`, `--nvenc-preset`,
`--b-frames`, `--encoder-tune`, `--h264-coder`, `--conversion`, `--pool`, optional
`--async-depth`, `--bitrate-mbps` or `--qp`, `--gop`, `--max-fps`, `--audio`,
`--verify-frame-ids`,
and repeated `--env NAME=VALUE` assignments for diagnostic environment
switches. `--encoder-tune` accepts `default`, `ll`, `ull`, or `hq`; `--max-fps`
accepts 0 (uncapped), 60, 120, 240, or 360. To compare 60/120/240 capture
caps, keep all other options unchanged and vary only `--max-fps`.
`--h264-coder vlc` requests CAVLC for H.264; `default` uses the recorder's
default CAVLC selection, and `cabac` requests CABAC. The coder option is rejected for
HEVC. The summary records the request and the coder observed in the saved
stream's H.264 PPS separately. The harness uses FFmpeg `trace_headers` on the
first video packet; if the probe cannot read the PPS, the observed coder is
`unknown`. Replay-only runs inspect the saved replay export.
`--b-frames 0..4` is omitted by default and otherwise forwarded to the
recorder. Positive values require NVENC and a GOP of at least `b-frames + 2`;
the requested value is recorded in `conditions.b_frames` and `recording.b_frames`.
For B-frame MKVs, packet readback permits a missing DTS on exactly the first
video packet when all later DTS values increase strictly. The packet is still
included in counts; `packet_probe.dts_validation` reports
`bframe-first-video-dts-missing-allowed`. Any other missing DTS or a nonmonotonic
sequence remains invalid.
`tools/run_cavlc_ab.sh` runs a matched 12-second CABAC-versus-CAVLC pair
with the same 18-second plasma stimulus and pool48/two-pass settings.
