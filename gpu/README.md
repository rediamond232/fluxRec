# GPU encoder diagnostics and backends

For bitrate-mode diagnostics, set `FLUXREC_GPU_RC_MODE` to `auto`, `cbr`, or
`vbr`. If unset, Fluxrec leaves the selected encoder's rate-control option at
its default automatic behavior. Vulkan Video receives supported explicit mode
overrides; NVENC's `auto` leaves its mode at the FFmpeg default, while `cbr`
and `vbr` select those modes. Unsupported values or encoder options return a
startup error.

The startup log records the requested mode and configured target bitrate in
bits per second. In `auto` mode, that is the requested automatic policy, not a
report of which hardware rate-control mode the driver ultimately selected or
whether it met the target. `--qp` CQP sessions bypass this environment
override and retain their existing behavior.

## CUDA/NVENC backend

The recorder CLI defaults to NVENC; pass `--backend vulkan` to select Vulkan
Video. When `--backend` is omitted, `FLUXREC_GPU_ENCODER_BACKEND=vulkan` can
select Vulkan for diagnostics. An explicit CLI backend takes precedence. NVENC
keeps the two Vulkan conversion passes, then uses FFmpeg's Vulkan-to-CUDA
hardware transfer to copy both NV12 planes into a separately allocated,
pitch-linear CUDA pool before submitting to `h264_nvenc` or `hevc_nvenc`. This
is a GPU-to-GPU plane copy: the application keeps raw pixels out of CPU memory,
but this is not allocation-level zero-copy. An initial three-second H.264 desktop smoke
on the RTX 3090 Ti muxed 725 packets and filled the 48-slot capture pool with
NVENC's delay set to zero. Using the configured async depth as NVENC's bounded
delay produced 1,044 decodable packets in three seconds, with zero internal
drops or pool starvation and a 20/48 pool high-water mark. The adjacent Vulkan
Video desktop run produced 1,080 packets. Subsequent fullscreen source-ID
checks exposed periodic black output: 54/1,801 H.264 frames and 200/1,800
HEVC frames. A depth-sixteen H.264 run reduced the failure to one black frame
in an eight-second, 2,880-frame numbered-source recording; it still missed one
source ID. With the synchronized Vulkan filter and depth four, an eight-second
H.264 run decoded 2,880 consecutive IDs with no black output. A later fixed-
workload 60-second production-binary run decoded 21,598 consecutive H.264 IDs
and a matched HEVC run decoded 21,596, both without black output, source gaps,
internal drops, or pool starvation. An earlier run whose stimulus received
keyboard pattern changes missed 58 source IDs and filled the pool. Locked high-motion tests then recorded 21,599 consecutive HEVC IDs over 60 seconds and 32,392 consecutive H.264 IDs over 90 seconds with explicit CAVLC at a 50 Mbps VBR request. The latter H.264 run had zero starvation and pool high-water 15/48; its measured file rate was about 83 Mbps. A matched 20-second CABAC run missed 41 source IDs and filled the pool, so the recorder now passes its default VLC choice to NVENC as CAVLC. The rebuilt default also passed a 20-second high-motion source-ID recording with audio and replay export. The CLI now defaults to this backend. The high-motion evidence covers one H.264 configuration on the RTX 3090 Ti; HEVC passed the listed 60-second checks but remains less broadly validated. Game workloads, other presets and devices, and indefinite high-motion headroom remain unproven. See `docs/measurements.md` for those rates and conditions.

This route requires FFmpeg `libavutil` built with CUDA hardware-frame support,
Vulkan external memory/semaphore support, and the selected NVENC codec in
`libavcodec`. Fluxrec creates the Vulkan device with `disable_multiplane=1`
because CUDA interop needs one Vulkan image per NV12 plane. Startup fails with
an error if those requirements are unavailable; it does not fall back to CPU
pixel copies. The path is unavailable with `--conversion direct`.

With no `--backend` argument or environment override, the recorder selects
NVENC. `--backend nvenc` selects it explicitly, while `--backend vulkan` selects
Vulkan Video. The environment switch remains available for diagnostics when
the CLI argument is omitted. If async depth is not specified, the
recorder selects the validated depth of four and enables synchronized Vulkan
scale before filter setup. `--nvenc-preset p1..p7` selects the preset;
`FLUXREC_NVENC_PRESET=p1|p2|p3|p4|p5|p6|p7` remains available for diagnostics.
P1 is the default. `--encoder-tune` maps to
NVENC's `hq`, `ll`, or `ull` tuning names on this route; the default maps to
`ll`. `FLUXREC_GPU_RC_MODE=auto|cbr|vbr` leaves NVENC's policy at its default
for `auto`, or explicitly selects CBR/VBR. Existing `--qp` selects NVENC
constant-QP mode. The startup log records the accepted preset, tune, surface
count, async delay, and rate-control request. H.264 defaults to NVENC's `cavlc`
coder option; `--h264-coder cabac` selects `cabac` explicitly.

