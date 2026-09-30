import argparse
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import controlled_benchmark as benchmark


class StimulusStatisticsTests(unittest.TestCase):
    def test_parses_positive_presentation_rate_and_total(self):
        line = (
            "stimulus: submitted/s=359.8 presented/s=359.6 frame-callbacks/s=360.0 "
            "discarded/s=0.0 presented-id-gaps=4 refresh-us=2778.0 totals=1800/1798/0 "
            "pattern=plasma"
        )
        self.assertEqual(
            benchmark.parse_stimulus_statistics(line),
            {"presented_fps": 359.6, "presented_total": 1798},
        )

    def test_rejects_zero_presentation_rate_immediately(self):
        monitor = benchmark.StimulusHealthMonitor(
            "DP-1", 4242, "plasma", 0.20, started_at=10.0
        )
        line = "stimulus: submitted/s=0.0 presented/s=0.0 frame-callbacks/s=0.0 discarded/s=0.0 totals=1800/1798/0 pattern=plasma"
        with self.assertRaisesRegex(
            benchmark.InvalidBenchmarkEnvironment,
            "presentation feedback stopped.*presented/s=0",
        ):
            monitor.observe_line(line, 11.0)

    def test_rejects_a_nonadvancing_presented_total(self):
        monitor = benchmark.StimulusHealthMonitor(
            "DP-1", 4242, "plasma", 0.20, started_at=10.0
        )
        line = "stimulus: submitted/s=60.0 presented/s=60.0 frame-callbacks/s=60.0 discarded/s=0.0 totals=60/60/0 pattern=plasma"
        monitor.observe_line(line, 11.0)
        with self.assertRaisesRegex(
            benchmark.InvalidBenchmarkEnvironment,
            "presented total did not advance",
        ):
            monitor.observe_line(line, 12.0)

    def test_feedback_watchdog_expires_without_fresh_statistics(self):
        monitor = benchmark.StimulusHealthMonitor(
            "DP-1", 4242, "plasma", 0.20, started_at=10.0
        )
        monitor.observe_line(
            "stimulus: submitted/s=60.0 presented/s=60.0 frame-callbacks/s=60.0 "
            "discarded/s=0.0 totals=60/60/0 pattern=plasma",
            11.0,
        )
        with self.assertRaisesRegex(
            benchmark.InvalidBenchmarkEnvironment,
            "statistics heartbeat stopped",
        ):
            monitor.check_feedback(13.6)

    def test_rejects_missing_or_malformed_cadence_fields(self):
        with self.assertRaisesRegex(
            benchmark.InvalidBenchmarkEnvironment,
            "missing presented/s or totals",
        ):
            benchmark.parse_stimulus_statistics("stimulus: presented/s=60.0")

    def test_rejects_unexpected_logged_pattern(self):
        with self.assertRaisesRegex(
            benchmark.InvalidBenchmarkEnvironment,
            "pattern changed.*expected plasma, logged bars",
        ):
            benchmark.validate_stimulus_settings_line(
                "pattern=bars", "plasma", 0.20
            )

    def test_rejects_unexpected_logged_motion(self):
        with self.assertRaisesRegex(
            benchmark.InvalidBenchmarkEnvironment,
            "motion changed.*expected 0.2, logged 0.45",
        ):
            benchmark.validate_stimulus_settings_line(
                "motion=0.45", "plasma", 0.20
            )

    def test_accepts_expected_pattern_and_rounded_motion(self):
        benchmark.validate_stimulus_settings_line(
            "stimulus: presented/s=359.7 totals=360/360/0 pattern=plasma",
            "plasma",
            0.20,
        )
        benchmark.validate_stimulus_settings_line(
            "motion=0.234568", "plasma", 0.23456789
        )

    def test_classifies_content_separately_from_missing_source_ids(self):
        frame_content_clean, source_continuity_clean = (
            benchmark.classify_frame_content_and_source_continuity(
                decoder_succeeded=True,
                black_frames=0,
                invalid_frames=0,
                invalid_digit_masks=0,
                valid_id_frames=100,
                source_gaps=0,
                recorded_missing_ids=58,
                recorded_range_valid=True,
                duplicate_transitions=0,
                backward_transitions=0,
            )
        )
        self.assertTrue(frame_content_clean)
        self.assertFalse(source_continuity_clean)

    def test_cap_sampling_gets_an_expected_label_without_changing_raw_checks(self):
        kwargs = dict(
            max_fps=120,
            frame_content_clean=True,
            source_gaps=0,
            recorded_missing_ids=41,
            recorded_range_valid=True,
            duplicate_transitions=0,
            backward_transitions=0,
            source_continuity_clean=False,
            unknown_transition_spans=False,
        )
        self.assertEqual(
            benchmark.classify_source_continuity_label(**kwargs),
            "expected-subsampling",
        )
        kwargs["source_continuity_clean"] = True
        self.assertEqual(benchmark.classify_source_continuity_label(**kwargs), "clean")
        kwargs["source_continuity_clean"] = False
        kwargs["frame_content_clean"] = False
        self.assertEqual(benchmark.classify_source_continuity_label(**kwargs), "FAIL")
        kwargs["source_continuity_clean"] = True
        kwargs["unknown_transition_spans"] = True
        self.assertEqual(benchmark.classify_source_continuity_label(**kwargs), "unknown")
        self.assertFalse(benchmark.recording_identity_accepted("FAIL", True))
        self.assertFalse(benchmark.recording_identity_accepted("unknown", True))
        self.assertFalse(benchmark.recording_identity_accepted("clean", False))
        self.assertTrue(benchmark.recording_identity_accepted("clean", True))
        self.assertTrue(benchmark.recording_identity_accepted("expected-subsampling", True))
        self.assertTrue(benchmark.recording_identity_accepted("n/a (replay-only)", None))

    def test_mux_label_identifies_total_packets_separately_from_video_frames(self):
        label = benchmark.format_recording_count_label(5399, 5399, 5399, 6811)
        self.assertEqual(
            label,
            "5399 captured/5399 submitted/5399 encoded/muxed-total-packets=6811",
        )

    def test_missing_primary_mux_count_prints_not_applicable(self):
        label = benchmark.format_recording_count_label(4320, 4320, 4320, None)
        self.assertEqual(
            label,
            "4320 captured/4320 submitted/4320 encoded/muxed-total-packets=n/a",
        )


