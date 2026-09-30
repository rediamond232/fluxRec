#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
deps_root="$repo_root/.deps"
cache_root="$deps_root/cache"
source_root="${FLUXREC_FFMPEG_SOURCE_ROOT:-"$deps_root/src"}"
logs_root="${FLUXREC_FFMPEG_LOG_ROOT:-"$deps_root/logs"}"
build_root="${FLUXREC_FFMPEG_BUILD_ROOT:-"$deps_root/ffmpeg-bgra-b8"}"
ffmpeg_archive="${FLUXREC_FFMPEG_ARCHIVE:-"$cache_root/ffmpeg-9.0.2.tar.xz"}"
amf_archive="${FLUXREC_AMF_HEADERS_ARCHIVE:-"$cache_root/AMF-headers-v1.5.2.tar.gz"}"
ffnvcodec_archive="${FLUXREC_FFNVCODEC_ARCHIVE:-"$cache_root/nv-codec-headers-1889e62.tar.gz"}"
ffmpeg_sha256="8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e"
amf_sha256="d3c12eb324edf05e214608b6a395a51dd95770ed9d45520185d6c3a206811c99"
ffnvcodec_commit="1889e62e2d35ff7aa9baca2bceb14f053785e6f1"
ffnvcodec_version="12.1.14.0"
ffnvcodec_archive_sha256="e30d4791fd386de8b5c9f88dec781e546ec148b8808440dd421da5f26c2700ae"
patch_sha256="d203edf5bbbbd5106d3a33d666ece5d6605c81ab81452b2b8d92ea47fdcb27d9"
filter_wait_patch_sha256="12a16423200365eaa445d705e36649dffe26f135c9224d2224689e2c2c773c16"
timestamp_query_patch_file="$repo_root/patches/ffmpeg-n9.0.2-vulkan-timestamp-query.patch"
filter_timing_patch_file="$repo_root/patches/ffmpeg-n9.0.2-vulkan-filter-timing.patch"
filter_timing_callback_patch_file="$repo_root/patches/ffmpeg-n9.0.2-vulkan-filter-timing-callback.patch"
encode_timing_patch_file="$repo_root/patches/ffmpeg-n9.0.2-vulkan-encode-timing.patch"
timestamp_query_patch_sha256="df223b5163d920ed4fce21502058eb41df99711de9faafca3f7ada8622a9afac"
filter_timing_patch_sha256="db261cbbacf9ca85a65e6eda413cd43152e947c04cf056f015bc08ca84838a13"
filter_timing_callback_patch_sha256="4bbbfd56e09edfccb740545b767816132d2a3aede9efae05551f0f5132825940"
encode_timing_patch_sha256="b46dc777833571f8d60922d4c3b32068742d2dae4b4dd25a27243804c64cb135"
patch_file="$repo_root/patches/ffmpeg-n9.0.2-vulkan-bgra-b8.patch"
filter_wait_patch_file="$repo_root/patches/ffmpeg-n9.0.2-vulkan-filter-wait-env.patch"
ffmpeg_source="$source_root/ffmpeg-9.0.2"
ffmpeg_filter_source="$source_root/ffmpeg-9.0.2-filterwait"
ffnvcodec_source="$source_root/nv-codec-headers-$ffnvcodec_commit"
amf_include_root="$source_root/amf-headers-v1.5.2"
private_lib="$build_root/lib"
ffnvcodec_prefix="$build_root/ffnvcodec"

fail() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

verify_sha256() {
  local expected="$1"
  local path="$2"
  local actual
  actual="$(sha256sum "$path" | awk '{print $1}')"
  [[ "$actual" == "$expected" ]] || fail "SHA-256 mismatch for $path (expected $expected, got $actual)"
}

fetch_verified() {
  local path="$1"
  local url="$2"
  local expected="$3"
  mkdir -p "$(dirname -- "$path")"
  if [[ ! -f "$path" ]]; then
    command -v curl >/dev/null 2>&1 || fail "missing source cache $path and curl is unavailable; provide the verified archive at that path"
    local partial="${path}.part.$$"
    rm -f -- "$partial"
    printf 'Fetching %s\n' "$url"
    if ! curl --fail --location --retry 3 --silent --show-error "$url" --output "$partial"; then
      rm -f -- "$partial"
      fail "could not fetch $url; an offline build can set FLUXREC_FFMPEG_ARCHIVE, FLUXREC_AMF_HEADERS_ARCHIVE, and FLUXREC_FFNVCODEC_ARCHIVE to verified cached archives"
    fi
    if ! verify_sha256 "$expected" "$partial"; then
      rm -f -- "$partial"
      exit 1
    fi
    mv -- "$partial" "$path"
  fi
  verify_sha256 "$expected" "$path"
}

