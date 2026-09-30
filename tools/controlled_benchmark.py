#!/usr/bin/env python3
"""Coordinate a full-screen stimulus, recorder run, and offline ID decode."""

from __future__ import annotations

import argparse
import fcntl
import json
import math
import os
from pathlib import Path
import queue
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from datetime import datetime
from dataclasses import dataclass, field


ROOT = Path(__file__).resolve().parents[1]
KEY_VALUE = re.compile(r"([A-Za-z0-9_.-]+)=([^\s]+)")
READY_LINE = re.compile(r"stimulus-ready: first-presented id=(\d+) refresh-ns=(\d+)")
PRESENTATION_RATE = re.compile(r"\bpresented/s=([^\s]+)")
PRESENTATION_TOTAL = re.compile(r"\btotals=\d+/(\d+)/\d+")
STIMULUS_PATTERN = re.compile(r"\bpattern=([^\s]+)")
STIMULUS_MOTION = re.compile(r"\bmotion=([^\s]+)")
NAL_UNIT_TYPE = re.compile(r"\bnal_unit_type\s+\d+\s*=\s*(\d+)\b")
H264_ENTROPY_CODING_FLAG = re.compile(
    r"\bentropy_coding_mode_flag\s+\d+\s*=\s*([01])\b"
)
STIMULUS_CLASS = "io.fluxrec.stimulus"
STIMULUS_TITLE = "Fluxrec Motion Stimulus"
GPU_PATH_ENCODER = re.compile(
    r"^fluxrec GPU path:.*->\s*(?:h264|hevc)_(nvenc|vulkan)\s+\(",
    re.MULTILINE,
)
DIRECT_CUDA_EGL_READY = re.compile(
    r"^EXPERIMENTAL direct CUDA EGL encoder initialized\b",
    re.MULTILINE,
)
STIMULUS_FEEDBACK_TIMEOUT_SECONDS = 2.5
WORKSPACE_POLL_INTERVAL_SECONDS = 0.25
HYPRCTL_TIMEOUT_SECONDS = 1.0


class InvalidBenchmarkEnvironment(RuntimeError):
    """The run cannot be treated as a valid controlled capture."""


def validate_stimulus_settings_line(
    line: str,
    expected_pattern: str,
    expected_motion: float,
) -> None:
    """Reject logged runtime changes from the requested stimulus condition."""
    if "pattern=" in line:
        match = STIMULUS_PATTERN.search(line)
        if match is None:
            raise InvalidBenchmarkEnvironment("stimulus emitted a malformed pattern setting")
        actual = match.group(1)
        if actual != expected_pattern:
            raise InvalidBenchmarkEnvironment(
                f"stimulus pattern changed during the benchmark "
                f"(expected {expected_pattern}, logged {actual})"
            )
    if "motion=" in line:
        match = STIMULUS_MOTION.search(line)
        if match is None:
            raise InvalidBenchmarkEnvironment("stimulus emitted a malformed motion setting")
        try:
            actual_motion = float(match.group(1))
        except ValueError as error:
            raise InvalidBenchmarkEnvironment(
                f"stimulus logged an invalid motion setting: {match.group(1)}"
            ) from error
        if not math.isfinite(actual_motion) or not math.isclose(
            actual_motion, expected_motion,
            rel_tol=1e-6, abs_tol=1e-6,
        ):
            raise InvalidBenchmarkEnvironment(
                f"stimulus motion changed during the benchmark "
                f"(expected {expected_motion:g}, logged {actual_motion:g})"
            )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run a Wayland motion stimulus, record an overlapping interval, "
            "then decode the GPU-drawn frame IDs offline."
        )
    )
    parser.add_argument("--display", default="DP-1")
    parser.add_argument("--stimulus-seconds", type=float, default=12.0)
    parser.add_argument("--record-seconds", type=int, default=5)
    parser.add_argument("--pattern", choices=("plasma", "bars", "checker", "solid", "colorchart"),
                        default="plasma")
    parser.add_argument("--motion", type=float, default=0.20)
    parser.add_argument("--codec", choices=("h264", "hevc"), default="h264")
    parser.add_argument("--conversion", choices=("auto", "direct", "two-pass"),
                        help="conversion mode; omitted uses two-pass except the native direct CUDA/EGL backend")
    parser.add_argument("--encoder-tune", choices=("default", "ll", "ull", "hq"),
                        default="default")
    parser.add_argument("--backend", choices=("vulkan", "nvenc", "experimental-cuda-egl"),
                        help="video encoder backend; omitted uses the recorder's configured default")
    parser.add_argument("--nvenc-preset", choices=tuple(f"p{i}" for i in range(1, 8)),
                        help="NVENC preset (requires the NVENC backend)")
    parser.add_argument("--h264-coder", choices=("default", "cabac", "vlc"),
                        default="default",
                        help="H.264 entropy coder request; default leaves the encoder's choice unchanged")
    parser.add_argument("--max-fps", type=int, choices=(0, 60, 120, 240, 360),
                        default=0, help="capture rate cap; 0 leaves capture uncapped")
    parser.add_argument("--bitrate-mbps", type=int, default=None,
                        help="target bitrate in Mbps (bitrate mode default: 50)")
    parser.add_argument("--qp", type=int,
                        help="experimental constant-QP mode; range 0–51, mutually exclusive with --bitrate-mbps")
    parser.add_argument("--gop", type=int, default=120)
    parser.add_argument("--b-frames", type=int, choices=range(5), default=None,
                        help="NVENC B-frame count (0..4); omitted uses the encoder default")
    parser.add_argument("--pool", type=int,
                        help="capture pool size (default: 16 for experimental-cuda-egl, otherwise 24)")
    parser.add_argument("--async-depth", type=int,
                        help="explicit validated-frame queue depth; omission uses the recorder's backend default")
    parser.add_argument("--audio", choices=("on", "off"), default="off")
    parser.add_argument("--verify-frame-ids", action="store_true",
                        help="enable the GPU full-frame digest diagnostic; instrumented throughput may differ")
    parser.add_argument("--replay-seconds", type=int, default=0)
    parser.add_argument("--replay-only", action="store_true",
                        help="capture into the replay ring without writing a primary MKV")
    parser.add_argument("--save-replay-after", type=float,
                        help="request one replay export this many seconds after recorder start")
    parser.add_argument("--replay-output", type=Path,
                        help="saved replay MKV destination (default: beside the primary MKV)")
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                        help="set a recorder environment variable; may be repeated")
    parser.add_argument("--output", type=Path,
                        help=("MKV destination (default: unique file under /tmp); "
                              "in replay-only mode, this names the run's sidecar artifacts"))
    parser.add_argument("--stimulus-binary", type=Path,
                        default=ROOT / "stimulus/build/fluxrec-stimulus")
    parser.add_argument("--recorder-binary", type=Path,
                        default=ROOT / "build-root/engine/fluxrec-record")
    parser.add_argument("--decoder-binary", type=Path,
                        default=ROOT / "build-root/tools/fluxrec-decode-stimulus-ids")
    parser.add_argument("--ready-timeout", type=float, default=10.0,
                        help="seconds to wait for the first presentation feedback")
    parser.add_argument("--settle-seconds", type=float, default=0.20,
                        help="wait after first presentation for fullscreen transition to settle")
    parser.add_argument("--skip-initial-frames", type=int, default=0,
                        help="exclude this many initial decoded frames from identity counts")
    parser.add_argument("--record-timeout", type=float,
                        help="maximum recorder runtime (default: record duration plus 90 s)")
    parser.add_argument("--decode-threshold", type=int, default=100,
                        help="ID glyph luma threshold (default: 100)")
    parser.add_argument("--decode-debug-frames", type=int, default=0)
    parser.add_argument("--lock-file", type=Path,
                        help="benchmark lock path (default: under XDG_RUNTIME_DIR or /tmp)")
    return parser.parse_args()


def make_paths(
    output: Path | None,
    *,
    save_replay: bool = False,
    replay_output: Path | None = None,
    replay_only: bool = False,
) -> dict[str, Path]:
    if output is None:
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        output = Path(tempfile.gettempdir()) / f"fluxrec-controlled-{stamp}.mkv"
    output = output.expanduser().resolve()
    paths = {
        "output": output,
        "stimulus_log": output.with_suffix(output.suffix + ".stimulus.log"),
        "record_stdout": output.with_suffix(output.suffix + ".record.stdout.log"),
        "record_stderr": output.with_suffix(output.suffix + ".record.stderr.log"),
        "summary": output.with_suffix(output.suffix + ".summary.json"),
    }
    if not replay_only:
        paths["decoder_log"] = output.with_suffix(output.suffix + ".decoder.log")
    if save_replay:
        if replay_output is None:
            replay_output = output.with_name(
                f"{output.stem}-replay{output.suffix or '.mkv'}"
            )
        else:
            replay_output = replay_output.expanduser().resolve()
        if replay_output == output:
            raise ValueError("replay output must be different from the primary output")
        if replay_output.suffix.lower() != ".mkv":
            raise ValueError("replay output must use the .mkv extension")
        paths.update({
            "replay_output": replay_output,
            "replay_probe_log": replay_output.with_suffix(
                replay_output.suffix + ".ffprobe.json"
            ),
            "replay_probe_stderr": replay_output.with_suffix(
                replay_output.suffix + ".ffprobe.stderr.log"
            ),
            "replay_decode_log": replay_output.with_suffix(
                replay_output.suffix + ".ffmpeg-decode.log"
            ),
            "replay_identity_log": replay_output.with_suffix(
                replay_output.suffix + ".identity-decoder.log"
            ),
        })
    return paths


def summarize_artifact_paths(
    paths: dict[str, Path],
    *,
    replay_only: bool = False,
) -> dict[str, str]:
    artifacts = {key: str(path) for key, path in paths.items() if key != "output"}
    if replay_only:
        artifacts["run_prefix"] = str(paths["output"])
    else:
        artifacts["output"] = str(paths["output"])
    return artifacts


