# Isolated Vulkan filter wait ablation

This diagnostic FFmpeg overlay can skip the host fence wait after one named
simple Vulkan filter while retaining the wait after the other filter. It also
reports the host duration of waits when explicitly enabled. The default
`FLUXREC_FF_VK_WAIT_SIMPLE=1` behavior remains unchanged unless
`FLUXREC_FF_VK_SKIP_WAIT_FILTER` exactly matches a filter context name.

The experiment built a separate runtime and recorder at:

- FFmpeg source: `.validation/ffmpeg-stage2-wait-ablation-20260928/source`
- private libraries: `.validation/ffmpeg-stage2-wait-ablation-20260928/lib`
- recorder: `.validation/ffmpeg-stage2-wait-ablation-20260928/recorder-build/engine/fluxrec-record`
- patch: `patches/ffmpeg-n9.0.2-vulkan-filter-wait-stage-ablation.patch`

The repo-default FFmpeg runtime and production recorder settings were not
changed. The overlay was built from the already-patched
`.deps/src/ffmpeg-9.0.2-filterwait` source and uses its private `libavutil`.
The isolated `libavfilter` and `libswscale` resolve against the overlay; the
recorder loader check passed.

## Controls and diagnostics

`FLUXREC_FF_VK_SKIP_WAIT_FILTER=gpu_output_resize` skips only the wait after
the second scale. The wait after `gpu_rgb_to_nv12` remains enabled because the
recorder selects `FLUXREC_FF_VK_WAIT_SIMPLE=1` for NVENC. The filter logs one
`FLUXREC_FF_VK_WAIT` line per stage showing `action=wait` or `action=skip`.

`FLUXREC_FF_VK_WAIT_TIMING=1` measures only the host time inside each executed
`ff_vk_exec_wait()`. `FLUXREC_FF_VK_WAIT_TIMING` lines report the mean, minimum,
and maximum microseconds for batches of 360 waits; filter teardown prints any
remaining partial batch. The skipped stage reports its action but has no wait
duration sample. With neither opt-in variable set, the patch emits no new log
lines and preserves the existing wait behavior.

## Bounded numbered-source run

This matches the previously clean depth-four H.264 baseline: a ten-second
fullscreen plasma window, eight seconds recorded, 48-slot pool, depth four,
H.264 NVENC P1, 50 Mbps request, and the VLC entropy coder.

```sh
python3 tools/controlled_benchmark.py \
  --display DP-1 \
  --stimulus-seconds 10 \
  --record-seconds 8 \
  --pattern plasma \
  --motion 0.20 \
  --codec h264 \
  --backend nvenc \
  --nvenc-preset p1 \
  --h264-coder vlc \
  --conversion two-pass \
  --pool 48 \
  --async-depth 4 \
  --gop 120 \
  --bitrate-mbps 50 \
  --audio off \
  --env FLUXREC_FF_VK_SKIP_WAIT_FILTER=gpu_output_resize \
  --env FLUXREC_FF_VK_WAIT_TIMING=1 \
  --decode-threshold 93 \
  --recorder-binary .validation/ffmpeg-stage2-wait-ablation-20260928/recorder-build/engine/fluxrec-record \
  --output .validation/ffmpeg-stage2-wait-ablation-20260928/h264-nvenc-depth4-skip-outputwait-plasma020-8s.mkv
```

The harness uses its default `build-root/tools/fluxrec-decode-stimulus-ids`
offline decoder and checks that presentation feedback and active-workspace
validity hold through the run. Compare its summary against the matched control
with both waits retained:
`.validation/ffmpeg-stage2-wait-ablation-20260928/h264-nvenc-depth4-bothwait-plasma020-8s.mkv.summary.json`.
The ablation passes this short correctness gate only if every decoded frame
contains a consecutive source ID, with zero black, invalid, duplicate,
backward, or missing frames; recorder counts match; and there are no internal
drops or pool starvation. Follow a clean short run with a 60-second locked
source soak before considering any default change.

## Observed outcome

The live tests below used the isolated recorder and runtime, a valid locked
DP-1 plasma source at about 360 presented frames per second, and the same
depth-four H.264 NVENC configuration. All three recorder runs captured,
submitted, encoded, and muxed 2,881 frames, with zero capture drops, packet
drops, or pool-starvation periods. The numbered-source decoder found:

| Wait configuration | Decoded frames | Valid source IDs | Black frames | Invalid spans | Pool high-water | Result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Both waits retained, timing enabled | 2,881 | 2,881 | 0 | 0 | 14/48 | Pass |
| Skip `gpu_output_resize` wait | 2,881 | 2,880 | 1 | 1 | 19/48 | Fail |
| Skip `gpu_rgb_to_nv12` wait | 2,881 | 2,794 | 87 | 87 | 13/48 | Fail |

The clean control decoded one consecutive source-ID segment (IDs 251–3131).
Skipping only the second-filter wait produced one full black frame and one
unknown source-ID transition across it. Skipping only the first-filter wait
produced 87 black frames and 87 unknown transitions across invalid spans.
Therefore the wait-timing instrumentation and isolated overlay passed the
control, but neither single-wait removal preserved correctness in this run.
Keep both waits enabled in production.

The current recorder enables `FLUXREC_FF_VK_WAIT_SIMPLE=1` for NVENC at
`engine/record.cpp:490-509`. FFmpeg's simple scale helper submits each Vulkan
compute dispatch and then conditionally calls `ff_vk_exec_wait()` at
`libavfilter/vulkan_filter.c:360-367`; `ff_vk_exec_wait()` blocks in
`WaitForFences` before releasing execution dependencies at
`libavutil/vulkan.c:578-583`. If the immediate wait is absent, the execution
pool still waits on that fence when reusing the slot
(`libavutil/vulkan.c:585-615`). Thus the ablation removes the post-submit host
stall, not all synchronization. It also moves dependency release from just
after submission to later fence completion/slot reuse.

The emitted `FLUXREC_FF_VK_WAIT` markers confirm the requested ablations were
active: the stage being tested reported `action=skip`, and the other reported
`action=wait`. The matching `bothwait` control reported `action=wait` for both
stages. Artifacts and full diagnostics are in
`.validation/ffmpeg-stage2-wait-ablation-20260928/`:

- `h264-nvenc-depth4-bothwait-plasma020-8s.mkv.summary.json`
- `h264-nvenc-depth4-skip-outputwait-plasma020-8s.mkv.summary.json`
- `h264-nvenc-depth4-skip-rgbwait-plasma020-8s.mkv.summary.json`
- The corresponding `.record.stderr.log` files contain activation markers,
  wait timings, and final recorder counters.

With both waits enabled, the logged 360-sample batch means ranged from
131–153 µs for `gpu_rgb_to_nv12` and 92–108 µs for `gpu_output_resize`, or
about 0.24 ms per frame combined. With only the `gpu_rgb_to_nv12` wait
retained, its measured batch means ranged from 138–184 µs; with only the
`gpu_output_resize` wait retained, its means ranged from 126–164 µs. These are
host wall durations inside `ff_vk_exec_wait()` under these runs. They are not
end-to-end frame latency, GPU execution time, or a prediction of throughput
gain from removing a wait.