class RecorderSummaryParserTests(unittest.TestCase):
    def test_current_summary_keeps_muxed_video_frames_and_packet_total_distinct(self):
        log = (
            "startup diagnostic\n"
            "captured=2881 capture_admitted=2881 capture_rate_limited=0 "
            "distinct_presentation_times=2881 submitted=2881 encoded=2881 "
            "capture_dropped=0 capture_unarmed=3 capture_cancelled_at_stop=1 "
            "pool_starvation_periods=0 pool_starvation_ms=0 packet_dropped=0 "
            "packet_queue_dropped=0 muxed_video_frames=2881 "
            "muxed_packets_total=3727 output_bytes=58173333\n"
            "shutdown diagnostic\n"
        )

        counters = benchmark.parse_recorder_counters(log)

        self.assertEqual(counters["captured"], 2881)
        self.assertEqual(counters["encoded"], 2881)
        self.assertEqual(counters["muxed_video_frames"], 2881)
        self.assertEqual(counters["muxed_packets_total"], 3727)
        self.assertEqual(counters["muxed"], 3727)
        self.assertEqual(counters["capture_unarmed"], 3)

    def test_legacy_muxed_field_remains_a_packet_total_fallback(self):
        counters = benchmark.parse_recorder_counters(
            "captured=100 submitted=100 encoded=100 muxed=145 output_bytes=9000\n"
        )

        self.assertEqual(counters["muxed_packets_total"], 145)
        self.assertNotIn("muxed_video_frames", counters)


