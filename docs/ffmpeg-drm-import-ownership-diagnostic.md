# FFmpeg DRM PRIME import ownership diagnostic

This is an isolated source patch proposal for FFmpeg 9.0.2. It is not applied
by the production FFmpeg builder and has not been tested on the GPU.

## Source finding

`libavutil/hwcontext_vulkan.c` initializes imported `AVVkFrame` images with
`queue_family = VK_QUEUE_FAMILY_EXTERNAL`. In the DMA-BUF sync-file path,
`vulkan_map_from_drm_frame_sync()` waits on the exported producer sync file and
records a barrier, but passes `VK_QUEUE_FAMILY_IGNORED` as the destination
family when `p->nb_img_qfs > 1`. `ff_vk_exec_submit()` then stores that ignored
destination in `AVVkFrame.queue_family`.

The fallback path exposed a second source bug: `prepare_frame()` skips the host
transition for external import/export modes while leaving `err` initialized to
zero, then returns that zero before calling `switch_layout()`. This made the
fallback DRM acquire and unmap release no-ops. The same early return also
skips some Vulkan/CUDA external transitions. This diagnostic patch works
around that bug at the DRM mapper's fallback acquire and unmap release sites
by invoking `switch_layout()` directly; it leaves generic Vulkan/CUDA callers
of `prepare_frame()` unchanged.

The Vulkan specification says external-memory ownership must be transferred
explicitly regardless of the image's sharing mode. An acquire uses the
external family as `srcQueueFamilyIndex` and the destination Vulkan family as
`dstQueueFamilyIndex`; a release uses the reverse pair. See the Khronos
[Vulkan synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
and the [external-resource-sharing section](https://docs.vulkan.org/spec/latest/chapters/resources.html).

## Proposed change

Apply
[`ffmpeg-n9.0.2-vulkan-drm-import-ownership-diagnostic.patch`](../patches/ffmpeg-n9.0.2-vulkan-drm-import-ownership-diagnostic.patch)
after the existing BGRA-to-B8 patch. It makes the import barrier target the
actual queue family used by the import submit: `exec->qf` in both the
DMA-BUF sync-file path and the DRM fallback's direct
`switch_layout()` call. That helper uses its execution context's family; the
same external-import destination-family fix may also be useful to the separate
Vulkan/CUDA interop probe. The submitted barrier can then acquire from
`VK_QUEUE_FAMILY_EXTERNAL` and retain the actual local family in frame state.

On DRM unmap, it queues `PREP_MODE_EXTERNAL_EXPORT` on the compute execution
pool, calling `switch_layout()` directly to avoid the `prepare_frame()` early
return. FFmpeg's frame timeline dependency orders that release after prior
work; the existing frame-free path waits for the release signal before
destroying the imported image. If concurrent-image barriers have reset the
public family field to `VK_QUEUE_FAMILY_IGNORED`, the diagnostic restores the
compute family for this release.

This patch assumes the imported capture image is accessed only by the Vulkan
compute queue family, as in Fluxrec's direct converter and `scale_vulkan`
source path. It is not a general FFmpeg fix for imported images that may be
used from graphics, transfer, or video queue families. It also adds a release
submission at unmap, so it is diagnostic code rather than a throughput
optimization.

The change moves the acquire into FFmpeg's producer-sync import submission and
adds the matching release at unmap. It is distinct from the prior direct-path
experiment that inserted an external acquire/release after `av_hwframe_map()`;
that experiment still showed periodic black frames. The patch has no live
result yet and should not be taken as evidence that ownership caused those
frames.

## Compile check

The patch was applied after the BGRA-to-B8 patch to the SHA-256-verified
FFmpeg 9.0.2 archive (`8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`).
In an isolated `/tmp/ffmpeg-drm-ownership-final-drm-check2` tree, the following
configured and compiled successfully, including the corrected
`libavutil/hwcontext_vulkan.o` and `libavutil/libavutil.so.61`:

```sh
./configure \
  --enable-shared --disable-static \
  --disable-programs --disable-doc \
  --enable-vulkan --enable-libdrm
make -j4 libavutil/libavutil.so
```

No production `.deps` libraries, builder scripts, or GPU state were changed.

## GPU validation still required

Build a separate private FFmpeg stack with both patches, then compare the
current direct-conversion baseline and this patch under the same controlled
source, pool depth, encoder depth, and duration. Decode the output and check
the full frame-ID sequence, black/invalid frames, duplicates, backward IDs,
and misses. Preserve Vulkan queue-family and semaphore traces around import,
conversion, release, and slot reuse. A successful compile alone does not
prove the external acquire/release matches the compositor's DMA-BUF lifecycle,
and the existing post-map acquire failure remains an unresolved counterpoint.
