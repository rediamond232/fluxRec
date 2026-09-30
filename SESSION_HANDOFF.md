# Fluxrec session handoff — 2026-09-29 (later)

## Objective and current position

Build the native Hyprland/Wayland RTX 3090 Ti recorder described in the original user brief. The primary milestone is genuine near-360 FPS 2560×1440 H.264 NVENC recording with raw pixels kept off the CPU, maximum practical capture/encode overlap, valid MKV output, and source-identity evidence. The full goal also includes HEVC, replay, PipeWire audio, a native Qt UI, and serious telemetry.

**Estimated full-goal progress: about 90%, subjective.** The default Vulkan→CUDA NVENC path is unchanged. The opt-in direct path has source-ID-continuous H.264 and HEVC runs through 60 seconds, plus replay, replay-only, and two-track audio. A 15-second fullscreen `vkgears` (vsync, not mailbox) was captured at the display rate; that demo does not provide source IDs. Qt controls for this backend, Minecraft or another heavy game, and an NVENC lock-bitstream deadline remain open. Do not claim allocation-level zero-copy.

The user authorized fullscreen tests at any time. Keep the stimulus workspace visible throughout a measurement; restore the user's original workspace and any open special workspace afterward. If `special:communication` is shown, close it before launching the stimulus or the window can land there and invalidate the run; restore it afterward. At handoff there is no running `fluxrec-record`, stimulus, or controlled benchmark process. The last observed active workspace was 5. Do not assume that workspace state will stay unchanged.

## What changed this session

Hyprland 0.56.2 copies one `ext-image-copy-capture` frame per session on output commit and ignores later commits while `m_copyInFlight` is set. The protocol forbids a second frame object on that session. That dropped source presentations (`capture_dropped` stayed 0) and, when an extra commit copied an unchanged mirror, produced the 55-byte near-duplicate.

`capture/capture.cpp` now creates three capture sessions. `presentation_time` means the copy has started; the client then arms exactly one idle session. A slot stores its lease sequence, and a release frees the slot only when that sequence still matches. A queue-full drop no longer marks the slot free while the lease object still exists.

`gpu/gpu.cpp` skips FFmpeg Vulkan device creation for `experimental-cuda-egl`. The direct encoder records the CUDA device name via `cuDeviceGetName`. Startup log from `.validation/direct-novulkan-plasma-10s-20260929T131840Z/`:

`EXPERIMENTAL direct CUDA EGL encoder initialized for native 2560x1440 H.264 on NVIDIA GeForce RTX 3090 Ti; FFmpeg Vulkan device was not created`

The default NVENC path still creates the Vulkan device and derives CUDA from it. Do not regress that path.

## Evidence (offline source IDs are authoritative)

Original anomalies, not erased:

- `.validation/direct-recorder-plasma-motion1-60s-20260929T083734Z/`: decoded frame 102 jumps 820→822. Both packets are ~28 KB. PTS steps 5 ms. Distinct pictures.
- `.validation/direct-recorder-replay-plasma-30s-20260929T084230Z/`: packet 7481 is 55 bytes, one non-IDR P-slice, `frame_num` still increments. Decoded pixels match the previous frame except 39 macroblocks. Marker 8186 repeats, then the next frame is 8188. Stimulus reported no forward presentation-ID gap.

After the overlap change:

| Run | Result | Artifact |
| --- | --- | --- |
| 30 s plasma | IDs 723–11521 exact | `.validation/direct-overlap3-plasma-30s-20260929T095245Z/` |
| 60 s plasma | IDs 705–22262 exact, no packet under 8 KB, periodic means ~359.3 FPS, pool 8/16, encoder utilization mean 98.8% | `.validation/direct-overlap3-plasma-60s-20260929T130702Z/` |
| 30 s + replay | primary 711–11489 exact; replay 3351–6291 exact, keyframe start, strict DTS | `.validation/direct-overlap3-replay-plasma-30s-20260929T130905Z/` |
| replay-only, save at 12 s | no primary MKV; replay 1912–4856 exact; captured=encoded=7188 | `.validation/direct-overlap3-replay-only-20s-20260929T131036Z/` |
| 20 s audio + replay | video 705–7890 exact; two AAC tracks; sync_errors=0; desktop digitally silent (idle sink); mic noise floor | `.validation/direct-overlap3-audio-replay-20s-20260929T131211Z/` |
| 8 s with 880 Hz tone | desktop capture linked to the Arctis monitor; 880 Hz band max −45.3 dB vs −78.3 dB at 2000 Hz; mic did not show the same tone | `.validation/direct-audio-tone-link-20260929T131727Z/` |
| 10 s after Vulkan skip | IDs 612–4209 exact; 3.0 ms total pool starvation | `.validation/direct-novulkan-plasma-10s-20260929T131840Z/` |

`ctest --test-dir build-root --output-on-failure -j2` passed 20/20 after these edits. Focused controlled-benchmark Python tests were not re-run this session.

## Direct HEVC and vkgears, same day

HEVC uses the same encoder. Main profile, level 6.1, 8-bit 4:2:0, BT.709 limited, `idrPeriod = gop`, `repeatSPSPPS`. H.264 CAVLC is unchanged. `ctest` after this change: 20/20.