command -v tar >/dev/null 2>&1 || fail "tar is required"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum is required"
command -v patch >/dev/null 2>&1 || fail "patch is required to apply the source patch"
command -v make >/dev/null 2>&1 || fail "make is required"
command -v pkg-config >/dev/null 2>&1 || fail "pkg-config is required to validate the private ffnvcodec headers"

mkdir -p "$source_root" "$logs_root" "$build_root"
verify_sha256 "$patch_sha256" "$patch_file"
verify_sha256 "$filter_wait_patch_sha256" "$filter_wait_patch_file"
verify_sha256 "$timestamp_query_patch_sha256" "$timestamp_query_patch_file"
verify_sha256 "$filter_timing_patch_sha256" "$filter_timing_patch_file"
verify_sha256 "$filter_timing_callback_patch_sha256" "$filter_timing_callback_patch_file"
verify_sha256 "$encode_timing_patch_sha256" "$encode_timing_patch_file"
fetch_verified "$ffmpeg_archive" \
  "https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz" "$ffmpeg_sha256"
fetch_verified "$amf_archive" \
  "https://github.com/GPUOpen-LibrariesAndSDKs/AMF/releases/download/v1.5.2/AMF-headers-v1.5.2.tar.gz" \
  "$amf_sha256"
fetch_verified "$ffnvcodec_archive" \
  "https://codeload.github.com/FFmpeg/nv-codec-headers/tar.gz/$ffnvcodec_commit" \
  "$ffnvcodec_archive_sha256"

if [[ ! -f "$ffmpeg_source/configure" ]]; then
  if [[ -e "$ffmpeg_source" ]]; then
    fail "$ffmpeg_source exists but is not an FFmpeg source tree; remove it and retry"
  fi
  mkdir -p "$ffmpeg_source"
  tar -xJf "$ffmpeg_archive" --strip-components=1 -C "$ffmpeg_source"
fi
[[ -f "$ffmpeg_source/VERSION" ]] || fail "FFmpeg source tree has no VERSION file: $ffmpeg_source"
[[ "$(tr -d '\r\n' < "$ffmpeg_source/VERSION")" == "9.0.2" ]] || \
  fail "expected FFmpeg 9.0.2 source in $ffmpeg_source"

if [[ ! -f "$ffmpeg_filter_source/configure" ]]; then
  if [[ -e "$ffmpeg_filter_source" ]]; then
    fail "$ffmpeg_filter_source exists but is not an FFmpeg source tree; remove it and retry"
  fi
  mkdir -p "$ffmpeg_filter_source"
  tar -xJf "$ffmpeg_archive" --strip-components=1 -C "$ffmpeg_filter_source"
fi
[[ -f "$ffmpeg_filter_source/VERSION" ]] || fail "FFmpeg filter source tree has no VERSION file: $ffmpeg_filter_source"
[[ "$(tr -d '\r\n' < "$ffmpeg_filter_source/VERSION")" == "9.0.2" ]] || \
  fail "expected FFmpeg 9.0.2 filter source in $ffmpeg_filter_source"

if [[ ! -f "$ffnvcodec_source/Makefile" ]]; then
  if [[ -e "$ffnvcodec_source" ]]; then
    fail "$ffnvcodec_source exists but is not an nv-codec-headers source tree; remove it and retry"
  fi
  mkdir -p "$ffnvcodec_source"
  tar -xzf "$ffnvcodec_archive" --strip-components=1 -C "$ffnvcodec_source"
fi
[[ -f "$ffnvcodec_source/Makefile" && -f "$ffnvcodec_source/ffnvcodec.pc.in" ]] || \
  fail "nv-codec-headers source tree is incomplete: $ffnvcodec_source"
[[ -f "$ffnvcodec_source/README" ]] || \
  fail "nv-codec-headers source tree has no README: $ffnvcodec_source"

if [[ ! -f "$amf_include_root/AMF/core/Factory.h" ]]; then
  rm -rf -- "$amf_include_root"
  mkdir -p "$source_root"
  tar -xzf "$amf_archive" -C "$source_root"
