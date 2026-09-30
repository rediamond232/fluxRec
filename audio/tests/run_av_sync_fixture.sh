#!/usr/bin/env bash
set -euo pipefail

if (($# < 1 || $# > 4)); then
  echo "usage: $0 BUILD_DIR [OUTPUT.mkv] [SECONDS] [PULSE_PERIOD_MS]" >&2
  exit 2
fi

build_dir=$1
output=${2:-"/tmp/fluxrec-av-sync-$(date +%Y%m%d-%H%M%S).mkv"}
seconds=${3:-60}
pulse_period_ms=${4:-5000}
fixture="$build_dir/audio/fluxrec-audio-av-sync-fixture"
analyzer="$(dirname "$0")/analyze_av_sync.py"

command -v pactl >/dev/null || { echo "pactl is required" >&2; exit 1; }
command -v ffprobe >/dev/null || { echo "ffprobe is required" >&2; exit 1; }
command -v ffmpeg >/dev/null || { echo "ffmpeg is required" >&2; exit 1; }
[[ -x "$fixture" ]] || { echo "fixture executable missing: $fixture" >&2; exit 1; }

desktop_module=
microphone_module=
fixture_pid=
cleanup() {
  if [[ -n "$fixture_pid" ]] && kill -0 "$fixture_pid" 2>/dev/null; then
    kill -TERM "$fixture_pid" 2>/dev/null || true
    wait "$fixture_pid" 2>/dev/null || true
  fi
  if [[ -n "$microphone_module" ]]; then pactl unload-module "$microphone_module" >/dev/null 2>&1 || true; fi
  if [[ -n "$desktop_module" ]]; then pactl unload-module "$desktop_module" >/dev/null 2>&1 || true; fi
}
trap cleanup EXIT INT TERM

desktop_module=$(pactl load-module module-null-sink \
  sink_name=fluxrec_sync_desktop rate=48000 channels=2)
microphone_module=$(pactl load-module module-null-sink \
  sink_name=fluxrec_sync_microphone rate=48000 channels=2)

# These ephemeral sinks are explicit targets. They are never made default, so
# the test pulses remain in PipeWire and are not sent to the user's speakers.
pactl list short sinks | awk '$2 == "fluxrec_sync_desktop" { found=1 } END { exit !found }'
pactl list short sinks | awk '$2 == "fluxrec_sync_microphone" { found=1 } END { exit !found }'
pactl list short sources | awk '$2 == "fluxrec_sync_microphone.monitor" { found=1 } END { exit !found }'

desktop_monitor_id=$(pactl list short sources | awk '$2 == "fluxrec_sync_desktop.monitor" { print $1; exit }')
microphone_monitor_id=$(pactl list short sources | awk '$2 == "fluxrec_sync_microphone.monitor" { print $1; exit }')
if [[ -z "$desktop_monitor_id" || -z "$microphone_monitor_id" ]]; then
  echo "could not resolve the two fixture monitor source IDs" >&2
  exit 1
fi
printf 'explicit monitor source IDs: desktop=%s microphone=%s\n' \
  "$desktop_monitor_id" "$microphone_monitor_id"

declare -A baseline_source_outputs=()
while read -r output_id _; do
  [[ -z "$output_id" ]] || baseline_source_outputs["$output_id"]=1
done < <(pactl list short source-outputs)
declare -A verified_capture_links=()
seen_desktop_monitor=0
seen_microphone_monitor=0
verify_fixture_capture_links() {
  local output_id source_id
  while read -r output_id source_id _; do
    [[ -z "$output_id" ]] && continue
    [[ -n "${baseline_source_outputs[$output_id]+present}" ]] && continue
    case "$source_id" in
      "$desktop_monitor_id")
        seen_desktop_monitor=1
        verified_capture_links["$output_id"]=$source_id
        ;;
      "$microphone_monitor_id")
        seen_microphone_monitor=1
        verified_capture_links["$output_id"]=$source_id
        ;;
      *)
        echo "fixture capture stream linked to unexpected PipeWire source $source_id" >&2
        return 1
        ;;
    esac
  done < <(pactl list short source-outputs)
}

mkdir -p "$(dirname "$output")"
"$fixture" \
  --desktop-sink fluxrec_sync_desktop \
  --microphone-sink fluxrec_sync_microphone \
  --microphone-source fluxrec_sync_microphone.monitor \
  --output "$output" --seconds "$seconds" \
  --pulse-period-ms "$pulse_period_ms" &
fixture_pid=$!
while kill -0 "$fixture_pid" 2>/dev/null; do
  verify_fixture_capture_links || exit 1
  sleep 0.1
done
wait "$fixture_pid"
fixture_pid=
verify_fixture_capture_links
if ((seen_desktop_monitor == 0 || seen_microphone_monitor == 0)); then
  echo "fixture did not link both capture streams to their explicit monitor sources" >&2
  exit 1
fi
mapfile -t verified_output_ids < <(printf '%s\n' "${!verified_capture_links[@]}" | sort -n)
for output_id in "${verified_output_ids[@]}"; do
  printf 'verified capture link: source-output=%s source=%s\n' \
    "$output_id" "${verified_capture_links[$output_id]}"
done
python3 "$analyzer" "$output"