For source-versus-first-scale black-frame localization, the opt-in
`FLUXREC_GPU_BLACKDETECT_RGB_SOURCE=1` inserts `rgb_blackdetect_before_scale`
on the imported RGB image. Pair it with
`FLUXREC_GPU_BLACKDETECT_NV12=first` to compare the same frame timestamps after
the first NV12 pass. This requires the isolated RGB-capable FFmpeg patch and
runtime described in [patches/README.md](../patches/README.md); the normal
private FFmpeg build and default recorder graph are unchanged. The probe
reads back aggregate black-pixel counters, not source pixels, and waits for
those counters each frame, so it can perturb synchronization timing.

For an opt-in audit of references retained by the input Vulkan execution pool,
set `FLUXREC_GPU_INPUT_REF_AUDIT=1` and
`FLUXREC_GPU_INPUT_CTX_DRAIN=1`. The drain flag requires the private
`av_vk_frame_ctx_exec_pool_drain` symbol and fails startup if the loaded
`libavutil` does not export it. The call runs after filter EOF, codec output
drain, and capture-lease release; it polls the input frames context's compute
pool with a two-second total budget and discards dependencies only after their
submissions complete. Refcount logging is metadata-only. If completion remains
unproven, the encoder destructor retries before tearing down the codec or
filter graph, then exits the process without C++ resource unwinding if the
retry also fails. In the patched FFmpeg, ordinary Vulkan pools retain their
existing `UINT64_MAX` teardown wait; only a pool enrolled by this opt-in API
uses the five-second final-free check, which fail-stops if work is still
pending. A pending submission with no fence also fail-stops. The experimental
patch is built only into an isolated runtime described in
[`patches/README.md`](../patches/README.md); the default private FFmpeg runtime
is unchanged.

The authorized 2026-09-28 three-second numbered-source check against the
scoped patch completed 1,078 of 1,078 captured/submitted/encoded/muxed frames,
with consecutive nonblack IDs 254–1,331, zero drops or pool starvation, and
pool high-water 14/48. The input frames-context reference count fell from 11
before the drain to 3 after the pool dependencies were released, then to 1
after filter-graph teardown. This supports the execution-pool retention
diagnosis for this build and run; it does not identify each individual
retained reference. The recording and diagnostics are in
`.validation/input-ctx-drain-scoped-3s-20260928.mkv*`.

To isolate producer-fence waiting without a separate Vulkan image, shader, or
counter readback, set `FLUXREC_GPU_DMABUF_SYNC_WAIT=1`. Before each FFmpeg
DMA-BUF import, Fluxrec exports a `DMA_BUF_SYNC_READ` sync-file and waits up to
10 seconds for the published writer dependencies. Export or wait failure stops
the recording before import. This still serializes capture and can change the
black-frame cadence, but it does not submit probe GPU work or inspect pixels.
In a locked depth-eight test on 2026-09-28, this wait alone still produced 320
black frames among 2,881 H.264 frames, with zero drops or pool starvation. The
producer-fence wait alone therefore did not suppress the observed cadence; the
separate raw Vulkan probe's clean run remains timing-perturbed and inconclusive.
A later raw-probe-only matched run sampled 2,881 frames with zero near-black
counts and decoded 2,881 clean IDs, while the sync-wait-only run retained 320
black frames. The added Vulkan dispatch/fence path or its timing changes the
outcome; the logs do not measure that path's per-frame elapsed time. Its
recorder counters showed 2,881 captured, submitted, encoded, and muxed frames,
zero drops/starvation, and pool high-water 16/48. These paired diagnostics do
not establish the uninstrumented DMA-BUF's contents.

