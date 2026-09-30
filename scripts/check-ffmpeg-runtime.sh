#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage:
  check-ffmpeg-runtime.sh --library-only PRIVATE_LIB_DIR
  check-ffmpeg-runtime.sh FLUXREC_RECORD_PATH PRIVATE_LIB_DIR
EOF
}

fail() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

check_ldd_resolution() {
  local object="$1"
  local private_dir="$2"
  local search_path="$3"
  shift 3
  local soname
  local output
  local selected
  local selected_real
  local expected_real

  output="$(LD_LIBRARY_PATH="$search_path" ldd -r "$object" 2>&1)" || {
    printf '%s\n' "$output" >&2
    fail "ldd -r failed for $object"
  }
  if grep -Eq 'not found|undefined symbol:' <<<"$output"; then
    printf 'error: unresolved dependency for %s:\n%s\n' "$object" "$output" >&2
    exit 1
  fi

  for soname in "$@"; do
    selected="$(awk -v soname="$soname" '$1 == soname && $2 == "=>" { print $3; exit }' <<<"$output")"
    [[ -n "$selected" ]] || {
      printf '%s\n' "$output" >&2
      fail "ldd did not report $soname for $object"
    }
    selected_real="$(realpath -e -- "$selected")"
    expected_real="$(realpath -e -- "$private_dir/$soname")"
    [[ "$selected_real" == "$expected_real" ]] || {
      fail "$object resolves $soname to $selected_real, expected private $expected_real"
    }
  done
}

