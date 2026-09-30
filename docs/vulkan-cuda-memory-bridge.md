# Vulkan to CUDA external-memory bridge check

## Local result

The metadata query ran on the NVIDIA GeForce RTX 3090 Ti with driver
615.71.09 (Vulkan device API 1.4.351). DP-1's advertised AR24/XR24 modifier set
was queried with both `VK_FORMAT_B8G8R8A8_UNORM` and
`VK_FORMAT_R8G8B8A8_UNORM`, 2D,
`VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`, exclusive sharing, and
`VK_IMAGE_USAGE_SAMPLED_BIT`. B8 is the Vulkan view format used for the
little-endian AR24 capture bytes; R8 is the alternate FFmpeg storage view with
channel correction. `VkPhysicalDeviceImageFormatInfo2` accepts a Vulkan format
and modifier, not a DRM fourcc; the listed modifier rows therefore describe
each Vulkan view format queried for the advertised AR24/XR24 modifier set. The
device reports `VK_KHR_external_memory_fd`,
`VK_EXT_external_memory_dma_buf`, and `VK_EXT_image_drm_format_modifier`.

| DP-1 advertised modifier | `DMA_BUF`: result; compatible / export-from-import | `OPAQUE_FD`: result; compatible / export-from-import | DMA-BUF import → OPAQUE-FD export |
| --- | --- | --- | --- |
| `DRM_FORMAT_MOD_INVALID` (`0x00ffffffffffffff`) | `VK_ERROR_FORMAT_NOT_SUPPORTED` | `VK_ERROR_FORMAT_NOT_SUPPORTED` | No |
| `0x0300000000606010` | `VK_SUCCESS`; `0x200` / `0x200` | `VK_SUCCESS`; `0x1` / `0x1` | No |
| `0x0300000000606011` | `VK_SUCCESS`; `0x200` / `0x200` | `VK_SUCCESS`; `0x1` / `0x1` | No |
| `0x0300000000606012` | `VK_SUCCESS`; `0x200` / `0x200` | `VK_SUCCESS`; `0x1` / `0x1` | No |
| `0x0300000000606013` | `VK_SUCCESS`; `0x200` / `0x200` | `VK_SUCCESS`; `0x1` / `0x1` | No |
| `0x0300000000606014` | `VK_SUCCESS`; `0x200` / `0x200` | `VK_SUCCESS`; `0x1` / `0x1` | No |
| `0x0300000000606015` | `VK_SUCCESS`; `0x200` / `0x200` | `VK_SUCCESS`; `0x1` / `0x1` | No |
| `0x0300000000e08010` | `VK_ERROR_FORMAT_NOT_SUPPORTED` | `VK_ERROR_FORMAT_NOT_SUPPORTED` | No |
| `0x0300000000e08011` | `VK_ERROR_FORMAT_NOT_SUPPORTED` | `VK_ERROR_FORMAT_NOT_SUPPORTED` | No |
| `0x0300000000e08012` | `VK_ERROR_FORMAT_NOT_SUPPORTED` | `VK_ERROR_FORMAT_NOT_SUPPORTED` | No |
| `0x0300000000e08013` | `VK_ERROR_FORMAT_NOT_SUPPORTED` | `VK_ERROR_FORMAT_NOT_SUPPORTED` | No |
| `0x0300000000e08014` | `VK_ERROR_FORMAT_NOT_SUPPORTED` | `VK_ERROR_FORMAT_NOT_SUPPORTED` | No |
| `0x0300000000e08015` | `VK_ERROR_FORMAT_NOT_SUPPORTED` | `VK_ERROR_FORMAT_NOT_SUPPORTED` | No |

For each successful row, both handle types report `externalMemoryFeatures=0x7`
(importable, exportable, dedicated-only). The hex values in the table are
`compatibleHandleTypes` / `exportFromImportedHandleTypes`; `0x200` is
`DMA_BUF`, and `0x1` is `OPAQUE_FD`. Vulkan defines
`exportFromImportedHandleTypes` as the imported handle types from which the
queried `handleType` can be exported. Thus the decisive OPAQUE-FD query would
need its `exportFromImportedHandleTypes` mask to contain `DMA_BUF` (`0x200`);
none does. The compatible-handle masks for the six supported modifiers are
disjoint as well (their intersection is `0x0`). Therefore no tested tuple
advertises the proposed GBM DMA-BUF import → OPAQUE-FD export → CUDA bridge.

The complete 13-modifier sweep was repeated with
`VK_FORMAT_R8G8B8A8_UNORM`. Every result matches the table row by row: the six
`0x0300000000606010–15` modifiers return DMA-BUF masks `0x200/0x200` and
OPAQUE-FD masks `0x1/0x1`; `DRM_FORMAT_MOD_INVALID` and
`0x0300000000e08010–15` return `VK_ERROR_FORMAT_NOT_SUPPORTED`. Thus the R8
view also has no DMA-BUF import → OPAQUE-FD export path.

This is a Vulkan properties query only. It did not create an image or allocate
memory, import a live capture FD, export a handle, call CUDA/NVENC, capture, or
submit GPU work. `VK_ERROR_FORMAT_NOT_SUPPORTED` means Vulkan provides no
properties for that exact Vulkan-format/modifier/usage/handle query; it is not a
successful query with empty masks. The result does not rule out another
Vulkan format, a different capture-buffer allocator, or an API path outside
these tuples. In particular, a Vulkan-owned opaque-FD image can be exported to
CUDA, but it would be a separate encode target populated from the capture
image by a GPU copy. A CUDA-owned DMA-BUF capture pool is another allocation
design to assess; this query says nothing about whether its layout can be
negotiated with Wayland and the compositor.

## CUDA-owned DMA-BUF export check

