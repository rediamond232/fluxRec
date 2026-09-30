# EGLImage to CUDA array to NVENC probe

This opt-in diagnostic is isolated from the production recorder. It negotiates
one-plane ARGB/XRGB Wayland capture DMA-BUFs, imports each used pool slot as an
EGLImage on the matching DRM render node, registers it with CUDA EGL interop,
and registers the resulting CUDA array with NVENC as `CUDAARRAY`. It reads no
pixels on the CPU. The only CPU copy is the compressed H.264 bitstream written
from NVENC's locked output buffer.

## Local hardware result and portability

On this host, the parent-run live probe on an RTX 3090 Ti with NVIDIA driver
615.71.09 passed exact-modifier `AR24` import (`0x0300000000606010`) through
EGLImage, CUDA EGL array inspection, NVENC CUDAARRAY register/map, and a
one-frame H.264 encode. The resulting 2560x1440 frame decoded and was visually
correct. This establishes that the route works on that local driver/GPU; it
does not establish support across desktop NVIDIA drivers.

NVIDIA's current CUDA Driver API reference documents
`cuGraphicsEGLRegisterImage`; NVIDIA's current CUDA for Tegra 13.4 application
note explicitly says that `cudaGraphicsEGLRegisterImage()` is supported only
on Tegra. The local desktop result is outside that documented support scope.
Treat EGL CUDA interop as an experimentally observed, driver-specific behavior
and repeat the probe for every driver/GPU combination. By contrast, the current
CUDA external-resource reference explicitly limits importing `DMABUF_FD` to
Tegra Thor and later, so direct DMA-BUF-to-CUDA external-memory import is not
documented for this desktop GPU.

NVENC's 13.1 guide describes CUDA-array input as block-linear input and requires
register/map before encode and unmap/unregister after use. This diagnostic
caches one EGLImage, CUDA registration, and NVENC registration for each pool
slot; it maps and unmaps the corresponding NVENC input for every encoded frame.
The source lease remains held through bitstream unlock and NVENC unmap. In
duration mode, one frame warms up the first encountered slot; other slots are
registered lazily if capture uses them. The capture module selects the first
free slot, so the probe does not assume that every configured slot will be
visited.

References:

