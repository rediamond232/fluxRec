# Optional GPU frame identity digest

`frame_digest.comp` and `frame_digest_reduce.comp` calculate a fixed 128-bit
digest of every visible RGB pixel in a captured frame. They exist to measure
whether consecutive capture completions contain different image content.
They do not read back, map, copy to CPU, or expose raw pixels. The CPU receives
only four 32-bit digest words (16 bytes).

The digest is deterministic and coordinate-aware. Each pixel's 8-bit RGB
components and `(x, y)` coordinate feed four domain-separated 32-bit
avalanche hashes. Pass one sums each hash lane modulo 2³² inside workgroups;
pass two reduces the workgroup records and mixes in the image extent and
pixel count. The digest is a collision-resistant practical identity check,
not a cryptographic checksum or proof: a 128-bit aggregate can collide, and
the mixer is not designed to resist adversarial inputs. It hashes visible RGB
only, so differences confined to X/alpha are intentionally ignored.

## Descriptor and dispatch ABI

Pass one uses local size `(128, 1, 1)`:

| Descriptor | Resource |
| --- | --- |
| set 0, binding 0 | sampled source image whose `.rgb` returns semantic RGB; this may be `VK_FORMAT_B8G8R8A8_UNORM`, or FFmpeg's `VK_FORMAT_R8G8B8A8_UNORM` with `{B,G,R,A}` component mapping for BGRA memory |
| set 0, binding 1 | write-only `std430` SSBO of `uvec4` partial digests |

Its push constants are `uvec2 sourceExtent` (8 bytes). Dispatch
`(ceil(width * height / 128), 1, 1)`. Each partial record is 16 bytes, so the
partial SSBO size is `16 * ceil(width * height / 128)` bytes. At 2560×1440,
pass one dispatches 28,800 workgroups and writes 460,800 bytes.

Pass two also uses local size `(128, 1, 1)` and exactly one workgroup:

| Descriptor | Resource |
| --- | --- |
| set 0, binding 0 | read-only partial-digest SSBO from pass one |
| set 0, binding 1 | write-only result SSBO containing one `uvec4` (16 bytes) |

Its 16-byte push constant is `uvec4 values = {width, height, partialCount,
pixelCount}`. CPU code must reject zero dimensions and dimensions whose
product overflows `uint32_t`; 2560×1440 is well within range. Read result
words in index order as four little-endian `uint32_t` values.

## Submission, synchronization, and overhead

Keep the pass disabled by default during throughput measurements, then enable
it explicitly for capture-identity validation. When enabled:

1. Submit pass one after the capture producer signals that the imported image
   is ready. Keep that capture slot leased until the pass-one source read has
   completed.
2. Insert a compute-shader-write to compute-shader-read buffer dependency for
   the partial SSBO, then submit pass two. These shaders may use the same
   queue and command buffer with the corresponding barrier.
3. Make pass-two's 16-byte result visible to a transfer read, copy only those
   16 bytes to a host-visible readback-ring slot, then signal a fence or
   timeline value. Poll completed readbacks asynchronously; never wait for
   this digest on the capture event thread.
4. Map/invalidate/read only the completed 16-byte readback slot as required by
   its memory properties. Do not map the source, partial buffer, converted
   Y/UV images, or any other pixel-bearing resource.

Pass one reads every RGB texel once, approximately 14.7 MB per 2560×1440
frame. At 360 FPS that is about 5.3 GB/s of additional source reads, before
hashing and reduction work. Timestamp the two passes with GPU timestamp
queries, report digest time and queue cost separately, and omit the work from
performance runs whose purpose is to measure the uninstrumented capture path.
The readback ring should be large enough to keep this optional work
asynchronous and should count any digest-queue backpressure separately from
capture drops.

## Build and validate

From the repository root:

```sh
mkdir -p build/gpu/shaders
glslangValidator -V --target-env vulkan1.2 -S comp \
  -o build/gpu/shaders/frame_digest.comp.spv \
  gpu/shaders/frame_digest.comp
spirv-val --target-env vulkan1.2 build/gpu/shaders/frame_digest.comp.spv

glslangValidator -V --target-env vulkan1.2 -S comp \
  -o build/gpu/shaders/frame_digest_reduce.comp.spv \
  gpu/shaders/frame_digest_reduce.comp
spirv-val --target-env vulkan1.2 \
  build/gpu/shaders/frame_digest_reduce.comp.spv
```

Use an isolated test harness to dispatch small images, copy back only the
16-byte digest, and compare it with a CPU reference implementing the integer
operations in `hashPixel` and both reductions. Expected vectors are shown as
`words[0] words[1] words[2] words[3]`, each word in 8-digit hexadecimal:

| Input image | Expected digest |
| --- | --- |
| 2×2 all black RGB | `743e25b1 27411ae2 960e06ed abf17794` |
| 2×2 all white RGB | `880f0e88 6d202db0 332ffa50 a89149c8` |
| 2×2 black except red at (0,0) | `d21d1108 9e83a475 e633ad69 8a3dc828` |
| 2×2 black except red at (1,0) | `2eea4c86 c77ad318 2baaf912 aeb859aa` |
| 2560×1440 all black RGB | `742ce06b 56ce4451 f243046a cf3ea599` |

