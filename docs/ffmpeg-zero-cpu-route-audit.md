# Isolated FFmpeg zero-CPU-pixel route audit

This opt-in diagnostic instruments private FFmpeg 9.0.2's runtime paths for direct DRM PRIME/DMA-BUF to Vulkan import, both `scale_vulkan` compute stages, and Vulkan-to-CUDA copy descriptors. It does not read pixel values or change the default private FFmpeg runtime. Build and run it with an isolated overlay and recorder; the normal `.deps/ffmpeg-bgra-b8/lib` stays untouched.

## Build

With the normal private FFmpeg sources and dependencies prepared, build the patched overlay and recorder:

```sh
FLUXREC_BUILD_JOBS=4 tools/build_ffmpeg_zero_cpu_audit.sh all
python3 tools/nvenc_input_audit.py build
python3 tools/ffmpeg_zero_cpu_audit.py self-test
```

The FFmpeg overlay is staged under `.validation/ffmpeg-zero-cpu-audit/lib`. The alternate recorder is `.validation/ffmpeg-zero-cpu-audit/recorder-build/engine/fluxrec-record`, with an RPATH to that overlay. `scripts/check-ffmpeg-runtime.sh` checks loader resolution and dependencies after each build. The system `libavcodec` remains loaded from `/usr/lib`; the NVENC proxy forwards its calls to the real NVIDIA library.

## Run and verify

Use a unique run ID in every artifact name and in `FLUXREC_ZERO_CPU_AUDIT_RUN_ID`. Prefix the benchmark's recorder environment with the diagnostic switches below. The benchmark keeps its normal stimulus/workload checks, writes the FFmpeg log beside the MKV, and decodes the numbered source frames offline after recording.

```sh
run_id=route-audit-$(date -u +%Y%m%dT%H%M%SZ)
repo="$PWD"
proxy_dir="$repo/.validation/nvenc-input-audit"
report="$proxy_dir/zero-cpu-route-$run_id.json"
recording="$repo/.validation/fluxrec-zero-cpu-route-audit-$run_id.mkv"
real_nvenc="$(readlink -f /usr/lib/libnvidia-encode.so.1)"

python3 tools/controlled_benchmark.py \
  --stimulus-seconds 14 --record-seconds 10 --pattern plasma --motion 1.0 \
  --codec h264 --conversion two-pass --backend nvenc --nvenc-preset p1 \
  --bitrate-mbps 50 --gop 120 --pool 48 --output "$recording" \
  --recorder-binary "$repo/.validation/ffmpeg-zero-cpu-audit/recorder-build/engine/fluxrec-record" \
  --env FLUXREC_FF_ZERO_CPU_AUDIT=1 \
  --env "FLUXREC_ZERO_CPU_AUDIT_RUN_ID=$run_id" \
  --env "FLUXREC_NVENC_AUDIT_REAL_LIBRARY=$real_nvenc" \
  --env "FLUXREC_NVENC_AUDIT_JSON=$report" \
  --env "LD_LIBRARY_PATH=$proxy_dir"

python3 tools/ffmpeg_zero_cpu_audit.py verify-recording \
  --recording "$recording" \
  --ffmpeg-log "$recording.record.stderr.log" \
  --nvenc-report "$report" \
  --run-id "$run_id"
python3 tools/nvenc_input_audit.py check-report "$report"
```

The verifier requires the direct import marker, both named scale stages, a successful two-plane `CU_MEMORYTYPE_ARRAY` to `CU_MEMORYTYPE_DEVICE` transfer, and a passing NVENC CUDA-device-pointer report. It also requires aggregate per-frame counts for live DMA-BUF imports, each Vulkan compute stage, and Vulkan-to-CUDA transfers to match the recorder's submitted and encoded video totals. At the NVENC boundary, it requires the successful `nvEncEncodePicture` count and the CUDA-backed picture count to match the recorder's submitted frames, with every successful picture using a mapped CUDA device pointer. FFmpeg summaries and the NVENC report are correlated by run ID and recorder PID. The verifier fails on rejected-route markers, missing or unequal stage counts, a software NVENC input branch, non-CUDA/unmapped picture submissions, or cross-process evidence. The self-test includes a complete fixture and fail-closed cases for missing stages, incomplete FFmpeg and NVENC counts, process mismatch, and a software input branch.

## Evidence scope

The route identity markers remain first-success samples per frame context or filter instance. Separate 64-bit counters account for each live DMA-BUF import after its Vulkan map/synchronization succeeds, each `scale_vulkan` dispatch after `ff_vk_filter_process_simple` returns, and each Vulkan-to-CUDA frame after all plane copies, the external semaphore signal, and the final Vulkan handoff succeed. These counters are reported once at frame-context/filter teardown; there are no per-frame log lines or allocations. Live import accounting uses the capture PTS, so the one-time startup import probe with `AV_NOPTS_VALUE` is excluded. Audited `scale_vulkan` frames fail closed on every call if the CPU scaler or a non-Vulkan frame/context appears.

