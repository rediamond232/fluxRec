#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
audit_root="$repo_root/.validation/ffmpeg-zero-cpu-audit"
source_avutil="$audit_root/source-avutil"
source_filter="$audit_root/source-filter"
runtime_lib="$audit_root/lib"
recorder_build="$audit_root/recorder-build"
production_source_avutil="$repo_root/.deps/src/ffmpeg-9.0.2"
production_source_filter="$repo_root/.deps/src/ffmpeg-9.0.2-filterwait"
production_lib="$repo_root/.deps/ffmpeg-bgra-b8/lib"
avutil_patch="$repo_root/patches/ffmpeg-n9.0.2-zero-cpu-route-audit-avutil.patch"
avfilter_patch="$repo_root/patches/ffmpeg-n9.0.2-zero-cpu-route-audit-avfilter.patch"
avutil_patch_sha="a23d9ac8e83ac81ee815229b2c4c657615a87e8c3d38423ef0f0bc666212c860"
avfilter_patch_sha="dd233f667e697effa35885bf783573fe0565246fecd2264c614e8546822e45b4"

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

apply_audit_patch() {
  local source_tree="$1"
  local patch_file="$2"
  local marker="$3"
  local marker_source="$4"
  if grep -Fq "$marker" "$marker_source"; then
    printf 'Audit patch already present in %s\n' "$marker_source"
    return
  fi
  if grep -Fq 'FLUXREC_ZERO_CPU_AUDIT' "$marker_source"; then
    fail "partial zero-CPU audit patch in $marker_source"
  fi
  patch --batch --dry-run --forward --directory="$source_tree" \
    --strip=1 --input="$patch_file" >/dev/null || \
    fail "zero-CPU audit patch does not apply cleanly to $source_tree"
  patch --batch --forward --directory="$source_tree" \
    --strip=1 --input="$patch_file"
  grep -Fq "$marker" "$marker_source" || \
    fail "zero-CPU audit patch did not install expected marker in $marker_source"
}

