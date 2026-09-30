# Source-patched FFmpeg Vulkan runtime

`ffmpeg-n9.0.2-vulkan-bgra-b8.patch` changes FFmpeg 9.0.2's Vulkan format
table for `AV_PIX_FMT_BGRA` from `VK_FORMAT_R8G8B8A8_UNORM` to
`VK_FORMAT_B8G8R8A8_UNORM`, including the fallback view format. The recorder
imports Hyprland's little-endian AR24 DMA-BUFs into this BGRA frames context;
the B8 view matches those bytes and keeps conversion in FFmpeg's existing
Vulkan filter and synchronization path.

The patch affects every Vulkan frame represented as `AV_PIX_FMT_BGRA` in the
private libavutil. Validate other BGRA Vulkan uses before distributing the
library outside Fluxrec.

`ffmpeg-n9.0.2-vulkan-filter-wait-env.patch` adds a conditional wait after a
successful simple Vulkan filter submission. The recorder enables
`FLUXREC_FF_VK_WAIT_SIMPLE=1` before filter setup for HEVC, NVENC, and the
H.264 ULL tune. Default Vulkan Video H.264 leaves the switch unset to preserve
its measured asynchronous throughput; an explicit environment setting remains
available for diagnostics. The patch
does not change FFmpeg's original `scale_vulkan` execution-pool size. The
wait prevents the periodic black outputs seen in an isolated HEVC test, while
the exact upstream synchronization fault remains unidentified.

`ffmpeg-n9.0.2-vulkan-blackdetect-rgb-source.patch` extends the Vulkan black
detector to packed BGRA/BGR0/RGBA/RGB0 input. It computes Rec. 709 luma on the
GPU and retains blackdetect's small aggregate-counter readback. The recorder
adds the `rgb_blackdetect_before_scale` tap only when
`FLUXREC_GPU_BLACKDETECT_RGB_SOURCE=1`; pair it with
`FLUXREC_GPU_BLACKDETECT_NV12=first` for a source/first-scale timestamp
comparison. The switch is off by default and does not change the normal graph.
This patch is not part of `scripts/build-ffmpeg-bgra-b8.sh`; keep its runtime
isolated from `.deps/ffmpeg-bgra-b8/lib`. It adds a per-frame GPU pass and a
wait for its aggregate counts, which can perturb a synchronization problem.

To reproduce the source and recorder compile check after the standard private
FFmpeg source tree is configured:

```sh
mkdir -p .validation/ffmpeg-rgb-source-probe
cp -a --reflink=auto .deps/src/ffmpeg-9.0.2-filterwait \
  .validation/ffmpeg-rgb-source-probe/source
patch --batch --forward --directory=.validation/ffmpeg-rgb-source-probe/source \
  --strip=1 --input="$PWD/patches/ffmpeg-n9.0.2-vulkan-blackdetect-rgb-source.patch"
make -C .validation/ffmpeg-rgb-source-probe/source -j4 \
  libavfilter/libavfilter.so libswscale/libswscale.so
```

Stage the copied filter library under
`.validation/ffmpeg-rgb-source-probe/lib/` with matching private
`libavutil.so.61` and `libswscale.so.10`, then configure a separate recorder
build with
`-DFLUXREC_PRIVATE_FFMPEG_LIBDIR=$PWD/.validation/ffmpeg-rgb-source-probe/lib`.
Run `scripts/check-ffmpeg-runtime.sh` against the resulting recorder and that
library directory before using the opt-in probe. The 2026-09-28 isolated build
passed shader validation, recorder compilation, and private-loader checks; no
live workload is implied by those checks.