class RecorderOptionsTests(unittest.TestCase):
    def parsed_args(self, *arguments):
        with patch("sys.argv", ["controlled_benchmark.py", *arguments]):
            return benchmark.parse_args()

    def test_nvenc_omits_async_depth_for_recorder_default(self):
        args = self.parsed_args("--backend", "nvenc", "--nvenc-preset", "p1")
        benchmark.validate(args)
        command = benchmark.build_recorder_command(
            args, Path("/recorder"), Path("/capture.mkv")
        )
        self.assertIn(("--backend", "nvenc"), list(zip(command, command[1:])))
        self.assertIn(("--nvenc-preset", "p1"), list(zip(command, command[1:])))
        self.assertNotIn("--async-depth", command)
        self.assertIsNone(args.async_depth)

    def test_existing_harness_defaults_remain_for_vulkan_and_nvenc(self):
        args = self.parsed_args()
        benchmark.validate(args)
        command = benchmark.build_recorder_command(
            args, Path("/recorder"), Path("/capture.mkv")
        )
        self.assertEqual(benchmark.selected_pool(args), 24)
        self.assertEqual(benchmark.conversion_summary(args), "two-pass")
        self.assertEqual(command[command.index("--pool") + 1], "24")
        self.assertEqual(command[command.index("--conversion") + 1], "two-pass")

    def test_direct_cuda_egl_uses_native_capture_and_compatible_defaults(self):
        args = self.parsed_args("--backend", "experimental-cuda-egl")
        benchmark.validate(args)
        command = benchmark.build_recorder_command(
            args, Path("/recorder"), Path("/capture.mkv")
        )

        self.assertEqual(benchmark.selected_pool(args), 16)
        self.assertEqual(benchmark.conversion_summary(args), "native")
        self.assertEqual(command[command.index("--pool") + 1], "16")
        self.assertNotIn("--conversion", command)
        self.assertEqual(command[command.index("--async-depth") + 1], "12")
        self.assertIsNone(args.async_depth)
        self.assertEqual(benchmark.effective_async_depth(args, Path("/missing")), 12)

    def test_direct_cuda_egl_respects_explicit_supported_pool_and_depth(self):
        args = self.parsed_args(
            "--backend", "experimental-cuda-egl", "--pool", "8",
            "--async-depth", "6",
        )
        benchmark.validate(args)
        command = benchmark.build_recorder_command(
            args, Path("/recorder"), Path("/capture.mkv")
        )
        self.assertEqual(command[command.index("--pool") + 1], "8")
        self.assertEqual(command[command.index("--async-depth") + 1], "6")

    def test_direct_cuda_egl_rejects_explicit_conversion_and_out_of_range_pool(self):
        args = self.parsed_args("--backend", "experimental-cuda-egl", "--conversion", "auto")
        with self.assertRaisesRegex(ValueError, "omit it to use native capture frames"):
            benchmark.validate(args)

        args = self.parsed_args("--backend", "experimental-cuda-egl", "--pool", "24")
        with self.assertRaisesRegex(ValueError, "requires --pool between 3 and 16"):
            benchmark.validate(args)

    def test_explicit_async_depth_eight_is_forwarded_unchanged(self):
        args = self.parsed_args("--backend", "nvenc", "--async-depth", "8")
        benchmark.validate(args)
        command = benchmark.build_recorder_command(
            args, Path("/recorder"), Path("/capture.mkv")
        )
        option_index = command.index("--async-depth")
        self.assertEqual(command[option_index + 1], "8")
        self.assertEqual(benchmark.effective_async_depth(args, Path("/missing")), 8)

    def test_frame_digest_diagnostic_is_explicit(self):
        args = self.parsed_args("--verify-frame-ids")
        benchmark.validate(args)
        command = benchmark.build_recorder_command(
            args, Path("/recorder"), Path("/capture.mkv")
        )
        self.assertIn("--verify-frame-ids", command)
        defaults = self.parsed_args()
        benchmark.validate(defaults)
        self.assertNotIn(
            "--verify-frame-ids",
            benchmark.build_recorder_command(
                defaults, Path("/recorder"), Path("/capture.mkv")
            ),
        )

    def test_default_decode_threshold_is_100_and_remains_overridable(self):
        self.assertEqual(self.parsed_args().decode_threshold, 100)
        self.assertEqual(self.parsed_args("--decode-threshold", "93").decode_threshold, 93)

    def test_gpu_path_log_reports_the_opened_nvenc_backend(self):
        log = (
            "NVENC selected: default async depth set to 4 validated frames\n"
            "fluxrec GPU path: DRM_PRIME direct import -> Vulkan two-pass NV12 -> "
            "h264_nvenc (NVIDIA GeForce RTX 3090 Ti)\n"
        )
        self.assertEqual(benchmark.parse_observed_backend(log), "nvenc")

    def test_gpu_path_log_reports_the_opened_vulkan_backend(self):
        log = (
            "fluxrec GPU path: DRM_PRIME direct import -> native-size Vulkan NV12 -> "
            "hevc_vulkan (NVIDIA GeForce RTX 3090 Ti)\n"
        )
        self.assertEqual(benchmark.parse_observed_backend(log), "vulkan")

    def test_successful_direct_cuda_egl_initialization_reports_its_backend(self):
        log = (
            "EXPERIMENTAL direct CUDA EGL encoder initialized for native 2560x1440 H.264; "
            "CUDA EGL desktop support is outside NVIDIA's documented Tegra support scope\n"
        )
        self.assertEqual(benchmark.parse_observed_backend(log), "experimental-cuda-egl")

    def test_direct_backend_observation_keeps_mismatch_and_mixed_path_checks(self):
        direct_log = (
            "EXPERIMENTAL direct CUDA EGL encoder initialized for native 2560x1440 H.264\n"
        )
        with self.assertRaisesRegex(
            benchmark.InvalidBenchmarkEnvironment,
            "opened the experimental-cuda-egl backend despite explicit nvenc request",
        ):
            benchmark.validate_observed_backend(
                "nvenc", benchmark.parse_observed_backend(direct_log)
            )

        mixed = direct_log + "fluxrec GPU path: -> h264_nvenc (device)\n"
        self.assertEqual(benchmark.parse_observed_backend(mixed), "mixed")

    def test_uninitialized_direct_path_is_not_guessed_as_observed_backend(self):
        self.assertIsNone(
            benchmark.parse_observed_backend(
                "failed to initialize direct CUDA EGL encoder\n"
            )
        )

    def test_missing_or_conflicting_gpu_path_logs_are_not_guessed(self):
        self.assertIsNone(benchmark.parse_observed_backend("recorder starting\n"))
        mixed = (
            "fluxrec GPU path: -> h264_nvenc (device)\n"
            "fluxrec GPU path: -> h264_vulkan (device)\n"
        )
        self.assertEqual(benchmark.parse_observed_backend(mixed), "mixed")

    def test_explicit_backend_mismatch_invalidates_but_inherited_is_reportable(self):
        with self.assertRaisesRegex(
            benchmark.InvalidBenchmarkEnvironment,
            "opened the nvenc backend despite explicit vulkan request",
        ):
            benchmark.validate_observed_backend("vulkan", "nvenc")
        benchmark.validate_observed_backend(None, "nvenc")
        benchmark.validate_observed_backend("nvenc", "nvenc")
        benchmark.validate_observed_backend("vulkan", None)

    def test_invalid_summary_keeps_requested_backend_and_adds_observed_backend(self):
        args = self.parsed_args()
        with tempfile.TemporaryDirectory() as directory:
            paths = benchmark.make_paths(Path(directory) / "capture.mkv")
            paths["record_stderr"].write_text(
                "fluxrec GPU path: DRM_PRIME direct import -> Vulkan two-pass NV12 -> "
                "h264_nvenc (NVIDIA GeForce RTX 3090 Ti)\n",
                encoding="utf-8",
            )
            benchmark.write_invalid_environment_summary(
                paths, args, None, "stimulus ended", None
            )
            summary = json.loads(paths["summary"].read_text(encoding="utf-8"))
        self.assertEqual(summary["conditions"]["backend"], "inherited")
        self.assertEqual(summary["conditions"]["backend_observed"], "nvenc")
        self.assertEqual(summary["recording"]["backend"], "inherited")
        self.assertEqual(summary["recording"]["backend_observed"], "nvenc")

    def test_auto_depth_summary_uses_nvenc_startup_selection(self):
        args = self.parsed_args("--backend", "nvenc")
        with tempfile.TemporaryDirectory() as directory:
            stderr = Path(directory) / "record.stderr.log"
            stderr.write_text(
                "NVENC selected: default async depth set to 4 validated frames\n",
                encoding="utf-8",
            )
            self.assertEqual(benchmark.effective_async_depth(args, stderr), 4)

    def test_backend_default_depth_summary_is_eight_for_vulkan(self):
        args = self.parsed_args("--backend", "vulkan")
        with tempfile.TemporaryDirectory() as directory:
            stderr = Path(directory) / "record.stderr.log"
            stderr.write_text("Vulkan Video encoder opened\n", encoding="utf-8")
            self.assertEqual(benchmark.effective_async_depth(args, stderr), 8)

    def test_b_frames_option_is_forwarded_and_defaults_to_omission(self):
        args = self.parsed_args("--backend", "nvenc", "--b-frames", "2")
        benchmark.validate(args)
        command = benchmark.build_recorder_command(
            args, Path("/recorder"), Path("/capture.mkv")
        )
        option_index = command.index("--b-frames")
        self.assertEqual(command[option_index + 1], "2")
        self.assertEqual(args.b_frames, 2)

        defaults = self.parsed_args()
        benchmark.validate(defaults)
        default_command = benchmark.build_recorder_command(
            defaults, Path("/recorder"), Path("/capture.mkv")
        )
        self.assertNotIn("--b-frames", default_command)

    def test_b_frames_validation_matches_recorder_backend_and_gop_limits(self):
        args = self.parsed_args("--backend", "vulkan", "--b-frames", "1")
        with self.assertRaisesRegex(ValueError, "requires the NVENC backend"):
            benchmark.validate(args)

        args = self.parsed_args("--backend", "nvenc", "--gop", "3", "--b-frames", "2")
        with self.assertRaisesRegex(ValueError, "--gop must be at least"):
            benchmark.validate(args)

    def test_b_frames_option_is_limited_to_zero_through_four(self):
        with patch("sys.stderr", new=io.StringIO()):
            with self.assertRaises(SystemExit):
                self.parsed_args("--b-frames", "5")

    def test_timed_replay_save_requires_a_buffer_and_mid_recording_time(self):
        args = self.parsed_args(
            "--record-seconds", "10", "--replay-seconds", "6",
            "--save-replay-after", "4.5",
        )
        benchmark.validate(args)
        with tempfile.TemporaryDirectory() as directory:
            paths = benchmark.make_paths(Path(directory) / "capture.mkv", save_replay=True)
            self.assertTrue(paths["replay_output"].is_absolute())
            self.assertEqual(paths["replay_output"].name, "capture-replay.mkv")

        with self.assertRaisesRegex(ValueError, "requires --replay-seconds"):
            args = self.parsed_args("--record-seconds", "10", "--save-replay-after", "4")
            benchmark.validate(args)
        with self.assertRaisesRegex(ValueError, "before the recording ends"):
            args = self.parsed_args(
                "--record-seconds", "10", "--replay-seconds", "6",
                "--save-replay-after", "10",
            )
            benchmark.validate(args)

    def test_replay_output_without_a_save_time_is_rejected(self):
        args = self.parsed_args("--replay-output", "/tmp/replay.mkv")
        with self.assertRaisesRegex(ValueError, "requires --save-replay-after"):
            benchmark.validate(args)

    def test_default_paths_do_not_add_replay_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = benchmark.make_paths(Path(directory) / "capture.mkv")
        self.assertNotIn("replay_output", paths)

    def test_replay_only_requires_timed_export_and_omits_primary_output(self):
        args = self.parsed_args(
            "--record-seconds", "10", "--replay-seconds", "8",
            "--replay-only", "--save-replay-after", "4",
        )
        benchmark.validate(args)
        command = benchmark.build_recorder_command(
            args, Path("/recorder"), Path("/run-prefix.mkv")
        )
        self.assertIn("--replay-only", command)
        self.assertNotIn("--output", command)
        with tempfile.TemporaryDirectory() as directory:
            paths = benchmark.make_paths(
                Path(directory) / "run-prefix.mkv",
                save_replay=True,
                replay_only=True,
            )
            self.assertNotIn("decoder_log", paths)
            artifacts = benchmark.summarize_artifact_paths(paths, replay_only=True)
        self.assertIn("run_prefix", artifacts)
        self.assertNotIn("output", artifacts)

    def test_replay_only_without_a_timed_save_is_rejected(self):
        args = self.parsed_args(
            "--record-seconds", "10", "--replay-seconds", "8", "--replay-only"
        )
        with self.assertRaisesRegex(ValueError, "requires --save-replay-after"):
            benchmark.validate(args)


