# NVENC input residency audit

This opt-in Linux x86_64 proxy observes the NVENC function table used by the recorder. It wraps resource registration, mapping, unmapping, per-picture submission, software input-buffer allocation/locking, and encoder initialization; all calls and caller-owned structures are forwarded unchanged to the system NVIDIA library.

The proxy reports a pass only when the NVENC API function-list version and the per-picture/resource structure layouts match the pinned header, at least one initialization succeeds with validated initialize/config/rate-control layout, no initialization fails or succeeds without a reportable config, and at least one picture is accepted by NVENC with its `inputBuffer` traced through `nvEncMapInputResource` to a successfully registered `CUDADEVICEPTR`. `NV_ENC_SUCCESS` and `NV_ENC_ERR_NEED_MORE_INPUT` count as accepted picture submissions; the latter means NVENC buffered the input for reordering. Flush calls with a null `encodePicParams` pointer, and calls with a null input plus the EOS flag, are excluded. Every non-flush picture input must match an active CUDA mapping, and any software input-buffer call, non-CUDA registration, unknown mapping, structure-version mismatch, or tracking-table overflow fails the report. Initialize and outer config structure versions must match exactly. The embedded `NV_ENC_RC_PARAMS.version` accepts either zero or `NV_ENC_RC_PARAMS_VER`; other values fail the report.

Resource and mapped-input handles are matched in the scope of their NVENC encoder handle. Successful unregister/unmap calls retire the records. Repeated active mappings that return the same mapped pointer are treated as CUDA-backed only if every tracked mapping of that pointer comes from a successful CUDA device-pointer registration. Fixed-size tables avoid allocation in the proxy; a table overflow is reported and fails closed. The prior report fields remain present; `cuda_device_pointer_input_observed` continues to mean that such a registration was observed, while the added per-picture counters and `cuda_device_pointer_picture_input_observed` show whether submitted pictures used those mapped inputs.

For a successful zero-B-frame recorder run, `counts.successful_picture_submissions` should equal the recorder's final `submitted=` frame count: every accepted input frame is observed at `nvEncEncodePicture`, regardless of whether NVENC immediately emits a packet. `NV_ENC_ERR_NEED_MORE_INPUT` still counts because the input was accepted and buffered. Flush calls with a null `encodePicParams` pointer and null-input EOS calls are excluded; a null input without EOS is treated as an unqualified picture input and fails the report. A count mismatch on an otherwise successful zero-B-frame run needs investigation before using the report as a complete per-frame audit.

This zero value is intentional compatibility with FFmpeg's NVENC setup path: FFmpeg 9.0.2 copies the driver's `nvEncGetEncodePresetConfigEx` result into `NV_ENC_CONFIG` and updates the outer config version without assigning `rcParams.version`. The production probe likewise observed zero while the driver returned success from `nvEncInitializeEncoder`. The outer `NV_ENC_CONFIG.version` remains checked before reading the embedded rate-control fields. The initialization record captures the rate-control mode, average/max bitrate, VBV size, constant P/I/B QPs, enabled P/I min/max/initial QP values, target quality and fraction, spatial/temporal AQ flags, multipass, low-delay key-frame scale, frame rate, codec GUID, preset, tune, and dimensions immediately before forwarding `nvEncInitializeEncoder`.

The header is a diagnostic-only copy from [FFmpeg/nv-codec-headers release n13.1.15.0](https://github.com/FFmpeg/nv-codec-headers/releases/tag/n13.1.15.0), pinned at commit `0a6fba9a2820628b8103464f4c8753ee05838baa` (NVENC API 13.1). Its path, hash, and embedded license details are recorded in `deps/nv-codec-headers-13.1.15.0/UPSTREAM.md`. Compile-time assertions pin the function-list version, size, and wrapped callback offsets, as well as the picture, mapping, and registration structure sizes and relevant field offsets. At runtime, a caller with any different `NV_ENCODE_API_FUNCTION_LIST_VER` is rejected with `NV_ENC_ERR_INVALID_VERSION` before the driver fills the table. The initialize/config/rate-control, registration, map, and picture structure versions are checked before those fields are read. The proxy returns a failing report if an ABI check fails.

When `FLUXREC_ZERO_CPU_AUDIT_RUN_ID` is set, the JSON report also contains that `audit_run_id` and the current `process_pid`. The isolated FFmpeg route verifier uses them to require that its first-success route markers and this NVENC report came from the same recorder process and run. The field is optional for standalone proxy use; see [`docs/ffmpeg-zero-cpu-route-audit.md`](../../docs/ffmpeg-zero-cpu-route-audit.md) for the combined audit.

## Build and self-test

From the repository root:

```sh
python3 tools/nvenc_input_audit.py self-test
```

The proxy and fixtures are built under `.validation/nvenc-input-audit/`. The CPU-only fake-driver self-test submits three CUDA-mapped pictures (including one `NV_ENC_ERR_NEED_MORE_INPUT` and one failed submission), checks that only accepted pictures are counted, exercises map/unmap and register/unregister tracking, and verifies that both a DirectX-mapped picture and an `NvEncCreateInputBuffer` picture make the report fail. It also covers a function-list version mismatch, a driver initialization failure, and an invalid RC structure version, and checks that initialization values survive interception and are reported in JSON.

## Audit an existing recorder run

Build the proxy first, then prefix the recorder's normal command with these environment variables. Use the existing short recording command and arguments unchanged:

```sh
audit_dir="$PWD/.validation/nvenc-input-audit"
mkdir -p "$PWD/.validation"
FLUXREC_NVENC_AUDIT_REAL_LIBRARY="$(readlink -f /usr/lib/libnvidia-encode.so.1)" \
FLUXREC_NVENC_AUDIT_JSON="$PWD/.validation/nvenc-input-audit.json" \
LD_LIBRARY_PATH="$audit_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
./build-root/engine/fluxrec-record <existing recorder arguments>
```

The output JSON is written when FFmpeg unloads the NVENC library, which normally happens while the recorder closes its encoder. If the recorder exits before opening NVENC, the proxy cannot produce a useful pass report. Confirm that the JSON exists and has `"status": "pass"`; the `check-report` command performs that check and exits nonzero for a fail or incomplete report:

```sh
python3 tools/nvenc_input_audit.py check-report .validation/nvenc-input-audit.json
```

`check-report` requires the per-picture counters and confirms that all accepted picture submissions were CUDA-mapped inputs. A report from an earlier proxy build that lacks those fields is rejected rather than treated as evidence for the per-picture claim.

If the report is missing, the application did not load the proxy. Check the process's library lookup order and ensure `LD_LIBRARY_PATH` puts the audit directory first. Do not set the real-library variable to the proxy itself.

## What the result proves

A passing report proves that every accepted non-flush `NvEncEncodePicture` call observed by the proxy used an input mapping whose registered resource had been successfully declared to the driver as `NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR`, and that NVENC's software input-buffer create/lock branch was not used. It also records the encoder configuration values passed at the driver API boundary.

It does not prove that the pointer contains valid GPU pixels, that the pointer originated from a particular allocation API, that no CPU copies occurred earlier in the application, or that the driver does not internally stage/copy data. Bitrate fields are the values requested at initialization, not a guarantee of the resulting bitstream rate. Pair this check with the source audit and capture/recording validation for those separate claims.