fi
[[ -f "$amf_include_root/AMF/core/Factory.h" ]] || \
  fail "AMF header archive did not provide AMF/core/Factory.h"

printf 'Staging private ffnvcodec headers at %s\n' "$ffnvcodec_prefix"
make -C "$ffnvcodec_source" -j 1 PREFIX="$ffnvcodec_prefix" LIBDIR=lib install
ffnvcodec_pc="$ffnvcodec_prefix/lib/pkgconfig/ffnvcodec.pc"
[[ -f "$ffnvcodec_pc" ]] || fail "nv-codec-headers did not install $ffnvcodec_pc"
grep -Fqx "Version: $ffnvcodec_version" "$ffnvcodec_pc" || \
  fail "staged ffnvcodec.pc does not report version $ffnvcodec_version"
for header in nvEncodeAPI.h dynlink_cuda.h dynlink_cuviddec.h dynlink_nvcuvid.h; do
  [[ -f "$ffnvcodec_prefix/include/ffnvcodec/$header" ]] || \
    fail "nv-codec-headers did not stage required header $header"
done
ffnvcodec_pkgconfig_path="$ffnvcodec_prefix/lib/pkgconfig"
if [[ -n "${PKG_CONFIG_PATH:-}" ]]; then
  ffnvcodec_pkgconfig_path+=":$PKG_CONFIG_PATH"
fi
if ! PKG_CONFIG_PATH="$ffnvcodec_pkgconfig_path" \
  pkg-config --atleast-version="$ffnvcodec_version" ffnvcodec; then
  fail "pkg-config cannot resolve staged ffnvcodec >= $ffnvcodec_version from $ffnvcodec_prefix"
fi
staged_ffnvcodec_version="$(PKG_CONFIG_PATH="$ffnvcodec_pkgconfig_path" pkg-config --modversion ffnvcodec)"
[[ "$staged_ffnvcodec_version" == "$ffnvcodec_version" ]] || \
  fail "expected private ffnvcodec $ffnvcodec_version, found $staged_ffnvcodec_version"

filter_source="$ffmpeg_filter_source/libavfilter/vulkan_filter.c"
filter_wait_patch_applied() {
  grep -Fq '#include "libavutil/getenv_utf8.h"' "$filter_source" &&
    grep -Fq 'char *sync_wait;' "$filter_source" &&
    grep -Fq 'err = ff_vk_exec_submit(vkctx, exec);' "$filter_source" &&
    grep -Fq 'sync_wait = getenv_utf8("FLUXREC_FF_VK_WAIT_SIMPLE");' "$filter_source" &&
    grep -Fq 'ff_vk_exec_wait(vkctx, exec);' "$filter_source" &&
    grep -Fq 'freeenv_utf8(sync_wait);' "$filter_source"
}
if filter_wait_patch_applied; then
  printf 'FFmpeg Vulkan simple-wait source patch is already applied\n'
elif grep -Fq 'FLUXREC_FF_VK_WAIT_SIMPLE' "$filter_source" ||
     grep -Fq 'libavutil/getenv_utf8.h' "$filter_source" ||
     grep -Fq 'char *sync_wait;' "$filter_source" ||
     grep -Fq 'ff_vk_exec_wait(vkctx, exec);' "$filter_source"; then
  fail "the Vulkan simple-wait patch is only partially present in $filter_source"
elif grep -Fq 'return ff_vk_exec_submit(vkctx, exec);' "$filter_source" &&
     (patch --batch --dry-run --forward --directory="$ffmpeg_filter_source" \
       --strip=1 --input="$filter_wait_patch_file" >/dev/null 2>&1); then
  patch --batch --forward --directory="$ffmpeg_filter_source" \
    --strip=1 --input="$filter_wait_patch_file"
  filter_wait_patch_applied || fail "the Vulkan simple-wait patch did not fully apply to $filter_source"
  printf 'Applied FFmpeg Vulkan simple-wait source patch\n'
else
  fail "the Vulkan simple-wait patch does not apply cleanly to FFmpeg 9.0.2"
fi
if ! grep -Fq 's->qf->num*4' "$ffmpeg_filter_source/libavfilter/vf_scale_vulkan.c"; then
  fail "the private filter source does not have FFmpeg 9.0.2's original scale_vulkan pool size (num*4)"
