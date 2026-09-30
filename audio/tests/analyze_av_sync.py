#!/usr/bin/env python3
"""Offline pulse/flash timing and drift analysis for av_sync_fixture."""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import subprocess
import sys
from pathlib import Path


def run(command: list[str]) -> bytes:
    completed = subprocess.run(command, check=True, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    return completed.stdout


def probe(path: Path, args: list[str]) -> str:
    return subprocess.check_output(["ffprobe", "-v", "error", *args, str(path)],
                                   text=True)


def stream_info(path: Path) -> list[dict]:
    data = json.loads(probe(path, ["-show_entries",
        "stream=index,codec_name,codec_type,sample_rate,channels,time_base,start_time,duration",
        "-of", "json"]))
    return data["streams"]


def packet_times(path: Path, selector: str) -> list[float]:
    output = probe(path, ["-select_streams", selector, "-show_entries", "packet=dts_time",
                         "-of", "csv=p=0"])
    result: list[float] = []
    for line in output.splitlines():
        value = line.strip().split(",", 1)[0]
        if value and value != "N/A":
            result.append(float(value))
    return result


def stream_end_time(path: Path, selector: str) -> float:
    output = probe(path, ["-select_streams", selector, "-show_entries",
                         "packet=pts_time,duration_time", "-of", "csv=p=0"])
    ends: list[float] = []
    for line in output.splitlines():
        fields = line.strip().split(",")
        if len(fields) < 2 or fields[0] == "N/A" or fields[1] == "N/A":
            continue
        ends.append(float(fields[0]) + float(fields[1]))
    if not ends:
        raise RuntimeError(f"{selector} has no timestamped packets")
    return max(ends)


def detect_video_flashes(path: Path) -> list[float]:
    pts_output = probe(path, ["-select_streams", "v:0", "-show_entries",
        "frame=best_effort_timestamp_time", "-of", "csv=p=0"])
    pts: list[float] = []
    for line in pts_output.splitlines():
        value = line.strip().split(",", 1)[0]
        if value and value != "N/A":
            pts.append(float(value))

    # The test track is 160x90. Read a 2x2 luma patch from its center, one
    # decoded frame at a time; this path never sees or imports recorder frames.
    patch = run(["ffmpeg", "-v", "error", "-i", str(path), "-map", "0:v:0",
                 "-vf", "crop=2:2:80:44,format=gray", "-fps_mode", "passthrough",
                 "-f", "rawvideo", "-pix_fmt", "gray", "pipe:1"])
    if len(patch) != len(pts) * 4:
        raise RuntimeError(f"decoded {len(patch) // 4} video patches for {len(pts)} frame timestamps")
    states = [sum(patch[index:index + 4]) / 4.0 > 100.0
              for index in range(0, len(patch), 4)]
    flashes: list[float] = []
    previous = False
    for timestamp, active in zip(pts, states):
        if active and not previous:
            flashes.append(timestamp)
        previous = active
    return flashes


def detect_audio_pulses(path: Path, audio_ordinal: int) -> list[float]:
    selector = f"a:{audio_ordinal}"
    completed = subprocess.run([
        "ffmpeg", "-hide_banner", "-nostats", "-copyts", "-i", str(path),
        "-map", f"0:{selector}", "-af", "silencedetect=noise=-35dB:d=0.02",
        "-f", "null", "-"], check=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True)
    # silencedetect's silence_end is the first non-silent sample time. Its
    # timestamp comes from the decoded packet timeline, independently of the
    # fixture's intended marker schedule and AudioSession sync counter.
    detected = [float(value) for value in re.findall(
        r"silence_end:\s*(-?[0-9]+(?:\.[0-9]+)?)", completed.stderr)]
    # silencedetect emits one final silence_end when it closes an unfinished
    # silence interval at EOF. That is a decoder flush event, not an audio
    # pulse. Ignore only events at the encoded stream boundary; real fixture
    # pulses end well before recording EOF.
    end_time = stream_end_time(path, selector)
    return [timestamp for timestamp in detected if timestamp < end_time - 0.100]


def fit_drift(audio_times: list[float], video_times: list[float]) -> dict:
    count = min(len(audio_times), len(video_times))
    if count < 3:
        raise RuntimeError(f"need at least 3 paired markers; audio={len(audio_times)}, video={len(video_times)}")
    paired_audio = audio_times[:count]
    paired_video = video_times[:count]
    offsets = [audio - video for audio, video in zip(paired_audio, paired_video)]
    x = [value - paired_video[0] for value in paired_video]
    x_mean = statistics.fmean(x)
    y_mean = statistics.fmean(offsets)
    denom = sum((value - x_mean) ** 2 for value in x)
    slope = (sum((x_value - x_mean) * (y_value - y_mean)
                 for x_value, y_value in zip(x, offsets)) / denom) if denom else math.nan
    intercept = y_mean - slope * x_mean
    residuals = [offset - (intercept + slope * position)
                 for position, offset in zip(x, offsets)]
    return {
        "matched_markers": count,
        "initial_offset_ms": intercept * 1000.0,
        "drift_ppm": slope * 1_000_000.0,
        "max_fit_residual_ms": max(abs(value) for value in residuals) * 1000.0,
        "offsets_ms": [value * 1000.0 for value in offsets],
        "audio_marker_times_s": paired_audio,
        "video_marker_times_s": paired_video,
    }


def validate_packet_order(path: Path, stream_count: int) -> dict[str, int]:
    selectors = ["v:0", "a:0", "a:1"]
    if stream_count != len(selectors):
        raise RuntimeError(f"expected three streams (H.264 plus two AAC), found {stream_count}")
    counts: dict[str, int] = {}
    for name, selector in zip(("video", "track0", "track1"), selectors):
        times = packet_times(path, selector)
        if not times or any(second <= first for first, second in zip(times, times[1:])):
            raise RuntimeError(f"{name} DTS is empty or not strictly increasing")
        counts[name] = len(times)
    return counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--events", type=Path)
    args = parser.parse_args()
    events_path = args.events or Path(str(args.recording) + ".events.json")
    if not args.recording.is_file() or not events_path.is_file():
        raise RuntimeError("recording or marker sidecar does not exist")

    events = json.loads(events_path.read_text(encoding="utf-8"))
    streams = stream_info(args.recording)
    counts = validate_packet_order(args.recording, len(streams))
    if [stream["codec_type"] for stream in streams].count("video") != 1 or \
       [stream["codec_type"] for stream in streams].count("audio") != 2:
        raise RuntimeError("expected one video stream and two audio streams")

    video_markers = detect_video_flashes(args.recording)
    track0_markers = detect_audio_pulses(args.recording, 0)
    track1_markers = detect_audio_pulses(args.recording, 1)
    results = {
        "track0": fit_drift(track0_markers, video_markers),
        "track1": fit_drift(track1_markers, video_markers),
    }
    expected = len(events["markers_ns"])
    if len(video_markers) != expected:
        raise RuntimeError(f"expected {expected} video flashes, found {len(video_markers)}")
    for source, markers in (("track0", track0_markers), ("track1", track1_markers)):
        if len(markers) != expected:
            raise RuntimeError(f"expected {expected} {source} pulses, found {len(markers)}")

    report = {
        "recording": str(args.recording),
        "clock": events["clock"],
        "duration_seconds": events["duration_seconds"],
        "pulse_period_ns": events["pulse_period_ns"],
        "packets": counts,
        "streams": streams,
        "av_sync": results,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"audio A/V sync analysis: {error}", file=sys.stderr)
        raise SystemExit(1)
