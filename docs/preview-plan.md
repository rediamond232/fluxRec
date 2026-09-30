# GPU-resident preview bridge plan

## Decision

Keep the existing Widgets application and recorder as separate processes. Add an
optional preview channel over a Unix `SOCK_SEQPACKET` socket. The recorder owns a
small GPU DMA-BUF preview pool; Qt imports those buffers into its Vulkan device
and displays them through a Qt Quick scene embedded in the existing Widgets
preview panel.

The recorder produces no more than 60 preview frames per second, at 960×540
RGBA8 initially. It downsamples on the GPU from a captured Vulkan frame into a
dedicated preview slot. It never gives the Qt process a capture-pool buffer or
extends that buffer's `FrameLease` lifetime. If no preview slot is free, it
skips the preview pass for that frame. It never waits for Qt, IPC, or a GPU
fence on the capture/encode path.

This uses the existing process boundary: `ProcessRecorderBackend` already
starts `fluxrec-record` with `QProcess`; the UI continues to own controls and
telemetry while the engine owns capture, Vulkan, and encode. The preview socket
is optional, so `fluxrec-record` remains usable headlessly.

## Why Qt Quick inside the Widgets panel

The placeholder is the private `PreviewCanvas` widget in
`ui/src/main_window.cpp`, created by `buildPreviewPanel()`. The implemented
adapter is `PreviewQuickWidget`, a Widgets container that embeds a configured
`QQuickView` with `QWidget::createWindowContainer()`. Its custom `QQuickItem`
uses a scene-graph node to sample the imported Vulkan image, while the rest of
the interface remains Widgets.

The implementation uses `QQuickView` because a `QQuickWidget` initializes its
offscreen RHI in the base constructor, before a subclass can request the
external-memory and sync-file Vulkan device extensions needed by DMA-BUF
import. `QQuickView::setGraphicsConfiguration()` applies those extensions
before the scene graph creates its Vulkan device. The native window container
keeps Qt Quick rendering on its render thread and leaves the recorder/UI
process boundary unchanged.

Use `QNativeInterface::QSGVulkanTexture::fromNative()` to wrap the imported
2D RGBA `VkImage` as a non-owning `QSGTexture`. The wrapper does not own the
image and must be created/destroyed on the render thread. Keep each Vulkan image
and imported `VkDeviceMemory` alive until Qt has finished its last draw using
that slot. Do not route through `QImage`, `QPixmap`, `QVideoFrame::map()`, or
any pixel readback.

Qt Quick must select Vulkan before creating a Quick window and enable the
required Vulkan device extensions before scene-graph initialization. Pin its
physical device to the recorder's device identity where possible; otherwise
compare the initialized Quick device identity to the recorder handshake and
disable preview if they differ. DMA-BUF import failure must leave recording
available and show the existing preview-unavailable state.

## Buffer and socket protocol

The UI creates a private runtime-directory Unix socket before starting the
recorder and passes its path as `--preview-socket PATH`. Use mode `0600`, a
per-session unpredictable name, `SOCK_SEQPACKET`, and `CLOEXEC` on the listening
socket. The UI owns the listener and accepts one engine connection. This avoids
inheriting an FD through `QProcess` and gives the UI a clear lifetime for the
channel.

Use `sendmsg()`/`recvmsg()` with `SCM_RIGHTS`; ordinary `QProcess` stdout remains
the JSON telemetry channel. A bounded nonblocking transport sends only control
records, DMA-BUF FDs, and fence FDs. Never put raw pixel bytes in a socket
message. The channel has these messages:

1. `HELLO`: protocol version and engine session id.
2. `POOL`: generation, 960×540 dimensions, DRM fourcc/modifier, plane count,
   per-plane offset/stride, physical-device UUID, and the preview slot DMA-BUF
   FDs. Send each pool FD once. Start with three slots; size can become
   configurable after measurement.
3. `FRAME`: pool generation, slot index, capture sequence, presentation time,
   and an acquire `sync_file` FD signaled when the GPU preview write completes.
4. `RELEASE`: generation and slot index plus a completion FD signaled after
   Qt's last GPU sampling command for that image completes. This is normally
   an exported `sync_file`; if Vulkan reports that the point is already
   signaled, the UI sends a pre-signaled `eventfd` which the producer polls.
5. `STOP`/`ERROR`: clean shutdown or a diagnostic reason.

Slot ownership is explicit:

```text
free → writing → ready → UI-reading → release-pending → free
```

The engine may replace an older unsent `ready` frame with a newer one. It never
reuses a `UI-reading` or `release-pending` slot. It does not block when all
slots are busy; it increments a preview-drop counter and continues recording.
On socket loss, it stops preview production and releases the preview pool after
its own outstanding GPU work completes.

The initial ABI fixes producer output to DRM_FORMAT_ABGR8888 (little-endian
byte RGBA), `VK_IMAGE_LAYOUT_GENERAL`, and `VK_QUEUE_FAMILY_EXTERNAL`. The UI
polls each acquire sync-file without reading image contents on the CPU. After
it signals, Qt records the EXTERNAL→Qt queue ownership and GENERAL→shader-read
transition before sampling. When Qt switches away from a slot, `afterFrameEnd`
queues the Qt→EXTERNAL ownership/layout transition on Qt's graphics queue,
signals an exportable sync-fd semaphore, and attaches that fd to RELEASE. A
local Vulkan fence tracks when the same submission has completed so the UI can
destroy retired imports safely. `afterFrameEnd()` alone is not proof of GPU
completion, and `frameSwapped()` is never used as the release signal. If fence
export or the ownership transition fails, the UI withholds RELEASE and the
producer keeps that slot busy; a permanent export failure closes the optional
preview channel so the producer can drain and discard its pool.

The producer polls RELEASE completion FDs on its preview worker; it does not
import them as Vulkan semaphores. This permits an already-signaled `eventfd`
completion descriptor when Vulkan returns `-1` for a completed `SYNC_FD` export.

All socket sends from the recording path are nonblocking. If sending a `FRAME`
message would block, drop that preview publication. No queue in the engine may
wait for a UI acknowledgment. Fence completion is polled asynchronously by the
preview worker; only the preview pool is affected by a slow or dead UI.

## Code insertion points

### New `preview/` component

A narrow `fluxrec_preview` library contains the transport framing, FD
ownership, slot-state transitions, and nonblocking Unix socket helpers. It has
no Qt, Vulkan, GBM, FFmpeg, or capture dependency. The root component list adds
`preview`; the engine and Qt consumer link the same protocol library.
Protocol tests should cover malformed/truncated messages, FD cleanup on every
error path, generation changes, invalid slot transitions, nonblocking send
failure, and disconnect cleanup. Keep protocol messages fixed-size/versioned
and cap all ancillary-FD counts.

### Engine option and setup

- Add optional `--preview-socket PATH` to `Options`, argument parsing, and usage
  in `engine/record.cpp`.
- After `GpuEncoder::create()` succeeds and before capture starts, connect the
  preview client and pass the optional output config to the GPU encoder. If the
  UI is absent or the socket cannot connect within a short bounded startup
  window, disable preview and continue recording.
- Report preview state, output rate, and `preview_skipped_pool_full`,
  `preview_transport_drops`, and import/handshake errors through existing
  telemetry JSON. Do not report preview drops as capture/encode drops.
- The UI listener belongs in `ProcessRecorderBackend::start_recording()`, before
  `QProcess::start()`. Add the socket path to the recorder arguments there.
  Stop/close the listener on process finish or startup failure. UI/widget edits
  are intentionally deferred for coordination.

### GPU copy and output allocation

The producer pass belongs inside `GpuEncoder::Impl::submit()` in
`gpu/gpu.cpp`, where the captured DMA-BUF has already been mapped directly to
an `AV_PIX_FMT_VULKAN` `AVFrame`. This is the only location that has both the
imported source image and the FFmpeg Vulkan device. Dispatch at most once per
60 Hz interval, before the existing direct-conversion/fallback branch. It must
submit a source read and downscale into a dedicated preview-ring slot without
changing the capture image's lifetime contract. The enclosing captured lease
already remains pending until encode completion, so the preview dispatch must
be ordered before that lease can be released.

Implement the pass and pool in new GPU-side source files rather than expanding
`gpu/direct_nv12.cpp`. Allocate three exportable 960×540 RGBA8 DMA-BUF images
with a modifier accepted by the importing Vulkan device. Query exact format,
modifier, sampled/storage usage, external-memory import/export, and external
semaphore support; fail preview setup cleanly if any required capability is
missing. Prefer a Vulkan-created exportable image pool if the NVIDIA driver
reports one; otherwise use a GBM allocation and direct `AV_PIX_FMT_DRM_PRIME`
→ `AV_PIX_FMT_VULKAN` import, after verifying writable external-image support.
Do not assume that the capture pool's modifier or its input-import proof also
proves output export/import support.