fi

timestamp_patch_applied() {
  local source_tree="$1"
  grep -Fq 'pool->query_type = query_type;' "$source_tree/libavutil/vulkan.c" &&
    grep -Fq 'query_data_valid' "$source_tree/libavutil/vulkan.h" &&
    grep -Fq 'CmdWriteTimestamp' "$source_tree/libavutil/vulkan_functions.h" &&
    grep -Fq 'pool->query_statuses = 0;' "$source_tree/libavutil/vulkan.c"
}
apply_timestamp_query_patch() {
  local source_tree="$1"
  local timestamp_source="$source_tree/libavutil/vulkan.c"
  if timestamp_patch_applied "$source_tree"; then
    printf 'FFmpeg Vulkan timestamp-query support patch is already applied in %s\n' "$source_tree"
  elif grep -Fq 'query_data_valid' "$source_tree/libavutil/vulkan.h" ||
       grep -Fq 'pool->query_type = query_type;' "$timestamp_source" ||
       grep -Fq 'CmdWriteTimestamp' "$source_tree/libavutil/vulkan_functions.h"; then
    fail "the Vulkan timestamp-query support patch is only partially present in $source_tree"
  elif patch --batch --dry-run --forward --directory="$source_tree" \
       --strip=1 --input="$timestamp_query_patch_file" >/dev/null 2>&1; then
    patch --batch --forward --directory="$source_tree" \
      --strip=1 --input="$timestamp_query_patch_file"
    timestamp_patch_applied "$source_tree" || fail "the Vulkan timestamp-query patch did not fully apply to $source_tree"
    printf 'Applied FFmpeg Vulkan timestamp-query support patch in %s\n' "$source_tree"
  else
    fail "the Vulkan timestamp-query support patch does not apply cleanly to $source_tree"
  fi
}
apply_timestamp_query_patch "$ffmpeg_source"
apply_timestamp_query_patch "$ffmpeg_filter_source"

encode_timing_patch_applied() {
  grep -Fq 'FLUXREC_FF_VK_DEVICE_TIMING' "$ffmpeg_source/libavcodec/vulkan_encode.c" &&
    grep -Fq 'device_timing;' "$ffmpeg_source/libavcodec/vulkan_encode.h" &&
    grep -Fq 'VK_QUERY_TYPE_TIMESTAMP' "$ffmpeg_source/libavcodec/vulkan_encode.c" &&
    grep -Fq 'CmdWriteTimestamp' "$ffmpeg_source/libavcodec/vulkan_encode.c"
}
if encode_timing_patch_applied; then
  printf 'FFmpeg Vulkan Video encode-timing patch is already applied\n'
elif grep -Fq 'FLUXREC_FF_VK_DEVICE_TIMING' "$ffmpeg_source/libavcodec/vulkan_encode.c" ||
     grep -Fq 'device_timing;' "$ffmpeg_source/libavcodec/vulkan_encode.h" ||
     grep -Fq 'VK_QUERY_TYPE_TIMESTAMP' "$ffmpeg_source/libavcodec/vulkan_encode.c"; then
  fail "the Vulkan Video encode-timing patch is only partially present in $ffmpeg_source"
elif patch --batch --dry-run --forward --directory="$ffmpeg_source" \
     --strip=1 --input="$encode_timing_patch_file" >/dev/null 2>&1; then
  patch --batch --forward --directory="$ffmpeg_source" \
    --strip=1 --input="$encode_timing_patch_file"
  encode_timing_patch_applied || fail "the Vulkan Video encode-timing patch did not fully apply to $ffmpeg_source"
  printf 'Applied FFmpeg Vulkan Video encode-timing patch\n'
else
  fail "the Vulkan Video encode-timing patch does not apply cleanly to FFmpeg 9.0.2"
fi

filter_timing_patch_applied() {
  grep -Fq 'FLUXREC_FF_VK_DEVICE_TIMING' "$ffmpeg_filter_source/libavfilter/vf_scale_vulkan.c" &&
    grep -Fq 'FFVkFilterTiming' "$ffmpeg_filter_source/libavfilter/vulkan_filter.h" &&
    grep -Fq 'CmdWriteTimestamp' "$ffmpeg_filter_source/libavfilter/vulkan_filter.c"
}
if filter_timing_patch_applied; then
  printf 'FFmpeg scale_vulkan device-timing patch is already applied\n'
