#!/usr/bin/env python3
"""Fail-closed verifier for one opt-in Fluxrec GPU-route recording."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
import tempfile


MARKER = "FLUXREC_ZERO_CPU_AUDIT"
AV_NOPTS_VALUE = -(1 << 63)
RUN_ID_RE = re.compile(r"^[A-Za-z0-9_.-]{1,80}$")
RECORDER_TOTALS_RE = re.compile(
    r"(?m)^captured=(?P<captured>\d+)\b.*?\bsubmitted=(?P<submitted>\d+)\b"
    r".*?\bencoded=(?P<encoded>\d+)\b"
)


def parse_markers(log_text: str) -> list[dict[str, str]]:
    events: list[dict[str, str]] = []
    for line in log_text.splitlines():
        if MARKER not in line:
            continue
        payload = line.split(MARKER, 1)[1].strip()
        fields: dict[str, str] = {}
        for token in payload.split():
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        if "event" in fields:
            events.append(fields)
    return events


def validate(recording: Path, log_path: Path, report_path: Path, run_id: str) -> list[str]:
    errors: list[str] = []
    if not RUN_ID_RE.fullmatch(run_id):
        return ["run ID must contain 1–80 letters, digits, dots, underscores, or hyphens"]

    for artifact in (recording, log_path, report_path):
        if not artifact.is_file():
            errors.append(f"artifact is missing: {artifact}")
        elif run_id not in artifact.name:
            errors.append(f"artifact filename does not include run ID {run_id!r}: {artifact.name}")
    if errors:
        return errors
    if recording.stat().st_size == 0:
        errors.append("recording output is empty")

    try:
        log_text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        return [f"could not read FFmpeg log: {error}"]
    events = parse_markers(log_text)
    if not events:
        return errors + ["no FFmpeg route audit markers were found"]
    if any(event["event"].endswith("rejected") for event in events):
        errors.append("the FFmpeg route audit logged a rejected frame/copy path")

    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return errors + [f"could not read NVENC proxy report: {error}"]
    if report.get("schema") != "fluxrec.nvenc_input_audit.v1":
        errors.append("NVENC report schema is not fluxrec.nvenc_input_audit.v1")
    if report.get("status") != "pass" or report.get("pass") is not True:
        errors.append("NVENC proxy did not pass its CUDA-device-pointer input audit")
    if report.get("audit_run_id") != run_id:
        errors.append("NVENC report run ID does not match this recording")
    try:
        recorder_pid = int(report["process_pid"])
    except (KeyError, TypeError, ValueError):
        recorder_pid = -1
        errors.append("NVENC report has no valid recorder process ID")
    if recorder_pid <= 0:
        errors.append("NVENC report recorder process ID must be positive")

    counts = report.get("counts", {})
    if report.get("cuda_device_pointer_input_observed") is not True:
        errors.append("NVENC proxy did not observe CUDA device-pointer input")
    if report.get("cuda_device_pointer_picture_input_observed") is not True:
        errors.append("NVENC proxy did not verify CUDA pointers on successful picture submissions")
    if report.get("non_cuda_or_unmapped_picture_input_observed") is not False:
        errors.append("NVENC proxy observed or could not exclude a non-CUDA picture submission")
    if report.get("software_input_branch_observed") is not False:
        errors.append("NVENC proxy observed or could not exclude the software input branch")
    if not isinstance(counts, dict) or counts.get("cuda_device_ptr_registrations", 0) < 1:
        errors.append("NVENC proxy did not observe a CUDA device-pointer resource registration")
    elif counts.get("non_cuda_resource_registrations", 0) != 0:
        errors.append("NVENC proxy observed a non-CUDA resource registration")
    if isinstance(counts, dict) and (
        counts.get("create_input_buffer_calls", 0) != 0
        or counts.get("lock_input_buffer_calls", 0) != 0
    ):
        errors.append("NVENC proxy observed software input-buffer allocation or locking")

    picture_count_fields = (
        "successful_picture_submissions",
        "successful_cuda_device_ptr_picture_submissions",
        "successful_non_cuda_or_unmapped_picture_submissions",
    )
    picture_counts: dict[str, int] = {}
    for field in picture_count_fields:
        value = counts.get(field) if isinstance(counts, dict) else None
        if type(value) is not int or value < 0:
            errors.append(f"NVENC proxy report has no valid per-picture {field} count")
        else:
            picture_counts[field] = value
    if picture_counts.get("successful_picture_submissions", 0) <= 0:
        errors.append("NVENC proxy observed no successful picture submissions")
    if ("successful_picture_submissions" in picture_counts and
            "successful_cuda_device_ptr_picture_submissions" in picture_counts and
            picture_counts["successful_cuda_device_ptr_picture_submissions"] !=
            picture_counts["successful_picture_submissions"]):
        errors.append("not every successful NVENC picture submission used a CUDA device pointer")
    if picture_counts.get("successful_non_cuda_or_unmapped_picture_submissions", 0) != 0:
        errors.append("NVENC proxy observed a non-CUDA or unmapped successful picture submission")

    for event in events:
        if event.get("audit_run_id") != run_id:
            errors.append(f"FFmpeg event {event['event']} has a mismatched audit run ID")
        try:
            event_pid = int(event["process_pid"])
        except (KeyError, ValueError):
            errors.append(f"FFmpeg event {event['event']} has no valid process ID")
            continue
        if event_pid != recorder_pid:
            errors.append(f"FFmpeg event {event['event']} came from a different process than NVENC")

    def integer_field(event: dict[str, str], key: str, label: str) -> int | None:
        try:
            value = int(event[key])
        except (KeyError, TypeError, ValueError):
            errors.append(f"{label} has no valid {key} count")
            return None
        if value < 0:
            errors.append(f"{label} has a negative {key} count")
            return None
        return value

    totals_match = RECORDER_TOTALS_RE.search(log_text)
    if not totals_match:
        errors.append("recorder did not emit captured/submitted/encoded totals")
        expected_frames = None
    else:
        submitted = int(totals_match.group("submitted"))
        encoded = int(totals_match.group("encoded"))
        if submitted <= 0:
            errors.append("recorder submitted no video frames")
        if encoded != submitted:
            errors.append(
                f"recorder encoded {encoded} frames for {submitted} submitted frames"
            )
        expected_frames = submitted

    picture_submissions = picture_counts.get("successful_picture_submissions")
    cuda_picture_submissions = picture_counts.get(
        "successful_cuda_device_ptr_picture_submissions"
    )
    if expected_frames is not None and picture_submissions is not None:
        if picture_submissions != expected_frames:
            errors.append(
                f"NVENC accepted {picture_submissions} picture submissions for "
                f"{expected_frames} recorder-submitted video frames"
            )
    if (expected_frames is not None and cuda_picture_submissions is not None and
            cuda_picture_submissions != expected_frames):
        errors.append(
            f"NVENC submitted {cuda_picture_submissions} CUDA-backed pictures for "
            f"{expected_frames} recorder-submitted video frames"
        )

    imports = [event for event in events if event["event"] == "dmabuf_vulkan_import"]
    if not any(
        event.get("status") == "ok"
        and event.get("source") == "AV_PIX_FMT_DRM_PRIME"
        and event.get("destination") == "AV_PIX_FMT_VULKAN"
        and event.get("api") == "VkImportMemoryFdInfoKHR"
        and event.get("handle") == "DMA_BUF"
        for event in imports
    ):
        errors.append("successful direct DMA-BUF to Vulkan external-memory import was not observed")

    live_imports = [
        event for event in events if event["event"] == "dmabuf_vulkan_live_import"
    ]

    def is_live_import_with_pts(event: dict[str, str]) -> bool:
        try:
            pts = int(event["pts"])
        except (KeyError, ValueError):
            return False
        return (
            event.get("status") == "ok"
            and event.get("source") == "AV_PIX_FMT_DRM_PRIME"
            and event.get("destination") == "AV_PIX_FMT_VULKAN"
            and pts != AV_NOPTS_VALUE
        )

    if not any(is_live_import_with_pts(event) for event in live_imports):
        errors.append("successful live DMA-BUF import with a valid frame PTS was not observed")

    scales = [event for event in events if event["event"] == "scale_vulkan_submit"]
    expected_scales = {
        "gpu_rgb_to_nv12": ("bgra", "nv12"),
        "gpu_output_resize": ("nv12", "nv12"),
    }
    for stage, (input_format, output_format) in expected_scales.items():
        if not any(
            event.get("status") == "ok"
            and event.get("stage") == stage
            and event.get("source") == "AV_PIX_FMT_VULKAN"
            and event.get("destination") == "AV_PIX_FMT_VULKAN"
            and event.get("source_sw_format") == input_format
            and event.get("destination_sw_format") == output_format
            and event.get("operation") == "vkCmdDispatch+ff_vk_exec_submit"
            and event.get("cpu_fallback") == "disabled"
            for event in scales
        ):
            errors.append(f"successful GPU-only Vulkan scale stage {stage} was not observed")

    copies = [event for event in events if event["event"] == "vulkan_to_cuda_copy_submit"]
    if not any(
        event.get("status") == "ok"
        and event.get("source") == "AV_PIX_FMT_VULKAN"
        and event.get("destination") == "AV_PIX_FMT_CUDA"
        and event.get("source_sw_format") == "nv12"
        and event.get("destination_sw_format") == "nv12"
        and event.get("src_memory_type") == "CU_MEMORYTYPE_ARRAY"
        and event.get("dst_memory_type") == "CU_MEMORYTYPE_DEVICE"
        and event.get("planes") == "2"
        and event.get("path") == "CUDA_stream"
        for event in copies
    ):
        errors.append("successful Vulkan array to CUDA device-pointer copy was not observed")

    # First-success markers establish route identity. The process summary
    # aggregates successful API submissions even when a frames context stays
    # referenced until process exit.
    process_summaries = [
        event for event in events if event["event"] == "process_frame_route_summary"
    ]
    if len(process_summaries) != 1:
        errors.append(
            f"found {len(process_summaries)} process frame-route summaries; expected one"
        )
    else:
        event = process_summaries[0]
        dmabuf_frames = integer_field(
            event, "dmabuf_vulkan_frames", "FFmpeg process frame-route summary"
        )
        cuda_frames = integer_field(
            event, "vulkan_cuda_frames", "FFmpeg process frame-route summary"
        )
        if expected_frames is not None:
            if dmabuf_frames is not None and dmabuf_frames != expected_frames:
                errors.append(
                    f"direct DMA-BUF import covered {dmabuf_frames} live frames; "
                    f"recorder submitted {expected_frames}"
                )
            if cuda_frames is not None and cuda_frames != expected_frames:
                errors.append(
                    f"Vulkan-to-CUDA transfer covered {cuda_frames} live frames; "
                    f"recorder submitted {expected_frames}"
                )

    summaries = [event for event in events if event["event"] == "frame_route_summary"]
    scale_summaries: dict[str | None, list[dict[str, str]]] = {}
    for event in summaries:
        if event.get("route") == "scale_vulkan":
            scale_summaries.setdefault(event.get("stage"), []).append(event)
        else:
            errors.append(
                f"FFmpeg frame-route summary has an unrecognized route kind {event.get('route')!r}"
            )
    for stage in scale_summaries:
        if stage not in expected_scales:
            errors.append(f"aggregate summary contains unexpected scale stage {stage!r}")
    for stage in expected_scales:
        stage_events = scale_summaries.get(stage, [])
        if not stage_events:
            errors.append(f"aggregate per-frame summary for scale stage {stage} is missing")
            continue
        if len(stage_events) != 1:
            errors.append(
                f"scale stage {stage} emitted {len(stage_events)} aggregate summaries; expected one"
            )
            continue
        event = stage_events[0]
        count = integer_field(
            event, "successful_dispatches", f"scale stage {stage} summary"
        )
        rejected = integer_field(
            event, "rejected_frames", f"scale stage {stage} summary"
        )
        if rejected is not None and rejected != 0:
            errors.append(
                f"scale stage {stage} rejected {rejected} frame routes"
            )
        if count is not None and count <= 0:
            errors.append(f"scale stage {stage} submitted no Vulkan frames")
        if count is not None and expected_frames is not None and count != expected_frames:
            errors.append(
                f"scale stage {stage} covered {count} frames; "
                f"recorder submitted {expected_frames}"
            )

    return errors


def self_test() -> int:
    base_event = {
        "status": "ok",
        "process_pid": "4123",
        "audit_run_id": "test-001",
    }
    events = [
        base_event | {
            "event": "dmabuf_vulkan_import",
            "source": "AV_PIX_FMT_DRM_PRIME",
            "destination": "AV_PIX_FMT_VULKAN",
            "api": "VkImportMemoryFdInfoKHR",
            "handle": "DMA_BUF",
        },
        base_event | {
            "event": "dmabuf_vulkan_live_import",
            "pts": "0",
            "source": "AV_PIX_FMT_DRM_PRIME",
            "destination": "AV_PIX_FMT_VULKAN",
        },
        base_event | {
            "event": "scale_vulkan_submit",
            "stage": "gpu_rgb_to_nv12",
            "source": "AV_PIX_FMT_VULKAN",
            "destination": "AV_PIX_FMT_VULKAN",
            "source_sw_format": "bgra",
            "destination_sw_format": "nv12",
            "operation": "vkCmdDispatch+ff_vk_exec_submit",
            "cpu_fallback": "disabled",
        },
        base_event | {
            "event": "scale_vulkan_submit",
            "stage": "gpu_output_resize",
            "source": "AV_PIX_FMT_VULKAN",
            "destination": "AV_PIX_FMT_VULKAN",
            "source_sw_format": "nv12",
            "destination_sw_format": "nv12",
            "operation": "vkCmdDispatch+ff_vk_exec_submit",
            "cpu_fallback": "disabled",
        },
        base_event | {
            "event": "vulkan_to_cuda_copy_submit",
            "source": "AV_PIX_FMT_VULKAN",
            "destination": "AV_PIX_FMT_CUDA",
            "source_sw_format": "nv12",
            "destination_sw_format": "nv12",
            "src_memory_type": "CU_MEMORYTYPE_ARRAY",
            "dst_memory_type": "CU_MEMORYTYPE_DEVICE",
            "planes": "2",
            "path": "CUDA_stream",
        },
        base_event | {
            "event": "process_frame_route_summary",
            "dmabuf_vulkan_frames": "10",
            "vulkan_cuda_frames": "10",
        },
        base_event | {
            "event": "frame_route_summary",
            "route": "scale_vulkan",
            "stage": "gpu_rgb_to_nv12",
            "successful_dispatches": "10",
            "rejected_frames": "0",
        },
        base_event | {
            "event": "frame_route_summary",
            "route": "scale_vulkan",
            "stage": "gpu_output_resize",
            "successful_dispatches": "10",
            "rejected_frames": "0",
        },
    ]
    lines = [f"prefix {MARKER} " + " ".join(f"{key}={value}" for key, value in event.items())
             for event in events]
    report = {
        "schema": "fluxrec.nvenc_input_audit.v1",
        "status": "pass",
        "pass": True,
        "process_pid": 4123,
        "audit_run_id": "test-001",
        "cuda_device_pointer_input_observed": True,
        "cuda_device_pointer_picture_input_observed": True,
        "non_cuda_or_unmapped_picture_input_observed": False,
        "software_input_branch_observed": False,
        "counts": {
            "cuda_device_ptr_registrations": 1,
            "non_cuda_resource_registrations": 0,
            "create_input_buffer_calls": 0,
            "lock_input_buffer_calls": 0,
            "successful_picture_submissions": 10,
            "successful_cuda_device_ptr_picture_submissions": 10,
            "successful_non_cuda_or_unmapped_picture_submissions": 0,
        },
    }

    recorder_totals = (
        "captured=10 capture_admitted=10 capture_rate_limited=0 "
        "distinct_presentation_times=10 submitted=10 encoded=10\n"
    )

    def write_log(path: Path, rows: list[dict[str, str]]) -> None:
        path.write_text("\n".join(
            f"prefix {MARKER} " + " ".join(f"{key}={value}" for key, value in row.items())
            for row in rows
        ) + "\n" + recorder_totals, encoding="utf-8")

    with tempfile.TemporaryDirectory(prefix="fluxrec-zero-cpu-audit-test-") as directory:
        folder = Path(directory)
        recording = folder / "recording-test-001.mkv"
        log_path = folder / "ffmpeg-test-001.log"
        report_path = folder / "nvenc-test-001.json"
        recording.write_bytes(b"fixture")
        write_log(log_path, events)
        report_path.write_text(json.dumps(report), encoding="utf-8")
        fixture_errors = validate(recording, log_path, report_path, "test-001")
        if fixture_errors:
            print("self-test failed: complete correlated route fixture was rejected: " +
                  "; ".join(fixture_errors), file=sys.stderr)
            return 1

        no_live_pts = [event for event in events
                       if event.get("event") != "dmabuf_vulkan_live_import"]
        write_log(log_path, no_live_pts)
        if not any("valid frame PTS was not observed" in error for error in validate(
            recording, log_path, report_path, "test-001")):
            print("self-test failed: live DMA-BUF import without PTS evidence was accepted",
                  file=sys.stderr)
            return 1

        no_process_summary = [event for event in events
                              if event.get("event") != "process_frame_route_summary"]
        write_log(log_path, no_process_summary)
        if not any("process frame-route summaries; expected one" in error for error in validate(
            recording, log_path, report_path, "test-001")):
            print("self-test failed: missing process route summary was accepted", file=sys.stderr)
            return 1

        missing_stage = [event for event in events if event.get("stage") != "gpu_output_resize"]
        write_log(log_path, missing_stage)
        if not any("gpu_output_resize" in error for error in validate(
            recording, log_path, report_path, "test-001")):
            print("self-test failed: missing scale stage was not rejected", file=sys.stderr)
            return 1

        write_log(log_path, events)
        short_route = [dict(event) for event in events]
        next(row for row in short_route
             if row.get("route") == "scale_vulkan" and
             row.get("stage") == "gpu_output_resize")["successful_dispatches"] = "9"
        write_log(log_path, short_route)
        if not any("covered 9 frames" in error for error in validate(
            recording, log_path, report_path, "test-001")):
            print("self-test failed: incomplete per-frame route count was not rejected", file=sys.stderr)
            return 1

        write_log(log_path, events)
        short_import = [dict(event) for event in events]
        next(row for row in short_import
             if row.get("event") == "process_frame_route_summary")["dmabuf_vulkan_frames"] = "9"
        write_log(log_path, short_import)
        if not any("direct DMA-BUF import covered 9 live frames" in error for error in validate(
            recording, log_path, report_path, "test-001")):
            print("self-test failed: incomplete live import count was not rejected", file=sys.stderr)
            return 1

        write_log(log_path, events)
        report["counts"]["successful_cuda_device_ptr_picture_submissions"] = 9
        report_path.write_text(json.dumps(report), encoding="utf-8")
        if not any("NVENC submitted 9 CUDA-backed pictures" in error for error in validate(
            recording, log_path, report_path, "test-001")):
            print("self-test failed: incomplete NVENC picture count was not rejected", file=sys.stderr)
            return 1
        report["counts"]["successful_cuda_device_ptr_picture_submissions"] = 10
        report_path.write_text(json.dumps(report), encoding="utf-8")

        write_log(log_path, events)
        wrong_pid = [dict(event) for event in events]
        wrong_pid[0]["process_pid"] = "9001"
        write_log(log_path, wrong_pid)
        if not any("different process" in error for error in validate(
            recording, log_path, report_path, "test-001")):
            print("self-test failed: cross-process FFmpeg marker was not rejected", file=sys.stderr)
            return 1

        write_log(log_path, events)
        report["software_input_branch_observed"] = True
        report_path.write_text(json.dumps(report), encoding="utf-8")
        if not any("software input branch" in error for error in validate(
            recording, log_path, report_path, "test-001")):
            print("self-test failed: NVENC software input route was not rejected", file=sys.stderr)
            return 1

    print(
        "FFmpeg route audit verifier self-test: positive fixture and eight fail-closed cases passed; "
        "aggregate counts cover successful API submissions, not driver-internal copies or PTS identity"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("self-test")
    verify = subparsers.add_parser("verify-recording")
    verify.add_argument("--recording", type=Path, required=True)
    verify.add_argument("--ffmpeg-log", type=Path, required=True)
    verify.add_argument("--nvenc-report", type=Path, required=True)
    verify.add_argument("--run-id", required=True)
    args = parser.parse_args()
    if args.command == "self-test":
        return self_test()

    errors = validate(args.recording, args.ffmpeg_log, args.nvenc_report, args.run_id)
    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"PASS: per-stage successful API frame counts and successful CUDA-backed NVENC picture submissions matched the recorder's submitted/encoded totals in process {json.loads(args.nvenc_report.read_text(encoding='utf-8'))['process_pid']} for run {args.run_id}.")
    print("Scope: counts cover successful API submissions for this session; they do not correlate exact PTS identities, prove GPU completion per frame, or reveal proprietary driver-internal staging.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