- [CUDA Driver API: EGL Interoperability](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__EGL.html)
- [CUDA for Tegra 13.4 application note, EGLImage interop](https://docs.nvidia.com/cuda/cuda-for-tegra-appnote/)
- [CUDA Driver API: External Resource Interoperability](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__EXTRES__INTEROP.html)
- [NVENC Video Encoder API Guide 13.1, CUDA-array input](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html#nvenc-block-linear-cuda-array-input)
- [NVENC Video Encoder API Guide 13.1, synchronous mode and threading](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html#synchronous-mode)

## Build

The standalone tools project keeps this target separate from
`fluxrec-record`:

```sh
cmake -S tools -B .validation/nvenc-egl-cudaarray-probe-build
cmake --build .validation/nvenc-egl-cudaarray-probe-build \
  --target fluxrec-nvenc-egl-cudaarray-probe -j2
```

CPU compilation does not initialize EGL, CUDA, NVENC, or capture. The current
source builds successfully with this command.

## Bounded multi-frame run

Run from the target live Wayland session after coordinating the display window.
The command below uses DP-1, an 8-slot pool, a 5-second measured window, and a
1800-frame ceiling. The measurement window begins after first-slot import and
NVENC setup. A 20-second external timeout bounds startup and driver calls.
Choose an output path that does not already exist; both files are created with
`O_EXCL`:

```sh
timeout --signal=TERM --kill-after=2s 20s \
  .validation/nvenc-egl-cudaarray-probe-build/fluxrec-nvenc-egl-cudaarray-probe \
  --output DP-1 --capture-wait-ms 3000 --duration-ms 5000 \
  --max-frames 1800 --pool-slots 8 --pipeline-depth 4 \
  --output-file /tmp/fluxrec-egl-cudaarray-multiframe.h264
```

The probe writes the elementary H.264 stream and
`/tmp/fluxrec-egl-cudaarray-multiframe.h264.frames.csv`. The CSV maps encode
index to capture source sequence, compositor presentation timestamp (when
provided), pool slot, NVENC output timestamp, byte count, and picture type.
The log gives captured/dequeued and encoded counts, source-sequence gaps,
timestamp mismatches, capture completion/drop/starvation stats, queue/busy
high-water values, achieved FPS/bitrate, and count/total/average/max timings
for capture wait, DMA-BUF fence wait, per-slot EGL/CUDA/NVENC registration,
per-frame map/unmap, encode submission, bitstream lock, compressed-output
write, and unlock.

The initial bounded run uses depth 1: it maps one input, submits it, blocks on
that output's bitstream lock, then unmaps and releases the lease. On
2026-09-28, the parent-run 5-second DP-1 test on the RTX 3090 Ti encoded 1269
frames at 253.678 FPS. The offline decoder found 1269 valid numbered stimulus
IDs from 4073 through 5860, 519 source-ID gaps, and no black frames. The source
comparison separately counted 360 missed presentations. This validates the
color/content path for those outputs, while the gaps show that capture and
serial output handling did not preserve every source presentation. The average
blocking `nvEncLockBitstream` time was 3.622 ms. That run used the EGL
registration flag value `1`; the CUDA for Tegra interop reference says the
argument is reserved and must be zero. The isolated probe has now been changed
to use zero, so the earlier live success must be repeated before relying on it
with the documented argument.

`--pipeline-depth N` adds a bounded producer/consumer mode (N=1..16). Linux
remains in NVENC synchronous mode because the SDK guide documents
completion-event asynchronous mode only for Windows WDDM. The capture/submit
thread keeps submitting with distinct bitstream buffers; a dedicated worker
performs blocking bitstream locks and writes outputs. Each pending input keeps
its DMA-BUF lease and mapped NVENC input until the worker copies and unlocks
the output, unmaps the input, and releases the lease. In-flight inputs and
output buffers are bounded by N. The worker processes output in submission
order; the P-only configuration keeps frame ordering simple. `NEED_MORE_INPUT`
submissions stay pending until a later success or the EOS flush makes outputs
drainable. `ENCODER_BUSY` retries the same frame within a bounded limit. The
mode reports submit, success, delayed, lock/unlock, unmap, release, and EOS
counts so the queue can be checked after the live run. Its EOS flush follows
SDK 13.1 section 5.1: a zeroed `NV_ENC_PIC_PARAMS` with only version and
`NV_ENC_PIC_FLAG_EOS` set.

### Parent-run pipeline measurements (2026-09-28)

Two 5-second runs on the same fullscreen numbered source completed with all
submissions reconciled at EOS and no black or invalid decoded frames:

| Pipeline depth / pool | Encoded | FPS | Mean blocking lock | Pool starvation | Numbered-source IDs |
| --- | ---: | ---: | ---: | ---: | --- |
| 4 / 8 | 1371 | 273.564 | 3.589 ms | 2.3047 s | 5591–7382; 421 missing IDs |
| 8 / 16 | 1373 | 273.051 | 3.602 ms | 2.2495 s | 3232–5021; 417 missing IDs |

The corresponding streams are
`.validation/egl-cudaarray-pipeline-20260928-01.h264` and
`.validation/egl-cudaarray-pipeline-depth8-20260928.h264`; each has a
`.frames.csv` correlation sidecar. Both runs reported equal submission,
successful-lock, unlock, unmap, and lease-release counts at EOS. The depth-8
run reached an in-flight high-water mark of 8. It captured no callback-sequence
gaps, while offline stimulus-ID inspection found 417 missed presentations;
these are different counters. The depth-4 decode similarly contained 421 gaps
in the numbered stimulus IDs. All decoded frames were valid and nonblack.

Increasing depth from 4 to 8 did not improve throughput: the measured rates
differ by about 0.2%, within run-to-run variation. In the depth-8 log,
`nvEncEncodePicture` submission averaged 0.074 ms, per-frame map averaged
0.0045 ms, bitstream writing averaged 0.040 ms, and unlock/unmap averaged
about 0.002 ms combined; the blocking lock averaged 3.602 ms. The lock wait is
therefore the dominant per-frame service interval in this run. Since Linux
supports only NVENC synchronous output processing, the single lock worker
cannot retire output faster by adding more queued buffers once its queue is
full. Depth 8 already reached its configured in-flight limit, and increasing
to 16 is unlikely to raise sustained encode throughput; the eight extra
in-flight entries would buffer only about 22 ms at a 360 Hz source rate before
capture backpressure. The observed pool starvation and numbered-ID gaps show
that this path did not keep pace with every source presentation.

The probe already uses NVIDIA's P1 ultra-low-latency preset. Splitting the
small compressed-stream write away from the lock worker would have little
expected effect given its 0.040 ms mean, and multiple concurrent blocking
lock workers on one Linux encoder session are not established by the SDK's
threading guidance. The low-risk next experiment, if more evidence is needed,
is a matched depth-8/depth-16 run (`--pipeline-depth` and `--pool-slots` both
set to the selected depth) that keeps every encoder setting fixed and compares
lock time, FPS, and source-ID gaps; it should be treated as a burst buffering
check rather than an expected throughput optimization.

The earlier depth-1 5-second run described below predates the flags correction:
it passed `1` to `cuGraphicsEGLRegisterImage`. The source now passes `0` as
required by NVIDIA's Tegra EGL interop note. The two pipeline results above
were run against the corrected source; the older depth-1 metrics should not be
used as a flags-0 comparison.

## One-frame and registration-only modes

Registration-only mode captures one frame and stops after EGL import, CUDA
array inspection, NVENC CUDAARRAY registration, and one map/unmap cycle. It
does not require an output path:

```sh
probe_run_dir=".validation/egl-cudaarray-matroska-colorchart-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$probe_run_dir"
timeout --signal=TERM --kill-after=2s 25s \
  .validation/nvenc-egl-cudaarray-probe-build/fluxrec-nvenc-egl-cudaarray-probe \
  --output DP-1 --capture-wait-ms 3000 --register-only
```

Default mode captures and encodes one frame, writing an H.264 stream and CSV
sidecar. `--pool-slots` accepts 3 through 16 and defaults to 8.
`--pipeline-depth` accepts 1 through 16 and defaults to 1; values greater than
1 enable the bounded lock-worker mode and require duration mode. Duration mode
accepts 100 through 60000 milliseconds and at most 21600 frames. More than one
frame requires `--duration-ms`. To preserve the prior implicit frame ceiling,
omitting `--max-frames` in duration mode still defaults to 3600; a 60-second
run near 360 FPS must set `--max-frames 21600` explicitly.

### Bounded 60-second extension

The parent-run 10-second LL/CAVLC/VBR50 depth-eight run completed 3,590 frames
at 358.944 FPS with consecutive source IDs and zero black frames. Its stream
was 72,483,291 bytes; at the same observed output rate, a 60-second stream
would be about 435 MB. The run had one 139 ms startup starvation interval.
Output bytes are streamed to disk rather than held in memory, but the requested
bitrate is not a strict file-size limit. The pool,
per-slot imports, eight NVENC output buffers, and in-flight leases remain
bounded independently of duration. At EOS the probe stops and drains capture,
flushes NVENC, joins the output worker, and requires submissions, successful
locks, unmaps, lease releases, and encoded frames to reconcile.

For the 60-second extension, use a fresh output path and allow 120 seconds for
startup, encoding, EOS drain, and file flush. The external timeout is a final
bound for a stalled driver call; if it fires, it cannot run C++ cleanup, so
remove any partial stream and `.frames.csv` before retrying.

```sh
probe_run_dir=".validation/egl-cudaarray-ll-cavlc-vbr50-depth8-60s-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$probe_run_dir"
timeout --signal=TERM --kill-after=2s 120s \
  .validation/nvenc-egl-cudaarray-probe-build/fluxrec-nvenc-egl-cudaarray-probe \
  --output DP-1 --capture-wait-ms 3000 --duration-ms 60000 \
  --max-frames 21600 --pool-slots 16 --pipeline-depth 8 \
  --nvenc-tuning ll --bitrate-mbps 50 --h264-coder vlc \
  --output-file "$probe_run_dir/result.h264"
```

### Optional Matroska packet output

`--output-mkv PATH` is an opt-in H.264 container mode for bounded multi-frame
runs. It requires `--duration-ms` (100–60000) and `--pipeline-depth` greater
than 1, and cannot be combined with `--output-file`. If `--max-frames` is
omitted, its ceiling is derived from the requested duration at 360 frames per
second and capped at 21600. This mode does not write the raw H.264 stream or
the `.frames.csv` sidecar.

After NVENC initialization, the probe fetches the Annex-B SPS/PPS using
`nvEncGetSequenceParams` and supplies those bytes as the Matroska track's codec
extradata. The output worker copies only each completed compressed access unit
from the locked NVENC buffer into an `EncodedPacket`; MuxWriter's separate
worker performs Matroska and file I/O through a bounded queue (1024 packets,
128 MiB). The queue is nonblocking. Sustained queue saturation fails the
probe rather than waiting on disk from the capture/submit thread. The output
worker may still report queue errors or backpressure through the finite NVENC
pipeline.

Packet PTS and DTS use the Wayland source `presentation_time_ns` with a
`1/1000000000` time base. `DTS=PTS` is valid for this probe's P-only stream
(`frameIntervalP=1`, with no B-frame reordering). To set each packet's duration
from the source clock, the output worker retains one compressed packet until
the next presentation timestamp arrives; it assigns their timestamp delta to
the previous packet. The final packet uses the last measured delta, or the
nominal 1/360-second interval if only one packet was encoded. A missing or
non-increasing compositor timestamp, NVENC/source-sequence mismatch, or mux
queue failure ends the run. The Matroska time base chosen by FFmpeg may quantize
the nanosecond source clock in the container.

This mode has been CPU-built and its option validation and MuxWriter tests pass.
A bounded live GPU Matroska check is now documented below. MuxWriter opens
the selected path with truncate semantics rather than exclusive create, so
choose a fresh path.
On a reported error or external timeout, inspect and remove the partial MKV
before retrying.

Example for a future five-second color-chart check:

```sh
timeout --signal=TERM --kill-after=2s 25s \
  .validation/nvenc-egl-cudaarray-probe-build/fluxrec-nvenc-egl-cudaarray-probe \
  --output DP-1 --capture-wait-ms 3000 --duration-ms 5000 \
  --max-frames 1800 --pool-slots 16 --pipeline-depth 12 \
  --nvenc-tuning ll --bitrate-mbps 50 --h264-coder vlc \
  --color-metadata bt709-limited \
  --output-mkv "$probe_run_dir/result.mkv"
```

## Independent NVENC configuration controls

The direct probe keeps its original P1/ULL preset configuration when these
options are omitted. Each option changes only its named control:

- `--h264-coder auto|cabac|vlc` writes the corresponding H.264
  `entropyCodingMode`; `vlc` maps to the NVENC SDK's CAVLC value. This is the
  narrow comparison against production's CAVLC setting:

  ```sh
  timeout --signal=TERM --kill-after=2s 20s \
    .validation/nvenc-egl-cudaarray-probe-build/fluxrec-nvenc-egl-cudaarray-probe \
    --output DP-1 --capture-wait-ms 3000 --duration-ms 5000 \
    --max-frames 1800 --pool-slots 8 --pipeline-depth 4 \
    --h264-coder vlc \
    --output-file /tmp/fluxrec-egl-cudaarray-cavlc-5s.h264
  ```

  This changes the coder only. Tuning stays ULL, and the preset's rate control,
  GOP, and other fields remain as returned by the driver.
- `--nvenc-tuning ull|ll` selects the tuning value passed both to
  `NvEncGetEncodePresetConfigEx(P1, ...)` and
  `NV_ENC_INITIALIZE_PARAMS::tuningInfo`. Omission preserves ULL.
- `--bitrate-mbps N` accepts 1–500, selects VBR, and writes
  `NV_ENC_RC_PARAMS::averageBitRate = N * 1,000,000`. It leaves the preset's
  maximum bitrate and VBV buffer values intact. Omission leaves the complete
  preset rate-control block untouched.

The `stage=nvenc_config passed` line is printed immediately before
`nvEncInitializeEncoder` and reports the exact tuning value and selected
`NV_ENC_CONFIG` fields, including bitrate, coder, frame interval, and GOP. The
initialize call receives that same config through
`NV_ENC_INITIALIZE_PARAMS::encodeConfig`. This log proves what the probe passes
at the API boundary; it is not a driver readback. The defaults remain the
previous ULL preset with no explicit bitrate or coder overrides.

### Matched CABAC/CAVLC coder comparison (2026-09-28)

The parent-run A/B changed only the H.264 entropy coder: CABAC control used the
P1/ULL preset with no coder override; CAVLC arm set `entropyCodingMode=CAVLC`.
Both used DP-1 at 2560×1440, CUDAARRAY/ARGB input, pipeline depth 4, a 16-slot
pool, no explicit bitrate, and a five-second measurement window. The summary
records both trials exit 0, with all 22 fullscreen/workspace checks passing.
The locked plasma stimulus remained active on workspace 3 and reported no
presented-ID gaps; after startup its per-second reports were generally about
358–361 presented FPS.

| Coder | Encoded / FPS | Mean blocking lock | Starvation | Decoded source IDs |
| --- | ---: | ---: | ---: | --- |
| CABAC | 1,430 / 285.286 | 3.441 ms | 1,338 periods / 1.989 s | 219–1,996; 348 skipped IDs |
| CAVLC | 1,455 / 290.364 | 3.379 ms | 1,342 periods / 1.944 s | 2,145–3,929; 330 skipped IDs |

Both streams decoded to the logged frame count with zero black or invalid
frames. The capture source-sequence CSVs were contiguous (17–1,446 and
17–1,471 respectively), with zero timestamp mismatches. The larger skipped-ID
counts from offline decode are compositor presentations missed under capture
backpressure, not gaps in the capture callback sequence. An independent
`trace_headers` inspection of the H.264 PPS found
`entropy_coding_mode_flag=1` in the CABAC stream and `=0` in the CAVLC stream,
confirming the bitstreams used the requested coder. CAVLC improved this
sequential five-second sample by 5.078 FPS (about 1.8%) and reduced mean lock
wait by 0.063 ms, but its 290.364 FPS result still trails the roughly 360 FPS
source by about 70 FPS. CAVLC alone therefore does not close the throughput
gap on this direct CUDAARRAY path. The short sequential A/B does not establish
that the small measured difference is repeatable. Artifacts:
`.validation/egl-cudaarray-coder-ab-20260928T134405Z/` (`summary.json`, logs,
streams, and `.frames.csv` sidecars).

### Low-latency tuning and pipeline-depth follow-up (2026-09-28)

A matched five-second P1/CAVLC run changed tuning from ULL to LL, keeping depth
4 and a 16-slot pool. `trace_headers` confirmed CAVLC PPS flag 0 in both
streams. The ULL control encoded 1,442 frames at 287.759 FPS; its decoded IDs
were 218–1,996 with 337 missed presentations, zero black/invalid frames, mean
blocking lock 3.412 ms, and 1.895 s of pool starvation. The LL arm encoded
1,796 frames at 359.111 FPS, with exact consecutive IDs 2,150–3,945, zero
black/invalid frames, mean lock 2.725 ms, and one 97 ms starvation period.
Capture sequence and timestamps were contiguous/matched in both logs. The
summary records both trials exit 0 and all 22 fullscreen/workspace checks
passing; source presentation logs report no stimulus-ID gaps and approximately
358–360 FPS after startup. This is a short sequential comparison, not a
repeatability estimate.

Two subsequent ten-second P1/LL/CAVLC runs compared depth 4 and depth 8 with
the same 16-slot pool. Both completed with reconciled submissions, bitstream
locks, unlocks, unmaps, and lease releases; both had contiguous capture
sequences, zero timestamp mismatches, no black/invalid frames, and passed all
41 fullscreen/workspace checks. `trace_headers` confirmed CAVLC PPS flag 0 in
both streams.

| Pipeline depth / pool | Encoded / FPS | Decoded source IDs | Mean blocking lock | Starvation |
| --- | ---: | --- | ---: | ---: |
| 4 / 16 | 3,568 / 356.466 | 217–3,796; 12 missed | 2.749 ms | 977 periods / 1.071 s |
| 8 / 16 | 3,586 / 357.890 | 214–3,800; 1 missed | 2.741 ms | 198 periods / 0.243 s |

The stimulus presentation log reported no presented-ID gaps and about
359–360 FPS during these tests. Raising pipeline depth reduced measured
starvation and missed IDs in this pair, but rate remained slightly below the
source and the comparison consists of sequential ten-second runs. It shows
near-source-rate capture for this workload, not a sustained exact-360 guarantee.
Artifacts: `.validation/egl-cudaarray-tuning-ab-20260928T134501Z/`,
`.validation/egl-cudaarray-ll-cavlc-10s-20260928T134601Z/`, and
`.validation/egl-cudaarray-ll-cavlc-depth8-10s-20260928T134647Z/`.

An additional ten-second P1/LL/CAVLC depth-8 run requested VBR at 50 Mbps. It
encoded 3,590 frames at 358.944 FPS; offline decode verified 3,590 consecutive
numbered source IDs (223–3,812), with zero black, invalid, duplicate,
backward, or missing IDs. The PPS again has CAVLC flag 0. Mean blocking lock
was 2.705 ms, and the measured stream rate was 57.978 Mbps against the 50 Mbps
request. Capture source sequence and timestamps were contiguous/matched;
submission, lock, unlock, unmap, and lease-release counts reconciled. The
fullscreen check passed all 41 samples and the source stimulus log had no
presented-ID gaps. The run reported one 138.9 ms pool-starvation period; the
log does not establish when it occurred, so no startup attribution is made.
This remains a single bounded ten-second result. Artifact:
`.validation/egl-cudaarray-ll-cavlc-vbr50-depth8-10s-20260928T135151Z/`.

### High-motion VBR50 depth-8/depth-12 comparison (2026-09-28)

Two further ten-second high-motion plasma runs used P1/LL/CAVLC, VBR with a
50 Mbps average-rate request, and the same 16-slot pool. Both passed all 41
fullscreen/workspace checks. The stimulus logs reported no presentation-ID
gaps, capture callback sequences had no gaps, and submissions, locks, unlocks,
unmaps, and lease releases reconciled. PPS inspection confirmed CAVLC flag 0
for both.

| Pipeline depth / pool | Encoded / FPS | Decoded source IDs | Mean blocking lock | Starvation | Measured stream rate |
| --- | ---: | --- | ---: | ---: | ---: |
| 8 / 16 | 3,589 / 358.832 | 226–3,815; 1 missed | 2.728 ms | 51 periods / 0.186 s | 83.066 Mbps |
| 12 / 16 | 3,597 / 359.545 | 227–3,823; 0 missed | 2.730 ms | 1 period / 0.139 s | 83.225 Mbps |

Offline decode found zero black or invalid frames in either stream. The source
presentation logs stayed around 359–360 FPS after startup. Depth 12 produced a
clean, source-ID-contiguous ten-second interval very near the source cadence;
this is one bounded sample, not evidence of sustained throughput. Both short
output rates exceeded the configured 50 Mbps average-rate request; these runs
do not establish longer-term rate-control behavior. The depth-12 log gives no
timing for its single starvation period, so it cannot be attributed to
startup. Artifacts: `.validation/egl-cudaarray-ll-cavlc-vbr50-motion1-depth8-10s-20260928T135322Z/`
and `.validation/egl-cudaarray-ll-cavlc-vbr50-motion1-depth12-10s-20260928T135422Z/`.

These throughput results are local experiments on the RTX 3090 Ti, not proof
of a supported or portable desktop interop path. The probe registers capture
EGLImages through CUDA EGL interop; NVIDIA's
[CUDA for Tegra 13.4 note](https://docs.nvidia.com/cuda/cuda-for-tegra-appnote/)
states that `cudaGraphicsEGLRegisterImage()` is supported only on Tegra devices.
The successful desktop result is therefore driver-specific behavior outside
that published support scope and must not be treated as a generally supported
desktop implementation.

### Bounded high-motion interval and color-chart check (2026-09-28)

One requested 60,000 ms high-motion P1/LL/CAVLC VBR50 depth-12 run, with a
16-slot pool, recorded 21,557 frames over a 60.002 s measurement window
(60.762 s external wall time). The measured rate was 359.271 FPS and the
bitstream size/rate were 622,391,831 bytes / 82.983 Mbps against the 50 Mbps
average-rate request. Offline decode recovered 21,557 consecutive valid source
IDs, 237–21,793, with zero black, invalid, duplicate, backward, or missing
frames. Mean blocking lock was 2.719825 ms. Submissions, locks, unlocks,
unmaps, and lease releases each counted 21,557; capture drops were zero. The
log reports one 146.7 ms starvation period, without locating it in time. All
229 fullscreen/workspace checks passed, and the stimulus presentation log
reported no source-ID gaps. This documents this single bounded measurement;
it is not an exact-360 result or a general duration guarantee. Artifact:
`.validation/egl-cudaarray-ll-cavlc-vbr50-motion1-depth12-60s-20260928T140617Z/`.

A separate five-second color-chart run used P1/LL/CAVLC VBR50 at depth 12
with the same 16-slot pool. It recorded and decoded 1,800 frames at 360.054
FPS, with consecutive valid numbered IDs 235–2,034 and no black, invalid,
duplicate, backward, or missing frames. All 22 fullscreen/workspace checks
passed. The red bar decoded red (30-frame sample mean RGB 233.3, 0.0, 0.4);
the blue bar decoded blue (2.6, 0.0, 241.7), preserving their order. The
default 30-frame color analysis reported bar mean MAE 5.61; analyzing all
1,800 frames reported 5.58. The bitstream declared neither color matrix nor
range, so the analyzer used BT.601 and limited-range fallbacks. These are
fallback-based end-to-end measurements, not a color-profile certification or
a localization of any conversion error. The simple chart encoded at a
measured 4.409 Mbps under the 50 Mbps average-rate request. Artifact:
`.validation/egl-cudaarray-ll-cavlc-vbr50-colorchart-depth12-5s-20260928T140811Z/`.

The CUDA EGL interop support-scope caveat above applies to both results.

### VUI color-metadata chart A/B (2026-09-28)

A paired five-second color-chart run kept P1/LL/CAVLC VBR50, depth 12, and the
16-slot pool, changing the encoder's VUI color metadata from unset to
BT.709/limited. Both files completed with reconciled pipeline counts, zero
capture drops, and no timestamp mismatches; both passed all 23
fullscreen/workspace checks and encoded near source rate. Offline decode
found consecutive numbered IDs 213–2,010 in the unspecified control (1,798
frames at 359.573 FPS) and 2,170–3,968 in the BT.709 arm (1,799 frames at
359.777 FPS), with zero black, invalid, duplicate, backward, or missing
frames.

`ffprobe` reported unknown range/matrix/transfer/primaries for the control and
`tv`/BT.709/BT.709/BT.709 for the arm. The 30-frame color decoder used a
BT.601 matrix fallback for the unspecified control and BT.709 for the arm; it
used its limited-range path for both (reported as `limited_fallback`, with
`declared_range=limited` for the arm). On that decoder pass, bar mean MAE fell
from 5.64 to 0.65 and ramp mean MAE from 4.56 to 0.63. This measures how the
offline decoder interprets each stream using its signaled VUI. The reduction
does not prove that underlying GPU/NVENC pixel conversion changed or that the
stored pixel values match an independent colorimetric reference. Artifact:
`.validation/egl-cudaarray-vui-colorchart-ab-20260928T141304Z/`.

### Settled live Matroska color-chart check (2026-09-29)

The probe wrote a live Matroska file during a bounded 2,000 ms run using
P1/LL, CAVLC, VBR50, pipeline depth 12, a 16-slot pool, and BT.709 limited
VUI metadata. Before starting capture, the runner waited 1.5 seconds after
Hyprland reported the fullscreen stimulus state, allowing its fullscreen
transition animation to finish. All 11 subsequent fullscreen checks passed.
The 720 submitted frames were encoded, unlocked, unmapped, released, and
muxed: each of those counts was 720, with zero mux queue drops. The probe
reported 359.559 FPS over its 2.002-second capture window.

Offline decoding recovered 720 consecutive valid source IDs, 608–1,327,
with no black, invalid, duplicate, backward, or missing frames. The muxed
H.264 stream is 2560×1440; `ffprobe` reports BT.709 primaries/transfer/matrix,
limited (`tv`) range, and 45 bytes of codec extradata. Its single keyframe
was followed by P-only frames. PTS and DTS were equal and strictly increasing
for every packet, with positive packet durations. On the color-chart decode,
bar mean MAE was 0.70 and ramp mean MAE was 0.63.

An earlier run started while the fullscreen transition was still visible in
the first decoded frame. Its offline analysis found 30 black frames, 51
invalid frames, and two source-ID gaps despite successful probe and mux
counters. This was a stimulus-settling/capture-content issue, not evidence of
a Matroska mux failure; that run is not used for the successful decode result
above. The settled 2-second sample validates basic live packet muxing and
readback only. It does not establish long-duration behavior or production
recorder performance. Artifacts: `.validation/egl-cudaarray-mkv-colorchart-settled-20260929T082226Z/`
and the earlier unsettled attempt `.validation/egl-cudaarray-mkv-colorchart-20260929T082049Z/`.

This live desktop run still uses CUDA EGL image registration. NVIDIA's
[CUDA for Tegra 13.4 note](https://docs.nvidia.com/cuda/cuda-for-tegra-appnote/)
states that `cudaGraphicsEGLRegisterImage()` is supported only on Tegra
devices; this local result remains outside the published desktop API support
scope.

### Settled live Matroska high-motion check (2026-09-29)

A separate 10,000 ms P1/LL, CAVLC, VBR50, depth-12 run used the same 16-slot
pool and BT.709 limited VUI metadata with a fullscreen plasma motion1 source.
The runner waited 1.5 seconds for the fullscreen transition to settle. All
48 fullscreen checks passed, and the stimulus presentation log reported no
presented-ID gaps. The probe encoded and muxed 3,593 packets over 10.0052 s
(359.113 FPS); submissions, successful locks, unlocks, unmaps, lease releases,
and muxed packets each totaled 3,593. Its capture-sequence IDs were 17–3,609
with zero gaps. Offline decode found 3,593 consecutive valid numbered plasma
frames, IDs 597–4,189, with zero black or invalid frames.

The MKV contains 3,593 H.264 packets and is 103,981,916 bytes (103,952,064
bytes of encoded video); the stream measured 83.118 Mbps despite the 50 Mbps
VBR average-rate request. Packet PTS and DTS were strictly increasing and
equal, durations were positive, and the stream had one keyframe. `ffprobe`
reported BT.709 limited-range metadata. The probe logged one 107.54 ms pool
starvation period without locating it in the interval; despite this, source
presentation and decoded frame IDs were consecutive. This is one bounded
10-second result and does not establish long-run throughput or rate-control
behavior. An earlier attempt left a Hyprland special workspace open and
produced no recording; it is excluded from these measurements. Artifact:
`.validation/egl-cudaarray-mkv-plasma-motion1-10s-20260929T082834Z/`.

As with the color-chart MKV check above, this is local desktop CUDA EGL
behavior outside the published Tegra-only support scope of
`cuGraphicsEGLRegisterImage()`.

## Failure stages and cleanup

The probe reports its last completed stage and exits nonzero on failure. It
stops capture before destroying GPU registrations on error and keeps the active
lease until NVENC/CUDA/EGL resources are torn down. The raw H.264 plus CSV
mode removes files it created after a reported failure. Matroska mode uses
MuxWriter's truncating path open and may leave a partial MKV after a reported
error. A hard external timeout cannot run cleanup in either mode; inspect and
remove any partial output before retrying.

Stage names identify capture negotiation/start/frame/warmup, DMA-BUF duplicate
and fence export/wait, EGL device/modifier/image import, CUDA context/EGL-array
registration, NVENC initialization/resource registration/map/encode/lock/
unlock/unmap, raw stream/CSV output, and Matroska writer failures. Successful
raw runs keep both exclusive output files for offline decode, source-ID, and
color checks; Matroska runs keep the muxed file.