class H264CoderProbeTests(unittest.TestCase):
    def test_trace_parser_reads_cabac_flag_from_pps(self):
        trace = """
[trace_headers @ 0x1] 8 nal_unit_type 3 = 7
[trace_headers @ 0x1] 9 seq_parameter_set_id 1 = 0
[trace_headers @ 0x1] 8 nal_unit_type 3 = 8
[trace_headers @ 0x1] 9 pic_parameter_set_id 1 = 0
[trace_headers @ 0x1] 10 entropy_coding_mode_flag 1 = 1
[trace_headers @ 0x1] 11 bottom_field_pic_order_in_frame_present_flag 1 = 0
"""
        self.assertEqual(benchmark.parse_h264_coder_from_trace(trace), "cabac")

    def test_trace_parser_reads_cavlc_flag_from_pps(self):
        trace = """
[trace_headers @ 0x1] 8 nal_unit_type 3 = 8
[trace_headers @ 0x1] 9 pic_parameter_set_id 1 = 0
[trace_headers @ 0x1] 10 entropy_coding_mode_flag 1 = 0
"""
        self.assertEqual(benchmark.parse_h264_coder_from_trace(trace), "cavlc")

    def test_trace_parser_ignores_flags_outside_a_pps(self):
        trace = """
[trace_headers @ 0x1] 8 nal_unit_type 3 = 7
[trace_headers @ 0x1] 10 entropy_coding_mode_flag 1 = 1
[trace_headers @ 0x1] 8 nal_unit_type 3 = 1
[trace_headers @ 0x1] 10 entropy_coding_mode_flag 1 = 0
"""
        self.assertEqual(benchmark.parse_h264_coder_from_trace(trace), "unknown")

    def test_trace_parser_reports_conflicting_pps_values_as_mixed(self):
        trace = """
[trace_headers @ 0x1] 8 nal_unit_type 3 = 8
[trace_headers @ 0x1] 10 entropy_coding_mode_flag 1 = 0
[trace_headers @ 0x1] 8 nal_unit_type 3 = 8
[trace_headers @ 0x1] 10 entropy_coding_mode_flag 1 = 1
"""
        self.assertEqual(benchmark.parse_h264_coder_from_trace(trace), "mixed")

    def test_probe_traces_only_first_video_frame_without_decoding(self):
        command = benchmark.h264_coder_probe_command(
            "/usr/bin/ffmpeg", Path("/tmp/capture.mkv")
        )
        self.assertIn("-frames:v", command)
        self.assertEqual(command[command.index("-frames:v") + 1], "1")
        self.assertEqual(command[command.index("-c:v") + 1], "copy")
        self.assertEqual(command[command.index("-bsf:v") + 1], "trace_headers")

    def test_replay_only_observes_the_saved_replay_export(self):
        with tempfile.TemporaryDirectory() as directory:
            args = argparse.Namespace(codec="h264", replay_only=True)
            paths = benchmark.make_paths(
                Path(directory) / "run-prefix.mkv",
                save_replay=True,
                replay_only=True,
            )
            with patch.object(benchmark, "probe_h264_coder", return_value="cabac") as probe:
                self.assertEqual(
                    benchmark.h264_coder_observation(args, paths),
                    ("cabac", "replay_export"),
                )
            probe.assert_called_once_with(paths["replay_output"])

    def test_non_h264_runs_do_not_probe_a_video_file(self):
        args = argparse.Namespace(codec="hevc", replay_only=False)
        with patch.object(benchmark, "probe_h264_coder") as probe:
            self.assertEqual(
                benchmark.h264_coder_observation(args, {"output": Path("capture.mkv")}),
                ("not_applicable", None),
            )
        probe.assert_not_called()