elif grep -Fq 'FLUXREC_FF_VK_DEVICE_TIMING' "$ffmpeg_filter_source/libavfilter/vf_scale_vulkan.c" ||
     grep -Fq 'FFVkFilterTiming' "$ffmpeg_filter_source/libavfilter/vulkan_filter.h" ||
     grep -Fq 'CmdWriteTimestamp' "$ffmpeg_filter_source/libavfilter/vulkan_filter.c"; then
  fail "the scale_vulkan device-timing patch is only partially present in $ffmpeg_filter_source"
elif patch --batch --dry-run --forward --directory="$ffmpeg_filter_source" \
     --strip=1 --input="$filter_timing_patch_file" >/dev/null 2>&1; then
  patch --batch --forward --directory="$ffmpeg_filter_source" \
    --strip=1 --input="$filter_timing_patch_file"
  filter_timing_patch_applied || fail "the scale_vulkan device-timing patch did not fully apply to $ffmpeg_filter_source"
  printf 'Applied FFmpeg scale_vulkan device-timing patch\n'
else
  fail "the scale_vulkan device-timing patch does not apply cleanly to FFmpeg 9.0.2"
fi

filter_timing_callback_patch_applied() {
  grep -Fq 'avfilter_graph_set_fluxrec_device_timing_callback' \
      "$ffmpeg_filter_source/libavfilter/avfiltergraph.c" &&
    grep -Fq 'fluxrec_device_timing_callback' \
      "$ffmpeg_filter_source/libavfilter/avfilter.h" &&
    grep -Fq 'exec->query_pts' \
      "$ffmpeg_filter_source/libavfilter/vulkan_filter.c" &&
    grep -Fq 'int64_t query_pts;' "$ffmpeg_filter_source/libavutil/vulkan.h"
}
if filter_timing_callback_patch_applied; then
  printf 'FFmpeg scale_vulkan telemetry-callback patch is already applied\n'
elif grep -Fq 'avfilter_graph_set_fluxrec_device_timing_callback' \
         "$ffmpeg_filter_source/libavfilter/avfiltergraph.c" ||
     grep -Fq 'fluxrec_device_timing_callback' \
         "$ffmpeg_filter_source/libavfilter/avfilter.h" ||
     grep -Fq 'exec->query_pts' "$ffmpeg_filter_source/libavfilter/vulkan_filter.c" ||
     grep -Fq 'int64_t query_pts;' "$ffmpeg_filter_source/libavutil/vulkan.h"; then
  fail "the scale_vulkan telemetry-callback patch is only partially present in $ffmpeg_filter_source"
elif patch --batch --dry-run --forward --directory="$ffmpeg_filter_source" \
     --strip=1 --input="$filter_timing_callback_patch_file" >/dev/null 2>&1; then
  patch --batch --forward --directory="$ffmpeg_filter_source" \
    --strip=1 --input="$filter_timing_callback_patch_file"
  filter_timing_callback_patch_applied || \
    fail "the scale_vulkan telemetry-callback patch did not fully apply to $ffmpeg_filter_source"
  printf 'Applied FFmpeg scale_vulkan telemetry-callback patch\n'
else
  fail "the scale_vulkan telemetry-callback patch does not apply cleanly to FFmpeg 9.0.2"
fi