`ffmpeg-n9.0.2-vulkan-timestamp-query.patch` adds per-execution-slot Vulkan
timestamp result readback after the slot's existing fence. The companion
`ffmpeg-n9.0.2-vulkan-filter-timing.patch` adds two timestamp writes around
each `scale_vulkan` compute dispatch. Set
`FLUXREC_FF_VK_DEVICE_TIMING=1` before recorder startup to enable it; the
default path creates no timestamp query pool and records no timing commands.
Each filter instance reports batches of 360 completed samples, labeled with
its filter name. The result is device-side compute time converted with the
Vulkan timestamp period. Readback happens when FFmpeg next reuses an already
fenced execution slot, so it does not add an extra synchronization point.
This experiment covers the two `scale_vulkan` conversion stages. Vulkan Video
encode instrumentation brackets `vkCmdEncodeVideoKHR` for FFmpeg's
`h264_vulkan` and `hevc_vulkan` encoders. The helper applies that source patch
and compiles `libavcodec/vulkan_encode.o` as a compile check, but it does not
build or stage a private `libavcodec`. The recorder's current codec library
therefore emits no encode timing samples. This patch also does not instrument
`h264_nvenc` or `hevc_nvenc`; those CUDA/NVENC paths need separate timing
instrumentation. No GPU runtime test is part of the isolated build check.

When `FLUXREC_FF_VK_DEVICE_TIMING=1` is set, the recorder registers a private
per-filter-graph callback with the patched `libavfilter`. Completed timestamp
queries call it directly with the filter instance name, input frame PTS, and
elapsed nanoseconds; the recorder does not parse log messages. The telemetry
bridge pairs `gpu_rgb_to_nv12` and `gpu_output_resize` by PTS and adds their
durations into one per-frame `timings_ns.gpu_conversion` sample. Unmatched
samples and frames without a valid PTS are omitted. The existing bounded
telemetry window then supplies average and percentile values to the JSON/UI.
`nvenc_submission` and `nvenc_completion` remain reserved for hardware timing
sources and are not populated by host API spans.

`ffmpeg-n9.0.2-nvenc-api-timing.patch` adds an opt-in private callback to the
FFmpeg 9.0.2 H.264/HEVC NVENC wrapper. It times the host wall span around
`nvEncEncodePicture` and the blocking `nvEncLockBitstream` call using
`av_gettime_relative()`. The first event carries the input frame PTS; the
second carries NVENC's returned output PTS. The recorder joins them by PTS,
including when callbacks arrive out of order, and emits explicitly named
`nvenc_encode_picture_api_wall` and `nvenc_lock_bitstream_api_wall` timing
distributions plus recent paired PTS samples in JSON. Each callback also
reports its monotonic return timestamp. The PTS-matched interval from an
accepted `nvEncEncodePicture` return to a successful `nvEncLockBitstream`
return is emitted as `nvenc_submit_to_bitstream_wall`. This is a host-observed
upper-bound proxy for when the bitstream became retrievable: it includes
NVENC/FFmpeg queueing, host scheduling, and driver wait. It does not measure
NVENC hardware execution, and the separately named `nvenc_completion` hardware
metric remains unpopulated.

The added timestamp reads and bounded PTS join run only when this opt-in
callback is enabled. No extra CUDA event, device synchronization, or bitstream
lock is issued; the instrumented lock is the blocking lock FFmpeg already uses
to retrieve each output packet.

Build this diagnostic codec in an isolated directory after the configured
private source tree exists:

```sh
FLUXREC_BUILD_JOBS=4 scripts/build-ffmpeg-nvenc-api-timing.sh
```

The helper copies the configured source into ignored
`.validation/ffmpeg-nvenc-api-timing/source`, builds only that copy's
`libavcodec`, and stages it under `.validation/ffmpeg-nvenc-api-timing/lib`.
It also creates a runtime overlay at
`.validation/ffmpeg-nvenc-api-timing/runtime`, with symlinks to the current
private `libavutil`, `libavfilter`, and `libswscale`. Configure an isolated
recorder build with
`-DFLUXREC_PRIVATE_FFMPEG_LIBDIR=$PWD/.validation/ffmpeg-nvenc-api-timing/runtime`.
Launch it with `FLUXREC_FF_NVENC_API_TIMING=1`; the option fails clearly if
the loaded codec does not export the private callback. The helper does not
overwrite the default `.deps` runtime. This isolated build passed its export
and dependency checks. Controlled live H.264 and HEVC NVENC recordings
exercised the callback and produced paired timing samples; their conditions,
source-ID results, and host-span limitations are in
[`docs/measurements.md`](../docs/measurements.md).