class ReplayReadbackTests(unittest.TestCase):
    def test_scheduled_command_uses_an_absolute_replay_path(self):
        class FakePipe:
            def __init__(self):
                self.value = ""
                self.flushed = False

            def write(self, value):
                self.value += value

            def flush(self):
                self.flushed = True

        class FakeProcess:
            def __init__(self):
                self.stdin = FakePipe()

        process = FakeProcess()
        with tempfile.TemporaryDirectory() as directory:
            replay_path = Path(directory) / "replay.mkv"
            benchmark.send_replay_save_command(process, replay_path)
            self.assertEqual(
                process.stdin.value,
                f"save_replay {replay_path.resolve()}\n",
            )
            self.assertTrue(process.stdin.flushed)

    def test_control_events_require_one_successful_save_for_the_requested_path(self):
        with tempfile.TemporaryDirectory() as directory:
            replay_path = (Path(directory) / "saved.mkv").resolve()
            stdout = Path(directory) / "record.stdout.log"
            stdout.write_text(
                "\n".join([
                    json.dumps({"type": "replay_queued", "path": str(replay_path), "job_id": 4}),
                    json.dumps({"type": "replay_saved", "path": str(replay_path), "ok": True,
                                "job_id": 4, "packets": 80, "bytes": 24000}),
                    "",
                ]),
                encoding="utf-8",
            )
            result = benchmark.summarize_replay_control_events(stdout, replay_path)
        self.assertTrue(result["acknowledged"])
        self.assertEqual(result["successful_save"]["packets"], 80)

    def test_packet_probe_checks_counts_and_per_stream_dts(self):
        result = benchmark.summarize_ffprobe_packets({
            "streams": [
                {"index": 0, "codec_type": "video", "codec_name": "h264",
                 "width": 2560, "height": 1440, "nb_read_packets": "3",
                 "nb_read_frames": "3"},
                {"index": 1, "codec_type": "audio", "codec_name": "aac",
                 "nb_read_packets": "2", "nb_read_frames": "2"},
            ],
            "packets": [
                {"stream_index": 0, "dts": "0"},
                {"stream_index": 1, "dts": "0"},
                {"stream_index": 0, "dts": "1"},
                {"stream_index": 1, "dts": "1"},
                {"stream_index": 0, "dts": "2"},
            ],
        })
        self.assertTrue(result["valid"])
        self.assertEqual(result["packet_count"], 5)
        self.assertEqual(result["video_decoded_frame_count"], 3)
        self.assertTrue(result["per_stream_dts_strictly_increasing"])

    def test_packet_probe_rejects_repeated_dts(self):
        result = benchmark.summarize_ffprobe_packets({
            "streams": [{
                "index": 0, "codec_type": "video", "codec_name": "h264",
                "nb_read_packets": "2", "nb_read_frames": "2",
            }],
            "packets": [
                {"stream_index": 0, "dts": "4"},
                {"stream_index": 0, "dts": "4"},
            ],
        })
        self.assertFalse(result["valid"])
        self.assertFalse(result["per_stream_dts_strictly_increasing"])

    def test_b_frame_probe_allows_missing_dts_only_on_first_video_packet(self):
        payload = {
            "streams": [
                {"index": 0, "codec_type": "video", "codec_name": "hevc",
                 "nb_read_packets": "3", "nb_read_frames": "3"},
                {"index": 1, "codec_type": "audio", "codec_name": "aac",
                 "nb_read_packets": "1", "nb_read_frames": "1"},
            ],
            "packets": [
                {"stream_index": 0, "pts": "2", "flags": "K_"},
                {"stream_index": 1, "dts": "0"},
                {"stream_index": 0, "dts": "0"},
                {"stream_index": 0, "dts": "1"},
            ],
        }
        result = benchmark.summarize_ffprobe_packets(
            payload, allow_missing_first_video_dts=True
        )
        self.assertTrue(result["valid"])
        self.assertEqual(result["packet_count"], 4)
        self.assertEqual(result["stream_packet_count"], 4)
        self.assertEqual(result["video_packet_count"], 3)
        self.assertEqual(result["missing_dts_packet_count"], 1)
        self.assertTrue(result["first_video_packet_missing_dts_allowed"])
        self.assertEqual(
            result["dts_validation"],
            "bframe-first-video-dts-missing-allowed",
        )

        strict_result = benchmark.summarize_ffprobe_packets(payload)
        self.assertFalse(strict_result["valid"])
        self.assertEqual(strict_result["packet_count"], 4)
        self.assertEqual(strict_result["dts_validation"], "invalid")

    def test_b_frame_probe_rejects_later_missing_dts_but_counts_the_packet(self):
        result = benchmark.summarize_ffprobe_packets({
            "streams": [{
                "index": 0, "codec_type": "video", "codec_name": "hevc",
                "nb_read_packets": "3", "nb_read_frames": "3",
            }],
            "packets": [
                {"stream_index": 0, "dts": "0"},
                {"stream_index": 0, "dts": "1"},
                {"stream_index": 0, "pts": "3"},
            ],
        }, allow_missing_first_video_dts=True)
        self.assertFalse(result["valid"])
        self.assertEqual(result["packet_count"], 3)
        self.assertEqual(result["stream_packet_count"], 3)
        self.assertEqual(result["missing_dts_packet_count"], 1)
        self.assertFalse(result["first_video_packet_missing_dts_allowed"])
        self.assertEqual(result["dts_validation"], "invalid")
        self.assertTrue(any("outside the first video packet" in reason
                            for reason in result["dts_validation_reasons"]))

        nonmonotonic = benchmark.summarize_ffprobe_packets({
            "streams": [{
                "index": 0, "codec_type": "video", "codec_name": "hevc",
                "nb_read_packets": "3", "nb_read_frames": "3",
            }],
            "packets": [
                {"stream_index": 0, "pts": "2"},
                {"stream_index": 0, "dts": "4"},
                {"stream_index": 0, "dts": "3"},
            ],
        }, allow_missing_first_video_dts=True)
        self.assertFalse(nonmonotonic["valid"])
        self.assertFalse(nonmonotonic["per_stream_dts_strictly_increasing"])
        self.assertEqual(nonmonotonic["packet_count"], 3)

    def test_replay_identity_span_is_reported_as_its_own_window(self):
        result = benchmark.summarize_replay_identity(
            "decoded_frames=8 analyzed_frames=8 first_id=120 last_id=134 "
            "valid_id_frames=8 black_frames=0 invalid_frames=0 "
            "invalid_digit_masks=0 duplicate_transitions=0 backward_transitions=0 "
            "missing_ids_between_adjacent_valid_frames=7",
            0,
            100,
            200,
        )
        self.assertEqual(result["id_span"], 15)
        self.assertEqual(result["first_id"], 120)
        self.assertEqual(result["last_id"], 134)
        self.assertEqual(result["missing_ids_between_adjacent_valid_frames"], 7)
        self.assertTrue(result["frame_content_clean"])
        self.assertTrue(result["id_order_and_source_range_valid"])

    def test_ffmpeg_decode_uses_demux_time_base_for_null_muxer(self):
        command = benchmark.build_ffmpeg_replay_decode_command(
            "/usr/bin/ffmpeg", Path("/tmp/replay.mkv")
        )
        self.assertEqual(command[command.index("-enc_time_base") + 1], "demux")
        self.assertEqual(command[-3:], ["-f", "null", "-"])