| Run | Result | Artifact |
| --- | --- | --- |
| 5 s color chart | IDs 615–2409 exact; HEVC Main, tv, bt709; bars within 2 code values of pure RGB; IDRs every 120 frames; first AU ends in IDR NAL type 19 | `.validation/direct-hevc-colorchart-5s-20260929T132701Z/` |
| 10 s plasma | IDs 618–4216 exact; encoded mean 359.0 FPS; pool 5/16; encoder utilization mean 75.9% | `.validation/direct-hevc-plasma-10s-20260929T132751Z/` |
| 60 s plasma | IDs 616–22174 exact; captured/encoded mean 359.28 FPS; pool 5/16; utilization mean 75.7%; no starvation | `.validation/direct-hevc-plasma-60s-20260929T132834Z/` |
| 15 s audio + replay | primary 611–6007 exact; replay 1211–4137 exact, keyframe start, strict DTS, HEVC + two AAC; sync_errors=0; desktop sink was idle | `.validation/direct-hevc-audio-replay-15s-20260929T133035Z/` |
| 15 s vkgears, no mailbox | gears reported 358.7–359.6 FPS; recorder captured=encoded=muxed=5388, no drops; no source IDs on this demo | `.validation/direct-vkgears-fifo-15s-20260929T133405Z/` |
| vkgears mailbox, with capture | gears fell to about 1–3 FPS. Do not use that present mode as a headroom result | `.validation/direct-vkgears-15s-20260929T133248Z/` |

## UI and bitstream-lock note

Settings offers `experimental-cuda-egl` as “Direct CUDA EGL · experimental”. Its pool (3–16, default 16) and async depth (1–16, default 12) are saved separately. While it is selected, NVENC preset and B-frames are hidden, constant QP and non-native sizes other than 2560×1440 block saving, and the note says this is not allocation-level zero-copy. The workbench toolbar and signal rail follow the saved backend: Direct CUDA EGL shows WAYLAND › EGL › NVENC › MATROSKA and an explicit unused preview. Launch omits `--conversion`, `--preview-socket`, `--verify-frame-ids`, and `--nvenc-preset`, and passes `--b-frames 0`. `QT_QPA_PLATFORM=offscreen ./build-root/ui/fluxrec-ui --smoke` passed after that change. `UX-CONTRACT.md` matches.

`nvEncLockBitstream` with `doNotWait=1` returned `NV_ENC_SUCCESS` and an empty bitstream on this driver (`.validation/direct-lockpoll-plasma-4s-20260929T134427Z/`, recorder exit 1). That is not a safe deadline. The blocking lock is restored. A following 4-second plasma run decoded IDs 610–2047 with no gaps (`.validation/direct-lockrestore-plasma-4s-20260929T134520Z/`). An external process timeout is still required. Do not destroy or reuse a capture lease if a future timeout fires while NVENC owns the buffer.

## Resume here

1. The authenticated `kinglxss` session was recorded while simulating. Artifact `.validation/direct-minecraft-live-account-20s-20260929T184819Z/`: 7,200 captured=encoded=muxed, distinct presentation times 7,200, no drops or starvation, pool high-water 5/16, encoded mean about 359.6 FPS. Mid-run F3 read 2,374 and 2,847 FPS. The last pause line is 21:47:11 local, before the recorder. Pause-on-lost-focus was toggled off and left off. GPU mean was only 45.7%, so this arena is not a saturated-game headroom test. Minecraft has no source IDs. The earlier paused file `.validation/direct-minecraft-account-20s-20260929T175407Z/` is not this result. Counter-Strike is installed and was not launched. The Minecraft window was fullscreen on workspace 1.
2. A normal capture now keeps one DMA-BUF slot free so the copy-start path can arm the next request. A failed arm increments `capture_unarmed` and is not included in `capture_dropped`. After that change, default pool 16 stayed exact (`.validation/direct-pool16-reserve-plasma-5s-20260929T182306Z/`, IDs 607–2404) and default NVENC stayed exact (`.validation/default-nvenc-reserve-plasma-5s-20260929T182411Z/`, IDs 712–2512). Pools of 4 and 8 still skipped source IDs with `capture_dropped` at 0; the harness now exits 1 on that failure. The direct recorder prints a startup warning when the pool is below 16. The earlier full-pool miss of ID 849 (`.validation/direct-lockdeadline-plasma-5s-20260929T181608Z/`) was before the spare slot.
3. The bitstream lock still has no cancel API. The encoder arms a 5-second deadline and calls `std::_Exit` so destructors do not unmap a lease NVENC may still own. That path was not hit on a happy-path plasma run. Do not retry `doNotWait`. Asynchronous completion events are not supported on Linux.

Build: `cmake --build build-root -j2`. CTest is 20/20. `PYTHONPATH=tools python3 -m unittest tools.tests.test_controlled_benchmark tools.tests.test_ui_freeze_benchmark` passed 70 tests on 2026-09-29. For another controlled test, use a unique output path, an external timeout, and `python3 tools/controlled_benchmark.py --backend experimental-cuda-egl ...` from the repo root. This Hyprland version accepts Lua dispatch such as `hyprctl eval 'hl.dispatch(hl.dsp.focus({ workspace = "4" }))'`.
