#!/usr/bin/env python3
"""Validate that a UI pause does not pause its separately running recorder.

The runner starts an isolated Fluxrec UI configured for a known recorder and
output directory. Its validation-only auto-start clicks Start recording; the
runner then starts a locked, fullscreen source-ID stimulus, SIGSTOPs only that
exact UI process for a short interval, resumes it in a finally block, stops the
recorder while the stimulus is still live, and decodes the resulting source IDs.
"""

from __future__ import annotations

import argparse
from collections.abc import Callable
import json
import math
import os
from pathlib import Path
import queue
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from datetime import datetime, timezone

import controlled_benchmark as benchmark


ROOT = Path(__file__).resolve().parents[1]
PROC = Path("/proc")
DEFAULT_UI = ROOT / "build-root/ui/fluxrec-ui"
DEFAULT_RECORDER = ROOT / "build-root/engine/fluxrec-record"
DEFAULT_STIMULUS = ROOT / "stimulus/build/fluxrec-stimulus"
DEFAULT_DECODER = ROOT / "build-root/tools/fluxrec-decode-stimulus-ids"
POLL_SECONDS = 0.05
WORKSPACE_POLL_SECONDS = 0.25


class ValidationError(RuntimeError):
    """The live validation cannot continue safely or produce valid evidence."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Pause one isolated Fluxrec UI process while its UI-started recorder "
            "continues capturing a locked fullscreen source-ID stimulus."
        )
    )
    parser.add_argument("--display", default="DP-1")
    parser.add_argument("--artifacts-dir", type=Path, default=ROOT / ".validation")
    parser.add_argument("--ui-binary", type=Path, default=DEFAULT_UI)
    parser.add_argument("--recorder-binary", type=Path, default=DEFAULT_RECORDER)
    parser.add_argument("--stimulus-binary", type=Path, default=DEFAULT_STIMULUS)
    parser.add_argument("--decoder-binary", type=Path, default=DEFAULT_DECODER)
    parser.add_argument("--stimulus-seconds", type=float, default=16.0)
    parser.add_argument(
        "--motion", type=float, default=0.20,
        help="plasma phase change per rendered frame (0 to 1000; default: 0.20)",
    )
    parser.add_argument("--pause-after", type=float, default=3.0,
                        help="seconds after first stimulus presentation before pausing the UI")
    parser.add_argument("--pause-seconds", type=float, default=5.0)
    parser.add_argument("--post-resume-seconds", type=float, default=2.0,
                        help="continue recording after SIGCONT before finalizing the MKV")
    parser.add_argument("--post-stop-observe-seconds", type=float, default=1.5,
                        help="keep monitoring the live stimulus after recorder shutdown")
    parser.add_argument("--ui-start-timeout", type=float, default=15.0,
                        help="time allowed for the isolated UI to auto-start recording")
    parser.add_argument("--ready-timeout", type=float, default=10.0)
    parser.add_argument("--settle-seconds", type=float, default=0.20)
    parser.add_argument("--decode-threshold", type=int, default=100,
                        help="source-ID glyph luma threshold (default: 100)")
    parser.add_argument("--decode-debug-frames", type=int, default=0)
    parser.add_argument("--lock-file", type=Path,
                        help="lock location (default: XDG_RUNTIME_DIR or /tmp)")
    return parser.parse_args()


def resolve_executable(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise FileNotFoundError(f"{label} executable is missing or not executable: {resolved}")
    return resolved


def validate_args(args: argparse.Namespace) -> None:
    for name in (
        "stimulus_seconds", "motion", "pause_after", "pause_seconds", "post_resume_seconds",
        "post_stop_observe_seconds", "settle_seconds", "ui_start_timeout",
    ):
        if not math.isfinite(getattr(args, name)):
            raise ValueError(f"--{name.replace('_', '-')} must be finite")
    if args.stimulus_seconds < 8.0:
        raise ValueError("--stimulus-seconds must be at least 8")
    if not 0.0 <= args.motion <= 1000.0:
        raise ValueError("--motion must be between 0 and 1000")
    if args.pause_seconds <= 0 or args.pause_seconds > 30:
        raise ValueError("--pause-seconds must be greater than 0 and at most 30")
    if args.pause_after < 0 or args.settle_seconds < 0:
        raise ValueError("--pause-after and --settle-seconds must not be negative")
    if args.post_resume_seconds <= 0:
        raise ValueError("--post-resume-seconds must be positive")
    if args.post_stop_observe_seconds <= 0:
        raise ValueError("--post-stop-observe-seconds must be positive")
    required_window = (
        args.settle_seconds + args.pause_after + args.pause_seconds +
        args.post_resume_seconds + args.post_stop_observe_seconds + 1.0
    )
    if required_window >= args.stimulus_seconds:
        raise ValueError(
            "stimulus must continue for at least one second after the post-stop observation"
        )
    if args.ui_start_timeout <= 0:
        raise ValueError("--ui-start-timeout must be positive")
    if not 1 <= args.decode_threshold <= 254:
        raise ValueError("--decode-threshold must be between 1 and 254")
    if args.decode_debug_frames < 0:
        raise ValueError("--decode-debug-frames must not be negative")


def proc_stat_fields(text: str) -> list[str]:
    """Return fields after pid/comm, handling spaces and ')' in comm."""
    closing = text.rfind(")")
    opening = text.find("(")
    if opening <= 0 or closing <= opening:
        raise ValueError("malformed /proc/PID/stat record")
    return text[closing + 1:].split()


def proc_start_ticks(text: str) -> int:
    # The post-comm list starts at field 3 (state), so field 22 is offset 19.
    fields = proc_stat_fields(text)
    if len(fields) <= 19:
        raise ValueError("/proc/PID/stat has no start-time field")
    return int(fields[19])


def proc_identity(pid: int, proc_root: Path = PROC) -> dict[str, object]:
    directory = proc_root / str(pid)
    raw_stat = (directory / "stat").read_text(encoding="utf-8")
    fields = proc_stat_fields(raw_stat)
    status = (directory / "status").read_text(encoding="utf-8")
    uid_line = next((line for line in status.splitlines() if line.startswith("Uid:")), None)
    if uid_line is None:
        raise ValidationError(f"could not read effective UID for process {pid}")
    uid_values = uid_line.split()[1:]
    if len(uid_values) < 2:
        raise ValidationError(f"invalid UID record for process {pid}")
    executable = (directory / "exe").resolve(strict=True)
    argv = [os.fsdecode(part) for part in (directory / "cmdline").read_bytes().split(b"\0") if part]
    environment = {}
    for item in (directory / "environ").read_bytes().split(b"\0"):
        if b"=" in item:
            name, value = item.split(b"=", 1)
            environment[os.fsdecode(name)] = os.fsdecode(value)
    return {
        "pid": pid,
        "state": fields[0],
        "ppid": int(fields[1]),
        "uid": int(uid_values[1]),
        "start_ticks": proc_start_ticks(raw_stat),
        "exe": executable,
        "argv": argv,
        "environment": environment,
    }


def exact_ui_identity(
    pid: int,
    *,
    expected_executable: Path,
    expected_start_ticks: int,
    validation_id: str,
    proc_root: Path = PROC,
) -> dict[str, object]:
    identity = proc_identity(pid, proc_root)
    if identity["uid"] != os.getuid():
        raise ValidationError("refusing to signal a UI owned by another user")
    if identity["exe"] != expected_executable:
        raise ValidationError("refusing to signal a process whose executable changed")
    if identity["start_ticks"] != expected_start_ticks:
        raise ValidationError("refusing to signal a reused PID")
    if identity["environment"].get("FLUXREC_UI_FREEZE_VALIDATION_ID") != validation_id:
        raise ValidationError("refusing to signal a UI without this run's private marker")
    if "--validation-auto-start" not in identity["argv"]:
        raise ValidationError("refusing to signal a UI not launched in validation auto-start mode")
    return identity


def children_of(pid: int, proc_root: Path = PROC) -> list[int]:
    path = proc_root / str(pid) / "task" / str(pid) / "children"
    try:
        return [int(value) for value in path.read_text(encoding="ascii").split()]
    except FileNotFoundError:
        return []


def all_fluxrec_pids(proc_root: Path = PROC) -> dict[str, list[int]]:
    found: dict[str, list[int]] = {"fluxrec-record": [], "fluxrec-stimulus": []}
    for entry in proc_root.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            identity = proc_identity(int(entry.name), proc_root)
        except (FileNotFoundError, ProcessLookupError, PermissionError, OSError, ValueError):
            continue
        name = Path(identity["exe"]).name
        if name in found:
            found[name].append(int(entry.name))
    return found


def flag_values(argv: list[str]) -> dict[str, str]:
    values: dict[str, str] = {}
    index = 0
    while index < len(argv):
        if argv[index] == "--until-signal":
            index += 1
            continue
        if argv[index].startswith("--") and index + 1 < len(argv):
            values[argv[index]] = argv[index + 1]
            index += 2
        else:
            index += 1
    return values


def validate_recorder_child(
    argv: list[str],
    *,
    recorder_binary: Path,
    output_directory: Path,
    display: str,
) -> dict[str, object]:
    if not argv:
        raise ValidationError("the UI recorder process has an empty command line")
    values = flag_values(argv[1:])
    required = {
        "--until-signal": None,
        "--display": display,
        "--conversion": "two-pass",
        "--codec": "h264",
        "--backend": "nvenc",
        "--nvenc-preset": "p1",
        "--async-depth": "4",
        "--b-frames": "0",
        "--pool": "48",
    }
    for flag, expected in required.items():
        if flag not in argv[1:]:
            raise ValidationError(f"UI recorder is missing required argument {flag}")
        if expected is not None and values.get(flag) != expected:
            raise ValidationError(
                f"UI recorder argument {flag} is {values.get(flag)!r}, expected {expected!r}"
            )
    if values.get("--audio") != "off":
        raise ValidationError("the isolated UI recorder must have audio disabled")
    output_text = values.get("--output")
    socket_text = values.get("--preview-socket")
    if not output_text or not socket_text:
        raise ValidationError("UI recorder has no output file or preview socket")
    output = Path(output_text).resolve()
    if output.parent != output_directory.resolve() or output.suffix.lower() != ".mkv":
        raise ValidationError("UI recorder output path is outside the isolated run directory")
    socket_path = Path(socket_text)
    return {"output": output, "preview_socket": socket_path}


def validate_preview_socket(path: Path, runtime_dir: Path, uid: int) -> dict[str, object]:
    resolved_runtime = runtime_dir.resolve()
    try:
        resolved = path.resolve(strict=True)
        parent = resolved.parent
        parent.relative_to(resolved_runtime)
    except (OSError, ValueError) as error:
        raise ValidationError("preview socket is outside this user's runtime directory") from error
    socket_status = resolved.stat()
    directory_status = parent.stat()
    if not stat.S_ISSOCK(socket_status.st_mode):
        raise ValidationError("the UI recorder preview path is not a Unix socket")
    if socket_status.st_uid != uid or directory_status.st_uid != uid:
        raise ValidationError("the UI recorder preview socket is not owned by this user")
    if (directory_status.st_mode & 0o777) != 0o700:
        raise ValidationError("the UI recorder preview directory is not private (mode 0700)")
    if not parent.name.startswith("fluxrec-preview-") or resolved.name != "preview.sock":
        raise ValidationError("the UI recorder preview socket has an unexpected path")
    return {
        "path": str(resolved),
        "owner_uid": socket_status.st_uid,
        "directory_mode": oct(directory_status.st_mode & 0o777),
    }


def write_isolated_settings(
    config_home: Path,
    *,
    recorder_binary: Path,
    output_directory: Path,
    display: str,
) -> Path:
    settings_path = config_home / "Fluxrec" / "Fluxrec.conf"
    settings_path.parent.mkdir(parents=True, exist_ok=True)
    # QSettings' INI format is deliberate: the runner uses a fresh XDG config
    # root, so no personal Fluxrec preferences are read or modified.
    settings_path.write_text(
        "\n".join([
            "[recorder]",
            f"executable={recorder_binary}",
            "",
            "[recording]",
            f"outputDirectory={output_directory}",
            "encoderBackend=nvenc",
            "nvencPreset=p1",
            "nvencBFrames=0",
            "codec=h264",
            "bitrateMbps=50",
            "gopSize=120",
            "asyncDepth=8",
            "nvencAsyncDepth=4",
            "",
            "[capture]",
            f"displayName={display}",
            "maxFps=0",
            "poolSize=48",
            "",
            "[audio]",
            "enabled=false",
            "",
            "[replay]",
            "historySeconds=0",
            "",
        ]),
        encoding="utf-8",
    )
    return settings_path


def atomic_summary(path: Path, value: dict[str, object]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def read_proc_size(path: Path) -> int:
    try:
        return path.stat().st_size
    except FileNotFoundError:
        return 0


def assert_only_owned_recorder(pid: int) -> None:
    processes = all_fluxrec_pids()
    others = [other for other in processes["fluxrec-record"] if other != pid]
    if others:
        raise ValidationError(f"another recorder appeared during the test: pids={others}")
    if processes["fluxrec-stimulus"]:
        raise ValidationError(
            f"another stimulus appeared during the test: pids={processes['fluxrec-stimulus']}"
        )


def wait_for_ui_recorder(
    ui: subprocess.Popen,
    *,
    ui_executable: Path,
    ui_start_ticks: int,
    validation_id: str,
    recorder_binary: Path,
    output_directory: Path,
    display: str,
    timeout: float,
) -> tuple[int, int, dict[str, object]]:
    print(
        f"UI test window is ready (pid={ui.pid}); waiting for its opt-in Start-button "
        "auto-start and recorder child.",
        flush=True,
    )
    deadline = time.monotonic() + timeout
    last_problem: str | None = None
    while time.monotonic() < deadline:
        if ui.poll() is not None:
            raise ValidationError(f"isolated UI exited before recording (status {ui.returncode})")
        exact_ui_identity(
            ui.pid, expected_executable=ui_executable,
            expected_start_ticks=ui_start_ticks, validation_id=validation_id,
        )
        matches: list[tuple[int, dict[str, object], int]] = []
        for child_pid in children_of(ui.pid):
            try:
                identity = proc_identity(child_pid)
            except (FileNotFoundError, ProcessLookupError, PermissionError, OSError, ValueError):
                continue
            if identity["exe"] == recorder_binary:
                matches.append((child_pid, identity, int(identity["start_ticks"])))
        if len(matches) > 1:
            raise ValidationError("the isolated UI started multiple recorder children")
        if matches:
            pid, child_identity, child_start_ticks = matches[0]
            if child_identity["ppid"] != ui.pid or child_identity["uid"] != os.getuid():
                raise ValidationError("recorder is not a same-user direct child of the isolated UI")
            try:
                evidence = validate_recorder_child(
                    child_identity["argv"], recorder_binary=recorder_binary,
                    output_directory=output_directory, display=display,
                )
                return pid, child_start_ticks, evidence
            except ValidationError as error:
                last_problem = str(error)
        time.sleep(POLL_SECONDS)
    detail = f"; last observed child issue: {last_problem}" if last_problem else ""
    raise TimeoutError(f"timed out waiting for Start recording in the isolated UI{detail}")


def wait_for_proc_state(pid: int, expected_states: set[str], timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            state = str(proc_identity(pid)["state"])
        except (FileNotFoundError, ProcessLookupError, PermissionError, OSError, ValueError):
            return False
        if state in expected_states:
            return True
        time.sleep(0.01)
    return False


def signal_owned_ui(
    pid: int,
    sig: signal.Signals,
    *,
    executable: Path,
    start_ticks: int,
    validation_id: str,
) -> None:
    exact_ui_identity(
        pid, expected_executable=executable,
        expected_start_ticks=start_ticks, validation_id=validation_id,
    )
    os.kill(pid, sig)


def owned_recorder_identity(
    pid: int,
    start_ticks: int,
    ui_pid: int,
    recorder_binary: Path,
    output: Path,
    socket_path: Path,
) -> dict[str, object]:
    identity = proc_identity(pid)
    if identity["uid"] != os.getuid() or identity["exe"] != recorder_binary:
        raise ValidationError("recorder identity changed; refusing to signal it")
    if identity["start_ticks"] != start_ticks or identity["ppid"] != ui_pid:
        raise ValidationError("recorder is no longer the exact UI child from this run")
    evidence = validate_recorder_child(
        identity["argv"], recorder_binary=recorder_binary,
        output_directory=output.parent, display=flag_values(identity["argv"][1:])["--display"],
    )
    if evidence["output"] != output or evidence["preview_socket"] != socket_path:
        raise ValidationError("recorder output or preview socket changed during the test")
    return identity


def check_stimulus_while_paused(
    stimulus: subprocess.Popen,
    lines: queue.Queue[str | None],
    monitor: benchmark.StimulusHealthMonitor,
    duration: float,
    *,
    recorder_check,
) -> None:
    deadline = time.monotonic() + duration
    next_workspace_check = 0.0
    while time.monotonic() < deadline:
        now = time.monotonic()
        if stimulus.poll() is not None:
            raise ValidationError(f"stimulus exited during UI pause (status {stimulus.returncode})")
        monitor.check_feedback(now)
        if now >= next_workspace_check:
            monitor.check_workspace(now, force=True)
            next_workspace_check = now + WORKSPACE_POLL_SECONDS
        recorder_check()
        remaining = deadline - now
        try:
            line = lines.get(timeout=min(POLL_SECONDS, remaining))
        except queue.Empty:
            continue
        if line is None:
            raise ValidationError("stimulus feedback stream closed during UI pause")
        monitor.observe_line(line, time.monotonic())


def stop_owned_recorder(
    pid: int,
    start_ticks: int,
    ui_pid: int,
    recorder_binary: Path,
    output: Path,
    socket_path: Path,
    timeout: float = 12.0,
    during_wait: Callable[[], None] | None = None,
) -> str:
    try:
        owned_recorder_identity(
            pid, start_ticks, ui_pid, recorder_binary, output, socket_path,
        )
    except (FileNotFoundError, ProcessLookupError):
        return "already-exited"
    os.kill(pid, signal.SIGINT)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            identity = proc_identity(pid)
        except (FileNotFoundError, ProcessLookupError):
            return "exited-after-sigint"
        if identity["start_ticks"] != start_ticks:
            return "pid-reused"
        if during_wait is not None:
            during_wait()
        time.sleep(0.05)
    # Escalation still targets only the same verified recorder instance.
    owned_recorder_identity(pid, start_ticks, ui_pid, recorder_binary, output, socket_path)
    os.kill(pid, signal.SIGTERM)
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        try:
            identity = proc_identity(pid)
        except (FileNotFoundError, ProcessLookupError):
            return "exited-after-sigterm"
        if identity["start_ticks"] != start_ticks:
            return "pid-reused"
        if during_wait is not None:
            during_wait()
        time.sleep(0.05)
    owned_recorder_identity(pid, start_ticks, ui_pid, recorder_binary, output, socket_path)
    os.kill(pid, signal.SIGKILL)
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        if not Path(f"/proc/{pid}").exists():
            return "killed-after-timeout"
        time.sleep(0.05)
    return "still-present-after-sigkill"


def monitor_until_stimulus_exit(
    stimulus: subprocess.Popen,
    lines: queue.Queue[str | None],
    monitor: benchmark.StimulusHealthMonitor,
) -> None:
    next_workspace_check = 0.0
    while stimulus.poll() is None:
        now = time.monotonic()
        monitor.check_feedback(now)
        if now >= next_workspace_check:
            monitor.check_workspace(now, force=True)
            next_workspace_check = now + WORKSPACE_POLL_SECONDS
        try:
            line = lines.get(timeout=POLL_SECONDS)
        except queue.Empty:
            continue
        if line is None:
            if stimulus.poll() is None:
                raise ValidationError("stimulus feedback stream closed before stimulus exit")
            break
        monitor.observe_line(line, time.monotonic())
    if stimulus.returncode not in (0, None):
        raise ValidationError(f"stimulus exited with status {stimulus.returncode}")
    while True:
        try:
            line = lines.get_nowait()
        except queue.Empty:
            break
        if line is not None:
            monitor.observe_line(line, time.monotonic())


def run(args: argparse.Namespace) -> int:
    validate_args(args)
    ui_binary = resolve_executable(args.ui_binary, "UI")
    recorder_binary = resolve_executable(args.recorder_binary, "recorder")
    stimulus_binary = resolve_executable(args.stimulus_binary, "stimulus")
    decoder_binary = resolve_executable(args.decoder_binary, "decoder")
    output_root = args.artifacts_dir.expanduser().resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    run_directory = output_root / f"ui-freeze-{run_id}"
    run_directory.mkdir(mode=0o700)
    recording_directory = run_directory / "recordings"
    recording_directory.mkdir(mode=0o700)
    log_paths = {
        "summary": run_directory / "summary.json",
        "ui_stdout": run_directory / "ui.stdout.log",
        "ui_stderr": run_directory / "ui.stderr.log",
        "stimulus": run_directory / "stimulus.log",
        "decoder": run_directory / "decoder.log",
    }
    runtime_text = os.environ.get("XDG_RUNTIME_DIR")
    if not runtime_text:
        raise ValidationError("XDG_RUNTIME_DIR is required to verify the UI preview socket")
    runtime_dir = Path(runtime_text).resolve(strict=True)
    validation_id = uuid.uuid4().hex
    config_home = run_directory / "config"
    settings_path = write_isolated_settings(
        config_home, recorder_binary=recorder_binary,
        output_directory=recording_directory, display=args.display,
    )
    environment = os.environ.copy()
    environment["XDG_CONFIG_HOME"] = str(config_home)
    environment["FLUXREC_UI_FREEZE_VALIDATION_ID"] = validation_id
    lock_base = Path(os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir())
    lock = benchmark.acquire_lock(
        args.lock_file.expanduser().resolve() if args.lock_file else
        lock_base / "fluxrec-ui-freeze-benchmark.lock"
    )
    active = all_fluxrec_pids()
    if active["fluxrec-record"] or active["fluxrec-stimulus"]:
        lock.close()
        raise ValidationError(f"another recorder or stimulus is active: {active}")
    with log_paths["ui_stdout"].open("w", encoding="utf-8") as ui_stdout, \
            log_paths["ui_stderr"].open("w", encoding="utf-8") as ui_stderr:
        try:
            ui = subprocess.Popen(
                [str(ui_binary), "--validation-auto-start"], stdin=subprocess.DEVNULL,
                stdout=ui_stdout, stderr=ui_stderr,
                env=environment, start_new_session=False,
            )
        except BaseException:
            lock.close()
            raise
        ui_start_ticks: int | None = None
        summary: dict[str, object] = {
            "status": "running",
            "run_id": run_id,
            "conditions": {
                "display": args.display,
                "backend": "nvenc",
                "codec": "h264",
                "preset": "p1",
                "async_depth": 4,
                "b_frames": 0,
                "pool": 48,
                "pattern": "plasma",
                "motion": args.motion,
                "stimulus_seconds": args.stimulus_seconds,
                "ui_pause_seconds_requested": args.pause_seconds,
                "post_resume_record_seconds": args.post_resume_seconds,
                "post_shutdown_observation_seconds": args.post_stop_observe_seconds,
            },
            "ui": {
                "pid": ui.pid,
                "executable": str(ui_binary),
                "settings_path": str(settings_path),
                "settings_isolated": True,
            },
            "artifacts": {key: str(path) for key, path in log_paths.items()},
            "recorder_telemetry": {
                "available": False,
                "reason": (
                    "The Qt UI consumes the recorder JSON stream internally and does not "
                    "write a telemetry sidecar. The UI process is paused during this check."
                ),
            },
            "preview": None,
            "stimulus": None,
            "pause": None,
            "recording": None,
            "decoded_identity": None,
        }
        stimulus: subprocess.Popen | None = None
        stimulus_reader: threading.Thread | None = None
        recorder_pid: int | None = None
        recorder_start_ticks: int | None = None
        recorder_output: Path | None = None
        preview_socket: Path | None = None
        ui_was_stopped = False
        valid = False
        failure: str | None = None
        try:
            ui_start_ticks = int(proc_identity(ui.pid)["start_ticks"])
            print(f"Starting isolated UI pid={ui.pid} with private settings.", flush=True)
            recorder_pid, recorder_start_ticks, child_evidence = wait_for_ui_recorder(
                ui, ui_executable=ui_binary, ui_start_ticks=ui_start_ticks,
                validation_id=validation_id, recorder_binary=recorder_binary,
                output_directory=recording_directory, display=args.display,
                timeout=args.ui_start_timeout,
            )
            recorder_output = child_evidence["output"]
            preview_socket = child_evidence["preview_socket"]
            assert isinstance(recorder_output, Path) and isinstance(preview_socket, Path)
            preview_summary = validate_preview_socket(preview_socket, runtime_dir, os.getuid())
            summary["preview"] = preview_summary
            summary["recording"] = {
                "pid": recorder_pid,
                "output": str(recorder_output),
                "started_monotonic": recorder_start_ticks / os.sysconf("SC_CLK_TCK"),
                "observed_monotonic": time.monotonic(),
            }
            assert_only_owned_recorder(recorder_pid)

            stimulus_command = [
                str(stimulus_binary), "--fullscreen", "--output", args.display,
                "--pattern", "plasma", "--motion", str(args.motion),
                "--duration", str(args.stimulus_seconds), "--lock-pattern-motion",
            ]
            stimulus = subprocess.Popen(
                stimulus_command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1,
                start_new_session=True,
            )
            lines: queue.Queue[str | None] = queue.Queue()
            assert stimulus.stderr is not None
            stimulus_reader = threading.Thread(
                target=benchmark.pump_stimulus_stderr,
                args=(stimulus.stderr, log_paths["stimulus"], lines), daemon=True,
            )
            stimulus_reader.start()
            first_id, refresh_ns = benchmark.wait_for_first_presentation(
                stimulus, lines, args.ready_timeout, "plasma", args.motion,
            )
            stimulus_ready_monotonic = time.monotonic()
            monitor = benchmark.StimulusHealthMonitor(
                display=args.display, stimulus_pid=stimulus.pid,
                expected_pattern="plasma", expected_motion=args.motion,
                started_at=stimulus_ready_monotonic,
            )
            summary["stimulus"] = {
                "pid": stimulus.pid,
                "pattern": "plasma",
                "motion": args.motion,
                "first_presented_id": first_id,
                "refresh_period_ns": refresh_ns,
            }
            if args.settle_seconds:
                benchmark.monitor_stimulus_interval(
                    stimulus, lines, monitor, args.settle_seconds, recording=False,
                )
            monitor.check_workspace(time.monotonic(), force=True)
            monitor.check_feedback(time.monotonic())
            benchmark.monitor_stimulus_interval(
                stimulus, lines, monitor, args.pause_after, recording=True,
            )

            exact_ui_identity(
                ui.pid, expected_executable=ui_binary,
                expected_start_ticks=ui_start_ticks, validation_id=validation_id,
            )
            recorder_check = lambda: owned_recorder_identity(
                recorder_pid, recorder_start_ticks, ui.pid, recorder_binary,
                recorder_output, preview_socket,
            )
            recorder_check()
            validate_preview_socket(preview_socket, runtime_dir, os.getuid())
            output_size_before = read_proc_size(recorder_output)
            pause_started = time.monotonic()
            signal_owned_ui(
                ui.pid, signal.SIGSTOP, executable=ui_binary,
                start_ticks=ui_start_ticks, validation_id=validation_id,
            )
            ui_was_stopped = True
            stopped = wait_for_proc_state(ui.pid, {"T", "t"}, timeout=1.0)
            if not stopped:
                raise ValidationError("the exact UI process did not enter a stopped state")
            print(
                f"Paused only UI pid={ui.pid} for {args.pause_seconds:.1f}s; "
                f"recorder pid={recorder_pid} remains separate.", flush=True,
            )
            try:
                check_stimulus_while_paused(
                    stimulus, lines, monitor, args.pause_seconds,
                    recorder_check=recorder_check,
                )
                validate_preview_socket(preview_socket, runtime_dir, os.getuid())
            finally:
                # Resume the same process incarnation even when monitoring fails.
                signal_owned_ui(
                    ui.pid, signal.SIGCONT, executable=ui_binary,
                    start_ticks=ui_start_ticks, validation_id=validation_id,
                )
                ui_was_stopped = False
            resumed = wait_for_proc_state(ui.pid, {"R", "S", "D", "I"}, timeout=2.0)
            pause_ended = time.monotonic()
            output_size_after = read_proc_size(recorder_output)
            summary["pause"] = {
                "requested_seconds": args.pause_seconds,
                "actual_seconds": pause_ended - pause_started,
                "ui_stopped": stopped,
                "ui_resumed": resumed,
                "signal_target_pid": ui.pid,
                "recorder_pid": recorder_pid,
                "output_bytes_before": output_size_before,
                "output_bytes_after": output_size_after,
                "output_grew_while_ui_stopped": output_size_after > output_size_before,
            }
            if not resumed:
                raise ValidationError("the Fluxrec UI did not resume after SIGCONT")
            benchmark.monitor_stimulus_interval(
                stimulus, lines, monitor, args.post_resume_seconds, recording=True,
            )
            if stimulus.poll() is not None:
                raise ValidationError("stimulus ended before the recorder could be finalized")
            exact_ui_identity(
                ui.pid, expected_executable=ui_binary,
                expected_start_ticks=ui_start_ticks, validation_id=validation_id,
            )
            recorder_check()
            validate_preview_socket(preview_socket, runtime_dir, os.getuid())
            monitor.check_workspace(time.monotonic(), force=True)
            presented_total_before_shutdown = monitor.last_presented_total
            if presented_total_before_shutdown is None:
                raise ValidationError("no presentation total was available before recorder shutdown")

            def monitor_recorder_shutdown() -> None:
                benchmark.monitor_stimulus_interval(
                    stimulus, lines, monitor, 0.05, recording=True,
                )

            recorder_shutdown_started = time.monotonic()
            recorder_stop_status = stop_owned_recorder(
                recorder_pid, recorder_start_ticks, ui.pid,
                recorder_binary, recorder_output, preview_socket,
                during_wait=monitor_recorder_shutdown,
            )
            # Reap direct child after its UI parent receives QProcess completion.
            recorder_deadline = time.monotonic() + 10.0
            while time.monotonic() < recorder_deadline:
                if not Path(f"/proc/{recorder_pid}").exists():
                    break
                time.sleep(0.05)
            summary["recording"]["shutdown"] = recorder_stop_status
            summary["recording"]["shutdown_started_monotonic"] = recorder_shutdown_started
            summary["recording"]["post_resume_record_elapsed_seconds"] = (
                recorder_shutdown_started - pause_ended
            )
            summary["recording"]["output_bytes"] = read_proc_size(recorder_output)
            if not recorder_output.is_file() or recorder_output.stat().st_size == 0:
                raise ValidationError("UI recorder finalized no non-empty MKV file")
            if recorder_stop_status != "exited-after-sigint":
                raise ValidationError(
                    f"UI recorder did not exit cleanly after SIGINT ({recorder_stop_status})"
                )

            benchmark.monitor_stimulus_interval(
                stimulus, lines, monitor, args.post_stop_observe_seconds, recording=True,
            )
            presented_total_after_shutdown = monitor.last_presented_total
            source_advanced_after_stop = (
                presented_total_after_shutdown is not None and
                presented_total_after_shutdown > presented_total_before_shutdown
            )
            summary["recording"]["source_total_before_shutdown"] = presented_total_before_shutdown
            summary["recording"]["source_total_after_shutdown"] = presented_total_after_shutdown
            summary["recording"]["stimulus_advanced_after_shutdown"] = source_advanced_after_stop
            if not source_advanced_after_stop:
                raise ValidationError(
                    "stimulus presentation did not advance after recorder shutdown"
                )

            monitor_until_stimulus_exit(stimulus, lines, monitor)
            stimulus_status = stimulus.wait(timeout=10.0)
            if stimulus_reader is not None:
                stimulus_reader.join(timeout=2.0)
            if stimulus_status != 0:
                raise ValidationError(f"stimulus exited with status {stimulus_status}")
            source_log = log_paths["stimulus"].read_text(encoding="utf-8")
            source_final = benchmark.find_summary_line(source_log, "final:")
            if not source_final:
                raise ValidationError("stimulus did not write its final source counters")
            source_first = source_final.get("source-first-id")
            source_last = source_final.get("source-last-id")
            summary["stimulus"]["source_summary"] = source_final
            summary["stimulus"]["health"] = monitor.summary("valid")

            # Skip pre-stimulus desktop frames based on the child process start
            # timestamp; a small margin covers startup and the fullscreen swap.
            child_start_monotonic = recorder_start_ticks / os.sysconf("SC_CLK_TCK")
            initial_skip = max(
                0,
                math.ceil(max(0.0, stimulus_ready_monotonic - child_start_monotonic) * 380.0) + 18,
            )
            summary["recording"]["pre_stimulus_frames_skipped"] = initial_skip

            decoded = subprocess.run(
                [str(decoder_binary), "--threshold", str(args.decode_threshold),
                 "--skip-initial-frames", str(initial_skip),
                 *( ["--debug-frames", str(args.decode_debug_frames)]
                    if args.decode_debug_frames else [] ),
                 str(recorder_output)],
                capture_output=True, text=True, timeout=300, check=False,
            )
            log_paths["decoder"].write_text(decoded.stdout + decoded.stderr, encoding="utf-8")
            if decoded.returncode != 0:
                raise ValidationError(f"source-ID decoder exited with status {decoded.returncode}")
            identity = benchmark.find_summary_line(decoded.stdout, "decoded_frames=")
            summary["decoded_identity"] = identity
            range_ok = (
                isinstance(source_first, int) and isinstance(source_last, int) and
                isinstance(identity.get("first_id"), int) and
                isinstance(identity.get("last_id"), int) and
                source_first <= identity["first_id"] <= source_last and
                source_first <= identity["last_id"] <= source_last
            )
            identity_clean = (
                int(identity.get("black_frames", 0)) == 0 and
                int(identity.get("invalid_frames", 0)) == 0 and
                int(identity.get("invalid_digit_masks", 0)) == 0 and
                int(identity.get("duplicate_transitions", 0)) == 0 and
                int(identity.get("backward_transitions", 0)) == 0 and
                int(identity.get("missing_ids_between_adjacent_valid_frames",
                                 identity.get("missing_ids_between_frames", 0))) == 0 and
                range_ok
            )
            summary["decoded_identity"]["source_range_valid"] = range_ok
            summary["decoded_identity"]["validity_clean"] = identity_clean
            pause = summary.get("pause")
            assert isinstance(pause, dict)
            valid = (
                identity_clean and pause.get("ui_stopped") is True and
                pause.get("ui_resumed") is True and
                pause.get("output_grew_while_ui_stopped") is True and
                summary["recording"].get("stimulus_advanced_after_shutdown") is True and
                monitor.summary("valid").get("valid") is True
            )
            if not valid:
                raise ValidationError("one or more UI freeze validation checks failed")
            summary["status"] = "valid"
        except BaseException as error:
            failure = str(error) or type(error).__name__
            summary["status"] = "invalid"
            summary["reason"] = failure
            raise
        finally:
            if ui_was_stopped:
                try:
                    signal_owned_ui(
                        ui.pid, signal.SIGCONT, executable=ui_binary,
                        start_ticks=ui_start_ticks, validation_id=validation_id,
                    )
                    ui_was_stopped = False
                    summary.setdefault("cleanup", {})["ui_resumed_in_finally"] = True
                except (OSError, ValidationError) as error:
                    summary.setdefault("cleanup", {})["ui_resume_error"] = str(error)
            if stimulus is not None and stimulus.poll() is None:
                benchmark.stop_process(stimulus)
            recorder_still_running = False
            if recorder_pid is not None and recorder_start_ticks is not None and \
                    recorder_output is not None and preview_socket is not None:
                try:
                    if Path(f"/proc/{recorder_pid}").exists():
                        cleanup_status = stop_owned_recorder(
                            recorder_pid, recorder_start_ticks, ui.pid,
                            recorder_binary, recorder_output, preview_socket,
                        )
                        summary.setdefault("cleanup", {})["recorder_shutdown"] = cleanup_status
                        recorder_still_running = Path(f"/proc/{recorder_pid}").exists()
                except (OSError, ValidationError, KeyError) as error:
                    summary.setdefault("cleanup", {})["recorder_stop_error"] = str(error)
                    recorder_still_running = True
            try:
                if ui.poll() is None and not recorder_still_running:
                    exact_ui_identity(
                        ui.pid, expected_executable=ui_binary,
                        expected_start_ticks=ui_start_ticks, validation_id=validation_id,
                    )
                    os.kill(ui.pid, signal.SIGTERM)
                    try:
                        ui.wait(timeout=5.0)
                    except subprocess.TimeoutExpired:
                        exact_ui_identity(
                            ui.pid, expected_executable=ui_binary,
                            expected_start_ticks=ui_start_ticks, validation_id=validation_id,
                        )
                        os.kill(ui.pid, signal.SIGKILL)
                        ui.wait(timeout=2.0)
                elif ui.poll() is None:
                    summary.setdefault("cleanup", {})["ui_left_running"] = True
            except (OSError, ValidationError, subprocess.TimeoutExpired) as error:
                summary.setdefault("cleanup", {})["ui_stop_error"] = str(error)
            if stimulus_reader is not None:
                stimulus_reader.join(timeout=2.0)
            if recorder_output is not None:
                summary.setdefault("recording", {})["output_bytes"] = read_proc_size(recorder_output)
            if failure is not None:
                summary["reason"] = failure
            if summary.get("stimulus") is not None and "health" not in summary["stimulus"]:
                try:
                    summary["stimulus"]["health"] = monitor.summary("invalid", failure)  # type: ignore[name-defined]
                except (NameError, AttributeError):
                    pass
            atomic_summary(log_paths["summary"], summary)
            print(f"UI freeze summary: {log_paths['summary']}", flush=True)
            import fcntl
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
            lock.close()
    if valid:
        return 0
    return 1


def main() -> int:
    args = parse_args()
    try:
        return run(args)
    except KeyboardInterrupt:
        print("interrupted; cleanup has resumed any stopped UI and stopped owned children", file=sys.stderr)
        return 130
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"UI freeze validation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
