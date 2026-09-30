#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
run_stamp="$(date -u +%Y%m%d-%H%M%S)"
output_prefix="${TMPDIR:-/tmp}/fluxrec-cavlc-ab-${run_stamp}"

common_args=(
  --display DP-1
  --stimulus-seconds 18
  --record-seconds 12
  --pattern plasma
  --motion 0.20
  --codec h264
  --conversion two-pass
  --bitrate-mbps 50
  --gop 120
  --pool 48
  --async-depth 8
  --audio off
  --settle-seconds 0.20
)

# Explicit CABAC control, compared with the recorder's default VLC/CAVLC path.
python3 "${script_dir}/controlled_benchmark.py" \
  "${common_args[@]}" \
  --h264-coder cabac \
  --output "${output_prefix}-cabac.mkv"

# FFmpeg's `vlc` mode is CAVLC for the H.264 encoder.
python3 "${script_dir}/controlled_benchmark.py" \
  "${common_args[@]}" \
  --h264-coder vlc \
  --output "${output_prefix}-cavlc.mkv"
