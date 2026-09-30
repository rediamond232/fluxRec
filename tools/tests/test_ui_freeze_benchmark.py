import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ui_freeze_benchmark as freeze


class ProcParsingTests(unittest.TestCase):
    def test_start_ticks_parse_comm_with_spaces_and_closing_parenthesis(self):
        fields = ["S", "1"] + ["0"] * 17 + ["987654"]
        stat_line = f"42 (ui name ) with paren) {' '.join(fields)}"
        self.assertEqual(freeze.proc_start_ticks(stat_line), 987654)

    def test_boolean_recorder_argument_does_not_hide_following_display(self):
        values = freeze.flag_values([
            "--output", "/tmp/a.mkv", "--until-signal",
            "--display", "DP-1", "--backend", "nvenc",
        ])
        self.assertEqual(values["--display"], "DP-1")
        self.assertNotIn("--until-signal", values)

    def test_private_ui_marker_and_executable_are_checked(self):
        marker = "freeze-test-marker"
        with tempfile.TemporaryDirectory() as temporary:
            sleeper = Path(temporary) / "validation-ui.py"
            sleeper.write_text("import time; time.sleep(20)\n", encoding="utf-8")
            executable = Path(sys.executable).resolve()
            child = subprocess.Popen(
                [str(executable), str(sleeper), "--validation-auto-start"],
                env={**os.environ, "FLUXREC_UI_FREEZE_VALIDATION_ID": marker},
            )
            try:
                identity = freeze.proc_identity(child.pid)
                verified = freeze.exact_ui_identity(
                    child.pid,
                    expected_executable=executable,
                    expected_start_ticks=identity["start_ticks"],
                    validation_id=marker,
                )
                self.assertEqual(verified["pid"], child.pid)
                with self.assertRaisesRegex(freeze.ValidationError, "private marker"):
                    freeze.exact_ui_identity(
                        child.pid,
                        expected_executable=executable,
                        expected_start_ticks=identity["start_ticks"],
                        validation_id="another-run",
                    )
                unmarked_mode = subprocess.Popen(
                    [str(executable), str(sleeper)],
                    env={**os.environ, "FLUXREC_UI_FREEZE_VALIDATION_ID": marker},
                )
                try:
                    other_identity = freeze.proc_identity(unmarked_mode.pid)
                    with self.assertRaisesRegex(freeze.ValidationError, "validation auto-start"):
                        freeze.exact_ui_identity(
                            unmarked_mode.pid,
                            expected_executable=executable,
                            expected_start_ticks=other_identity["start_ticks"],
                            validation_id=marker,
                        )
                finally:
                    unmarked_mode.terminate()
                    unmarked_mode.wait(timeout=3)
            finally:
                child.terminate()
                child.wait(timeout=3)


class RecorderConfigurationTests(unittest.TestCase):
    def test_default_decode_threshold_is_100(self):
        with patch("sys.argv", ["ui_freeze_benchmark.py"]):
            self.assertEqual(freeze.parse_args().decode_threshold, 100)

    def test_motion_is_configurable_and_defaults_to_existing_value(self):
        with patch("sys.argv", ["ui_freeze_benchmark.py"]):
            args = freeze.parse_args()
        self.assertEqual(args.motion, 0.20)
        freeze.validate_args(args)

        with patch("sys.argv", ["ui_freeze_benchmark.py", "--motion", "4.5"]):
            args = freeze.parse_args()
        self.assertEqual(args.motion, 4.5)
        freeze.validate_args(args)

    def test_motion_validation_matches_stimulus_range(self):
        with patch("sys.argv", ["ui_freeze_benchmark.py", "--motion", "1000"]):
            freeze.validate_args(freeze.parse_args())
        for value, message in (("-0.01", "between 0 and 1000"),
                               ("1000.01", "between 0 and 1000"),
                               ("nan", "must be finite")):
            with self.subTest(value=value), patch(
                "sys.argv", ["ui_freeze_benchmark.py", "--motion", value]
            ):
                with self.assertRaisesRegex(ValueError, message):
                    freeze.validate_args(freeze.parse_args())

    def test_shutdown_window_fits_before_stimulus_finishes(self):
        args = SimpleNamespace(
            stimulus_seconds=16.0,
            pause_after=3.0,
            pause_seconds=5.0,
            post_resume_seconds=2.0,
            post_stop_observe_seconds=1.5,
            settle_seconds=0.20,
            motion=0.20,
            ui_start_timeout=15.0,
            decode_threshold=100,
            decode_debug_frames=0,
        )
        freeze.validate_args(args)
        args.stimulus_seconds = 12.0
        with self.assertRaisesRegex(ValueError, "after the post-stop observation"):
            freeze.validate_args(args)

    def test_recorder_arguments_require_validated_nvenc_depth_and_preview(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            recorder = directory / "fluxrec-record"
            output_directory = directory / "recordings"
            output_directory.mkdir()
            argv = [
                str(recorder), "--output", str(output_directory / "run.mkv"),
                "--until-signal", "--display", "DP-1", "--conversion", "two-pass",
                "--codec", "h264", "--gop", "120", "--max-fps", "0", "--pool", "48",
                "--audio", "off", "--backend", "nvenc", "--nvenc-preset", "p1",
                "--async-depth", "4", "--b-frames", "0", "--bitrate-mbps", "50",
                "--preview-socket", "/run/user/1000/fluxrec-preview-test/preview.sock",
            ]
            evidence = freeze.validate_recorder_child(
                argv, recorder_binary=recorder, output_directory=output_directory,
                display="DP-1",
            )
            self.assertEqual(evidence["output"], output_directory / "run.mkv")
            argv[argv.index("--async-depth") + 1] = "8"
            with self.assertRaisesRegex(freeze.ValidationError, "async-depth"):
                freeze.validate_recorder_child(
                    argv, recorder_binary=recorder, output_directory=output_directory,
                    display="DP-1",
                )

    def test_settings_use_private_config_and_nvenc_depth_four(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            settings = freeze.write_isolated_settings(
                root / "config", recorder_binary=root / "fluxrec-record",
                output_directory=root / "recordings", display="DP-1",
            )
            contents = settings.read_text(encoding="utf-8")
            self.assertIn("encoderBackend=nvenc", contents)
            self.assertIn("nvencAsyncDepth=4", contents)
            self.assertIn("outputDirectory=" + str(root / "recordings"), contents)
            self.assertFalse((root / ".config/Fluxrec/Fluxrec.conf").exists())

    def test_preview_socket_must_be_user_owned_and_in_private_runtime_dir(self):
        with tempfile.TemporaryDirectory() as temporary:
            runtime = Path(temporary)
            directory = runtime / "fluxrec-preview-test"
            directory.mkdir(mode=0o700)
            os.chmod(directory, 0o700)
            socket_path = directory / "preview.sock"
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                listener.bind(str(socket_path))
                evidence = freeze.validate_preview_socket(socket_path, runtime, os.getuid())
                self.assertEqual(evidence["path"], str(socket_path))
                self.assertEqual(evidence["directory_mode"], "0o700")
            finally:
                listener.close()


if __name__ == "__main__":
    unittest.main()