build_overlay() {
  verify_sha256 "$avutil_patch_sha" "$avutil_patch"
  verify_sha256 "$avfilter_patch_sha" "$avfilter_patch"
  [[ -x "$production_source_avutil/configure" && -x "$production_source_filter/configure" ]] || \
    fail "prepare the standard private FFmpeg sources first with scripts/build-ffmpeg-bgra-b8.sh"
  [[ "$(tr -d '\r\n' < "$production_source_avutil/VERSION")" == "9.0.2" ]] || \
    fail "expected FFmpeg 9.0.2 in $production_source_avutil"
  [[ "$(tr -d '\r\n' < "$production_source_filter/VERSION")" == "9.0.2" ]] || \
    fail "expected FFmpeg 9.0.2 in $production_source_filter"
  [[ -f "$production_lib/BUILD-MANIFEST" ]] || \
    fail "standard private FFmpeg runtime is missing $production_lib/BUILD-MANIFEST"
  [[ -f "$repo_root/.deps/ffmpeg-bgra-b8/ffnvcodec/lib/pkgconfig/ffnvcodec.pc" ]] || \
    fail "staged ffnvcodec pkg-config metadata is missing"
  [[ -f "$repo_root/.deps/src/amf-headers-v1.5.2/AMF/core/Factory.h" ]] || \
    fail "staged AMF headers are missing"

  rm -rf -- "$source_avutil" "$source_filter" "$runtime_lib"
  mkdir -p "$audit_root" "$source_avutil" "$source_filter" "$runtime_lib" "$audit_root/logs"
  cp -a "$production_source_avutil/." "$source_avutil/"
  cp -a "$production_source_filter/." "$source_filter/"

  make -C "$source_avutil" distclean >"$audit_root/logs/avutil-distclean.log" 2>&1 || \
    fail "could not clean isolated libavutil source; see $audit_root/logs/avutil-distclean.log"
  make -C "$source_filter" distclean >"$audit_root/logs/filter-distclean.log" 2>&1 || \
    fail "could not clean isolated libavfilter source; see $audit_root/logs/filter-distclean.log"

  apply_audit_patch "$source_avutil" "$avutil_patch" \
    'event=process_frame_route_summary' "$source_avutil/libavutil/hwcontext_vulkan.c"
  apply_audit_patch "$source_filter" "$avfilter_patch" \
    'event=scale_vulkan_submit' "$source_filter/libavfilter/vf_scale_vulkan.c"

  local jobs="${FLUXREC_BUILD_JOBS:-4}"
  [[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail "FLUXREC_BUILD_JOBS must be a positive integer"
  local pkgconfig_path="$repo_root/.deps/ffmpeg-bgra-b8/ffnvcodec/lib/pkgconfig"
  local amf_include="$repo_root/.deps/src/amf-headers-v1.5.2"
  local avutil_configure_log="$audit_root/logs/avutil-configure.log"
  if ! (
    cd "$source_avutil"
    PKG_CONFIG_PATH="$pkgconfig_path" ./configure \
      --prefix="$audit_root/install-avutil" \
      --enable-shared --disable-static --disable-programs --disable-doc \
      --enable-vulkan --enable-libdrm --enable-amf --enable-ffnvcodec \
      "--extra-cflags=-I$amf_include" >"$avutil_configure_log" 2>&1
  ); then
    cat "$avutil_configure_log" >&2
    fail "isolated libavutil configure failed"
  fi
  for flag in CONFIG_VULKAN CONFIG_AMF CONFIG_FFNVCODEC CONFIG_CUDA; do
    grep -qx "$flag=yes" "$source_avutil/ffbuild/config.mak" || \
      fail "isolated libavutil did not enable $flag"
  done
  if ! make -C "$source_avutil" -j "$jobs" libavutil/libavutil.so \
      >"$audit_root/logs/avutil-build.log" 2>&1; then
    cat "$audit_root/logs/avutil-build.log" >&2
    fail "isolated libavutil build failed"
  fi

  local filter_configure_log="$audit_root/logs/filter-configure.log"
  if ! (
    cd "$source_filter"
    ./configure --prefix="$audit_root/install-filter" \
      --enable-shared --disable-static --disable-programs --disable-doc \
      --disable-avdevice --disable-avcodec --disable-avformat --disable-swresample \
      --disable-everything --enable-vulkan --enable-libdrm --enable-swscale \
      --enable-filter=buffer --enable-filter=buffersink --enable-filter=scale \
      --enable-filter=scale_vulkan --enable-filter=blackdetect_vulkan \
      >"$filter_configure_log" 2>&1
  ); then
    cat "$filter_configure_log" >&2
    fail "isolated libavfilter configure failed"
  fi
  for flag in CONFIG_VULKAN CONFIG_SWSCALE CONFIG_AVFILTER CONFIG_SCALE_VULKAN_FILTER; do
    grep -qx "$flag=yes" "$source_filter/ffbuild/config.mak" || \
      fail "isolated libavfilter did not enable $flag"
  done
  if ! make -C "$source_filter" -j "$jobs" \
      libavfilter/libavfilter.so libswscale/libswscale.so \
      >"$audit_root/logs/filter-build.log" 2>&1; then
    cat "$audit_root/logs/filter-build.log" >&2
    fail "isolated libavfilter/libswscale build failed"
  fi

  install -m 755 "$source_avutil/libavutil/libavutil.so.61" \
    "$runtime_lib/libavutil.so.61.1.102"
  install -m 755 "$source_filter/libavfilter/libavfilter.so.12" \
    "$runtime_lib/libavfilter.so.12.1.102"
  install -m 755 "$source_filter/libswscale/libswscale.so.10" \
    "$runtime_lib/libswscale.so.10.1.102"
  ln -sfn libavutil.so.61.1.102 "$runtime_lib/libavutil.so.61"
  ln -sfn libavutil.so.61.1.102 "$runtime_lib/libavutil.so"
  ln -sfn libavfilter.so.12.1.102 "$runtime_lib/libavfilter.so.12"
  ln -sfn libavfilter.so.12.1.102 "$runtime_lib/libavfilter.so"
  ln -sfn libswscale.so.10.1.102 "$runtime_lib/libswscale.so.10"
  ln -sfn libswscale.so.10.1.102 "$runtime_lib/libswscale.so"
  cp "$production_lib/BUILD-MANIFEST" "$runtime_lib/BUILD-MANIFEST"
  cat >>"$runtime_lib/BUILD-MANIFEST" <<EOF
Zero CPU route audit avutil patch SHA-256: $avutil_patch_sha
Zero CPU route audit avfilter patch SHA-256: $avfilter_patch_sha
Zero CPU route audit activation: FLUXREC_FF_ZERO_CPU_AUDIT=1 (opt-in)
EOF

  grep -aFq 'FLUXREC_ZERO_CPU_AUDIT event=dmabuf_vulkan_import' \
    "$runtime_lib/libavutil.so.61.1.102" || fail "staged avutil lacks its import audit marker"
  grep -aFq 'FLUXREC_ZERO_CPU_AUDIT event=scale_vulkan_submit' \
    "$runtime_lib/libavfilter.so.12.1.102" || fail "staged avfilter lacks its scale audit marker"
  grep -aFq 'event=vulkan_to_cuda_copy_submit' \
    "$runtime_lib/libavutil.so.61.1.102" || fail "staged avutil lacks its CUDA copy audit marker"
  "$repo_root/scripts/check-ffmpeg-runtime.sh" --library-only "$runtime_lib"
  printf 'Isolated zero-CPU route-audit libraries staged at %s\n' "$runtime_lib"
  printf 'No files in .deps/ffmpeg-bgra-b8/lib were changed.\n'
}

build_recorder() {
  [[ -f "$runtime_lib/BUILD-MANIFEST" ]] || \
    fail "build the isolated FFmpeg overlay first: $0 build-overlay"
  cmake -S "$repo_root" -B "$recorder_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DFLUXREC_PRIVATE_FFMPEG_LIBDIR="$runtime_lib"
  local jobs="${FLUXREC_BUILD_JOBS:-4}"
  cmake --build "$recorder_build" --target fluxrec-record -j "$jobs"
  "$repo_root/scripts/check-ffmpeg-runtime.sh" \
    "$recorder_build/engine/fluxrec-record" "$runtime_lib"
  printf 'Audit recorder: %s/engine/fluxrec-record\n' "$recorder_build"
  printf 'RPATH library overlay: %s\n' "$runtime_lib"
}

case "${1:-build-overlay}" in
  build-overlay) build_overlay ;;
  build-recorder) build_recorder ;;
  all) build_overlay; build_recorder ;;
  *) fail "usage: $0 [build-overlay|build-recorder|all]" ;;
esac
