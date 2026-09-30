# EGL DMA-BUF to OpenGL texture to NVENC probe

This opt-in diagnostic tests the Linux NVENC OpenGL input interface with the
Wayland capture module's GBM buffer pool. It allocates one-plane ARGB/XRGB
DMA-BUFs with a modifier negotiated for the selected output, imports each used
slot as an EGLImage on the matching DRM render node, attaches the image to a
`GL_TEXTURE_2D`, and registers that texture with NVENC as
`NV_ENC_INPUT_RESOURCE_TYPE_OPENGL_TEX` / `NV_ENC_BUFFER_FORMAT_ARGB`.

The probe contains no CPU raw-pixel read or conversion. It reads and writes
only NVENC's compressed H.264 bitstream on the CPU. This does not prove that
the display driver or NVENC driver avoids an internal GPU copy or staging step.
It is isolated from the production recorder and never silently falls back to
CUDA, Vulkan, or a CPU path.

`--gl-owned-control` is a one-frame discriminator for NVENC's GL texture
registration. It allocates a same-size `GL_TEXTURE_2D` with
`glTexImage2D(GL_RGBA8, ..., nullptr)` and passes that texture through the same
NVENC ARGB registration/map/encode calls. It does not read from the captured
DMA-BUF or initialize CPU pixel data, so its encode output is not a valid
capture-content check. A registration result here distinguishes an NVENC GL
session/input setup problem from rejection specific to EGLImage-backed texture
storage.

## API provenance

The NVENC Video Codec SDK 13.1 Programming Guide documents that OpenGL encode
sessions are Linux-only, require a current OpenGL context on the calling
thread, and must open with `deviceType = NV_ENC_DEVICE_TYPE_OPENGL` and
`device = NULL`. It documents externally allocated GL textures as input
resources. The included `nvEncodeAPI.h` defines
`NV_ENC_INPUT_RESOURCE_OPENGL_TEX` with a GL texture name and target, and says
`NV_ENC_REGISTER_RESOURCE::resourceToRegister` points to that structure for
`NV_ENC_INPUT_RESOURCE_TYPE_OPENGL_TEX`.
The guide's texture example allocates storage through `glTexImage2D`, which is
the storage form used by `--gl-owned-control`.

The EGL import path uses the Khronos `EGL_EXT_image_dma_buf_import` and
`EGL_EXT_image_dma_buf_import_modifiers` interfaces. The probe checks that EGL
advertises the exact capture fourcc/modifier pair and rejects a modifier marked
external-only, because NVENC accepts only `GL_TEXTURE_2D` or
`GL_TEXTURE_RECTANGLE` textures. `glEGLImageTargetTexture2DOES` then attaches
the imported EGLImage to a 2D texture. These checks show that the interfaces
advertise and accept the import on the active driver; only a live encode run
can establish that the complete path works.

## Local GL-texture registration result

On 2026-09-28, the live imported-texture probe successfully imported the
capture DMA-BUF with its exact modifier as an EGLImage and attached it to a GL
texture. NVENC then rejected that texture at `nvEncRegisterResource` with
status 23, `NV_ENC_ERR_RESOURCE_REGISTER_FAILED`. In the same GL context and
NVENC session setup, `--gl-owned-control` allocated a `GL_RGBA8` texture with
null pixel data and passed NVENC registration and a one-frame H.264 encode.
The control output is [nvenc-egl-gl-owned-control-20260928.h264](/home/lev/Fluxrec/.validation/nvenc-egl-gl-owned-control-20260928.h264).

This local result isolates the rejection to EGLImage-backed texture storage
for the observed driver path; the owned-texture control does not validate
capture pixels. `--gl-copy-from-import` tests a GPU-copy workaround by sampling
the EGLImage texture in a fragment shader and rendering into a GL-owned
`GL_RGBA8` FBO texture registered with NVENC. This route copies the full frame
on the GPU and is not allocation-level zero-copy. It calls `glFinish()` after
each render before NVENC maps the output, and retains the source capture lease
through encode completion and unmap.

The one-frame copy run at
[`nvenc-egl-gl-copy-20260928.h264`](/home/lev/Fluxrec/.validation/nvenc-egl-gl-copy-20260928.h264)
decoded as one valid, nonblack numbered source frame (ID 3888). The five-second
run at
[`nvenc-egl-gl-copy-5s-20260928b.h264`](/home/lev/Fluxrec/.validation/nvenc-egl-gl-copy-5s-20260928b.h264)
completed the API and cleanup path and encoded 1167 frames at 233.129 FPS.
It reported 1151 capture-pool starvation periods, queue high-water 7/8,
busy high-water 8/8, and zero capture drops. The matching fullscreen stimulus
log reported roughly 360 presentations per second with zero presentation-ID
gaps. Offline decode classified 1123 frames with valid IDs, 3 black frames,
and 41 frames without a valid ID. A 60-frame diagnostic localized all 44 bad
frames to the initial contiguous output span, indices 0 through 43: black at
indices 0, 39, and 40, and invalid IDs in the other 41. The following 1123
frames had valid IDs 164 through 1886, with 599 adjacent source-ID gaps and
600 missing IDs. This shows that the GL-copy route can register and encode a
real captured frame, but the run did not sustain source rate, preserve every
source ID, or meet the zero-black/zero-invalid target; the bad frames were
confined to startup in this sample, not recurring through the measured run.