The verifier compares these per-session successful API-operation totals with the recorder's submitted and encoded frame totals. The NVENC proxy additionally wraps `nvEncMapInputResource` and `nvEncEncodePicture`, verifies the mapped registered resource behind each successful picture submission is CUDA-backed, excludes EOS from picture counts, and reconciles both picture totals with recorder submissions. This is aggregate count evidence, not exact PTS-set correlation: equal counts cannot rule out a hypothetical drop paired with a duplicate. It also reports successful API submissions, not GPU completion for each frame. `cpu_mapping=none_in_importer` describes the inspected FFmpeg importer source path; neither it nor the counters observe ordinary loads through an existing CPU mapping or proprietary driver-internal staging. The probe reads no raw pixel values. Pair it with source-ID decode and DMA-BUF syscall tracing for independent evidence at those boundaries. Even together, these checks do not prove absolute driver-internal zero copy.

The production NVENC path also has a post-transfer guard in `gpu/gpu.cpp:1683-1696` that rejects a result unless it is `AV_PIX_FMT_CUDA` with a CUDA hardware-frame context and NV12 storage.

## Validated run

On 2026-09-28, run ID `route-audit-20260928a` passed the route verifier and NVENC proxy checker. The fullscreen 2560×1440 plasma source presented **5,037 consecutive IDs** at 359.719 FPS over about 14 seconds. The H.264 NVENC P1 two-pass run captured, submitted, encoded, and muxed **3,601 frames**; offline decode recovered IDs **263–3,863 consecutively**, with zero black, invalid, duplicate, backward, or missing frames. Internal drops, packet loss, and pool starvation were zero; pool high-water was 20/48. Mean steady encoded telemetry was about 361.3 FPS; this short-window rate is subject to sample-boundary effects.

The same recorder PID (205883) emitted successful FFmpeg markers for DMA-BUF import, `gpu_rgb_to_nv12` BGRA→NV12, `gpu_output_resize` NV12→NV12, and Vulkan ARRAY→CUDA DEVICE transfer. The proxy observed four CUDADEVICEPTR registrations, zero non-CUDA registrations, zero software input-buffer create/lock calls, and one successful encoder initialization. Registrations are per CUDA surface, not per encoded frame. This recorded run predates the aggregate FFmpeg counters and per-picture NVENC proxy checks, so it does not validate the strengthened verifier; rerun the audit with the updated build to collect that evidence.

Artifacts: `.validation/fluxrec-zero-cpu-route-audit-route-audit-20260928a.mkv` with `.record.stderr.log`, `.summary.json`, and `.decoder.log` sidecars; NVENC report `.validation/nvenc-input-audit/zero-cpu-route-route-audit-20260928a.json`.

### Strict aggregate-count run

Run ID `route-audit-20260928c` passed the strengthened verifier and NVENC report check for recorder PID 248649. The controlled fullscreen source presented **5,026 consecutive IDs** at 358.931 FPS. The H.264 NVENC run captured, submitted, encoded, and muxed **3,601 frames**; offline decode found **3,601 consecutive, nonblack IDs, 296–3,896**, with no invalid, duplicate, backward, or missing frames. Capture drops, packet loss, and pool starvation were zero; pool high-water was 15/48.

The FFmpeg process summary counted **3,601 live DMA-BUF imports**, **3,601 successful dispatches in each named Vulkan scale stage**, and **3,601 successful Vulkan-to-CUDA transfers**. The NVENC proxy counted **3,601 successful picture submissions backed by mapped CUDA device pointers**, with zero non-CUDA/unmapped submissions and zero software input-buffer create/lock calls. The route and proxy reports shared the same PID and run ID, and each aggregate matched the recorder's 3,601 submitted/encoded frames. This is per-run successful API-operation count evidence. Equal aggregate totals do not establish identical PTS sets across stages, GPU completion for each frame, or driver-internal behavior.

The preceding run `route-audit-20260928b` recorded 3,600 clean decoded IDs and a passing NVENC input report, but the strict route verifier rejected its diagnostics: teardown summaries split the hwframe counts across contexts, including zero-count context records, and no live-import-with-PTS marker was emitted. The revised process-wide teardown accounting and live import marker were exercised by run `20260928c`, whose aggregate counts reconciled. Artifacts: `.validation/fluxrec-zero-cpu-route-audit-route-audit-20260928c.mkv` with `.record.stderr.log`, `.summary.json`, and `.decoder.log` sidecars; NVENC report `.validation/nvenc-input-audit/zero-cpu-route-route-audit-20260928c.json`. Run `20260928b` artifacts are saved under the matching `.validation/` prefix.