CUDA does document exporting a page-aligned address range from `cuMemAlloc()`
or mapped `cuMemAddressReserve()` memory as `CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD`
through `cuMemGetHandleForAddressRange()`, gated by
`CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED`. This is a real alternative API to
investigate on devices that report the attribute as true; CUDA does not promise
the capability on every GPU. On this RTX 3090 Ti, CUDA Driver API 13.4 reports
`DMA_BUF_SUPPORTED=0`, `HOST_ALLOC_DMA_BUF_SUPPORTED=0`, and
`DMA_BUF_MMAP_SUPPORTED=0`. The driver therefore does not advertise a CUDA
device-memory DMA-BUF export route for this machine. The read-only capability
query used no allocations or GPU work; reproduce it with
`python3 tools/cuda_dmabuf_capability_query.py`.

This API exports a `CUdeviceptr` address range, not a CUDA array/image. Even on
a device that supports the export, CUDA specifies no DRM fourcc, plane layout,
stride, or modifier in the returned FD; those would still need a separate
GBM/Wayland compatibility check. NVENC can take a CUDA device pointer, but its
documented pitch-linear input path converts to block-linear internally. This
does not provide the requested same-allocation CUDA-array input with no copy.
The attribute result does not exclude a future driver/GPU combination with
DMA-BUF export or another allocator/API design.

NVIDIA's CUDA reference supports Vulkan external-memory import through
opaque-FD handles and CUDA arrays, while its CUDA external-memory reference
limits direct `DMABUF_FD` import/mipmapped-array mapping to Tegra Thor and later.
The CUDA EGLImage interoperability reference likewise says EGLImage
registration is Tegra-only. Those EGL/CUDA results observed on this desktop
driver remain experimental. CUDA/OpenGL texture registration is a distinct,
documented desktop API, so it was tested on the capture EGLImage's
`GL_TEXTURE_2D` representation. On the RTX 3090 Ti / 615.71.09 stack, both
owned controls (`GL_RGBA8` and unsized `GL_RGBA`) registered, mapped to the
expected CUDA array descriptor, and cleaned up; registration of the imported
AR24 EGLImage-backed texture failed with `CUDA_ERROR_INVALID_VALUE`. This
rejects the tested CUDA/OpenGL alias for the active AR24 modifier, but does
not exclude other modifiers, driver versions, or devices. A GPU copy into an
encoder-accepted owned texture remains a no-CPU-pixel option to measure, but
does not meet the no-copy goal.

### CUDA/OpenGL capture-texture result

The bounded one-frame probe is documented in
[`tools/nvenc_egl_gl_probe.md`](../tools/nvenc_egl_gl_probe.md) and saved at
[`cuda-gl-import-controlled-final-20260928.log`](../.validation/cuda-gl-import-controlled-final-20260928.log).
It used DP-1 at 2560×1440, DRM fourcc AR24 (`0x34325241`), and modifier
`0x0300000000606010`. The probe confirmed the current GL context was associated
with one CUDA device. The owned `GL_RGBA8` control (`0x8058`) and an unsized
`GL_RGBA` control (`0x1908`, matching the imported texture's reported GL
internal format) each passed register, map, CUDA-array descriptor validation
(2560×1440, format `0x1`, four channels), unmap, and unregister. The imported
capture texture failed at `cuGraphicsGLRegisterImage` with CUDA code 1,
`CUDA_ERROR_INVALID_VALUE`; no mapped array was obtained for it. Exit status 2
is the probe's designed “import failed, control passed” result.

After that CUDA failure, teardown found one pending `GL_INVALID_ENUM` (`0x500`)
and drained it. The subsequent `glFinish` and texture deletion succeeded; the
probe completed EGLImage teardown and released the capture lease. The matching
unsized control passing shows that the imported texture's reported
`GL_TEXTURE_INTERNAL_FORMAT=GL_RGBA` alone does not explain the registration
failure. This result closes only the tested CUDA/OpenGL alias for AR24 modifier
`0x0300000000606010` on this machine and driver; alternate modifiers,
allocators, format negotiations, and driver versions remain untested.

## Reproduce the property query

The small standalone source is [tools/vulkan_external_memory_caps.cpp](../tools/vulkan_external_memory_caps.cpp).
It creates a Vulkan instance, enumerates device properties/extensions, and
calls `vkGetPhysicalDeviceImageFormatProperties2` for both handle types on all
13 listed modifiers, for both B8 and R8 Vulkan formats. It does not create a
Vulkan device, allocate an image, or submit GPU work. The captured output is
`.validation/vulkan-external-memory-caps-sweep-b8-r8-20260928.txt`.

```sh
c++ -std=c++20 tools/vulkan_external_memory_caps.cpp -lvulkan -o /tmp/fluxrec-vulkan-external-memory-caps
/tmp/fluxrec-vulkan-external-memory-caps
```

The relevant Vulkan fields are `VkExternalMemoryProperties::compatibleHandleTypes`
and `VkExternalMemoryProperties::exportFromImportedHandleTypes`:
[Vulkan external-memory properties](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalMemoryProperties.html),
[external image-format properties](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalImageFormatProperties.html),
[memory FD import](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryFdInfoKHR.html),
and [memory FD export](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetMemoryFdKHR.html).

CUDA references:
[Vulkan-CUDA interoperability](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/graphics-interop.html),
[CUDA external-resource interoperability](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__EXTRES__INTEROP.html),
[CUDA `cuMemGetHandleForAddressRange`](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__MEM.html),
[CUDA DMA-BUF device attributes](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__TYPES.html),
and [CUDA EGL interoperability](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__EGL.html).
NVENC references:
[CUDA-array input and Linux OpenGL device support](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html).