The probe waits the producer DMA-BUF read fence, renders the full-size
`texelFetch` copy, calls `glFinish()` before NVENC maps the output, and waits
for bitstream lock/unlock plus unmap before releasing the lease. The saved
logs do not identify why the initial 44 frames were black or invalid. The high
pool-starvation count and source/output rate gap do establish that this
serialized path is not keeping up with the display in the measured run. The
first five-second attempt is excluded as inconclusive: the stimulus expired
after warmup, causing a capture wait timeout.

The saved five-second log predates a diagnostic-label fix: its successful
`nvenc_register_opengl_texture` lines print the imported source texture ID,
although `resourceToRegister` points to the GL-owned copy texture. The updated
probe logs both `registered_texture` and `source_texture` so future runs show
the NVENC input name explicitly. This logging defect did not change which
texture the API received.

## CUDA/OpenGL registration boundary probe

`--cuda-gl-only` reuses this probe's negotiated GBM capture pool and EGL/GL
import path, captures one ready frame, and stops capture before testing the
imported `GL_TEXTURE_2D`. It dynamically loads the CUDA Driver API, asks
`cuGLGetDevices` for the CUDA device associated with the current EGL OpenGL
context, and fails closed unless there is exactly one. It creates a CUDA
context on that device, registers the imported texture with
`cuGraphicsGLRegisterImage` and `CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY`, maps
the resource, obtains mip 0/layer 0 as a `CUarray`, and checks its descriptor
against the captured dimensions and four 8-bit unsigned channels. CUDA error
code, name, and message are logged. The probe performs no pixel access or
readback and does not initialize NVENC.

Before registering the imported texture, the probe runs owned-texture controls
in the same GL/CUDA contexts: a sized `GL_RGBA8` texture and an unsized
`GL_RGBA` texture, each with null pixel data. It checks the current EGL context
and checks/clears GL errors around every control call. The second control
matches the imported EGLImage texture's reported `GL_TEXTURE_INTERNAL_FORMAT`
(`GL_RGBA`, `0x1908` on the tested driver), while the first uses the sized
format recommended for CUDA interop. These controls separate context/setup and
format acceptance from the imported-storage result. The captured frame lease
remains held through control and imported CUDA unmap/unregister, context
destruction, and imported GL texture/EGLImage teardown. An unconfirmed CUDA
cleanup or GL completion fail-stops while retaining the source lease.

NVIDIA documents that the graphics API must not access a resource while CUDA
has it mapped, and that `cuGraphicsMapResources` orders prior graphics calls
before later CUDA work. This path issues no GL calls while mapped. A successful
descriptor check proves that this imported texture can be represented as a
CUDA array; it does not prove NVENC CUDAARRAY acceptance or absence of internal
driver staging.

Run in the active Wayland session with a changing image visible on the
selected output; pixels are not inspected:

```sh
timeout --signal=TERM --kill-after=2s 20s \
  build-root/tools/fluxrec-nvenc-egl-gl-probe \
  --output DP-1 --capture-wait-ms 5000 --pool-slots 3 --cuda-gl-only
```

Exit status 0 means imported texture registration, mapping, descriptor checks,
and cleanup passed. Status 2 means the imported path failed but the GL-owned
control passed. Status 1 means the control also failed or setup/cleanup could
not be verified.

### Live desktop result

The one-frame run on 2026-09-28 used the RTX 3090 Ti with driver 615.71.09,
DP-1 at 2560×1440, DRM fourcc AR24 (`0x34325241`), and modifier
`0x0300000000606010`. Both owned controls ran first in the same current EGL GL
context and CUDA device context. The `GL_RGBA8` control (`internal_format`
`0x8058`) and the unsized `GL_RGBA` control (`0x1908`) each registered,
mapped, exposed a 2560×1440 CUDA array with format `0x1` and four channels,
then unmapped and unregistered successfully. The imported DMA-BUF texture also
reported unsized `GL_RGBA` (`0x1908`), so the successful matching unsized
control rules out that reported internal format alone as the cause.

`cuGraphicsGLRegisterImage` on the EGLImage-backed capture texture failed with
code 1, `CUDA_ERROR_INVALID_VALUE` (`invalid argument`). The command exited 2,
the expected result when the imported texture fails and an owned control
passes. Teardown observed and drained one pending GL error (`0x500`,
`GL_INVALID_ENUM`); `glFinish` and imported texture deletion then passed, and
the lease was released after the EGLImage was destroyed. This closes the
tested CUDA/OpenGL alias for AR24 with modifier `0x0300000000606010` on this
RTX 3090 Ti / 615.71.09 stack. It does not rule out other modifiers, format
negotiations, devices, or driver versions. The captured log is
[`cuda-gl-import-controlled-final-20260928.log`](../.validation/cuda-gl-import-controlled-final-20260928.log).