bgra_source="$ffmpeg_source/libavutil/hwcontext_vulkan.c"
bgra_row_count="$(grep -cF 'AV_PIX_FMT_BGRA' "$bgra_source" || true)"
[[ "$bgra_row_count" == 1 ]] || fail "expected exactly one AV_PIX_FMT_BGRA row in $bgra_source, found $bgra_row_count"
bgra_row="$(grep -F 'AV_PIX_FMT_BGRA' "$bgra_source")"
bgra_row_has_formats() {
  local row="$1"
  local expected_format="$2"
  [[ "$row" =~ ^[[:space:]]*\{[[:space:]]${expected_format},[[:space:]]+AV_PIX_FMT_BGRA,.*\{[[:space:]]${expected_format}[[:space:]]+\} ]]
}

if bgra_row_has_formats "$bgra_row" VK_FORMAT_B8G8R8A8_UNORM; then
  printf 'FFmpeg BGRA-to-B8 source patch is already applied\n'
elif bgra_row_has_formats "$bgra_row" VK_FORMAT_R8G8B8A8_UNORM; then
  patch --batch --forward --directory="$ffmpeg_source" --strip=1 --input="$patch_file"
  bgra_row="$(grep -F 'AV_PIX_FMT_BGRA' "$ffmpeg_source/libavutil/hwcontext_vulkan.c" || true)"
  bgra_row_has_formats "$bgra_row" VK_FORMAT_B8G8R8A8_UNORM || \
    fail "BGRA-to-B8 patch command completed but the source row is still incorrect"
  printf 'Applied FFmpeg BGRA-to-B8 source patch\n'
else
  fail "expected matching primary and fallback BGRA formats in FFmpeg Vulkan source: ${bgra_row:-missing}"
fi

jobs="${FLUXREC_BUILD_JOBS:-4}"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail "FLUXREC_BUILD_JOBS must be a positive integer"
configure_log="$logs_root/ffmpeg-9.0.2-configure.log"
printf 'Configuring private FFmpeg libavutil (Vulkan, CUDA, DRM PRIME, and AMF symbols)\n'
if ! (
  cd "$ffmpeg_source"
  PKG_CONFIG_PATH="$ffnvcodec_pkgconfig_path" ./configure \
    --prefix="$build_root/install" \
    --enable-shared --disable-static \
    --disable-programs --disable-doc \
    --enable-vulkan --enable-libdrm --enable-amf --enable-ffnvcodec \
    "--extra-cflags=-I$amf_include_root" >"$configure_log" 2>&1
); then
  cat "$configure_log" >&2
  fail "FFmpeg configure failed; see $configure_log"
fi
if ! grep -qx 'CONFIG_VULKAN=yes' "$ffmpeg_source/ffbuild/config.mak"; then
  cat "$configure_log" >&2
  fail "FFmpeg did not enable CONFIG_VULKAN; see $configure_log"
fi
if ! grep -qx 'CONFIG_AMF=yes' "$ffmpeg_source/ffbuild/config.mak"; then
  cat "$configure_log" >&2
  fail "FFmpeg did not enable CONFIG_AMF, which is required for compatibility with the host FFmpeg libraries"
fi
for config_flag in CONFIG_FFNVCODEC CONFIG_CUDA; do
  if ! grep -qx "${config_flag}=yes" "$ffmpeg_source/ffbuild/config.mak"; then
    cat "$configure_log" >&2
    fail "FFmpeg did not enable ${config_flag}; private CUDA hwcontext support is required"
  fi
done

printf 'Building libavutil with %s jobs\n' "$jobs"
make -C "$ffmpeg_source" -j "$jobs" libavutil/libavutil.so libavcodec/vulkan_encode.o

built_lib="$ffmpeg_source/libavutil/libavutil.so.61"
[[ -f "$built_lib" ]] || fail "FFmpeg did not produce $built_lib"
mkdir -p "$private_lib"
install -m 755 "$built_lib" "$private_lib/libavutil.so.61.1.102.new.$$"
mv -f -- "$private_lib/libavutil.so.61.1.102.new.$$" \
  "$private_lib/libavutil.so.61.1.102"
ln -sfn libavutil.so.61.1.102 "$private_lib/libavutil.so.61"
ln -sfn libavutil.so.61.1.102 "$private_lib/libavutil.so"

filter_configure_log="$logs_root/ffmpeg-9.0.2-filterwait-configure.log"
printf 'Configuring private FFmpeg libavfilter/libswscale (minimal Vulkan filter set)\n'
if ! (
  cd "$ffmpeg_filter_source"
  ./configure \
    --prefix="$build_root/filter-install" \
    --enable-shared --disable-static \
    --disable-programs --disable-doc \
    --disable-avdevice --disable-avcodec --disable-avformat --disable-swresample \
    --disable-everything \
    --enable-vulkan --enable-libdrm --enable-swscale \
    --enable-filter=buffer \
    --enable-filter=buffersink \
    --enable-filter=scale \
    --enable-filter=scale_vulkan \
    --enable-filter=blackdetect_vulkan >"$filter_configure_log" 2>&1
); then
  cat "$filter_configure_log" >&2
  fail "FFmpeg minimal filter configure failed; see $filter_configure_log"
fi
for config_flag in CONFIG_VULKAN CONFIG_SWSCALE CONFIG_AVFILTER CONFIG_SCALE_FILTER \
                   CONFIG_SCALE_VULKAN_FILTER CONFIG_BLACKDETECT_VULKAN_FILTER; do
  if ! grep -qx "${config_flag}=yes" "$ffmpeg_filter_source/ffbuild/config.mak"; then
    cat "$filter_configure_log" >&2
    fail "FFmpeg minimal filter configure did not enable ${config_flag}; see $filter_configure_log"
  fi
done

printf 'Building private libavfilter and libswscale with %s jobs\n' "$jobs"
make -C "$ffmpeg_filter_source" -j "$jobs" \
  libavfilter/libavfilter.so libswscale/libswscale.so

built_filter="$ffmpeg_filter_source/libavfilter/libavfilter.so.12"
built_swscale="$ffmpeg_filter_source/libswscale/libswscale.so.10"
[[ -f "$built_filter" ]] || fail "FFmpeg did not produce $built_filter"
[[ -f "$built_swscale" ]] || fail "FFmpeg did not produce $built_swscale"
install -m 755 "$built_filter" "$private_lib/libavfilter.so.12.1.102.new.$$"
mv -f -- "$private_lib/libavfilter.so.12.1.102.new.$$" \
  "$private_lib/libavfilter.so.12.1.102"
ln -sfn libavfilter.so.12.1.102 "$private_lib/libavfilter.so.12"
ln -sfn libavfilter.so.12.1.102 "$private_lib/libavfilter.so"
install -m 755 "$built_swscale" "$private_lib/libswscale.so.10.1.102.new.$$"
mv -f -- "$private_lib/libswscale.so.10.1.102.new.$$" \
  "$private_lib/libswscale.so.10.1.102"
ln -sfn libswscale.so.10.1.102 "$private_lib/libswscale.so.10"
ln -sfn libswscale.so.10.1.102 "$private_lib/libswscale.so"

# The libraries above now contain the new capability, while any previous
# manifest still describes the old build. Validate the binaries first, then
# write and validate their matching manifest below.
rm -f -- "$private_lib/BUILD-MANIFEST"
"$repo_root/scripts/check-ffmpeg-runtime.sh" --library-only "$private_lib"

cat >"$private_lib/BUILD-MANIFEST" <<EOF
FFmpeg release: 9.0.2
FFmpeg source SHA-256: $ffmpeg_sha256
BGRA-to-B8 patch SHA-256: $patch_sha256
Vulkan filter simple-wait patch SHA-256: $filter_wait_patch_sha256
Vulkan timestamp-query patch SHA-256: $timestamp_query_patch_sha256
scale_vulkan device-timing patch SHA-256: $filter_timing_patch_sha256
scale_vulkan telemetry-callback patch SHA-256: $filter_timing_callback_patch_sha256
Vulkan Video encode-timing patch SHA-256: $encode_timing_patch_sha256
AMF headers: v1.5.2
AMF header archive SHA-256: $amf_sha256
nv-codec-headers release: n$ffnvcodec_version
nv-codec-headers commit: $ffnvcodec_commit
nv-codec-headers archive SHA-256: $ffnvcodec_archive_sha256
CUDA hwcontext: enabled (CONFIG_CUDA)
Vulkan-to-CUDA frame transfer: compiled (requires runtime external memory and semaphore support)
GPU conversion timing: callback API emits completed per-pass timestamp samples; recorder joins passes by frame PTS into one per-frame telemetry sample
Configure: --enable-shared --disable-static --disable-programs --disable-doc --enable-vulkan --enable-libdrm --enable-amf --enable-ffnvcodec
Filter configure: --enable-shared --disable-static --disable-programs --disable-doc --disable-avdevice --disable-avcodec --disable-avformat --disable-swresample --disable-everything --enable-vulkan --enable-libdrm --enable-swscale --enable-filter=buffer --enable-filter=buffersink --enable-filter=scale --enable-filter=scale_vulkan --enable-filter=blackdetect_vulkan
Staged runtime: libavutil.so.61.1.102 libavfilter.so.12.1.102 libswscale.so.10.1.102
EOF

if ! "$repo_root/scripts/check-ffmpeg-runtime.sh" --library-only "$private_lib"; then
  rm -f -- "$private_lib/BUILD-MANIFEST"
  fail "the staged FFmpeg runtime failed its post-manifest validation"
fi

printf 'Installed private FFmpeg libraries at %s\n' "$private_lib"
printf 'Next: cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo\n'