The first two vectors cover uniform data; the latter pair have identical color
counts but different locations, checking that coordinates are part of the
digest. Also test a production-size changing pixel at each corner, alpha-only
changes (digest must remain equal), and repeated identical frames (digest must
remain equal). These checks qualify the instrumented path; their CPU reference
must remain outside normal recording and must never receive captured pixels
from the production pipeline.

For a production-size source diagnostic, compare the four logged digest words
against the synthetic all-black vector at the matching frame dimensions. A
different value means the sampled RGB image is not entirely black, subject to
the digest's collision limitation. This checks full-frame values without
adding pixel readback or changing the GPU identity path.

## Live Wayland smoke

Build the standalone `fluxrec-frame-identity-smoke` executable with CMake, then
run it from the repository root (or pass the shader directory explicitly):

```sh
cmake -S . -B build
cmake --build build --target fluxrec-frame-identity-smoke
build/tools/fluxrec-frame-identity-smoke \
  --device /dev/dri/renderD128 --output DP-1 --seconds 8
```

## Recorder diagnostics

`fluxrec-record --verify-frame-ids` enables the same digest on each imported
RGB Vulkan frame before conversion. Completed digest JSON records are written
to stderr as `type=frame_identity`; each includes the capture `sequence`,
encoder `pts` when available, four digest words, and `unique`, `duplicate`, or
`unclassified` status. A digest is classified only when it can be compared
with the immediately preceding captured sequence. A skipped digest or sequence
gap therefore does not create a false duplicate/unique result. The periodic
telemetry JSON reports cumulative `identity_checked`, `identity_skipped`, and
`identity_unclassified` counts alongside `unique`, `duplicated`, and
`fps.unique`. The timing distribution named `frame_identity_gpu` measures the
two compute passes; it does not include source-image capture or encoder time.

For investigating a bad conversion frame, correlate the recorder record's
capture sequence and PTS with the converter's per-frame diagnostic. If the RGB
digest changes normally on a frame whose output NV12 is black, the defect is
after the identity dispatch in the conversion path. If the RGB digest equals
the neighboring black-frame digest, the imported/sampled source contained the
same RGB content at identity-dispatch time. A digest is not a pixel-value
inspection: for a controlled known-color stimulus, use a deliberately changing
test pattern or a separate GPU min/max or known-pixel probe if it is necessary
to prove that a uniform-black digest represents black values rather than some
other uniform RGB color. No raw pixels are read back by this option.

This is an opt-in diagnostic, not a benchmark setting. It adds a full read of
the source image, a 16-byte readback per completed frame, and one stderr JSON
line per digest. Keep it disabled for uninstrumented throughput runs.

`fluxrec-record --verify-frame-ids-source-only` is a split diagnostic that
submits the same all-pixel source-reading compute pass and frame semaphore
wait/signal, but skips reduction, digest copy, and host-visible digest
allocation/mapping. Its stderr records use `type=frame_identity_workload` and
contain mode, sequence, PTS, and optional GPU timestamp, but no digest. The
`frame_identity_gpu` timing then measures only the first compute pass, and
unique/duplicate counters remain absent. This flag is also diagnostic-only.

`fluxrec-record --verify-frame-ids-unrelated N` runs an unrelated compute
workload on the same FFmpeg Vulkan queue and dispatch grid. It waits and
signals the imported frame's timeline semaphore to preserve ordering, but does
not create an image view, bind the imported image, transition its layout, or
read its pixels. The workload writes per-workgroup math results to a device
buffer and skips digest reduction and readback. `N` is 1–64 rounds of integer
mixing per pixel and can be tuned to match the source-pass timing. Its stderr
records use mode `unrelated_workload`; unique/duplicate counts remain absent.
This comparison helps separate time spent reading captured image data from
similar same-queue compute delay.

It negotiates the output's AR24/XR24 DMA-BUF modifiers, allocates a GBM pool,
uses `WaylandCapture` and FFmpeg's direct DRM_PRIME-to-Vulkan import, then
submits each imported frame to `FrameIdentity`. The capture lease stays held
until that frame's asynchronous digest finishes. The smoke prints only the
16-byte digest, sequence/timing metadata, and capture counters; it never maps
or prints source pixels. It exits successfully only after observing at least
two unique digests. Move a window or play visible animation on the selected
output during the run; on a static desktop, repeated equal digests are the
expected result and the tool exits with an instruction to change the screen.

`--measure-gpu-time` enables Vulkan timestamp queries for the two digest
passes. This instruments the GPU path and should be omitted for normal capture
performance measurements. The digest reads every RGB texel (about 14.7 MB per
2560×1440 frame) and the CPU receives only 16 bytes per completed frame. The
ring is bounded at eight; if it fills, the tool releases that capture lease,
records a digest-ring skip, and continues rather than waiting on the GPU.