check_private_libraries() {
  local private_dir="$1"
  local avutil="$private_dir/libavutil.so.61"
  local avfilter="$private_dir/libavfilter.so.12"
  local swscale="$private_dir/libswscale.so.10"
  local manifest="$private_dir/BUILD-MANIFEST"

  for library in "$avutil" "$avfilter" "$swscale"; do
    [[ -f "$library" ]] || fail "missing private FFmpeg runtime library: $library"
  done

  check_ldd_resolution "$avutil" "$private_dir" "$private_dir"
  check_ldd_resolution "$swscale" "$private_dir" "$private_dir" libavutil.so.61
  check_ldd_resolution "$avfilter" "$private_dir" "$private_dir" libavutil.so.61 libswscale.so.10

  python3 - "$private_dir" <<'PY'
import ctypes
import os
import sys

directory = sys.argv[1]
mode = getattr(os, "RTLD_NOW", 2) | getattr(os, "RTLD_LOCAL", 0)

def load(soname):
    path = os.path.join(directory, soname)
    try:
        return ctypes.CDLL(path, mode=mode)
    except OSError as exc:
        print(f"cannot load private {soname} from {path}: {exc}", file=sys.stderr)
        raise SystemExit(1)

def check_version(library, soname, symbol, expected):
    try:
        version_fn = getattr(library, symbol)
    except AttributeError:
        print(f"{soname} does not export {symbol}", file=sys.stderr)
        raise SystemExit(1)
    version_fn.restype = ctypes.c_uint
    value = int(version_fn())
    actual = ((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF)
    if actual != expected:
        print(f"expected {soname} {expected}, got {actual}", file=sys.stderr)
        raise SystemExit(1)

# Preload the private dependency chain so RTLD_NOW resolves each DSO against
# these exact files rather than a host copy with the same SONAME.
avutil = load("libavutil.so.61")
swscale = load("libswscale.so.10")
avfilter = load("libavfilter.so.12")
check_version(avutil, "libavutil.so.61", "avutil_version", (61, 1, 102))
check_version(swscale, "libswscale.so.10", "swscale_version", (10, 1, 102))
check_version(avfilter, "libavfilter.so.12", "avfilter_version", (12, 1, 102))

try:
    find_hwdevice_type = avutil.av_hwdevice_find_type_by_name
except AttributeError:
    print("private libavutil does not export av_hwdevice_find_type_by_name", file=sys.stderr)
    raise SystemExit(1)
find_hwdevice_type.argtypes = (ctypes.c_char_p,)
find_hwdevice_type.restype = ctypes.c_int
cuda_hwdevice_type = int(find_hwdevice_type(b"cuda"))
if cuda_hwdevice_type == 0:  # AV_HWDEVICE_TYPE_NONE.
    print("private libavutil does not know the CUDA hardware-device enum", file=sys.stderr)
    raise SystemExit(1)

try:
    iterate_hwdevice_types = avutil.av_hwdevice_iterate_types
except AttributeError:
    print("private libavutil does not export av_hwdevice_iterate_types", file=sys.stderr)
    raise SystemExit(1)
iterate_hwdevice_types.argtypes = (ctypes.c_int,)
iterate_hwdevice_types.restype = ctypes.c_int
registered_types = set()
previous_type = 0  # AV_HWDEVICE_TYPE_NONE.
for _ in range(64):
    current_type = int(iterate_hwdevice_types(previous_type))
    if current_type == 0:  # No more registered hardware-device contexts.
        break
    if current_type <= previous_type:
        print("av_hwdevice_iterate_types returned a non-increasing device type", file=sys.stderr)
        raise SystemExit(1)
    registered_types.add(current_type)
    previous_type = current_type
else:
    print("av_hwdevice_iterate_types did not terminate after 64 device types", file=sys.stderr)
    raise SystemExit(1)

if cuda_hwdevice_type not in registered_types:
    print(
        "private libavutil knows the CUDA enum but has no registered CUDA hwcontext",
        file=sys.stderr,
    )
    raise SystemExit(1)

try:
    format_fn = avutil.av_vkfmt_from_pixfmt
except AttributeError:
    print("private libavutil does not export av_vkfmt_from_pixfmt", file=sys.stderr)
    raise SystemExit(1)
format_fn.argtypes = (ctypes.c_int,)
format_fn.restype = ctypes.POINTER(ctypes.c_int)
formats = format_fn(28)  # AV_PIX_FMT_BGRA in FFmpeg 9.0.2.
if not formats:
    print("av_vkfmt_from_pixfmt(AV_PIX_FMT_BGRA) returned null", file=sys.stderr)
    raise SystemExit(1)
vk_format = int(formats[0])
if vk_format != 44:  # VK_FORMAT_B8G8R8A8_UNORM.
    print(f"AV_PIX_FMT_BGRA maps to Vulkan format {vk_format}; expected B8 format 44", file=sys.stderr)
    raise SystemExit(1)

try:
    get_filter = avfilter.avfilter_get_by_name
except AttributeError:
    print("private libavfilter does not export avfilter_get_by_name", file=sys.stderr)
    raise SystemExit(1)
get_filter.argtypes = (ctypes.c_char_p,)
get_filter.restype = ctypes.c_void_p
required_filters = ("buffer", "buffersink", "scale", "scale_vulkan", "blackdetect_vulkan")
missing_filters = [name for name in required_filters if not get_filter(name.encode("ascii"))]
if missing_filters:
    print("private libavfilter is missing required filters: " + ", ".join(missing_filters), file=sys.stderr)
    raise SystemExit(1)

try:
    set_timing_callback = avfilter.avfilter_graph_set_fluxrec_device_timing_callback
except AttributeError:
    print("private libavfilter does not export the Fluxrec Vulkan timing callback API", file=sys.stderr)
    raise SystemExit(1)
set_timing_callback.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
set_timing_callback.restype = None
avfilter_graph_alloc = avfilter.avfilter_graph_alloc
avfilter_graph_alloc.restype = ctypes.c_void_p
avfilter_graph_free = avfilter.avfilter_graph_free
avfilter_graph_free.argtypes = (ctypes.POINTER(ctypes.c_void_p),)
graph = avfilter_graph_alloc()
if not graph:
    print("private libavfilter could not allocate a CPU-only timing callback test graph", file=sys.stderr)
    raise SystemExit(1)
set_timing_callback(graph, None, None)
graph_ref = ctypes.c_void_p(graph)
avfilter_graph_free(ctypes.byref(graph_ref))
if graph_ref.value:
    print("private libavfilter did not free the CPU-only timing callback test graph", file=sys.stderr)
    raise SystemExit(1)

print("Private FFmpeg runtime: avutil 61.1.102, avfilter 12.1.102, swscale 10.1.102")
print("AV_PIX_FMT_BGRA -> VK_FORMAT_B8G8R8A8_UNORM (44); CUDA hwcontext is compiled")
print("Required Vulkan filters are registered; Vulkan-to-CUDA transfer support is compiled")
print("Fluxrec GPU conversion telemetry callback is exported and accepts a CPU-only graph registration")
PY

  grep -aFq 'FLUXREC_FF_VK_WAIT_SIMPLE' "$avfilter" || \
    fail "$avfilter does not contain the environment-gated Vulkan wait patch marker"
  grep -aFq 'avfilter_graph_set_fluxrec_device_timing_callback' "$avfilter" || \
    fail "$avfilter does not export the Fluxrec Vulkan timing callback API"

  if [[ -f "$manifest" ]]; then
    local required_manifest_lines=(
      'nv-codec-headers release: n12.1.14.0'
      'nv-codec-headers commit: 1889e62e2d35ff7aa9baca2bceb14f053785e6f1'
      'nv-codec-headers archive SHA-256: e30d4791fd386de8b5c9f88dec781e546ec148b8808440dd421da5f26c2700ae'
      'CUDA hwcontext: enabled (CONFIG_CUDA)'
      'Vulkan-to-CUDA frame transfer: compiled (requires runtime external memory and semaphore support)'
      'GPU conversion timing: callback API emits completed per-pass timestamp samples; recorder joins passes by frame PTS into one per-frame telemetry sample'
    )
    local required_line
    for required_line in "${required_manifest_lines[@]}"; do
      grep -Fqx "$required_line" "$manifest" || \
        fail "$manifest is missing required build capability metadata: $required_line"
    done
  fi
}

if [[ "${1:-}" == "--library-only" ]]; then
  [[ $# -eq 2 ]] || { usage; exit 2; }
  private_dir="$(realpath -e -- "$2")"
  check_private_libraries "$private_dir"
  exit 0
fi

[[ $# -eq 2 ]] || { usage; exit 2; }
recorder="$(realpath -e -- "$1")"
private_dir="$(realpath -e -- "$2")"
[[ -x "$recorder" ]] || fail "recorder is not executable: $recorder"

dynamic_section="$(readelf -d "$recorder" 2>&1)" || {
  printf '%s\n' "$dynamic_section" >&2
  exit 1
}
if ! grep -Fq "$private_dir" <<<"$dynamic_section" || ! grep -Eq '\(RPATH\)' <<<"$dynamic_section"; then
  printf 'error: %s does not embed the private library directory in DT_RPATH\n' "$recorder" >&2
  printf '%s\n' "$dynamic_section" | grep -E 'RPATH|RUNPATH' >&2 || true
  exit 1
fi

check_ldd_resolution "$recorder" "$private_dir" /usr/lib \
  libavutil.so.61 libavfilter.so.12 libswscale.so.10
printf 'Recorder runtime resolution: %s uses private avutil, avfilter, and swscale from %s\n' \
  "$recorder" "$private_dir"
check_private_libraries "$private_dir"
