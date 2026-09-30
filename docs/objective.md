Build "Fluxrec", a new high-performance native Wayland screen recorder for
Hyprland, from scratch.

The primary engineering goal is extremely high and consistent capture
throughput on NVIDIA GPUs, initially targeting:

- RTX 3090 Ti / Ampere
- NVIDIA proprietary Linux driver
- Hyprland / Wayland
- 2560x1440 @ 359.979 Hz
- Genuine ~360 FPS capture
- H.264 and HEVC NVENC
- Arch Linux

This is NOT a wrapper around OBS, GPU Screen Recorder, wf-recorder, or
wl-screenrec. Build a purpose-designed recording engine.

The UI should take inspiration from the workflow/layout of OBS Studio because
I like its interface, but do not copy OBS source code, branding, icons, assets,
or pixel-for-pixel visual design. Create our own implementation and visual
identity.

============================================================
CORE ARCHITECTURAL REQUIREMENT: ZERO CPU PIXEL PROCESSING
============================================================

Raw captured video frames must remain GPU-resident throughout the entire
video pipeline.

Desired conceptual pipeline:

Hyprland
  -> ext-image-copy-capture-v1
  -> linux-dmabuf
  -> NVIDIA GPU import
  -> GPU XRGB/BGRX -> NV12 conversion
  -> NVENC H.264/HEVC
  -> compressed packet queue

The CPU must NEVER process, map, copy, convert, or otherwise handle raw video
pixel data during normal recording.

Explicitly DO NOT implement a final pipeline containing:

GPU -> CPU framebuffer
GPU -> hwdownload -> CPU -> hwupload -> GPU
CPU RGB -> NV12 conversion
CPU framebuffer copies

A CPU fallback may exist only as an explicitly selected debugging mode and
must never silently activate.

Investigate the technically best zero-copy NVIDIA path rather than assuming
a particular API.

Candidates include:

- direct DMA-BUF -> CUDA external memory
- DMA-BUF -> Vulkan external memory -> CUDA/NVENC
- EGL/DMA-BUF interop
- direct NVIDIA/NVENC-compatible GPU surfaces
- FFmpeg AVHWFramesContext interop
- NVIDIA Video Codec SDK APIs

Choose the lowest-overhead robust architecture based on actual API/driver
capabilities.

============================================================
KNOWN HARDWARE / PROTOCOL FACTS
============================================================

The target monitor is DP-1:

Dell AW2725DF
2560x1440
359.979 Hz

Hyprland supports:
- ext-image-copy-capture-v1
- linux-dmabuf-v1
- wlr-screencopy

Testing with wl-screenrec established:

- ext-image-copy-capture works at 2560x1440.
- Hyprland exposes XR24/AR24 DMA-BUF capture surfaces.
- NVIDIA BLOCK_LINEAR_2D DRM modifiers are exposed.
- NVIDIA Vulkan can directly import several of the uncompressed block-linear
  modifiers.
- wl-screenrec's Vulkan path successfully gets past DMA-BUF negotiation.
- wl-screenrec fails later because its Vulkan H.264 encoder cannot encode the
  selected format.
- Trying to force h264_nvenc into wl-screenrec's Vulkan architecture fails
  because it lacks an appropriate Vulkan -> NVENC hardware-frame bridge.
- RTX 3090 Ti supports H.264 and HEVC NVENC. It does NOT support AV1 NVENC.

Do not repeat already-solved investigation unless necessary.

============================================================
CPU / GPU DIVISION OF LABOR
============================================================

GPU:

- DMA-BUF import
- raw frame ownership
- colorspace conversion
- scaling if requested
- NV12/P010 generation
- NVENC
- optional GPU preview rendering

CPU:

- Wayland event dispatch
- synchronization/fence orchestration
- encoded packet queues
- audio capture
- audio encoding
- A/V synchronization
- container muxing
- replay-buffer management
- filesystem writes
- UI
- configuration
- statistics/telemetry

The CPU and GPU should operate concurrently.

CPU involvement is desirable for tasks suited to the CPU. The objective is
NOT minimum CPU utilization at all costs.

The objective is:

ZERO CPU VIDEO PIXEL PROCESSING
+
MAXIMUM ASYNCHRONOUS CPU/GPU/NVENC OVERLAP.

============================================================
PIPELINING
============================================================

Do not build a synchronous pipeline like:

capture
wait
convert
wait
encode
wait
write

Build an asynchronous multi-frame pipeline.

Conceptually:

Wayland capture
      |
      v
GPU frame pool
      |
      v
GPU conversion queue
      |
      v
NVENC queue
      |
      v
compressed packet queue
      |
      +----> mux queue ----> disk writer
      |
      +----> replay ring buffer

Use GPU fences/semaphores/events and bounded queues.

Support multiple frames in flight.

Determine the appropriate frame-pool/queue depth experimentally. Start around
8-16 if reasonable, but benchmark rather than assuming.

A slow UI, audio operation, mux operation, replay-buffer operation, or disk
write MUST NOT synchronously stall capture, GPU conversion, or NVENC
submission.

============================================================
360 FPS REQUIREMENTS
============================================================

360 FPS means approximately 2.778 ms between frames.

However, exploit pipeline parallelism rather than requiring every stage's
combined latency to fit serially inside 2.778 ms.

Measure BOTH throughput and latency.

Most importantly:

DO NOT FAKE 360 FPS.

The recorder must distinguish between:

- compositor frames captured
- unique frames captured
- frames submitted to NVENC
- frames encoded
- frames muxed
- dropped frames
- duplicated frames

Never claim 360 FPS merely because the container timebase says 360 FPS.

Expose real capture statistics.

