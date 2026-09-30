#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source_tree="${FLUXREC_FFMPEG_SOURCE_TREE:-$repo_root/.deps/src/ffmpeg-9.0.2}"
build_root="${FLUXREC_NVENC_API_TIMING_BUILD_ROOT:-$repo_root/.validation/ffmpeg-nvenc-api-timing}"
source_copy="$build_root/source"
private_lib="$build_root/lib"
runtime_dir="$build_root/runtime"
patch_file="$repo_root/patches/ffmpeg-n9.0.2-nvenc-api-timing.patch"
patch_sha256="921e1dd19af01daa040c32e00b4dcf749a1520dba0bd2c676fad7dd20de3d20f"
jobs="${FLUXREC_BUILD_JOBS:-4}"

fail() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

verify_sha256() {
  local expected="$1"
  local path="$2"
  local actual
  actual="$(sha256sum "$path" | awk '{print $1}')"
  [[ "$actual" == "$expected" ]] || \
    fail "SHA-256 mismatch for $path (expected $expected, got $actual)"
}

command -v cp >/dev/null 2>&1 || fail "cp is required"
command -v make >/dev/null 2>&1 || fail "make is required"
command -v patch >/dev/null 2>&1 || fail "patch is required"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum is required"
command -v nm >/dev/null 2>&1 || fail "nm is required to validate the private callback export"
command -v ln >/dev/null 2>&1 || fail "ln is required to stage the isolated runtime"
command -v pkg-config >/dev/null 2>&1 || fail "pkg-config is required to check the host FFmpeg ABI"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail "FLUXREC_BUILD_JOBS must be a positive integer"
[[ -f "$source_tree/configure" && -f "$source_tree/ffbuild/config.mak" ]] || \
  fail "configured FFmpeg source tree not found at $source_tree; run scripts/build-ffmpeg-bgra-b8.sh first"
[[ "$(tr -d '\r\n' < "$source_tree/VERSION")" == "9.0.2" ]] || \
  fail "expected FFmpeg 9.0.2 source in $source_tree"
for config_flag in CONFIG_FFNVCODEC CONFIG_H264_NVENC_ENCODER CONFIG_HEVC_NVENC_ENCODER; do
  grep -qx "${config_flag}=yes" "$source_tree/ffbuild/config.mak" || \
    fail "configured FFmpeg source does not enable ${config_flag}"
done
verify_sha256 "$patch_sha256" "$patch_file"

mkdir -p "$build_root"
if [[ ! -f "$source_copy/ffbuild/config.mak" ]]; then
  if [[ -e "$source_copy" ]]; then
    fail "$source_copy exists but is not a configured FFmpeg source tree; remove it and retry"
  fi
  printf 'Copying configured FFmpeg source into isolated build tree %s\n' "$source_copy"
  cp -a -- "$source_tree" "$source_copy"
fi
[[ "$(tr -d '\r\n' < "$source_copy/VERSION")" == "9.0.2" ]] || \
  fail "expected FFmpeg 9.0.2 source in isolated tree $source_copy"

nvenc_source="$source_copy/libavcodec/nvenc.c"
callback_marker='avcodec_set_fluxrec_nvenc_api_timing_callback'
if grep -Fq "$callback_marker" "$nvenc_source" && \
   grep -Fq "$callback_marker" "$source_copy/libavcodec/nvenc.h" && \
   grep -Fq 'nvenc_lock_bitstream_api_wall' "$nvenc_source" && \
   grep -Fq 'nvenc_encode_picture_api_wall' "$nvenc_source" && \
   grep -Fq 'fluxrec_api_monotonic_ns' "$nvenc_source" && \
   grep -Fq 'av_gettime_relative_is_monotonic' "$nvenc_source" && \
   grep -Fq 'event_monotonic_ns' "$source_copy/libavcodec/nvenc.h"; then
  printf 'NVENC API timing patch is already applied to isolated FFmpeg source\n'
elif grep -Fq "$callback_marker" "$nvenc_source" || \
     grep -Fq "$callback_marker" "$source_copy/libavcodec/nvenc.h" || \
     grep -Fq 'nvenc_lock_bitstream_api_wall' "$nvenc_source" || \
     grep -Fq 'nvenc_encode_picture_api_wall' "$nvenc_source" || \
     grep -Fq 'fluxrec_api_monotonic_ns' "$nvenc_source" || \
     grep -Fq 'av_gettime_relative_is_monotonic' "$nvenc_source" || \
     grep -Fq 'event_monotonic_ns' "$source_copy/libavcodec/nvenc.h"; then
  if grep -Fq "$callback_marker" "$nvenc_source" && \
     { ! grep -Fq 'fluxrec_api_monotonic_ns' "$nvenc_source" || \
       ! grep -Fq 'av_gettime_relative_is_monotonic' "$nvenc_source"; }; then
    fail "stale NVENC timing patch in $source_copy; set FLUXREC_NVENC_API_TIMING_BUILD_ROOT to a fresh directory"
  fi
  fail "NVENC API timing patch is only partially present in $source_copy"