def parse_env(items: list[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for item in items:
        name, separator, value = item.partition("=")
        if not separator or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            raise ValueError(f"invalid --env assignment: {item!r}; expected NAME=VALUE")
        result[name] = value
    return result


def selected_pool(args: argparse.Namespace) -> int:
    """Return the pool requested by the user or this backend's harness default."""
    if args.pool is not None:
        return args.pool
    return 16 if args.backend == "experimental-cuda-egl" else 24


def recorder_conversion_argument(args: argparse.Namespace) -> str | None:
    """Return an explicit recorder conversion mode, if this backend accepts one."""
    if args.backend == "experimental-cuda-egl" and args.conversion is None:
        # This route imports native-size DMA-BUFs directly. The recorder rejects
        # every explicit --conversion value for this backend.
        return None
    return args.conversion if args.conversion is not None else "two-pass"


def conversion_summary(args: argparse.Namespace) -> str:
    """Describe the selected mode even when native mode is expressed by omission."""
    conversion = recorder_conversion_argument(args)
    return "native" if conversion is None else conversion


def command_async_depth(args: argparse.Namespace) -> int | None:
    """Supply the direct route's measured queue depth when the user omits it."""
    if args.async_depth is not None:
        return args.async_depth
    if args.backend == "experimental-cuda-egl":
        return 12
    return None


def validate(args: argparse.Namespace) -> None:
    if args.stimulus_seconds < 3.0:
        raise ValueError("--stimulus-seconds must be at least 3")
    if args.record_seconds < 1:
        raise ValueError("--record-seconds must be at least 1")
    if args.stimulus_seconds < args.record_seconds + 1.0:
        raise ValueError("stimulus duration must exceed recorder duration by at least one second")
    if not 0.0 <= args.motion <= 1000.0:
        raise ValueError("--motion must be between 0 and 1000")
    if args.qp is None:
        args.rate_control = "bitrate"
        if args.bitrate_mbps is None:
            args.bitrate_mbps = 50
        if args.bitrate_mbps < 1 or args.bitrate_mbps > 500:
            raise ValueError("--bitrate-mbps must be between 1 and 500")
    else:
        if args.bitrate_mbps is not None:
            raise ValueError("--qp selects CQP and cannot be combined with --bitrate-mbps")
        if args.qp < 0 or args.qp > 51:
            raise ValueError("--qp must be between 0 and 51")
        args.rate_control = "constant_qp"
    pool = selected_pool(args)
    if args.gop < 1 or pool < 2 or pool > 64:
        raise ValueError("invalid GOP or pool size")
    if args.backend == "experimental-cuda-egl":
        if args.conversion is not None:
            raise ValueError(
                "--conversion cannot be used with --backend experimental-cuda-egl; "
                "omit it to use native capture frames"
            )
        if pool < 3 or pool > 16:
            raise ValueError(
                "--backend experimental-cuda-egl requires --pool between 3 and 16"
            )
        if args.async_depth is not None and not 1 <= args.async_depth <= 16:
            raise ValueError(
                "--backend experimental-cuda-egl requires --async-depth between 1 and 16"
            )
    if args.async_depth is not None and (args.async_depth < 1 or args.async_depth > 64):
        raise ValueError("--async-depth must be between 1 and 64")
    if args.b_frames is not None:
        if args.b_frames > 0 and args.backend == "vulkan":
            raise ValueError("--b-frames greater than zero requires the NVENC backend")
        if args.b_frames > 0 and args.gop < args.b_frames + 2:
            raise ValueError("--gop must be at least --b-frames + 2 when B-frames are enabled")
    if args.backend == "vulkan" and args.nvenc_preset is not None:
        raise ValueError("--nvenc-preset requires --backend nvenc or the inherited NVENC backend")
    if args.max_fps not in (0, 60, 120, 240, 360):
        raise ValueError("--max-fps must be 0, 60, 120, 240, or 360")
    if args.codec != "h264" and args.h264_coder != "default":
        raise ValueError("--h264-coder can only be set when --codec h264")
    if args.replay_seconds < 0 or not 1 <= args.decode_threshold <= 254:
        raise ValueError("invalid replay duration or ID decoder threshold")
    if args.ready_timeout <= 0:
        raise ValueError("--ready-timeout must be positive")
    if args.settle_seconds < 0 or args.skip_initial_frames < 0:
        raise ValueError("settle time and skipped-frame count must be non-negative")
    if args.record_timeout is not None and args.record_timeout <= 0:
        raise ValueError("--record-timeout must be positive")
    if args.replay_output is not None and args.save_replay_after is None:
        raise ValueError("--replay-output requires --save-replay-after")
    if args.save_replay_after is not None:
        if not math.isfinite(args.save_replay_after) or args.save_replay_after <= 0:
            raise ValueError("--save-replay-after must be a positive number of seconds")
        if args.save_replay_after >= args.record_seconds:
            raise ValueError("--save-replay-after must occur before the recording ends")
        if args.replay_seconds <= 0:
            raise ValueError("--save-replay-after requires --replay-seconds greater than zero")
        if args.replay_output is not None and args.replay_output.suffix.lower() != ".mkv":
            raise ValueError("--replay-output must use the .mkv extension")
    if args.replay_only:
        if args.replay_seconds <= 0:
            raise ValueError("--replay-only requires --replay-seconds greater than zero")
        if args.save_replay_after is None:
            raise ValueError("--replay-only requires --save-replay-after")


def lock_path(args: argparse.Namespace) -> Path:
    if args.lock_file is not None:
        return args.lock_file.expanduser().resolve()
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    base = Path(runtime) if runtime else Path(tempfile.gettempdir())
    return base / "fluxrec-controlled-benchmark.lock"


def acquire_lock(path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = path.open("a+")
    try:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as error:
        handle.close()
        raise RuntimeError(f"another controlled benchmark holds {path}") from error
    return handle


def active_fluxrec_processes() -> list[str]:
    active: list[str] = []
    proc = Path("/proc")
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            name = (entry / "comm").read_text(encoding="utf-8").strip()
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        executable = ""
        try:
            command_line = (entry / "cmdline").read_bytes().split(b"\0")
            if command_line and command_line[0]:
                executable = Path(os.fsdecode(command_line[0])).name
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            pass
        if (executable in {"fluxrec-stimulus", "fluxrec-record"} or
                name == "fluxrec-record" or name.startswith("fluxrec-stimul")):
            active.append(f"pid={entry.name} comm={name}")
    return active


def parse_pairs(line: str) -> dict[str, int | float | str]:
    values: dict[str, int | float | str] = {}
    for key, text in KEY_VALUE.findall(line):
        try:
            values[key] = int(text)
            continue
        except ValueError:
            pass
        try:
            values[key] = float(text)
        except ValueError:
            values[key] = text
    return values


def parse_stimulus_statistics(line: str) -> dict[str, int | float] | None:
    """Parse one compact cadence line emitted by fluxrec-stimulus."""
    if not line.startswith("stimulus:"):
        return None
    rate_match = PRESENTATION_RATE.search(line)
    total_match = PRESENTATION_TOTAL.search(line)
    if rate_match is None or total_match is None:
        raise InvalidBenchmarkEnvironment(
            "stimulus presentation statistics are missing presented/s or totals"
        )
    try:
        presented_fps = float(rate_match.group(1))
        presented_total = int(total_match.group(1))
    except ValueError as error:
        raise InvalidBenchmarkEnvironment(
            "stimulus presentation statistics contain an invalid rate or total"
        ) from error
    if not math.isfinite(presented_fps) or presented_fps < 0:
        raise InvalidBenchmarkEnvironment(
            f"stimulus reported an invalid presentation rate: {rate_match.group(1)}"
        )
    return {"presented_fps": presented_fps, "presented_total": presented_total}


def classify_frame_content_and_source_continuity(
    *,
    decoder_succeeded: bool,
    black_frames: int,
    invalid_frames: int,
    invalid_digit_masks: int,
    valid_id_frames: int,
    source_gaps: int,
    recorded_missing_ids: int,
    recorded_range_valid: bool,
    duplicate_transitions: int,
    backward_transitions: int,
) -> tuple[bool, bool]:
    frame_content_clean = (
        decoder_succeeded and black_frames == 0 and invalid_frames == 0 and
        invalid_digit_masks == 0 and valid_id_frames > 0
    )
    source_continuity_clean = (
        source_gaps == 0 and recorded_missing_ids == 0 and
        recorded_range_valid and duplicate_transitions == 0 and
        backward_transitions == 0
    )
    return frame_content_clean, source_continuity_clean


def classify_source_continuity_label(
    *,
    max_fps: int,
    frame_content_clean: bool,
    source_gaps: int,
    recorded_missing_ids: int,
    recorded_range_valid: bool,
    duplicate_transitions: int,
    backward_transitions: int,
    source_continuity_clean: bool,
    unknown_transition_spans: bool,
) -> str:
    """Label expected identity skips from deliberate capture-rate sampling."""
    if unknown_transition_spans:
        return "unknown"
    if source_continuity_clean:
        return "clean"
    if (
        max_fps in (60, 120, 240)
        and frame_content_clean
        and source_gaps == 0
        and recorded_missing_ids > 0
        and recorded_range_valid
        and duplicate_transitions == 0
        and backward_transitions == 0
    ):
        return "expected-subsampling"
    return "FAIL"


def recording_identity_accepted(
    source_continuity_label: str,
    frame_content_clean: bool | None,
) -> bool:
    """Exit success only when offline identity is clean or an expected cap skip."""
    if frame_content_clean is False:
        return False
    return source_continuity_label in {
        "clean",
        "expected-subsampling",
        "n/a (replay-only)",
    }


def format_recording_count_label(
    captured: object,
    submitted: object,
    encoded: object,
    muxed_packets: object,
) -> str:
    """Keep video-frame counts distinct from all-stream mux packet totals."""
    muxed_packet_count = muxed_packets if muxed_packets is not None else "n/a"
    return (
        f"{captured} captured/{submitted} submitted/{encoded} encoded/"
        f"muxed-total-packets={muxed_packet_count}"
    )


def decode_hyprctl_batch(text: str) -> tuple[list[object], list[object]]:
    """Decode the two JSON arrays returned by `hyprctl -j --batch`."""
    decoder = json.JSONDecoder()
    values: list[object] = []
    offset = 0
    while offset < len(text):
        while offset < len(text) and text[offset].isspace():
            offset += 1
        if offset == len(text):
            break
        value, offset = decoder.raw_decode(text, offset)
        values.append(value)
    if len(values) != 2 or not all(isinstance(value, list) for value in values):
        raise ValueError("expected JSON client and monitor arrays from Hyprland")
    return values[0], values[1]


def inspect_stimulus_workspace(
    clients: list[object],
    monitors: list[object],
    display: str,
    stimulus_pid: int,
) -> dict[str, object]:
    """Return whether the stimulus is fullscreen on its active target output."""
    matching_clients = [
        client for client in clients
        if isinstance(client, dict) and client.get("pid") == stimulus_pid
    ]
    if not matching_clients:
        matching_clients = [
            client for client in clients
            if isinstance(client, dict) and
            client.get("class") == STIMULUS_CLASS and
            client.get("title") == STIMULUS_TITLE
        ]
    if not matching_clients:
        return {"active": False, "reason": "Hyprland no longer lists the stimulus window"}
    if len(matching_clients) != 1:
        return {"active": False, "reason": "Hyprland lists multiple stimulus windows"}
    client = matching_clients[0]

    target_monitors = [
        monitor for monitor in monitors
        if isinstance(monitor, dict) and monitor.get("name") == display
    ]
    if len(target_monitors) != 1:
        return {
            "active": False,
            "reason": f"Hyprland does not report exactly one target output named {display}",
        }
    monitor = target_monitors[0]
    monitor_id = monitor.get("id")
    active_workspace = monitor.get("activeWorkspace")
    client_workspace = client.get("workspace")

    if client.get("mapped") is not True:
        return {"active": False, "reason": "Hyprland does not report the stimulus as mapped"}
    if client.get("hidden") is not False or client.get("visible") is not True:
        return {"active": False, "reason": "the stimulus window is not visible"}
    if client.get("fullscreen") not in (2, 3):
        return {"active": False, "reason": "the stimulus window is no longer fullscreen"}
    if client.get("monitor") != monitor_id:
        return {
            "active": False,
            "reason": f"the stimulus window moved off target output {display}",
        }
    if not isinstance(active_workspace, dict) or not isinstance(client_workspace, dict):
        return {"active": False, "reason": "Hyprland did not report stimulus workspace state"}

    active_workspace_id = active_workspace.get("id")
    client_workspace_id = client_workspace.get("id")
    if type(monitor_id) is not int or type(active_workspace_id) is not int or \
            type(client_workspace_id) is not int:
        return {"active": False, "reason": "Hyprland reported invalid monitor or workspace IDs"}
    details = {
        "active": active_workspace_id == client_workspace_id,
        "display": display,
        "monitor_id": monitor_id,
        "stimulus_pid": client.get("pid"),
        "stimulus_workspace_id": client_workspace_id,
        "active_workspace_id": active_workspace_id,
        "fullscreen_state": client.get("fullscreen"),
    }
    if active_workspace_id != client_workspace_id:
        details["reason"] = (
            f"stimulus fullscreen workspace {client_workspace_id} is inactive on "
            f"{display} (active workspace {active_workspace_id})"
        )
    return details


def query_hyprland_stimulus_state(display: str, stimulus_pid: int) -> dict[str, object]:
    hyprctl = shutil.which("hyprctl")
    if hyprctl is None:
        raise InvalidBenchmarkEnvironment(
            "cannot verify the stimulus workspace: hyprctl is unavailable"
        )
    try:
        result = subprocess.run(
            [hyprctl, "-j", "--batch", "clients;monitors"],
            capture_output=True,
            text=True,
            timeout=HYPRCTL_TIMEOUT_SECONDS,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise InvalidBenchmarkEnvironment(
            f"cannot query Hyprland stimulus workspace state: {error}"
        ) from error
    if result.returncode != 0:
        details = result.stderr.strip() or result.stdout.strip() or f"exit {result.returncode}"
        raise InvalidBenchmarkEnvironment(
            f"cannot query Hyprland stimulus workspace state: {details}"
        )
    try:
        clients, monitors = decode_hyprctl_batch(result.stdout)
    except (json.JSONDecodeError, ValueError) as error:
        raise InvalidBenchmarkEnvironment(
            f"cannot parse Hyprland stimulus workspace state: {error}"
        ) from error
    status = inspect_stimulus_workspace(clients, monitors, display, stimulus_pid)
    if status.get("active") is not True:
        raise InvalidBenchmarkEnvironment(str(status.get("reason", "stimulus workspace is inactive")))
    return status


@dataclass
class StimulusHealthMonitor:
    display: str
    stimulus_pid: int
    expected_pattern: str
    expected_motion: float
    started_at: float
    feedback_timeout_seconds: float = STIMULUS_FEEDBACK_TIMEOUT_SECONDS
    workspace_poll_interval_seconds: float = WORKSPACE_POLL_INTERVAL_SECONDS
    last_stats_at: float | None = None
    last_presented_total: int | None = None
    last_presented_fps: float | None = None
    presentation_rates: list[float] = field(default_factory=list)
    max_stats_gap_seconds: float = 0.0
    workspace_check_count: int = 0
    last_workspace_status: dict[str, object] | None = None
    next_workspace_check_at: float = 0.0

    def observe_line(self, line: str, now: float) -> None:
        validate_stimulus_settings_line(
            line, self.expected_pattern, self.expected_motion
        )
        sample = parse_stimulus_statistics(line)
        if sample is None:
            return
        rate = float(sample["presented_fps"])
        total = int(sample["presented_total"])
        if rate == 0.0:
            raise InvalidBenchmarkEnvironment(
                "stimulus presentation feedback stopped (reported presented/s=0)"
            )
        if self.last_presented_total is not None and total <= self.last_presented_total:
            raise InvalidBenchmarkEnvironment(
                "stimulus presentation feedback stopped (presented total did not advance)"
            )
        if self.last_stats_at is not None:
            self.max_stats_gap_seconds = max(
                self.max_stats_gap_seconds, now - self.last_stats_at
            )
        self.last_stats_at = now
        self.last_presented_total = total
        self.last_presented_fps = rate
        self.presentation_rates.append(rate)

    def check_feedback(self, now: float) -> None:
        anchor = self.last_stats_at if self.last_stats_at is not None else self.started_at
        silence = now - anchor
        if silence > self.feedback_timeout_seconds:
            if self.last_stats_at is None:
                message = "no stimulus presentation-cadence statistics arrived"
            else:
                message = "stimulus presentation-statistics heartbeat stopped"
            raise InvalidBenchmarkEnvironment(
                f"{message} for {silence:.2f}s "
                f"(limit {self.feedback_timeout_seconds:.2f}s)"
            )

    def check_workspace(self, now: float, force: bool = False) -> None:
        if not force and now < self.next_workspace_check_at:
            return
        status = query_hyprland_stimulus_state(self.display, self.stimulus_pid)
        self.workspace_check_count += 1
        self.last_workspace_status = status
        self.next_workspace_check_at = now + self.workspace_poll_interval_seconds

    def summary(self, status: str, reason: str | None = None) -> dict[str, object]:
        healthy = status == "valid"
        return {
            "status": status,
            "valid": healthy,
            "reasons": [reason] if reason else [],
            "presentation_feedback": {
                "statistics_samples": len(self.presentation_rates),
                "last_reported_fps": self.last_presented_fps,
                "min_reported_fps": min(self.presentation_rates)
                if self.presentation_rates else None,
                "max_stats_gap_seconds": self.max_stats_gap_seconds,
                "last_report_age_seconds": (
                    max(0.0, time.monotonic() - self.last_stats_at)
                    if self.last_stats_at is not None else None
                ),
            },
            "fullscreen_workspace": {
                "checks": self.workspace_check_count,
                "poll_interval_seconds": self.workspace_poll_interval_seconds,
                "last_observation": self.last_workspace_status,
            },
            "stimulus_controls": {
                "pattern": self.expected_pattern,
                "motion": self.expected_motion,
                "pattern_motion_locked": True,
            },
        }


def find_summary_line(text: str, prefix: str) -> dict[str, int | float | str]:
    for line in reversed(text.splitlines()):
        if line.startswith(prefix):
            return parse_pairs(line)
    return {}


def parse_recorder_counters(text: str) -> dict[str, int | float | str]:
    """Parse final recorder counters, including old and current mux fields."""
    counters = find_summary_line(text, "captured=")
    if not counters:
        return {}

    # `muxed` was the only field in older recorder summaries. Keep it as the
    # packet-total fallback, while exposing current video-frame and all-stream
    # packet counts under their unambiguous names.
    packet_total = counters.get("muxed_packets_total", counters.get("muxed"))
    if packet_total is not None:
        counters["muxed_packets_total"] = packet_total
        counters["muxed"] = packet_total
    return counters


def summarize_telemetry(path: Path) -> dict[str, object]:
    rates: dict[str, list[float]] = {
        name: [] for name in ("captured", "submitted", "encoded", "muxed")
    }
    encoder_utilization: list[float] = []
    pool_high_water: int | None = None
    pool_capacity: int | None = None
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            sample = json.loads(line)
        except json.JSONDecodeError:
            continue
        fps = sample.get("fps")
        if isinstance(fps, dict):
            has_rate = False
            for name in rates:
                value = fps.get(name)
                if isinstance(value, (int, float)):
                    rates[name].append(float(value))
                    has_rate = True
            # Ignore the startup sample before the first one-second rates exist.
            system = sample.get("system")
            utilization = (system.get("encoder_utilization_percent")
                           if isinstance(system, dict) else None)
            if has_rate and isinstance(utilization, (int, float)):
                encoder_utilization.append(float(utilization))
        queues = sample.get("queues")
        pool = queues.get("gpu_frame_pool") if isinstance(queues, dict) else None
        if isinstance(pool, dict):
            high_water = pool.get("high_water")
            capacity = pool.get("capacity")
            if isinstance(high_water, int):
                pool_high_water = max(pool_high_water or 0, high_water)
            if isinstance(capacity, int):
                pool_capacity = capacity

    def range_summary(values: list[float]) -> dict[str, int | float] | None:
        if not values:
            return None
        return {
            "samples": len(values),
            "min": min(values),
            "mean": statistics.fmean(values),
            "max": max(values),
        }

    return {
        "per_second_fps": {name: range_summary(values) for name, values in rates.items()},
        "encoder_utilization_percent": range_summary(encoder_utilization),
        "gpu_frame_pool_high_water": pool_high_water,
        "gpu_frame_pool_capacity": pool_capacity,
    }


def send_replay_save_command(
    recorder: subprocess.Popen,
    replay_output: Path,
) -> None:
    """Send the single save request over the recorder's control stdin."""
    if recorder.stdin is None:
        raise RuntimeError("recorder stdin is unavailable for the scheduled replay save")
    absolute_output = replay_output.expanduser().resolve()
    recorder.stdin.write(f"save_replay {absolute_output}\n")
    recorder.stdin.flush()


def summarize_replay_control_events(record_stdout: Path, replay_output: Path) -> dict[str, object]:
    """Match replay queue/completion events to the requested absolute path."""
    wanted = str(replay_output.expanduser().resolve())
    queued: list[dict[str, object]] = []
    saved: list[dict[str, object]] = []
    try:
        lines = record_stdout.read_text(encoding="utf-8").splitlines()
    except OSError:
        lines = []
    for line in lines:
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(event, dict) or event.get("path") != wanted:
            continue
        if event.get("type") == "replay_queued":
            queued.append(event)
        elif event.get("type") == "replay_saved":
            saved.append(event)
    successful_saves = [
        event for event in saved
        if event.get("ok") is True
        and isinstance(event.get("packets"), int)
        and event["packets"] > 0
        and isinstance(event.get("bytes"), int)
        and event["bytes"] > 0
    ]
    return {
        "queued_events": queued,
        "saved_events": saved,
        "queued_count": len(queued),
        "saved_count": len(saved),
        "acknowledged": (
            len(queued) == 1
            and len(saved) == 1
            and len(successful_saves) == 1
        ),
        "successful_save": successful_saves[0] if len(successful_saves) == 1 else None,
    }


def _probe_integer(value: object) -> int | None:
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value)
        except ValueError:
            return None
    return None


def summarize_ffprobe_packets(
    payload: object,
    *,
    allow_missing_first_video_dts: bool = False,
) -> dict[str, object]:
    """Validate packet counts and per-stream DTS, with the NVENC B-frame lead case."""
    if not isinstance(payload, dict):
        raise ValueError("ffprobe output is not a JSON object")
    streams = payload.get("streams")
    packets = payload.get("packets")
    if not isinstance(streams, list) or not isinstance(packets, list):
        raise ValueError("ffprobe output is missing streams or packets")

    streams_by_index: dict[int, dict[str, object]] = {}
    for stream in streams:
        if not isinstance(stream, dict):
            raise ValueError("ffprobe returned a malformed stream entry")
        index = _probe_integer(stream.get("index"))
        if index is None or index in streams_by_index:
            raise ValueError("ffprobe returned a missing or duplicate stream index")
        streams_by_index[index] = stream

    packet_counts = {index: 0 for index in streams_by_index}
    previous_dts: dict[int, int] = {}
    dts_increasing = True
    missing_dts_packet_count = 0
    first_video_packet_seen = False
    first_video_packet_missing_dts_allowed = False
    dts_validation_reasons: list[str] = []
    for packet in packets:
        if not isinstance(packet, dict):
            raise ValueError("ffprobe returned a malformed packet entry")
        index = _probe_integer(packet.get("stream_index"))
        dts = _probe_integer(packet.get("dts"))
        if index not in packet_counts:
            dts_increasing = False
            dts_validation_reasons.append("packet references an unknown stream index")
            continue
        packet_counts[index] += 1
        stream_type = streams_by_index[index].get("codec_type")
        is_first_video_packet = stream_type == "video" and not first_video_packet_seen
        if stream_type == "video":
            first_video_packet_seen = True
        if dts is None:
            missing_dts_packet_count += 1
            if allow_missing_first_video_dts and is_first_video_packet and \
                    not first_video_packet_missing_dts_allowed:
                first_video_packet_missing_dts_allowed = True
            else:
                dts_increasing = False
                dts_validation_reasons.append(
                    "DTS is missing outside the first video packet allowed for B-frame Matroska"
                )
            continue
        if index in previous_dts and dts <= previous_dts[index]:
            dts_increasing = False
            dts_validation_reasons.append(
                f"DTS is not strictly increasing on stream {index}"
            )
        previous_dts[index] = dts

    stream_summaries: list[dict[str, object]] = []
    counts_match = True
    video_packet_count = 0
    video_decoded_frames = 0
    has_video = False
    for index, stream in streams_by_index.items():
        packet_count = packet_counts[index]
        decoded_frames = _probe_integer(stream.get("nb_read_frames"))
        reported_packets = _probe_integer(stream.get("nb_read_packets"))
        if reported_packets is None or reported_packets != packet_count:
            counts_match = False
        stream_type = stream.get("codec_type")
        if stream_type == "video":
            has_video = True
            video_packet_count += packet_count
            video_decoded_frames += decoded_frames or 0
        stream_summaries.append({
            "index": index,
            "codec_type": stream_type,
            "codec_name": stream.get("codec_name"),
            "width": stream.get("width"),
            "height": stream.get("height"),
            "packet_count": packet_count,
            "reported_packet_count": reported_packets,
            "decoded_frame_count": decoded_frames,
        })

    total_packet_count = len(packets)
    stream_packet_count = sum(packet_counts.values())
    if stream_packet_count != total_packet_count:
        counts_match = False
    valid = (
        bool(stream_summaries)
        and has_video
        and total_packet_count > 0
        and video_packet_count > 0
        and video_decoded_frames > 0
        and dts_increasing
        and counts_match
    )
    if not dts_increasing:
        dts_validation = "invalid"
    elif first_video_packet_missing_dts_allowed:
        dts_validation = "bframe-first-video-dts-missing-allowed"
    else:
        dts_validation = "strict"
    return {
        "valid": valid,
        "packet_count": total_packet_count,
        "stream_packet_count": stream_packet_count,
        "video_packet_count": video_packet_count,
        "video_decoded_frame_count": video_decoded_frames,
        "per_stream_dts_strictly_increasing": dts_increasing,
        "missing_dts_packet_count": missing_dts_packet_count,
        "first_video_packet_missing_dts_allowed": first_video_packet_missing_dts_allowed,
        "dts_validation": dts_validation,
        "dts_validation_reasons": dts_validation_reasons,
        "reported_packet_counts_match": counts_match,
        "streams": stream_summaries,
    }


def summarize_replay_identity(
    decoder_stdout: str,
    decoder_returncode: int,
    source_first: object,
    source_last: object,
) -> dict[str, object]:
    """Summarize only IDs found in the replay file, separate from primary IDs."""
    line = find_summary_line(decoder_stdout, "decoded_frames=")
    first_id = line.get("first_id")
    last_id = line.get("last_id")
    first_source_id = source_first if isinstance(source_first, int) else None
    last_source_id = source_last if isinstance(source_last, int) else None
    id_span = (
        int(last_id) - int(first_id) + 1
        if isinstance(first_id, int) and isinstance(last_id, int) and last_id >= first_id
        else 0
    )
    range_valid = (
        first_source_id is not None
        and last_source_id is not None
        and isinstance(first_id, int)
        and isinstance(last_id, int)
        and first_source_id <= first_id <= last_source_id
        and first_source_id <= last_id <= last_source_id
    )
    duplicates = int(line.get("duplicate_transitions", 0))
    backwards = int(line.get("backward_transitions", 0))
    black_frames = int(line.get("black_frames", 0))
    invalid_frames = int(line.get("invalid_frames", 0))
    invalid_digit_masks = int(line.get("invalid_digit_masks", 0))
    valid_id_frames = int(line.get("valid_id_frames", 0))
    missing_ids = int(line.get(
        "missing_ids_between_adjacent_valid_frames",
        line.get("missing_ids_between_frames", 0),
    ))
    gap_transitions = int(line.get(
        "gap_transitions_between_adjacent_valid_frames",
        line.get("gap_transitions", 0),
    ))
    invalid_spans = int(line.get("invalid_spans", 0))
    unknown_transitions = int(line.get(
        "unknown_transitions_across_invalid_spans", 0
    ))
    frame_content_clean = (
        decoder_returncode == 0
        and black_frames == 0
        and invalid_frames == 0
        and invalid_digit_masks == 0
        and valid_id_frames > 0
    )
    ordered_and_in_source = range_valid and duplicates == 0 and backwards == 0
    return {
        "exit_status": decoder_returncode,
        "frames": line.get("analyzed_frames", line.get("decoded_frames")),
        "total_decoded_frames": line.get("decoded_frames"),
        "first_id": first_id,
        "last_id": last_id,
        "id_span": id_span,
        "missing_ids_between_adjacent_valid_frames": missing_ids,
        "missing_ids_percent_of_replay_id_span": (
            100.0 * missing_ids / id_span if id_span else None
        ),
        "gap_transitions_between_adjacent_valid_frames": gap_transitions,
        "duplicate_transitions": duplicates,
        "backward_transitions": backwards,
        "black_frames": black_frames,
        "invalid_frames": invalid_frames,
        "invalid_digit_masks": invalid_digit_masks,
        "invalid_spans": invalid_spans,
        "unknown_transitions_across_invalid_spans": unknown_transitions,
        "valid_id_frames": valid_id_frames,
        "valid_segments": line.get("valid_segments"),
        "frame_content_clean": frame_content_clean,
        "id_order_and_source_range_valid": ordered_and_in_source,
        "source_id_range": {
            "first_id": first_source_id,
            "last_id": last_source_id,
        },
    }


def build_ffmpeg_replay_decode_command(ffmpeg: str, replay_output: Path) -> list[str]:
    """Decode every replay stream while retaining its demuxer time base."""
    return [
        ffmpeg, "-nostdin", "-v", "error", "-xerror", "-i",
        str(replay_output), "-map", "0", "-enc_time_base", "demux",
        "-f", "null", "-",
    ]


def validate_replay_artifact(
    paths: dict[str, Path],
    args: argparse.Namespace,
    *,
    source_first: object,
    source_last: object,
    save_sent_after_seconds: float | None,
) -> dict[str, object]:
    """Read back an exported replay with demux, decode, and frame-ID checks."""
    replay_output = paths["replay_output"]
    control = summarize_replay_control_events(paths["record_stdout"], replay_output)
    errors: list[str] = []
    artifact_exists = replay_output.is_file() and replay_output.stat().st_size > 0
    if not artifact_exists:
        errors.append("replay MKV is missing or empty")

    probe_summary: dict[str, object] = {"valid": False}
    probe_returncode: int | None = None
    if artifact_exists:
        ffprobe = shutil.which("ffprobe")
        if ffprobe is None:
            errors.append("ffprobe executable is unavailable")
        else:
            probe_command = [
                ffprobe, "-v", "error", "-count_packets", "-count_frames",
                "-show_streams", "-show_packets",
                "-show_entries",
                "stream=index,codec_type,codec_name,width,height,nb_read_packets,nb_read_frames:packet=stream_index,dts,flags",
                "-of", "json", str(replay_output),
            ]
            try:
                probe = subprocess.run(
                    probe_command, capture_output=True, text=True,
                    timeout=300, check=False,
                )
                probe_returncode = probe.returncode
                paths["replay_probe_log"].write_text(
                    probe.stdout, encoding="utf-8")
                paths["replay_probe_stderr"].write_text(
                    probe.stderr, encoding="utf-8")
                if probe.returncode != 0:
                    errors.append(f"ffprobe exited with status {probe.returncode}")
                else:
                    try:
                        probe_summary = summarize_ffprobe_packets(
                            json.loads(probe.stdout),
                            allow_missing_first_video_dts=(
                                args.b_frames is not None
                                and args.b_frames > 0
                                and replay_output.suffix.lower() == ".mkv"
                            ),
                        )
                    except (json.JSONDecodeError, ValueError) as error:
                        errors.append(f"ffprobe packet validation failed: {error}")
                        probe_summary = {"valid": False, "error": str(error)}
                    if probe_summary.get("valid") is not True:
                        errors.append("ffprobe packet counts or DTS ordering are invalid")
            except (OSError, subprocess.TimeoutExpired) as error:
                errors.append(f"ffprobe could not read the replay MKV: {error}")

    decode_returncode: int | None = None
    decode_stderr = ""
    if artifact_exists:
        ffmpeg = shutil.which("ffmpeg")
        if ffmpeg is None:
            errors.append("ffmpeg executable is unavailable")
        else:
            decode_command = build_ffmpeg_replay_decode_command(ffmpeg, replay_output)
            try:
                decoded = subprocess.run(
                    decode_command, capture_output=True, text=True,
                    timeout=300, check=False,
                )
                decode_returncode = decoded.returncode
                decode_stderr = decoded.stderr
                paths["replay_decode_log"].write_text(
                    decoded.stdout + decoded.stderr, encoding="utf-8")
                if decoded.returncode != 0:
                    errors.append(f"ffmpeg full-stream decode exited with status {decoded.returncode}")
            except (OSError, subprocess.TimeoutExpired) as error:
                decode_stderr = str(error)
                errors.append(f"ffmpeg could not decode the replay MKV: {error}")

    identity_summary: dict[str, object] = {"exit_status": None}
    identity_returncode: int | None = None
    if artifact_exists:
        decoder_command = [
            str(args.decoder_binary.expanduser().resolve()),
            "--threshold", str(args.decode_threshold),
        ]
        if args.decode_debug_frames:
            decoder_command.extend(("--debug-frames", str(args.decode_debug_frames)))
        decoder_command.append(str(replay_output))
        try:
            identity = subprocess.run(
                decoder_command, capture_output=True, text=True,
                timeout=300, check=False,
            )
            identity_returncode = identity.returncode
            paths["replay_identity_log"].write_text(
                identity.stdout + identity.stderr, encoding="utf-8")
            identity_summary = summarize_replay_identity(
                identity.stdout, identity.returncode, source_first, source_last
            )
            if identity.returncode != 0:
                errors.append(
                    f"replay frame-ID decoder exited with status {identity.returncode}"
                )
        except (OSError, subprocess.TimeoutExpired) as error:
            identity_summary = {"exit_status": None, "error": str(error)}
            errors.append(f"replay frame-ID decode failed: {error}")

    if control["acknowledged"] is not True:
        errors.append("recorder did not acknowledge exactly one successful replay save")
    if identity_summary.get("frame_content_clean") is not True:
        errors.append("replay frame-ID content contains black, invalid, or undecodable frames")
    if identity_summary.get("id_order_and_source_range_valid") is not True:
        errors.append("replay frame IDs are duplicated, backwards, or outside the source range")
    valid = (
        artifact_exists
        and control["acknowledged"] is True
        and probe_summary.get("valid") is True
        and decode_returncode == 0
        and identity_returncode == 0
        and identity_summary.get("frame_content_clean") is True
        and identity_summary.get("id_order_and_source_range_valid") is True
    )
    return {
        "requested": True,
        "scheduled_after_seconds": args.save_replay_after,
        "sent_after_seconds": save_sent_after_seconds,
        "output_path": str(replay_output),
        "output_bytes": replay_output.stat().st_size if artifact_exists else 0,
        "recorder_control": control,
        "packet_probe": {
            "exit_status": probe_returncode,
            **probe_summary,
        },
        "full_stream_decode": {
            "exit_status": decode_returncode,
            "valid": decode_returncode == 0,
            "stderr": decode_stderr,
        },
        "decoded_identity": identity_summary,
        "validation": {
            "valid": valid,
            "errors": errors,
        },
    }


def pump_stimulus_stderr(stream, path: Path, lines: queue.Queue[str | None]) -> None:
    with path.open("w", encoding="utf-8", buffering=1) as log:
        for line in iter(stream.readline, ""):
            log.write(line)
            log.flush()
            lines.put(line)
    lines.put(None)


def stop_process(process: subprocess.Popen | None, grace: float = 2.0) -> None:
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def wait_for_first_presentation(
    process: subprocess.Popen,
    lines: queue.Queue[str | None],
    timeout: float,
    expected_pattern: str,
    expected_motion: float,
) -> tuple[int, int]:
    deadline = time.monotonic() + timeout
    while True:
        if process.poll() is not None:
            raise RuntimeError(f"stimulus exited before its first presentation (status {process.returncode})")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("timed out waiting for stimulus-ready first-presentation feedback")
        try:
            line = lines.get(timeout=min(remaining, 0.2))
        except queue.Empty:
            continue
        if line is None:
            raise RuntimeError("stimulus stderr closed before first presentation")
        validate_stimulus_settings_line(line, expected_pattern, expected_motion)
        match = READY_LINE.search(line)
        if match:
            return int(match.group(1)), int(match.group(2))


def monitor_stimulus_interval(
    process: subprocess.Popen,
    lines: queue.Queue[str | None],
    monitor: StimulusHealthMonitor,
    duration: float,
    *,
    recording: bool,
) -> None:
    deadline = time.monotonic() + duration
    while True:
        now = time.monotonic()
        if process.poll() is not None:
            raise InvalidBenchmarkEnvironment(
                f"stimulus exited {'while recording' if recording else 'during setup'} "
                f"(status {process.returncode})"
            )
        monitor.check_feedback(now)
        if recording:
            monitor.check_workspace(now)
        remaining = deadline - now
        if remaining <= 0:
            return
        try:
            line = lines.get(timeout=min(0.05, remaining))
        except queue.Empty:
            continue
        if line is None:
            raise InvalidBenchmarkEnvironment(
                "stimulus presentation-feedback log closed while the stimulus was running"
            )
        monitor.observe_line(line, time.monotonic())


def drain_stimulus_statistics(
    process: subprocess.Popen,
    lines: queue.Queue[str | None],
    monitor: StimulusHealthMonitor,
) -> None:
    while True:
        try:
            line = lines.get_nowait()
        except queue.Empty:
            break
        if line is None:
            raise InvalidBenchmarkEnvironment(
                "stimulus presentation-feedback log closed while the stimulus was running"
            )
        monitor.observe_line(line, time.monotonic())
    if process.poll() is not None:
        raise InvalidBenchmarkEnvironment(
            f"stimulus exited while recording (status {process.returncode})"
        )
    monitor.check_feedback(time.monotonic())


def write_invalid_environment_summary(
    paths: dict[str, Path],
    args: argparse.Namespace,
    monitor: StimulusHealthMonitor | None,
    reason: str,
    recorder: subprocess.Popen | None,
) -> None:
    output = paths["output"]
    replay_only = getattr(args, "replay_only", False)
    backend_observed = observed_backend(paths["record_stderr"])
    summary = {
        "environmental_validity": {
            "status": "invalid",
            "valid": False,
            "reasons": [reason],
        },
        "conditions": {
            "codec": args.codec,
            "conversion": conversion_summary(args),
            "encoder_tune": args.encoder_tune,
            "backend": args.backend or "inherited",
            "backend_observed": backend_observed,
            "nvenc_preset": args.nvenc_preset,
            "h264_coder_requested": (
                getattr(args, "h264_coder", "default")
                if args.codec == "h264" else "not_applicable"
            ),
            "h264_coder_observed": "unknown" if args.codec == "h264" else "not_applicable",
            "max_fps": args.max_fps,
            "b_frames": getattr(args, "b_frames", None),
            "replay_only": getattr(args, "replay_only", False),
            "pattern": args.pattern,
            "motion": args.motion,
            "record_seconds_requested": args.record_seconds,
            "async_depth_requested": args.async_depth,
            "async_depth": effective_async_depth(args, paths["record_stderr"]),
        },
        "environmental_checks": (
            monitor.summary("invalid", reason) if monitor is not None else None
        ),
        "recording": {
            "started": recorder is not None,
            "exit_status_at_abort": recorder.returncode if recorder is not None else None,
            "partial_output_bytes": (
                output.stat().st_size
                if not replay_only and output.is_file() else 0
            ),
            "output_path": None if replay_only else str(output),
            "backend": args.backend or "inherited",
            "backend_observed": backend_observed,
        },
        "artifacts": summarize_artifact_paths(paths, replay_only=replay_only),
    }
    paths["summary"].write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


def terminate_children(stimulus: subprocess.Popen | None,
                       recorder: subprocess.Popen | None) -> None:
    stop_process(recorder)
    if recorder is not None and recorder.stdin is not None and not recorder.stdin.closed:
        try:
            recorder.stdin.close()
        except OSError:
            pass
    stop_process(stimulus)


def recorder_rate_control_arguments(args: argparse.Namespace) -> list[str]:
    if args.qp is not None:
        return ["--qp", str(args.qp)]
    return ["--bitrate-mbps", str(args.bitrate_mbps)]


def build_recorder_command(
    args: argparse.Namespace,
    recorder_binary: Path,
    output: Path,
) -> list[str]:
    command = [str(recorder_binary)]
    if args.replay_only:
        command.append("--replay-only")
    else:
        command.extend(("--output", str(output)))
    command.extend([
        "--display", args.display,
        "--seconds", str(args.record_seconds),
        "--codec", args.codec,
        "--gop", str(args.gop),
        "--pool", str(selected_pool(args)),
        "--audio", args.audio,
        "--replay-seconds", str(args.replay_seconds),
    ])
    conversion = recorder_conversion_argument(args)
    if conversion is not None:
        command.extend(("--conversion", conversion))
    if args.backend is not None:
        command.extend(("--backend", args.backend))
    if args.nvenc_preset is not None:
        command.extend(("--nvenc-preset", args.nvenc_preset))
    async_depth = command_async_depth(args)
    if async_depth is not None:
        command.extend(("--async-depth", str(async_depth)))
    if args.b_frames is not None:
        command.extend(("--b-frames", str(args.b_frames)))
    command.extend(recorder_rate_control_arguments(args))
    if args.encoder_tune != "default":
        command.extend(("--encoder-tune", args.encoder_tune))
    if args.h264_coder != "default":
        command.extend(("--h264-coder", args.h264_coder))
    if args.max_fps:
        command.extend(("--max-fps", str(args.max_fps)))
    if args.verify_frame_ids:
        command.append("--verify-frame-ids")
    return command


def effective_async_depth(args: argparse.Namespace, recorder_stderr: Path) -> int | None:
    """Return the selected depth when runtime startup logs identify it."""
    if args.async_depth is not None:
        return args.async_depth
    if args.backend == "experimental-cuda-egl":
        return 12
    try:
        stderr = recorder_stderr.read_text(encoding="utf-8")
    except OSError:
        return None
    if "NVENC selected: default async depth set to 4 validated frames" in stderr:
        return 4
    if "NVENC encoder opened:" in stderr:
        # A successful NVENC run with no auto-depth marker does not expose its
        # selected depth in the log, so leave it unknown instead of guessing.
        return None
    # The recorder's current Vulkan default is eight, and the successful log
    # contains no NVENC startup line. For future backends, keep this unknown.
    if args.backend in (None, "vulkan"):
        return 8
    return None


def parse_observed_backend(log: str) -> str | None:
    """Extract the backend from a successful recorder path initialization log."""
    backends = set(GPU_PATH_ENCODER.findall(log))
    if DIRECT_CUDA_EGL_READY.search(log):
        backends.add("experimental-cuda-egl")
    if not backends:
        return None
    if len(backends) != 1:
        return "mixed"
    return next(iter(backends))


def observed_backend(recorder_stderr: Path) -> str | None:
    """Return the backend actually opened, or None when startup did not identify it."""
    try:
        log = recorder_stderr.read_text(encoding="utf-8")
    except OSError:
        return None
    return parse_observed_backend(log)


def validate_observed_backend(requested: str | None, observed: str | None) -> None:
    """Reject a contradictory successful startup path for an explicit request."""
    if requested is not None and observed is not None and observed != requested:
        raise InvalidBenchmarkEnvironment(
            f"recorder opened the {observed} backend despite explicit {requested} request"
        )


def parse_h264_coder_from_trace(trace: str) -> str:
    """Read entropy_coding_mode_flag values only from traced H.264 PPS NALs."""
    pps_flags: list[int] = []
    in_picture_parameter_set = False
    for line in trace.splitlines():
        nal_match = NAL_UNIT_TYPE.search(line)
        if nal_match is not None:
            in_picture_parameter_set = int(nal_match.group(1)) == 8
            continue
        if not in_picture_parameter_set:
            continue
        flag_match = H264_ENTROPY_CODING_FLAG.search(line)
        if flag_match is not None:
            pps_flags.append(int(flag_match.group(1)))

    if not pps_flags:
        return "unknown"
    if len(set(pps_flags)) != 1:
        return "mixed"
    return "cabac" if pps_flags[0] else "cavlc"


def h264_coder_probe_command(ffmpeg: str, bitstream: Path) -> list[str]:
    """Trace one video packet so PPS inspection stays cheap for long captures."""
    return [
        ffmpeg, "-hide_banner", "-loglevel", "trace", "-i", str(bitstream),
        "-map", "0:v:0", "-frames:v", "1", "-c:v", "copy",
        "-bsf:v", "trace_headers", "-f", "null", "-",
    ]


def probe_h264_coder(bitstream: Path) -> str:
    """Return the observed coder from the saved H.264 PPS, or unknown on failure."""
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None or not bitstream.is_file():
        return "unknown"
    try:
        result = subprocess.run(
            h264_coder_probe_command(ffmpeg, bitstream),
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"
    if result.returncode != 0:
        return "unknown"
    return parse_h264_coder_from_trace(result.stderr)


def h264_coder_observation(args: argparse.Namespace,
                           paths: dict[str, Path]) -> tuple[str, str | None]:
    """Return the coder observed in the relevant saved stream and its source."""
    if args.codec != "h264":
        return "not_applicable", None
    if args.replay_only:
        source = paths.get("replay_output")
        source_name = "replay_export"
    else:
        source = paths["output"]
        source_name = "primary_output"
    if source is None:
        return "unknown", None
    return probe_h264_coder(source), source_name


def run_benchmark(args: argparse.Namespace) -> int:
    validate(args)
    paths = make_paths(
        args.output,
        save_replay=args.save_replay_after is not None,
        replay_output=args.replay_output,
        replay_only=args.replay_only,
    )
    output = paths["output"]
    output.parent.mkdir(parents=True, exist_ok=True)
    if "replay_output" in paths:
        paths["replay_output"].parent.mkdir(parents=True, exist_ok=True)
    for path in paths.values():
        if path.exists():
            raise FileExistsError(f"refusing to overwrite existing benchmark artifact: {path}")

    lock = acquire_lock(lock_path(args))
    stimulus: subprocess.Popen | None = None
    recorder: subprocess.Popen | None = None
    reader: threading.Thread | None = None
    environment_monitor: StimulusHealthMonitor | None = None
    return_code = 1
    try:
        active = active_fluxrec_processes()
        if active:
            raise RuntimeError("refusing to overlap an existing Fluxrec process: " + ", ".join(active))

        binaries = {
            "stimulus": args.stimulus_binary.expanduser().resolve(),
            "recorder": args.recorder_binary.expanduser().resolve(),
            "decoder": args.decoder_binary.expanduser().resolve(),
        }
        for role, binary in binaries.items():
            if not binary.is_file() or not os.access(binary, os.X_OK):
                raise FileNotFoundError(f"{role} executable is missing or not executable: {binary}")

        stimulus_command = [
            str(binaries["stimulus"]), "--fullscreen", "--output", args.display,
            "--pattern", args.pattern, "--motion", str(args.motion),
            "--duration", str(args.stimulus_seconds), "--lock-pattern-motion",
        ]
        print("starting stimulus; waiting for first compositor presentation", flush=True)
        stimulus = subprocess.Popen(
            stimulus_command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            start_new_session=True,
        )
        stimulus_lines: queue.Queue[str | None] = queue.Queue()
        assert stimulus.stderr is not None
        reader = threading.Thread(
            target=pump_stimulus_stderr,
            args=(stimulus.stderr, paths["stimulus_log"], stimulus_lines),
            daemon=True,
        )
        reader.start()
        first_id, first_refresh_ns = wait_for_first_presentation(
            stimulus, stimulus_lines, args.ready_timeout, args.pattern, args.motion)
        environment_monitor = StimulusHealthMonitor(
            display=args.display,
            stimulus_pid=stimulus.pid,
            expected_pattern=args.pattern,
            expected_motion=args.motion,
            started_at=time.monotonic(),
        )
        print(f"stimulus active: first presented ID={first_id}, refresh={first_refresh_ns} ns",
              flush=True)
        if args.settle_seconds:
            print(f"waiting {args.settle_seconds:.3f}s for fullscreen transition to settle",
                  flush=True)
            monitor_stimulus_interval(
                stimulus, stimulus_lines, environment_monitor, args.settle_seconds,
                recording=False,
            )
        environment_monitor.check_workspace(time.monotonic(), force=True)
        environment_monitor.check_feedback(time.monotonic())

        record_command = build_recorder_command(args, binaries["recorder"], output)
        recorder_env = os.environ.copy()
        for name, value in parse_env(args.env).items():
            recorder_env[name] = value

        with paths["record_stdout"].open("w", encoding="utf-8") as stdout_log, \
             paths["record_stderr"].open("w", encoding="utf-8") as stderr_log:
            recorder = subprocess.Popen(
                record_command,
                stdin=(subprocess.PIPE if args.save_replay_after is not None
                       else subprocess.DEVNULL),
                stdout=stdout_log,
                stderr=stderr_log,
                env=recorder_env,
                text=True,
                encoding="utf-8",
                bufsize=1,
                start_new_session=True,
            )
            record_started_at = time.monotonic()
            save_sent_after_seconds: float | None = None
            record_timeout = args.record_timeout or args.record_seconds + 90.0
            deadline = record_started_at + record_timeout
            while recorder.poll() is None:
                now = time.monotonic()
                if (
                    args.save_replay_after is not None
                    and save_sent_after_seconds is None
                    and now - record_started_at >= args.save_replay_after
                ):
                    send_replay_save_command(recorder, paths["replay_output"])
                    save_sent_after_seconds = now - record_started_at
                    print(
                        "requested replay save after "
                        f"{save_sent_after_seconds:.3f}s: {paths['replay_output']}",
                        flush=True,
                    )
                if stimulus.poll() is not None:
                    raise InvalidBenchmarkEnvironment(
                        f"stimulus exited while recording (status {stimulus.returncode})"
                    )
                if now >= deadline:
                    raise TimeoutError("recorder exceeded its runtime timeout")
                monitor_interval = 0.10
                if args.save_replay_after is not None and save_sent_after_seconds is None:
                    until_save = args.save_replay_after - (now - record_started_at)
                    if until_save > 0:
                        monitor_interval = min(monitor_interval, until_save)
                monitor_stimulus_interval(
                    stimulus, stimulus_lines, environment_monitor, monitor_interval,
                    recording=True,
                )
            record_status = recorder.returncode
            if recorder.stdin is not None and not recorder.stdin.closed:
                recorder.stdin.close()

        drain_stimulus_statistics(stimulus, stimulus_lines, environment_monitor)
        environment_monitor.check_workspace(time.monotonic(), force=True)

        if record_status != 0:
            raise RuntimeError(f"recorder exited with status {record_status}; see {paths['record_stderr']}")
        if args.save_replay_after is not None and save_sent_after_seconds is None:
            raise RuntimeError(
                "recorder exited before the scheduled replay save command was sent"
            )

        stimulus_timeout = max(15.0, args.stimulus_seconds + 5.0)
        try:
            stimulus_status = stimulus.wait(timeout=stimulus_timeout)
        except subprocess.TimeoutExpired as error:
            raise TimeoutError("stimulus did not exit at its configured duration") from error
        if reader is not None:
            reader.join(timeout=2.0)
        if stimulus_status != 0:
            raise RuntimeError(f"stimulus exited with status {stimulus_status}; see {paths['stimulus_log']}")
        decoder = None
        if not args.replay_only:
            if not output.is_file() or output.stat().st_size == 0:
                raise RuntimeError("recorder exited successfully but produced no MKV output")

            decoder_command = [
                str(binaries["decoder"]), "--threshold", str(args.decode_threshold),
            ]
            if args.decode_debug_frames:
                decoder_command.extend(("--debug-frames", str(args.decode_debug_frames)))
            if args.skip_initial_frames:
                decoder_command.extend(("--skip-initial-frames", str(args.skip_initial_frames)))
            decoder_command.append(str(output))
            decoder = subprocess.run(decoder_command, capture_output=True, text=True,
                                     timeout=300, check=False)
            paths["decoder_log"].write_text(
                decoder.stdout + decoder.stderr, encoding="utf-8")

        source_log = paths["stimulus_log"].read_text(encoding="utf-8")
        for line in source_log.splitlines():
            validate_stimulus_settings_line(line, args.pattern, args.motion)
        record_log = paths["record_stderr"].read_text(encoding="utf-8")
        backend_observed = parse_observed_backend(record_log)
        validate_observed_backend(args.backend, backend_observed)
        decode_line = (
            find_summary_line(decoder.stdout, "decoded_frames=")
            if decoder is not None else {}
        )
        source_line = find_summary_line(source_log, "final:")
        record_line = parse_recorder_counters(record_log)
        telemetry = summarize_telemetry(paths["record_stdout"])
        if not source_line:
            raise RuntimeError(f"could not parse final stimulus counters; see {paths['stimulus_log']}")
        if not record_line:
            raise RuntimeError(f"could not parse final recorder counters; see {paths['record_stderr']}")

        source_first = source_line.get("source-first-id")
        source_last = source_line.get("source-last-id")
        record_first = decode_line.get("first_id")
        record_last = decode_line.get("last_id")
        range_ok = None if args.replay_only else (
            isinstance(source_first, int) and isinstance(source_last, int) and
            isinstance(record_first, int) and isinstance(record_last, int) and
            source_first <= record_first <= source_last and
            source_first <= record_last <= source_last
        )
        duplicates = int(decode_line.get("duplicate_transitions", 0))
        missing_ids = int(decode_line.get(
            "missing_ids_between_adjacent_valid_frames",
            decode_line.get("missing_ids_between_frames", 0),
        ))
        gap_transitions = int(decode_line.get(
            "gap_transitions_between_adjacent_valid_frames",
            decode_line.get("gap_transitions", 0),
        ))
        backwards = int(decode_line.get("backward_transitions", 0))
        invalid_masks = int(decode_line.get("invalid_digit_masks", 0))
        invalid_frames = int(decode_line.get("invalid_frames", 0))
        black_frames = int(decode_line.get("black_frames", 0))
        valid_id_frames = int(decode_line.get("valid_id_frames", 0))
        valid_segments = int(decode_line.get("valid_segments", 0))
        invalid_spans = int(decode_line.get("invalid_spans", 0))
        unknown_transitions = int(decode_line.get(
            "unknown_transitions_across_invalid_spans", 0
        ))
        source_gaps = int(source_line.get("presented-id-gaps", 0))
        source_span = (
            int(source_last) - int(source_first) + 1
            if isinstance(source_first, int) and isinstance(source_last, int) and
            int(source_last) >= int(source_first) else 0
        )
        recorded_span = (
            int(record_last) - int(record_first) + 1
            if isinstance(record_first, int) and isinstance(record_last, int) and
            int(record_last) >= int(record_first) else 0
        )
        source_gap_percent = 100.0 * source_gaps / source_span if source_span else None
        recorded_gap_percent = 100.0 * missing_ids / recorded_span if recorded_span else None
        # Numeric ID skips are reported as cadence/loss evidence, not treated as
        # decoder corruption: a recording may sample fewer frames than the source.
        if args.replay_only:
            identity_validity_clean = None
            frame_content_clean = None
            source_continuity_clean = None
            source_continuity_label = "n/a (replay-only)"
        else:
            assert decoder is not None
            identity_validity_clean = (
                decoder.returncode == 0 and duplicates == 0 and backwards == 0 and
                invalid_frames == 0 and black_frames == 0 and range_ok
            )
            frame_content_clean, source_continuity_clean = (
                classify_frame_content_and_source_continuity(
                    decoder_succeeded=decoder.returncode == 0,
                    black_frames=black_frames,
                    invalid_frames=invalid_frames,
                    invalid_digit_masks=invalid_masks,
                    valid_id_frames=valid_id_frames,
                    source_gaps=source_gaps,
                    recorded_missing_ids=missing_ids,
                    recorded_range_valid=range_ok,
                    duplicate_transitions=duplicates,
                    backward_transitions=backwards,
                )
            )
            source_continuity_label = classify_source_continuity_label(
                max_fps=args.max_fps,
                frame_content_clean=frame_content_clean,
                source_gaps=source_gaps,
                recorded_missing_ids=missing_ids,
                recorded_range_valid=range_ok,
                duplicate_transitions=duplicates,
                backward_transitions=backwards,
                source_continuity_clean=source_continuity_clean,
                unknown_transition_spans=(
                    invalid_spans > 0 or invalid_frames > 0 or black_frames > 0
                    or unknown_transitions > 0
                ),
            )

        rate_control_condition = {
            "mode": args.rate_control,
            "bitrate_mbps": args.bitrate_mbps if args.qp is None else None,
            "qp": args.qp,
        }
        selected_async_depth = effective_async_depth(args, paths["record_stderr"])
        replay_summary = None
        if args.save_replay_after is not None:
            replay_summary = validate_replay_artifact(
                paths,
                args,
                source_first=source_first,
                source_last=source_last,
                save_sent_after_seconds=save_sent_after_seconds,
            )
        h264_coder_observed, h264_coder_source = h264_coder_observation(args, paths)
        summary = {
            "environmental_validity": {
                "status": "valid",
                "valid": True,
                "reasons": [],
            },
            "environmental_checks": environment_monitor.summary("valid"),
            "conditions": {
                "codec": args.codec,
                "conversion": conversion_summary(args),
                "encoder_tune": args.encoder_tune,
                "backend": args.backend or "inherited",
                "backend_observed": backend_observed,
                "nvenc_preset": args.nvenc_preset,
                "replay_only": args.replay_only,
                "h264_coder_requested": (
                    args.h264_coder if args.codec == "h264" else "not_applicable"
                ),
                "h264_coder_observed": h264_coder_observed,
                "h264_coder_observed_from": h264_coder_source,
                "rate_control": rate_control_condition,
                "max_fps": args.max_fps,
                "gop": args.gop,
                "b_frames": args.b_frames,
                "pool": selected_pool(args),
                "verify_frame_ids": args.verify_frame_ids,
                "async_depth_requested": args.async_depth,
                "async_depth": selected_async_depth,
            },
            "stimulus": {
                "display": args.display,
                "pattern": args.pattern,
                "motion": args.motion,
                "settle_seconds": args.settle_seconds,
                "elapsed_seconds": source_line.get("elapsed-s"),
                "submitted": source_line.get("submitted"),
                "presented": source_line.get("presented"),
                "presented_fps": source_line.get("presented-fps"),
                "discarded": source_line.get("discarded"),
                "presented_id_gaps": source_line.get("presented-id-gaps"),
                "presented_id_gap_percent_of_source_span": source_gap_percent,
                "source_id_span": source_span,
                "first_id": source_first,
                "last_id": source_last,
                "first_refresh_ns": first_refresh_ns,
                "reported_next_refresh_us": source_line.get("next-refresh-us"),
            },
            "recording": {
                "mode": "replay_only" if args.replay_only else "recording",
                "exit_status": record_status,
                "seconds_requested": args.record_seconds,
                "codec": args.codec,
                "conversion": conversion_summary(args),
                "encoder_tune": args.encoder_tune,
                "backend": args.backend or "inherited",
                "backend_observed": backend_observed,
                "nvenc_preset": args.nvenc_preset,
                "h264_coder_requested": (
                    args.h264_coder if args.codec == "h264" else "not_applicable"
                ),
                "h264_coder_observed": h264_coder_observed,
                "h264_coder_observed_from": h264_coder_source,
                "rate_control": args.rate_control,
                "bitrate_mbps": args.bitrate_mbps if args.qp is None else None,
                "qp": args.qp,
                "max_fps": args.max_fps,
                "pool": selected_pool(args),
                "b_frames": args.b_frames,
                "async_depth_requested": args.async_depth,
                "async_depth": selected_async_depth,
                "captured": record_line.get("captured"),
                "distinct_presentation_times": record_line.get("distinct_presentation_times"),
                "submitted": record_line.get("submitted"),
                "encoded": record_line.get("encoded"),
                "muxed": record_line.get("muxed"),
                "muxed_video_frames": record_line.get("muxed_video_frames"),
                "muxed_packets_total": record_line.get("muxed_packets_total"),
                "capture_dropped": record_line.get("capture_dropped"),
                "capture_unarmed": record_line.get("capture_unarmed"),
                "capture_cancelled_at_stop": record_line.get("capture_cancelled_at_stop"),
                "pool_starvation_periods": record_line.get("pool_starvation_periods"),
                "pool_starvation_ms": record_line.get("pool_starvation_ms"),
                "packet_dropped": record_line.get("packet_dropped"),
                "output_bytes": 0 if args.replay_only else record_line.get("output_bytes"),
                "output_path": None if args.replay_only else str(output),
                "run_prefix": str(output) if args.replay_only else None,
            },
            **({"replay_export": replay_summary} if replay_summary is not None else {}),
            "recorder_telemetry": telemetry,
            "decoded_identity": None if args.replay_only else {
                "exit_status": decoder.returncode,
                "luma_threshold": args.decode_threshold,
                "frames": decode_line.get("analyzed_frames", decode_line.get("decoded_frames")),
                "total_decoded_frames": decode_line.get("decoded_frames"),
                "ignored_initial_frames": decode_line.get("ignored_initial_frames", 0),
                "first_id": record_first,
                "last_id": record_last,
                "duplicate_transitions": duplicates,
                "gap_transitions_between_adjacent_valid_frames": gap_transitions,
                "missing_ids_between_frames": missing_ids,
                "missing_ids_percent_of_overall_id_span": recorded_gap_percent,
                "backward_transitions": backwards,
                "black_frames": black_frames,
                "invalid_frames": invalid_frames,
                "invalid_digit_masks": invalid_masks,
                "valid_id_frames": valid_id_frames,
                "valid_segments": valid_segments,
                "invalid_spans": invalid_spans,
                "unknown_transitions_across_invalid_spans": unknown_transitions,
                "adjacent_valid_transitions": decode_line.get("adjacent_valid_transitions"),
                "mean_confidence": decode_line.get("mean_confidence"),
                "min_confidence": decode_line.get("min_confidence"),
            },
            "comparison": None if args.replay_only else {
                "recorded_id_range_within_presented_source_range": range_ok,
                "identity_validity_clean": identity_validity_clean,
                "source_id_span": source_span,
                "recorded_id_span": recorded_span,
                "source_id_missing_count": source_gaps,
                "source_id_missing_percent": source_gap_percent,
                "recorded_id_missing_count": missing_ids,
                "recorded_id_missing_percent_of_overall_id_span": recorded_gap_percent,
                "recorded_adjacent_valid_gap_transitions": gap_transitions,
                "recorded_invalid_spans": invalid_spans,
                "recorded_unknown_transitions_across_invalid_spans": unknown_transitions,
            },
            "artifacts": summarize_artifact_paths(paths, replay_only=args.replay_only),
        }
        paths["summary"].write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")

        def metric_mean(value: object) -> str:
            if isinstance(value, dict) and isinstance(value.get("mean"), (int, float)):
                return f"{value['mean']:.1f}"
            return "n/a"

        telemetry_fps = telemetry["per_second_fps"]
        encoder_util = telemetry["encoder_utilization_percent"]
        rate_control_label = (
            f"CQP/QP{args.qp}" if args.qp is not None
            else f"bitrate/{args.bitrate_mbps}Mbps"
        )
        recording_counts_console = format_recording_count_label(
            summary["recording"]["captured"],
            summary["recording"]["submitted"],
            summary["recording"]["encoded"],
            summary["recording"]["muxed_packets_total"],
        )
        mux_scope_console = " (video+audio streams)" if args.audio == "on" else ""
        h264_coder_console = (
            f"h264-coder=requested:{args.h264_coder} observed:{h264_coder_observed} "
            f"({h264_coder_source or 'no saved H.264 stream'})"
            if args.codec == "h264" else "h264-coder=not_applicable"
        )
        backend_console = (
            f"backend=requested:{args.backend or 'inherited'} "
            f"observed:{backend_observed or 'unknown'}"
        )
        if args.replay_only:
            decode_console = "primary-decode=n/a (replay-only)"
            content_console = "frame-content=n/a"
            output_console = f"run-prefix={output}"
        else:
            decode_console = (
                f"decode={summary['decoded_identity']['frames']} frames "
                f"(valid-IDs={valid_id_frames}, segments={valid_segments}, "
                f"IDs {record_first}..{record_last}, adjacent-valid-missing={missing_ids}/"
                f"{recorded_span} overall-ID-span "
                f"({recorded_gap_percent if recorded_gap_percent is not None else 'n/a'}%), "
                f"unknown-transitions-across-invalid={unknown_transitions} over {invalid_spans} spans, "
                f"black={black_frames}, invalid-nonblack-frames={invalid_frames}, "
                f"invalid-digit-masks={invalid_masks}, "
                f"duplicates={duplicates}, backwards={backwards})"
            )
            content_console = (
                f"frame-content={'clean' if frame_content_clean else 'ANOMALY'}"
            )
            output_console = f"output={output}"

        print(
            "result: "
            f"codec={args.codec}/{args.encoder_tune} "
            f"{backend_console} "
            f"{h264_coder_console} "
            f"rate-control={rate_control_label} "
            f"max-fps={args.max_fps or 'uncapped'}; "
            f"source={summary['stimulus']['presented']} presented "
            f"({summary['stimulus']['presented_fps']} fps, IDs {source_first}..{source_last}); "
            f"source-ID-missing={source_gaps}/{source_span} ({source_gap_percent if source_gap_percent is not None else 'n/a'}%); "
            f"record={recording_counts_console}{mux_scope_console} "
            f"(pool-starvation={summary['recording']['pool_starvation_periods']} periods/"
            f"{summary['recording']['pool_starvation_ms']} ms, "
            f"capture-unarmed={summary['recording'].get('capture_unarmed')}); "
            f"{decode_console}; "
            f"{content_console}; "
            f"source-continuity={source_continuity_label}; "
            f"steady-fps(c/e/m)={metric_mean(telemetry_fps['captured'])}/"
            f"{metric_mean(telemetry_fps['encoded'])}/"
            f"{metric_mean(telemetry_fps['muxed'])}; "
            f"encoder-util={metric_mean(encoder_util)}%; "
            f"pool={telemetry['gpu_frame_pool_high_water']}/"
            f"{telemetry['gpu_frame_pool_capacity']}; "
            f"output-bytes={record_line.get('output_bytes') if not args.replay_only else 0}; "
            f"{output_console}",
            flush=True,
        )
        if replay_summary is not None:
            replay_identity = replay_summary["decoded_identity"]
            replay_probe = replay_summary["packet_probe"]
            print(
                "replay: "
                f"valid={replay_summary['validation']['valid']}; "
                f"packets={replay_probe.get('packet_count', 0)} "
                f"(video={replay_probe.get('video_packet_count', 0)}); "
                f"IDs={replay_identity.get('first_id')}..{replay_identity.get('last_id')} "
                f"(span={replay_identity.get('id_span')}); "
                f"black={replay_identity.get('black_frames')}, "
                f"invalid={replay_identity.get('invalid_frames')}; "
                f"output={replay_summary['output_path']}",
                flush=True,
            )
        print(f"summary: {paths['summary']}", flush=True)
        return_code = (
            0 if (decoder is None or decoder.returncode == 0) and
            (replay_summary is None or replay_summary["validation"]["valid"]) and
            recording_identity_accepted(source_continuity_label, frame_content_clean)
            else 1
        )
        if decoder is not None and decoder.stdout:
            print("decoder: " + " | ".join(decoder.stdout.splitlines()[-2:]), flush=True)
        return return_code
    except InvalidBenchmarkEnvironment as error:
        terminate_children(stimulus, recorder)
        if reader is not None:
            reader.join(timeout=2.0)
        write_invalid_environment_summary(
            paths, args, environment_monitor, str(error), recorder
        )
        raise
    finally:
        terminate_children(stimulus, recorder)
        if reader is not None:
            reader.join(timeout=2.0)
        fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
        lock.close()


def main() -> int:
    args = parse_args()
    try:
        return run_benchmark(args)
    except KeyboardInterrupt:
        print("interrupted; child process groups were stopped", file=sys.stderr)
        return 130
    except InvalidBenchmarkEnvironment as error:
        print(f"benchmark invalid environment: {error}", file=sys.stderr)
        return 2
    except Exception as error:  # concise user-facing failure; logs retain details
        print(f"benchmark failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
