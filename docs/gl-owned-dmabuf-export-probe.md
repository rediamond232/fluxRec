# GL-owned texture DMA-BUF export probe

`fluxrec-gl-owned-dmabuf-export-probe` checks whether a GL-owned texture with
the same `GL_RGBA8` allocation form used by the successful NVENC OpenGL
control can be exported as a DMA-BUF whose format and modifier match the
selected output's `ext-image-copy-capture-v1` constraints.

The tool connects to the capture protocol only to read its output dimensions,
DRM device, and advertised fourcc/modifier pairs. It never sets a buffer pool,
starts capture, registers a resource with NVENC, or encodes. The texture is
allocated with null pixel data; the tool has no CPU pixel readback or upload.
It creates an EGLImage from that exact texture, queries
`EGL_MESA_image_dma_buf_export`, exports the plane descriptor(s), prints each
fourcc, modifier, stride, and offset, and closes every exported descriptor
before exit. It does not modify the production recorder.

The default `GL_RGBA8` form is the one used by the prior GL-owned NVENC
registration/encode control. This tool checks EGL export only; it does not
repeat NVENC registration. `--internal-format bgra8` is an optional narrow
probe that first requires the active GL implementation to advertise
`GL_EXT_texture_format_BGRA8888`, then tries `GL_BGRA8_EXT`. The alternate
storage form is not claimed to be NVENC-accepted unless separately tested.
`--internal-format rgb8` tries standard desktop `GL_RGB8` storage as one final
format-mapping candidate; this is also an export query only.

## Build and run

```sh
cmake -S tools -B .validation/gl-owned-export-probe-build -DCMAKE_BUILD_TYPE=Release
cmake --build .validation/gl-owned-export-probe-build \
  --target fluxrec-gl-owned-dmabuf-export-probe -j2
.validation/gl-owned-export-probe-build/fluxrec-gl-owned-dmabuf-export-probe --output DP-1
```

Exit status is 0 when the exported fourcc/modifier exactly matches an
advertised capture tuple, 3 when the query succeeds but no exact tuple matches,
and 1 for setup/export errors. The optional BGRA8 query exits 4 if the required
GL extension is absent.

## Local result, 2026-09-28

On the RTX 3090 Ti (`/dev/dri/renderD128`, NVIDIA GL 4.5), the selected output
was 2560×1440 and advertised 26 format/modifier pairs covering two fourccs:
`AR24` and `XR24`. The GL-owned `GL_RGBA8` texture exported successfully as
one-plane `AB24`, modifier `0x0300000000e08014`, stride 10240, offset 0. The
capture protocol advertised neither `AB24` nor the exact exported tuple, so
this allocation cannot be supplied directly to the current capture pool.

The optional BGRA8 query stopped before texture allocation because the active
driver did not advertise `GL_EXT_texture_format_BGRA8888`. The final `GL_RGB8`
query exported one-plane `XB24`, modifier `0x0300000000e08014`, stride 10240,
offset 0. `XB24` is absent from the capture constraints, including the exact
modifier tuple. Thus neither tested GL-owned export format maps to a DMA-BUF
that the current capture pool accepts. No capture or encoding ran during any
query.