For a host-timing-only check, `FLUXREC_GPU_PREIMPORT_DELAY_US=N` sleeps the
recorder thread after the optional producer-fence wait and before the FFmpeg
DMA-BUF import. `N` must be an integer from 0 to 2000 microseconds; unset or
zero leaves the production path unchanged. Nonzero values serialize per-frame
submission and can change the failure cadence; scheduler delays can make actual
wall time exceed the requested sleep. Existing raw-probe logs do not contain a
per-frame probe duration, so a chosen delay is an experimental bracket rather
than a measured match.
The 2026-09-28 matched upper-bound run combined the producer-fence wait with a
2,000 µs delay. It still had 207 black frames among 1,869 recorded, while
steady throughput averaged about 229 FPS; pool starvation lasted 4,223.82 ms
and filled the 48-frame pool. This upper-bound delay did not suppress the
cadence and degraded throughput; the run does not show whether shorter delays
could affect it.

For an independent observation of the capture DMA-BUF before FFmpeg imports it,
set `FLUXREC_GPU_DMABUF_BLACK_PROBE=1`. The probe imports each leased GBM slot
into its own modifier-backed Vulkan image, waits for a fresh `DMA_BUF_SYNC_READ`
sync-file to signal, acquires external queue ownership for a compute shader,
counts source pixels with Rec. 709 luma at or below 0.10, then releases
ownership and waits for its Vulkan fence before continuing to FFmpeg. Each
16×16 workgroup reduces locally and adds to one 32-bit aggregate counter. CPU
code reads only that counter; it never reads source pixels. JSON sample rows
contain the black-pixel count, near-black classification (at least 98% of pixels),
capture sequence, presentation timestamp, and pool slot. This is a serializing,
timing-changing diagnostic and is disabled by default. Matching black counts in
the raw and FFmpeg paths show both imports observed near-black content, but do
not distinguish compositor output from stale or failed compositor writes; a
clean raw count with a black FFmpeg tap localizes the difference to their import,
view, or synchronization paths. Both imports still share the same GPU and
Vulkan driver. If the completion-fence wait fails for any reason, including its
ten-second timeout, the opt-in probe logs the capture sequence and `VkResult`
then exits immediately without C++ unwinding. The DMA-BUF lease and Vulkan
resources may still be in use, so normal teardown would be unsafe; process
exit closes the capture connection and leaves resource cleanup to
process/driver teardown.

For controlled NVENC bitrate experiments, `FLUXREC_NVENC_MAXRATE_MBPS=N`
sets FFmpeg's maximum-rate field (`rc_max_rate`) to an integer Mbps value from
1 through 500. The cap must be at least the configured average bitrate and is
rejected for constant-QP sessions. When unset, the encoder's existing rate
control defaults are unchanged. The cap is NVENC-only; it is ignored by the
Vulkan Video backend. A driver-boundary readback using NVENC API 13.1 confirmed
that a 60 Mbps setting reached `NV_ENC_RC_PARAMS::maxBitRate`; it also read
50 Mbps average and a 100 Mbps VBV buffer for that VBR run. Matched four-second
readbacks showed default VBR as 50 Mbps average / no explicit maximum / 100 Mbps
VBV, and CBR as the same average and buffer with no explicit maximum. All three
four-second clips had clean consecutive source IDs, used CUDA device-pointer
inputs, and made no software input-buffer calls. A separate locked ten-second
high-motion H.264 CAVLC recording with the 60 Mbps maximum still produced about
82 Mbps, similar to the uncapped VBR run. The readback verifies the requested
values reached the driver; it does not explain the measured output rate or
guarantee a particular average or file size.

`tools/controlled_benchmark.py` records the requested H.264 coder separately
from the coder observed in the saved stream's PPS. It uses FFmpeg's
`trace_headers` on the first video packet; a missing FFmpeg probe or unreadable
PPS is reported as `unknown`. Replay-only runs inspect the saved replay export.

`--b-frames 0..4` is NVENC-only and defaults to zero. Nonzero values turn off the explicit low-delay and zero-reorder settings, preserve the encoder's distinct packet PTS/DTS, and require a GOP at least two frames longer than the requested B-frame count. H.264 with two B-frames passed an eight-second numbered-source recording and A/V replay check at depth four. HEVC with two B-frames decoded correctly but saturated the pool and fell to about 307 encoded FPS in a twelve-second test, so B-frame throughput must be validated separately from the default zero-B-frame mode.

Example:

```sh
./build-release/engine/fluxrec-record --output /tmp/fluxrec-nvenc.mkv \
  --display DP-1 --seconds 10 --codec h264 --backend nvenc \
  --nvenc-preset p1 --bitrate-bps 50000000 \
  --pool 48 --conversion two-pass
```