References:

- [NVENC Video Encoder API Programming Guide 13.1: OpenGL sessions and external texture inputs](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html)
- [CUDA Driver API 13.4: OpenGL interoperability](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__GL.html)
- [CUDA Driver API 13.4: graphics resource map/unmap synchronization and array access](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__GRAPHICS.html)
- [`nvEncodeAPI.h` 13.1.15.0, OpenGL device/resource declarations](nvenc_input_audit/deps/nv-codec-headers-13.1.15.0/include/ffnvcodec/nvEncodeAPI.h)
- [Khronos EGL_EXT_image_dma_buf_import](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import.txt)
- [Khronos EGL_EXT_image_dma_buf_import_modifiers](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import_modifiers.txt)
- [Khronos GL_OES_EGL_image](https://registry.khronos.org/OpenGL/extensions/OES/OES_EGL_image.txt)

## CPU-only build

This target compiles without initializing the capture session, EGL, OpenGL, or
NVENC:

```sh
cmake -S tools -B .validation/nvenc-egl-gl-probe-build -DCMAKE_BUILD_TYPE=Release
cmake --build .validation/nvenc-egl-gl-probe-build --target fluxrec-nvenc-egl-gl-probe -j2
```

## Bounded live commands

Run in the active Wayland session with the numbered stimulus visible on DP-1.
The one-frame command below writes a raw H.264 elementary stream and CSV
sidecar. Both paths are exclusive and must not already exist:

```sh
timeout --signal=TERM --kill-after=2s 20s \
  .validation/nvenc-egl-gl-probe-build/fluxrec-nvenc-egl-gl-probe \
  --output DP-1 --capture-wait-ms 3000 --pool-slots 8 \
  --output-file /tmp/fluxrec-egl-gl-oneframe.h264
```

To test NVENC registration with GL-owned storage instead of the imported
DMA-BUF texture, add `--gl-owned-control` and choose a new output path:

```sh
timeout --signal=TERM --kill-after=2s 20s \
  .validation/nvenc-egl-gl-probe-build/fluxrec-nvenc-egl-gl-probe \
  --output DP-1 --capture-wait-ms 3000 --pool-slots 8 --gl-owned-control \
  --output-file /tmp/fluxrec-egl-gl-owned-control.h264
```

The control is single-frame only. It logs the texture name, dimensions,
internal format, min/mag filters, wrap modes, GL error, and NVENC's
`nvEncGetLastErrorString` detail when resource registration fails.

To encode through the GPU-copy route for one frame, or for a bounded five
seconds (maximum 1800 frames), use:

```sh
timeout --signal=TERM --kill-after=2s 20s \
  .validation/nvenc-egl-gl-probe-build/fluxrec-nvenc-egl-gl-probe \
  --output DP-1 --capture-wait-ms 3000 --pool-slots 8 --gl-copy-from-import \
  --output-file /tmp/fluxrec-egl-gl-copy-oneframe.h264

timeout --signal=TERM --kill-after=2s 20s \
  .validation/nvenc-egl-gl-probe-build/fluxrec-nvenc-egl-gl-probe \
  --output DP-1 --capture-wait-ms 3000 --duration-ms 5000 \
  --max-frames 1800 --pool-slots 8 --gl-copy-from-import \
  --output-file /tmp/fluxrec-egl-gl-copy-multiframe.h264
```

To check reuse of cached texture/NVENC registrations over several captured
frames, run the five-second mode (maximum 1800 frames). The initial frame
performs capture, import, registration, and one encode before the measured
five-second window; later frames use a bounded eight-slot capture pool:

```sh
timeout --signal=TERM --kill-after=2s 20s \
  .validation/nvenc-egl-gl-probe-build/fluxrec-nvenc-egl-gl-probe \
  --output DP-1 --capture-wait-ms 3000 --duration-ms 5000 \
  --max-frames 1800 --pool-slots 8 \
  --output-file /tmp/fluxrec-egl-gl-multiframe.h264
```

The CSV associates each encoded frame with the capture source sequence,
presentation timestamp, pool slot, NVENC output timestamp, byte count, and
picture type. Summary output includes encoded FPS, source-sequence gaps,
timestamp mismatches, and capture drop/starvation counters. The active capture
lease stays held through successful bitstream lock/unlock and NVENC unmap;
only then is the buffer returned to Wayland. Textures and NVENC registrations
remain cached per pool slot until the session is destroyed.

Reported setup failures clean up output files and unwind the capture and
graphics objects. If NVENC accepts an input but does not confirm completion
with a successful bitstream lock, the probe terminates immediately while the
capture lease and GL/NVENC resources are still retained. This avoids returning
a potentially in-flight source buffer to Wayland. That fail-stop path and an
external hard timeout may leave partial output files; remove those before
retrying. The live tool is intentionally bounded, but a driver call may still
block until the external timeout terminates the process.