`ffmpeg-n9.0.2-zero-cpu-route-audit-avutil.patch` and
`ffmpeg-n9.0.2-zero-cpu-route-audit-avfilter.patch` add an opt-in route probe
to private FFmpeg 9.0.2. With `FLUXREC_FF_ZERO_CPU_AUDIT=1`, patched
`libavutil` reports successful DMA-BUF-to-Vulkan import and Vulkan ARRAY to
CUDA DEVICE copy descriptors; patched `libavfilter` reports successful named
`scale_vulkan` compute stages and rejects software fallback or unexpected
frame types. Markers are first-success samples per frames context or filter,
not per-frame logs; `cpu_mapping=none_in_importer` is evidence about that
FFmpeg source path rather than proprietary driver behavior. Build and stage
the diagnostic in an isolated overlay with
`tools/build_ffmpeg_zero_cpu_audit.sh all`. Its verifier, exact environment,
NVENC correlation, and validated run are documented in
[`docs/ffmpeg-zero-cpu-route-audit.md`](../docs/ffmpeg-zero-cpu-route-audit.md).
The helper leaves `.deps/ffmpeg-bgra-b8/lib` unchanged.

`ffmpeg-n9.0.2-vulkan-input-exec-pool-drain.patch` adds the private
`av_vk_frame_ctx_exec_pool_drain` helper used only when the recorder is started
with `FLUXREC_GPU_INPUT_CTX_DRAIN=1`. It polls accepted submissions in the
input frames context's Vulkan compute pool against one monotonic budget and
releases dependencies only after successful fence completion. On error it
preserves unfinished dependencies; the recorder retries before teardown and
fail-stops without unwinding if completion remains unproven. The patch keeps
FFmpeg's original infinite wait for un-enrolled Vulkan pools. A pool enrolled
in the opt-in drain uses a five-second final-free check and aborts if work is
still pending; any pool with `pending_submission` but no fence also aborts.
This diagnostic patch is not included in the default private-runtime build and
must be built and loaded in an isolated overlay. Its authorized three-second
run and refcount evidence are in
[`docs/measurements.md`](../docs/measurements.md#2026-09-28-isolated-input-vulkan-execution-pool-drain).

## Build and use

From the repository root, run:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-release
./scripts/check-ffmpeg-runtime.sh \
  ./build-release/engine/fluxrec-record \
  .deps/ffmpeg-bgra-b8/lib
```

On a clean checkout, the default CMake build runs the helper before linking
`fluxrec-record`. The helper fetches the official FFmpeg 9.0.2 and AMF v1.5.2
archives, plus the upstream `nv-codec-headers` commit
`1889e62e2d35ff7aa9baca2bceb14f053785e6f1` (release n12.1.14.0), and checks
all three pinned SHA-256 digests. It stages the codec headers and pkg-config
file under ignored `.deps/ffmpeg-bgra-b8/ffnvcodec/`; it does not install
anything into the system. The B8-patched `libavutil` is built with Vulkan,
DRM PRIME, AMF, and ffnvcodec support. FFmpeg uses that ffnvcodec support to
compile its CUDA hwcontext and Vulkan-to-CUDA frame transfer implementation.
The builder does not require the CUDA toolkit or `nvcc`; CUDA driver functions
are dynamically loaded at runtime. A separate source tree builds the
wait-, timing-, and telemetry-callback-patched `libavfilter` and matching
`libswscale`, enabling only the
buffer, buffersink, scale, scale_vulkan, and blackdetect_vulkan filters used
by this recorder and its diagnostics. The helper stages all three versioned
libraries and a build manifest under ignored `.deps/ffmpeg-bgra-b8/`; it does
not write to `/usr/lib`. Run the helper directly to prepare the private
runtime before configuring CMake. It also compiles the patched Vulkan Video
encoder source file, but does not package that object or build a private
`libavcodec`.

For an offline build, provide verified cached copies of all three archives to
the helper or to `cmake --build`:

```sh
FLUXREC_FFMPEG_ARCHIVE=/cache/ffmpeg-9.0.2.tar.xz \
FLUXREC_AMF_HEADERS_ARCHIVE=/cache/AMF-headers-v1.5.2.tar.gz \
FLUXREC_FFNVCODEC_ARCHIVE=/cache/nv-codec-headers-1889e62.tar.gz \
  ./scripts/build-ffmpeg-bgra-b8.sh
```

The archive hashes are checked on every run; the expected values and source
URLs are recorded in the build helper. `FLUXREC_BUILD_JOBS` defaults to four
and can change build parallelism. For isolated staging, set
`FLUXREC_FFMPEG_BUILD_ROOT`, `FLUXREC_FFMPEG_SOURCE_ROOT`, and
`FLUXREC_FFMPEG_LOG_ROOT`; their defaults use the normal `.deps` locations.
CMake defaults to `.deps/ffmpeg-bgra-b8/lib`
and builds the stack automatically when any member is missing or invalid.
Another complete private directory can be selected with
`-DFLUXREC_PRIVATE_FFMPEG_LIBDIR=/path/to/private/lib`; CMake rejects an
incomplete or invalid custom directory.

CMake embeds the selected directory in the recorder's `DT_RPATH` and verifies
loader resolution after linking. The Qt UI launches that same recorder binary,
so it does not need an inherited `LD_LIBRARY_PATH`. The runtime checker
requires `libavutil` 61.1.102, `libavfilter` 12.1.102, and `libswscale`
10.1.102; BGRA-to-B8 mapping 44; a registered CUDA hwdevice; the required
filter registry; matching CUDA metadata when a manifest is present; and no
unresolved `ldd -r` symbols. It confirms the CUDA and Vulkan-to-CUDA code was
compiled, but does not initialize a CUDA device or prove runtime external
memory interoperability. It checks that the three libraries still resolve
from the private directory even with `LD_LIBRARY_PATH=/usr/lib`.

## Validation

The isolated timing build compiled `libavcodec/vulkan_encode.o`, the patched
`libavfilter/vulkan_filter.o`, and the timing-enabled private `libavutil`,
`libavfilter`, and `libswscale` libraries. The private runtime checker passed
after staging. The encoder object compile check is not evidence that the
encoder patch runs in the recorder: the staged runtime does not contain
`libavcodec`. Set `FLUXREC_FF_VK_DEVICE_TIMING=1` before startup to collect
the two `scale_vulkan` stage timings from the staged private filter/runtime.
A three-second H.264 desktop smoke after rebuilding the production private
runtime reported two 360-sample batches for each conversion stage, averaging
45–47 microseconds per GPU dispatch, while the recorder encoded 1,081 frames
without reported drops or pool starvation. This validates the filter timestamp
path at runtime; it does not measure total conversion latency or encode time.
See `docs/measurements.md` for the exact samples and conditions.

The source-built library was checked against a color chart: 1,441 decoded
frames had consecutive source IDs, with no black or malformed frames; mean
bar RGB error was 0.78 code values. The same library supported a 60-second
low-motion CAVLC run with 21,600 consecutive decoded IDs at 360.1 mean steady
encoded FPS, no internal drops or pool starvation, and no black, invalid,
duplicate, backward, or missing frames. See
[`docs/measurements.md`](../docs/measurements.md) for the full conditions and
artifacts. These results do not establish performance under high-motion or
game workloads, or for HEVC at 360 Hz.