============================================================
REPLAY BUFFER
============================================================

Replay buffer is a first-class feature.

The replay buffer stores COMPRESSED NVENC packets in RAM, not raw video
frames.

Use a bounded circular packet buffer.

Allow configurable history duration, e.g.:

15 sec
30 sec
60 sec
120 sec
custom

Saving a replay should asynchronously mux/write the buffered packets without
stalling ongoing capture.

Recording and replay-buffer modes should share the same underlying capture
and encoding engine.

============================================================
AUDIO
============================================================

Use PipeWire for audio capture where appropriate.

Initially support:

- desktop audio
- microphone
- per-source volume
- mute
- audio meters

Audio work belongs on CPU threads and must not block video capture.

Correctly synchronize audio against video timestamps.

============================================================
CONTAINER / ENCODING
============================================================

Initial video encoders:

- H.264 NVENC
- HEVC NVENC

Expose useful controls:

- bitrate
- CQP / constant quality where supported
- NVENC preset
- GOP/keyframe interval
- B-frames
- codec
- resolution
- FPS

Prefer MKV for crash-safe recording.

Support MP4 later or through remuxing.

Do not let container muxing happen on the capture thread.

============================================================
UI
============================================================

Build a native desktop UI inspired by OBS's workflow.

Do NOT use Electron.

Prefer a native framework appropriate to the selected language and
architecture. C++23 + Qt 6 is a strong candidate, but evaluate alternatives
if there is a compelling technical reason.

Initial layout should include:

Large preview

Scenes panel
Sources panel
Audio Mixer
Statistics
Controls

Controls:

Start Recording
Stop Recording
Start Replay Buffer
Stop Replay Buffer
Save Replay
Settings

The visual design should be modern and dark, with our own identity rather
than an OBS clone.

The UI must remain completely decoupled from the recording engine.

A frozen or slow UI must have effectively zero effect on recording.

Preview rendering should NOT require CPU copies.

Preview may run at a lower refresh rate such as 60 FPS independently from a
360 FPS recording.

============================================================
PERFORMANCE TELEMETRY
============================================================

Build serious instrumentation from the beginning.

Display/log:

Capture FPS
Unique capture FPS
Encode FPS
Mux FPS

Capture latency
GPU conversion latency
NVENC submission latency
NVENC completion latency
Mux latency
Disk-write latency

Capture drops
Conversion drops
Encoder drops
Mux drops
Duplicated frames

GPU frame-pool occupancy
Capture queue occupancy
Encode queue occupancy
Packet queue occupancy
Queue high-water marks

Output bitrate
Recording filesize
Replay-buffer RAM usage

GPU utilization if practical
NVENC utilization if practical
CPU utilization if practical

Provide frame-time histograms/percentiles where useful:

average
p50
p95
p99
p99.9
maximum

============================================================
PERFORMANCE TARGET
============================================================

Primary target:

2560x1440 @ 359.979 Hz
RTX 3090 Ti
Hyprland
H.264 NVENC

Goal:

Sustain approximately 360 genuine captured/encoded FPS with effectively zero
drops under normal recording conditions.

The application itself should introduce minimal compositor/game performance
impact.

After achieving H.264, benchmark HEVC.

============================================================
IMPLEMENTATION STRATEGY
============================================================

Do NOT spend the first large portion of the project polishing the UI.

Work in this order:

1. Research exact zero-copy NVIDIA DMA-BUF -> NVENC path.
2. Build minimal headless capture prototype.
3. Prove DMA-BUF import without CPU pixel access.
4. Implement GPU colorspace conversion.
5. Integrate NVENC.
6. Produce a valid MKV recording.
7. Instrument real capture/encode FPS and dropped frames.
8. Benchmark 1440p360.
9. Optimize synchronization and queueing.
10. Implement replay buffer.
11. Implement audio.
12. Build the native UI around the proven engine.
13. Add settings/configuration.
14. Polish and harden.

Do not stop at an architecture/design document if implementation is possible.

Compile and test continuously.

============================================================
VALIDATION
============================================================

Prove zero-copy rather than merely claiming it.

Document exactly how DMA-BUF memory reaches the conversion stage and NVENC.

Instrument or otherwise verify that normal operation performs no CPU mapping
or framebuffer memcpy of captured video surfaces.

Benchmark:

- idle desktop
- high-motion desktop
- game/high-motion workload
- recording
- replay-buffer-only
- recording + replay buffer

Test at:

60 FPS
120 FPS
240 FPS
360 FPS

Measure dropped/duplicated frames and frametime distribution.

If 360 FPS cannot be sustained, identify the exact bottleneck using
instrumentation rather than guessing.

============================================================
ENGINEERING QUALITY
============================================================

Use clean modular architecture.

Separate:

capture/
gpu/
encoder/
audio/
mux/
replay/
telemetry/
ui/
config/

Use RAII for GPU/Wayland/NVENC resources.

Handle device loss and compositor restart where realistically possible.

Fail loudly rather than silently falling back to CPU pixel processing.

Keep detailed technical documentation describing:

- architecture
- synchronization
- ownership/lifetimes
- zero-copy path
- performance measurements
- known limitations

============================================================
END GOAL
============================================================

Fluxrec should eventually feel like a lightweight OBS-style recording
application whose engine was designed specifically around modern Wayland,
DMA-BUF, NVIDIA GPUs, NVENC, replay buffering, and extreme-framerate capture.

The first major milestone is not feature completeness.

The first major milestone is:

PROVEN ZERO-COPY 2560x1440 @ ~360 FPS RECORDING ON THE RTX 3090 Ti.

Prioritize achieving and objectively verifying that milestone above everything
else.