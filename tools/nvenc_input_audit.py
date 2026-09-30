#!/usr/bin/env python3
"""Build and exercise the opt-in NVENC function-table audit proxy."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "tools" / "nvenc_input_audit"
HEADER_DIR = ROOT / "tools" / "nvenc_input_audit" / "deps" / "nv-codec-headers-13.1.15.0" / "include"
DEFAULT_BUILD_DIR = ROOT / ".validation" / "nvenc-input-audit"
PROXY_NAME = "libnvidia-encode.so.1"


def run(command: list[str], *, env: dict[str, str] | None = None) -> None:
    subprocess.run(command, check=True, env=env)


def compile_outputs(build_dir: Path) -> tuple[Path, Path, Path]:
    compiler = os.environ.get("CC") or shutil.which("cc")
    if not compiler:
        raise RuntimeError("A C compiler was not found (set CC or install cc).")
    if not (HEADER_DIR / "ffnvcodec" / "nvEncodeAPI.h").is_file():
        raise RuntimeError(f"Pinned NVENC header is missing: {HEADER_DIR}")

    build_dir.mkdir(parents=True, exist_ok=True)
    fake_dir = build_dir / "fake-driver"
    fake_dir.mkdir(parents=True, exist_ok=True)
    proxy = build_dir / PROXY_NAME
    fake_driver = fake_dir / PROXY_NAME
    selftest = build_dir / "proxy-selftest"
    common = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{HEADER_DIR}"]

    run(common + ["-fPIC", "-shared", str(SOURCE_DIR / "nvenc_input_audit_proxy.c"),
                  "-o", str(proxy), "-ldl", "-pthread"])
    run(common + ["-fPIC", "-shared", str(SOURCE_DIR / "fake_nvenc_driver.c"),
                  "-Wl,-soname,libnvidia-encode.so.1", "-o", str(fake_driver)])
    run(common + [str(SOURCE_DIR / "proxy_selftest.c"), "-o", str(selftest), "-ldl"])
    return proxy, fake_driver, selftest


def self_test(build_dir: Path, fake_driver: Path, test_program: Path) -> None:
    cases = {
        "cuda": lambda report: (
            report["status"] == "pass"
            and report["pass"] is True
            and report["cuda_device_pointer_input_observed"] is True
            and report["cuda_device_pointer_picture_input_observed"] is True
            and report["non_cuda_or_unmapped_picture_input_observed"] is False
            and report["software_input_branch_observed"] is False
            and report["counts"]["cuda_device_ptr_registrations"] == 1
            and report["counts"]["register_resource_successes"] == 1
            and report["counts"]["cuda_device_ptr_maps"] == 1
            and report["counts"]["map_input_resource_successes"] == 1
            and report["counts"]["encode_picture_calls"] == 5
            and report["counts"]["picture_input_attempts"] == 3
            and report["counts"]["successful_picture_submissions"] == 2
            and report["counts"]["successful_cuda_device_ptr_picture_submissions"] == 2
            and report["counts"]["successful_non_cuda_or_unmapped_picture_submissions"] == 0
            and report["counts"]["non_cuda_or_unmapped_picture_inputs"] == 0
            and report["counts"]["encode_picture_failures"] == 1
            and report["counts"]["unmap_input_resource_successes"] == 1
            and report["counts"]["unregister_resource_successes"] == 1
            and report["counts"]["create_input_buffer_calls"] == 0
            and report["counts"]["lock_input_buffer_calls"] == 0
            and report["encoder_initializations"][0]["codec"] == "H264"
            and report["encoder_initializations"][0]["preset"] == "P4"
            and report["encoder_initializations"][0]["tuning"] == "ultra_low_latency"
            and report["encoder_initializations"][0]["rate_control_mode"] == "CBR"
            and report["encoder_initializations"][0]["average_bit_rate"] == 50_000_000
            and report["encoder_initializations"][0]["max_bit_rate"] == 60_000_000
            and report["encoder_initializations"][0]["vbv_buffer_size"] == 40_000_000
            and report["encoder_initializations"][0]["rc_params_version"] == "0x00000000"
            and report["encoder_initializations"][0]["rc_params_version_valid"] is True
            and report["encoder_initializations"][0]["rate_control_details"]["valid"] is True
            and report["encoder_initializations"][0]["rate_control_details"]["const_qp_p"] == 37
            and report["encoder_initializations"][0]["rate_control_details"]["const_qp_b"] == 39
            and report["encoder_initializations"][0]["rate_control_details"]["const_qp_i"] == 35
            and report["encoder_initializations"][0]["rate_control_details"]["enable_min_qp"] == 1
            and report["encoder_initializations"][0]["rate_control_details"]["min_qp_p"] == 18
            and report["encoder_initializations"][0]["rate_control_details"]["min_qp_i"] == 20
            and report["encoder_initializations"][0]["rate_control_details"]["enable_max_qp"] == 1
            and report["encoder_initializations"][0]["rate_control_details"]["max_qp_p"] == 44
            and report["encoder_initializations"][0]["rate_control_details"]["max_qp_i"] == 42
            and report["encoder_initializations"][0]["rate_control_details"]["enable_initial_rc_qp"] == 1
            and report["encoder_initializations"][0]["rate_control_details"]["initial_rc_qp_p"] == 28
            and report["encoder_initializations"][0]["rate_control_details"]["initial_rc_qp_i"] == 24
            and report["encoder_initializations"][0]["rate_control_details"]["target_quality"] == 33
            and report["encoder_initializations"][0]["rate_control_details"]["target_quality_lsb"] == 128
            and report["encoder_initializations"][0]["rate_control_details"]["enable_aq"] == 1
            and report["encoder_initializations"][0]["rate_control_details"]["enable_temporal_aq"] == 1
            and report["encoder_initializations"][0]["rate_control_details"]["multi_pass_mode"] == "two_pass_full_resolution"
            and report["encoder_initializations"][0]["rate_control_details"]["low_delay_key_frame_scale"] == 2
        ),
        "software": lambda report: (
            report["status"] == "fail"
            and report["pass"] is False
            and report["software_input_branch_observed"] is True
            and report["cuda_device_pointer_picture_input_observed"] is False
            and report["non_cuda_or_unmapped_picture_input_observed"] is True
            and report["counts"]["non_cuda_resource_registrations"] == 1
            and report["counts"]["non_cuda_maps"] == 1
            and report["counts"]["successful_picture_submissions"] == 2
            and report["counts"]["successful_cuda_device_ptr_picture_submissions"] == 0
            and report["counts"]["successful_non_cuda_or_unmapped_picture_submissions"] == 2
            and report["counts"]["non_cuda_or_unmapped_picture_inputs"] == 2
            and report["counts"]["encode_picture_calls"] == 4
            and report["counts"]["create_input_buffer_calls"] == 1
            and report["counts"]["lock_input_buffer_calls"] == 1
            and report["counts"]["valid_initialize_successes"] == 1
            and report["encoder_initializations"][0]["rc_params_version_valid"] is True
            and report["encoder_initializations"][0]["rate_control_details"]["valid"] is True
        ),
        "abi-mismatch": lambda report: (
            report["status"] == "fail"
            and report["pass"] is False
            and report["layout_guard"] == "failed"
            and report["counts"]["function_list_version_mismatches"] == 1
            and report["counts"]["api_create_successes"] == 0
        ),
        "init-failure": lambda report: (
            report["status"] == "fail"
            and report["pass"] is False
            and report["counts"]["initialize_encoder_calls"] == 1
            and report["counts"]["initialize_encoder_successes"] == 0
            and report["counts"]["initialize_encoder_failures"] == 1
            and report["counts"]["valid_initialize_successes"] == 0
        ),
        "init-version-mismatch": lambda report: (
            report["status"] == "fail"
            and report["pass"] is False
            and report["counts"]["initialize_encoder_successes"] == 1
            and report["counts"]["initialize_parameter_version_mismatches"] == 1
            and report["counts"]["valid_initialize_successes"] == 0
            and report["encoder_initializations"][0]["rc_params_version_valid"] is False
            and report["encoder_initializations"][0]["rate_control_details"]["valid"] is False
        ),
    }

    for mode, predicate in cases.items():
        report_path = build_dir / f"selftest-{mode}.json"
        try:
            report_path.unlink()
        except FileNotFoundError:
            pass
        env = os.environ.copy()
        old_library_path = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = str(build_dir) + (os.pathsep + old_library_path if old_library_path else "")
        env["FLUXREC_NVENC_AUDIT_REAL_LIBRARY"] = str(fake_driver.resolve())
        env["FLUXREC_NVENC_AUDIT_JSON"] = str(report_path)
        env["FLUXREC_ZERO_CPU_AUDIT_RUN_ID"] = f"nvenc-proxy-selftest-{mode}"
        if mode == "init-failure":
            env["NVENC_AUDIT_SELFTEST_INIT_FAIL"] = "1"
        else:
            env.pop("NVENC_AUDIT_SELFTEST_INIT_FAIL", None)
        run([str(test_program), mode], env=env)
        try:
            report = json.loads(report_path.read_text(encoding="utf-8"))
        except (FileNotFoundError, json.JSONDecodeError) as error:
            raise RuntimeError(f"{mode} self-test did not produce valid JSON: {error}") from error
        if not predicate(report):
            raise RuntimeError(f"{mode} self-test report failed assertions:\n{json.dumps(report, indent=2)}")
        if report.get("process_pid", 0) <= 0 or report.get("audit_run_id") != env["FLUXREC_ZERO_CPU_AUDIT_RUN_ID"]:
            raise RuntimeError(f"{mode} self-test report is missing process/run correlation fields")
        print(f"self-test {mode}: passed (report {report_path})")


def check_report(report_path: Path) -> int:
    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"nvenc input audit report could not be read: {error}", file=sys.stderr)
        return 1
    if report.get("schema") != "fluxrec.nvenc_input_audit.v1":
        print("unrecognized NVENC input audit report schema", file=sys.stderr)
        return 1
    print(f"NVENC input audit: {report.get('status', 'unknown')}")
    counts = report.get("counts")
    per_picture_fields = (
        "register_resource_successes",
        "cuda_device_ptr_maps",
        "successful_picture_submissions",
        "successful_cuda_device_ptr_picture_submissions",
        "successful_non_cuda_or_unmapped_picture_submissions",
        "non_cuda_or_unmapped_picture_inputs",
        "encode_picture_parameter_version_mismatches",
        "resource_tracking_overflows",
        "mapping_tracking_overflows",
        "tracking_lock_failures",
        "non_cuda_resource_registrations",
        "create_input_buffer_calls",
        "lock_input_buffer_calls",
    )
    missing_per_picture_fields = (
        [field for field in per_picture_fields if not isinstance(counts, dict) or field not in counts]
    )
    if missing_per_picture_fields:
        print("NVENC input audit report lacks per-picture input-path counters: " +
              ", ".join(missing_per_picture_fields), file=sys.stderr)
        return 1
    invalid_count_fields = [field for field in per_picture_fields if type(counts[field]) is not int]
    if invalid_count_fields:
        print("NVENC input audit report has invalid per-picture counters: " +
              ", ".join(invalid_count_fields), file=sys.stderr)
        return 1
    if report.get("status") != "pass" or report.get("pass") is not True:
        print(json.dumps(report, indent=2), file=sys.stderr)
        return 1
    if (
        report.get("cuda_device_pointer_picture_input_observed") is not True
        or report.get("non_cuda_or_unmapped_picture_input_observed") is not False
        or report.get("software_input_branch_observed") is not False
        or
        counts["register_resource_successes"] <= 0
        or counts["cuda_device_ptr_maps"] <= 0
        or counts["successful_picture_submissions"] <= 0
        or counts["successful_cuda_device_ptr_picture_submissions"] != counts["successful_picture_submissions"]
        or counts["successful_non_cuda_or_unmapped_picture_submissions"] != 0
        or counts["non_cuda_or_unmapped_picture_inputs"] != 0
        or counts["encode_picture_parameter_version_mismatches"] != 0
        or counts["resource_tracking_overflows"] != 0
        or counts["mapping_tracking_overflows"] != 0
        or counts["tracking_lock_failures"] != 0
        or counts["non_cuda_resource_registrations"] != 0
        or counts["create_input_buffer_calls"] != 0
        or counts["lock_input_buffer_calls"] != 0
    ):
        print("NVENC input audit report does not verify every successful picture's CUDA input path",
              file=sys.stderr)
        print(json.dumps(report, indent=2), file=sys.stderr)
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("build", "self-test", "check-report"), nargs="?", default="self-test")
    parser.add_argument("report", nargs="?", type=Path)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    args = parser.parse_args()
    if args.command == "check-report":
        if not args.report:
            parser.error("check-report requires a JSON report path")
        return check_report(args.report)
    build_dir = args.build_dir.resolve()

    try:
        proxy, fake_driver, test_program = compile_outputs(build_dir)
        print(f"built audit proxy: {proxy}")
        if args.command == "self-test":
            self_test(build_dir, fake_driver, test_program)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"nvenc input audit: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