elif patch --batch --dry-run --forward --directory="$source_copy" \
     --strip=1 --input="$patch_file" >/dev/null 2>&1; then
  patch --batch --forward --directory="$source_copy" \
    --strip=1 --input="$patch_file"
else
  fail "NVENC API timing patch does not apply cleanly to FFmpeg 9.0.2"
fi

printf 'Building private libavcodec with %s jobs\n' "$jobs"
make -C "$source_copy" -j "$jobs" libavcodec/libavcodec.so

avcodec_major="$(awk '$2 == "LIBAVCODEC_VERSION_MAJOR" { gsub(/[^0-9]/, "", $3); print $3; exit }' \
  "$source_copy/libavcodec/version_major.h")"
[[ "$avcodec_major" =~ ^[0-9]+$ ]] || fail "could not read FFmpeg libavcodec ABI major"
host_avcodec_major="$(pkg-config --modversion libavcodec | cut -d. -f1)"
[[ "$host_avcodec_major" == "$avcodec_major" ]] || \
  fail "isolated FFmpeg libavcodec ABI major $avcodec_major does not match the host development headers $host_avcodec_major"
built_lib="$source_copy/libavcodec/libavcodec.so.$avcodec_major"
[[ -f "$built_lib" ]] || fail "FFmpeg did not produce $built_lib"
mkdir -p "$private_lib"
install -m 755 "$built_lib" "$private_lib/libavcodec.so.$avcodec_major.new.$$"
mv -f -- "$private_lib/libavcodec.so.$avcodec_major.new.$$" \
  "$private_lib/libavcodec.so.$avcodec_major"
ln -sfn "libavcodec.so.$avcodec_major" "$private_lib/libavcodec.so"

nm -D --defined-only "$private_lib/libavcodec.so.$avcodec_major" | \
  awk '{print $3}' | cut -d@ -f1 | grep -Fx "$callback_marker" >/dev/null || \
  fail "$private_lib/libavcodec.so.$avcodec_major does not export $callback_marker"

dependency_dir="${FLUXREC_PRIVATE_FFMPEG_LIBDIR:-$repo_root/.deps/ffmpeg-bgra-b8/lib}"
[[ -d "$dependency_dir" ]] || fail "private FFmpeg dependency directory not found: $dependency_dir"
resolved="$(LD_LIBRARY_PATH="$private_lib:$dependency_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ldd -r "$private_lib/libavcodec.so.$avcodec_major" 2>&1)" || {
    printf '%s\n' "$resolved" >&2
    fail "ldd -r could not load the isolated private libavcodec"
  }
if grep -Eq 'not found|undefined symbol:' <<<"$resolved"; then
  printf '%s\n' "$resolved" >&2
  fail "isolated private libavcodec has unresolved runtime dependencies"
fi

mkdir -p "$runtime_dir"
for soname in libavutil.so.61 libavfilter.so.12 libswscale.so.10; do
  [[ -e "$dependency_dir/$soname" ]] || fail "missing private FFmpeg dependency: $dependency_dir/$soname"
  ln -sfn "$dependency_dir/$soname" "$runtime_dir/$soname"
done
ln -sfn "$private_lib/libavcodec.so.$avcodec_major" \
  "$runtime_dir/libavcodec.so.$avcodec_major"
ln -sfn "libavcodec.so.$avcodec_major" "$runtime_dir/libavcodec.so"

cat >"$private_lib/BUILD-MANIFEST" <<EOF
FFmpeg release: 9.0.2
FFmpeg source tree: $source_tree
NVENC API host-timing patch SHA-256: $patch_sha256
Callbacks: accepted nvEncEncodePicture input PTS and successful nvEncLockBitstream output PTS
Timing source: av_gettime_relative host monotonic API spans and return timestamps, converted from microseconds to nanoseconds
Completion proxy: PTS-matched accepted-submit return to successful bitstream-lock return wall time
Scope: isolated libavcodec only; not installed into the repo-default private runtime
EOF

printf 'Built isolated NVENC API timing runtime at %s\n' \
  "$private_lib/libavcodec.so.$avcodec_major"
printf 'CMake private runtime overlay: %s\n' "$runtime_dir"
printf 'Set FLUXREC_FF_NVENC_API_TIMING=1 when launching the isolated recorder.\n'