class HyprlandWorkspaceTests(unittest.TestCase):
    def setUp(self):
        self.clients = [{
            "pid": 4242,
            "class": "io.fluxrec.stimulus",
            "title": "Fluxrec Motion Stimulus",
            "mapped": True,
            "hidden": False,
            "visible": True,
            "fullscreen": 2,
            "monitor": 0,
            "workspace": {"id": 3, "name": "3"},
        }]
        self.monitors = [{
            "id": 0,
            "name": "DP-1",
            "activeWorkspace": {"id": 3, "name": "3"},
        }]

    def test_accepts_fullscreen_stimulus_on_active_target_workspace(self):
        status = benchmark.inspect_stimulus_workspace(
            self.clients, self.monitors, "DP-1", 4242
        )
        self.assertTrue(status["active"])
        self.assertEqual(status["stimulus_workspace_id"], 3)
        self.assertEqual(status["active_workspace_id"], 3)

    def test_rejects_stimulus_workspace_when_another_is_active(self):
        self.monitors[0]["activeWorkspace"] = {"id": 1, "name": "1"}
        status = benchmark.inspect_stimulus_workspace(
            self.clients, self.monitors, "DP-1", 4242
        )
        self.assertFalse(status["active"])
        self.assertEqual(
            status["reason"],
            "stimulus fullscreen workspace 3 is inactive on DP-1 (active workspace 1)",
        )

    def test_rejects_window_that_is_no_longer_fullscreen(self):
        self.clients[0]["fullscreen"] = 1
        status = benchmark.inspect_stimulus_workspace(
            self.clients, self.monitors, "DP-1", 4242
        )
        self.assertFalse(status["active"])
        self.assertEqual(status["reason"], "the stimulus window is no longer fullscreen")

    def test_decodes_batched_json_arrays(self):
        clients, monitors = benchmark.decode_hyprctl_batch('[{"pid":4242}] []')
        self.assertEqual(clients, [{"pid": 4242}])
        self.assertEqual(monitors, [])

    def test_invalid_environment_summary_is_explicit(self):
        args = argparse.Namespace(
            codec="h264",
            conversion="two-pass",
            encoder_tune="default",
            backend="nvenc",
            nvenc_preset="p1",
            max_fps=360,
            pattern="plasma",
            motion=1.0,
            record_seconds=5,
            async_depth=None,
        )
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "capture.mkv"
            summary_path = Path(directory) / "capture.mkv.summary.json"
            paths = {
                "output": output,
                "summary": summary_path,
                "record_stderr": Path(directory) / "capture.mkv.record.stderr.log",
            }
            reason = "stimulus fullscreen workspace 3 is inactive on DP-1 (active workspace 1)"
            benchmark.write_invalid_environment_summary(
                paths, args, None, reason, None
            )
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
        self.assertEqual(summary["environmental_validity"]["status"], "invalid")
        self.assertFalse(summary["environmental_validity"]["valid"])
        self.assertEqual(summary["environmental_validity"]["reasons"], [reason])


if __name__ == "__main__":
    unittest.main()
