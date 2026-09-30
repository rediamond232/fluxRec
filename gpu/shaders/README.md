# XRGB/ARGB to NV12 compute shader

`convert_xrgb_to_nv12.comp` converts one 2560×1440 DRM XR24/AR24 capture
image directly into NV12 planes. The shader has no scaling or CPU pixel path.
Its push-constant extent makes the kernel reusable at other even dimensions;
the capture pipeline should enforce that source and destination dimensions
remain identical.

## Pixel and color assumptions

- On the little-endian Linux targets, DRM XR24/AR24 bytes are B, G, R, X/A.
  Bind a sampled image view whose `.rgb` sampling result is semantic R, G, B;
  the shader ignores alpha. A raw DRM view using
  `VK_FORMAT_B8G8R8A8_UNORM` normally uses identity component mapping. The
  installed FFmpeg import path can instead describe those BGRA bytes as
  `VK_FORMAT_R8G8B8A8_UNORM` / `AV_PIX_FMT_BGRA`; for that view, set
  `VkComponentMapping` to `{B, G, R, A}` so sampling returns semantic RGB.
  Do not use an sRGB Vulkan view: the conversion matrix operates on the
  nonlinear R′G′B′ sample values without an automatic sRGB decode.
- Input values are SDR, nonlinear BT.709-compatible R′G′B′ in [0, 1]. The
  kernel does not linearize, tone-map, or convert HDR/PQ/HLG input.
- Output uses BT.709 matrix coefficients and 8-bit limited range. Luma is
  quantized to 16–235; Cb and Cr to 16–240, with neutral chroma at 128.
- Each invocation reads one exact 2×2 texel block, stores all four Y samples,
  and averages the four unquantized Cb/Cr values into one chroma sample. The
  interleaved chroma plane stores Cb/U in byte 0 and Cr/V in byte 1, matching
  NV12. Width and height must both be even.

## Vulkan descriptor and dispatch contract

| Descriptor | Resource | Vulkan view format |
| --- | --- | --- |
| set 0, binding 0 | sampled capture image with semantic RGB `.rgb` | `VK_FORMAT_B8G8R8A8_UNORM` or `VK_FORMAT_R8G8B8A8_UNORM` with the mapping above |
| set 0, binding 1 | write-only full-size Y plane | `VK_FORMAT_R8_UNORM` |
| set 0, binding 2 | write-only half-size interleaved CbCr plane | `VK_FORMAT_R8G8_UNORM` |

The 8-byte push-constant block is `uvec2 sourceExtent` (width, height). The
local size is 16×16 invocations, with one invocation per 2×2 source block.
For 2560×1440 dispatch `(80, 45, 1)`; in general dispatch
`(ceil(width / 32), ceil(height / 32), 1)`.

FFmpeg exposes an NV12 Vulkan pool as `AV_PIX_FMT_VULKAN` with
`AVHWFramesContext.sw_format = AV_PIX_FMT_NV12`. Inspect its
`AVVulkanFramesContext.format[]` and the allocated frame's `AVVkFrame.img[]`:

- When `format[1] != VK_FORMAT_UNDEFINED`, bind storage views for the Y and
  CbCr images directly (`R8_UNORM` and `R8G8_UNORM`). The shader writes into
  FFmpeg's encode input frame, avoiding an intermediate pixel copy.
- When `format[1] == VK_FORMAT_UNDEFINED`, the frame uses a single multiplanar
  NV12 image. Create plane-0 (`R8_UNORM`) and plane-1 (`R8G8_UNORM`) views and
  bind them only if the image was created with storage usage and the device
  reports the necessary storage-image support for those views. Otherwise,
  dispatch to separate `R8_UNORM`/`R8G8_UNORM` storage images, then issue GPU
  `vkCmdCopyImage` copies into the corresponding destination plane aspects.
  This keeps all raw pixels on the GPU and accommodates Vulkan Video images
  that permit transfer destination usage but not storage usage.

Keep FFmpeg's `AVVkFrame` locking, access/layout state, queue-family ownership,
and per-image timeline semaphore values consistent with the actual dispatch
and copy submissions. The compute writes must become visible to the next
encode read (`VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR`); use the layout and
queue-family transitions required by the queried encode profile and FFmpeg
frame context. Do not submit the frame to the encoder before that dependency
is established.

## Compile

From the repository root:

```sh
mkdir -p build/gpu/shaders
glslangValidator -V --target-env vulkan1.2 -S comp \
  -o build/gpu/shaders/convert_xrgb_to_nv12.comp.spv \
  gpu/shaders/convert_xrgb_to_nv12.comp
spirv-val --target-env vulkan1.2 \
  build/gpu/shaders/convert_xrgb_to_nv12.comp.spv
```

Equivalent CMake rule:

```cmake
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
find_program(SPIRV_VAL spirv-val REQUIRED)
set(NV12_SHADER "${CMAKE_CURRENT_BINARY_DIR}/convert_xrgb_to_nv12.comp.spv")
add_custom_command(
  OUTPUT "${NV12_SHADER}"
  COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 -S comp
          -o "${NV12_SHADER}"
          "${CMAKE_CURRENT_SOURCE_DIR}/convert_xrgb_to_nv12.comp"
  COMMAND "${SPIRV_VAL}" --target-env vulkan1.2 "${NV12_SHADER}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/convert_xrgb_to_nv12.comp"
  VERBATIM)
add_custom_target(fluxrec_nv12_shader DEPENDS "${NV12_SHADER}")
```

## Validation guidance

First query the exact image usage and view-format support on the selected
physical device. In particular, verify storage-image support for `R8_UNORM`
and `R8G8_UNORM`; a multi-planar NV12 base format may not itself support
`VK_IMAGE_USAGE_STORAGE_BIT`. For the FFmpeg route, confirm that the selected
`AVVkFrame` formats and usages match the descriptor views, that plane 1 maps
to Cb then Cr for the Vulkan Video encode profile, and that its negotiated
encode source format is NV12.

For an isolated shader qualification run, compare readback results against a
small CPU reference using the same formulas and half-up integer rounding.
The expected single-color codes are black `(Y,U,V) = (16,128,128)`, white
`(235,128,128)`, red `(63,102,240)`, green `(173,42,26)`, and blue
`(32,240,118)`. Also test four different colors in each 2×2 block to verify
that chroma averages the four source samples rather than taking one pixel.
Any readback belongs only in this offline qualification path; normal capture
must not map or download the raw source or converted surfaces.

At runtime, inspect the SPIR-V shader descriptors and push-constant size,
enable Vulkan validation layers, and check that the image barriers cover
compute shader writes before the video encode read. Verify output through an
encoded sample as well: luma/chroma ranges, Cb/Cr ordering, unchanged 2560×1440
dimensions, and no CPU mapping of the frame surfaces.