The preview pass reads only the needed samples from the 2560×1440 source,
producing 960×540 RGBA at no more than 60 Hz. The destination image is an
independent allocation and stays allocated until the UI's release fence signals.
Use explicit Vulkan/DMA-BUF ownership and layout transitions around the
cross-process handoff. Export an acquire sync-file for the completed write and
import or poll the UI release sync-file before recycling the slot. If external
sync-fd support is missing, preview is unavailable; never insert a CPU wait or
copy fallback into recording.

### Widgets rendering

The UI-side integration point is `PreviewCanvas`/`buildPreviewPanel()` in
`ui/src/main_window.cpp`; the bridge is owned by `ProcessRecorderBackend` and
the preview panel embeds its `PreviewQuickWidget`. The working implementation:

- add `Qt6::Quick`, Vulkan, and the Quick scene-graph item
  target under `ui/preview_consumer`;
- embed a configured `QQuickView` in a Widgets panel with
  `QWidget::createWindowContainer()`; retain the styled unavailable canvas for
  headless mode or Qt Quick Vulkan initialization failure;
- keep the capture aspect ratio and expose waiting/error status to accessibility;
- select `QSGRendererInterface::Vulkan` before the first Quick window and set
  required device extensions on the `QQuickView` before its scene graph starts;
- create/import the `VkImage` and memory on the Qt Quick render thread using the
  advertised modifier and format, then wrap it through
  `QSGVulkanTexture::fromNative()`;
- retain only the newest signaled frame; the producer caps output at 60 Hz;
- tie `RELEASE` to a Qt queue ownership transition and exported completion
  sync-file after the final Qt use.

The target-GPU visible-preview smoke is recorded in
[`docs/architecture.md`](architecture.md#gpu-preview-path-and-live-check): the
item reported `has_frame` in 515 mid-run samples and 324 visible frame swaps;
the producer published 175 frames with zero preview drops or full-pool skips,
and the consumer returned 174 releases before shutdown. Concurrent recording
saved and decoded 300 H.264 frames with zero recorder drops or capture-pool
starvation. A separate five-second UI-stop check continued writing and decoded
3,453 consecutive source IDs. The longer combined audio/replay run is transport
evidence only because its preview window was mostly occluded; its limitations
are recorded in [`docs/measurements.md`](measurements.md#combined-audio-replay-and-preview-transport-run).

## Verification sequence

The protocol and Qt consumer can be built and tested without a compositor or
Vulkan device. Target-GPU import, visible rendering, acquire/release recycling,
60 FPS production, and UI-stop isolation have live evidence as summarized
above. Remaining verification is:

1. Unit-test protocol and slot ownership with synthetic FDs; test disconnects
   and malformed ancillary data without a compositor or Vulkan device.
2. Exercise Qt import using a synthetic exportable RGBA DMA-BUF and verify the
   image through GPU-side validation/digest, with no CPU pixel readback.
3. Compare controlled 360 FPS capture/encode runs with preview disabled and
   enabled; report recorder FPS, capture/encode drops, preview drops, GPU
   utilization, and GPU timing. Preview is accepted only if recording remains
   within the established engine target and never waits on preview/Qt.

## Qt/API references

- [QQuickView](https://doc.qt.io/qt-6/qquickview.html)
- [QWidget::createWindowContainer](https://doc.qt.io/qt-6/qwidget.html#createWindowContainer)
- [Embedding Qt Quick in a Widgets UI](https://doc.qt.io/qt-6/qtquick-embeddedinwidgets-example.html)
- [QSGVulkanTexture native-image wrapper](https://doc.qt.io/qt-6/qnativeinterface-qsgvulkantexture.html)
- [QQuickGraphicsConfiguration device extensions](https://doc.qt.io/qt-6/qquickgraphicsconfiguration.html)
- [QSGRendererInterface Vulkan resources](https://doc.qt.io/qt-6/qsgrendererinterface.html)
- [Qt Quick frame/render ordering](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html)
- [FFmpeg AVVulkanDeviceContext](https://ffmpeg.org/doxygen/trunk/structAVVulkanDeviceContext.html)
- [Vulkan DRM format modifiers](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_image_drm_format_modifier.html)
