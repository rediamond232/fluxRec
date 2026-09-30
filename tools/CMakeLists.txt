cmake_minimum_required(VERSION 3.25)
project(fluxrec_drm_prime_vulkan_probe LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG_UTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm libdrm)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)
pkg_check_modules(FFMPEG_DECODE REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil)
pkg_check_modules(FFMPEG_COLOR REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale)
pkg_check_modules(GBM_PROBE REQUIRED IMPORTED_TARGET gbm libdrm)
pkg_check_modules(EGL_PROBE REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GL_PROBE REQUIRED IMPORTED_TARGET gl)

# Keep this probe independently buildable while reusing the project's live
# Wayland format negotiation code. When added from the repository root,
# capture is already present and must not be declared a second time.
if(NOT TARGET fluxrec_capture)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../capture"
                     "${CMAKE_CURRENT_BINARY_DIR}/capture")
endif()
if(NOT TARGET Fluxrec::Preview)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../preview"
                     "${CMAKE_CURRENT_BINARY_DIR}/preview")
endif()
if(NOT TARGET fluxrec_frame_identity)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../gpu"
                     "${CMAKE_CURRENT_BINARY_DIR}/gpu")
endif()
if(NOT TARGET fluxrec_telemetry)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../telemetry"
                     "${CMAKE_CURRENT_BINARY_DIR}/telemetry")
endif()
if(NOT TARGET fluxrec_mux)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../mux"
                     "${CMAKE_CURRENT_BINARY_DIR}/mux")
endif()

add_executable(drm_prime_vulkan_probe drm_prime_vulkan_probe.cpp)
target_compile_features(drm_prime_vulkan_probe PRIVATE cxx_std_23)
target_link_libraries(drm_prime_vulkan_probe PRIVATE
    Fluxrec::Capture
    PkgConfig::FFMPEG_UTIL
    PkgConfig::GBM
    PkgConfig::VULKAN
)

add_executable(fluxrec-frame-identity-smoke frame_identity_smoke.cpp)
target_compile_features(fluxrec-frame-identity-smoke PRIVATE cxx_std_23)
target_compile_definitions(fluxrec-frame-identity-smoke PRIVATE
    FLUXREC_FRAME_IDENTITY_SHADER_DIR="${CMAKE_BINARY_DIR}/gpu/shaders"
)
target_link_libraries(fluxrec-frame-identity-smoke PRIVATE
    Fluxrec::Capture
    Fluxrec::FrameIdentity
    PkgConfig::GBM
    PkgConfig::VULKAN
)
target_compile_options(fluxrec-frame-identity-smoke PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)
target_compile_options(drm_prime_vulkan_probe PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

# A deliberately isolated, opt-in hardware probe for the candidate
# DMA-BUF -> EGLImage -> CUDA array -> NVENC CUDAARRAY route. It is built with
# the diagnostic-only NVENC 13.1 headers and does not link into fluxrec-record.
# The optional Matroska writer reuses Fluxrec::Mux and receives compressed
# packets from the probe's output worker.
add_executable(fluxrec-nvenc-egl-cudaarray-probe nvenc_egl_cudaarray_probe.cpp)
target_compile_features(fluxrec-nvenc-egl-cudaarray-probe PRIVATE cxx_std_23)
target_include_directories(fluxrec-nvenc-egl-cudaarray-probe PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/nvenc_input_audit/deps/nv-codec-headers-13.1.15.0/include"
)
target_link_libraries(fluxrec-nvenc-egl-cudaarray-probe PRIVATE
    Fluxrec::Capture
    Fluxrec::Mux
    PkgConfig::GBM_PROBE
    PkgConfig::EGL_PROBE
    ${CMAKE_DL_LIBS}
)
target_compile_options(fluxrec-nvenc-egl-cudaarray-probe PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

# Independent Linux NVENC/OpenGL texture input experiment. This exercises a
# DMA-BUF -> EGLImage -> GL_TEXTURE_2D -> NV_ENC_INPUT_RESOURCE_TYPE_OPENGL_TEX
# path and deliberately remains outside the production recorder target.
add_executable(fluxrec-nvenc-egl-gl-probe nvenc_egl_gl_probe.cpp)
target_compile_features(fluxrec-nvenc-egl-gl-probe PRIVATE cxx_std_23)
target_include_directories(fluxrec-nvenc-egl-gl-probe PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/nvenc_input_audit/deps/nv-codec-headers-13.1.15.0/include"
)
target_link_libraries(fluxrec-nvenc-egl-gl-probe PRIVATE
    Fluxrec::Capture
    PkgConfig::GBM_PROBE
    PkgConfig::EGL_PROBE
    PkgConfig::GL_PROBE
    ${CMAKE_DL_LIBS}
)
target_compile_options(fluxrec-nvenc-egl-gl-probe PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

# Read-only capability probe for exporting GL-owned RGBA8 storage through
# EGL_MESA_image_dma_buf_export. It negotiates capture constraints but never
# starts capture, registers with NVENC, or reads pixel data.
add_executable(fluxrec-gl-owned-dmabuf-export-probe gl_owned_dmabuf_export_probe.cpp)
target_compile_features(fluxrec-gl-owned-dmabuf-export-probe PRIVATE cxx_std_23)
target_link_libraries(fluxrec-gl-owned-dmabuf-export-probe PRIVATE
    Fluxrec::Capture
    PkgConfig::GBM_PROBE
    PkgConfig::EGL_PROBE
    PkgConfig::GL_PROBE
)
target_compile_options(fluxrec-gl-owned-dmabuf-export-probe PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

add_executable(fluxrec-decode-stimulus-ids decode_stimulus_ids.cpp)
target_compile_features(fluxrec-decode-stimulus-ids PRIVATE cxx_std_23)
target_link_libraries(fluxrec-decode-stimulus-ids PRIVATE PkgConfig::FFMPEG_DECODE)
target_compile_options(fluxrec-decode-stimulus-ids PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

add_executable(fluxrec-decode-colorchart decode_colorchart.cpp)
target_compile_features(fluxrec-decode-colorchart PRIVATE cxx_std_23)
target_link_libraries(fluxrec-decode-colorchart PRIVATE PkgConfig::FFMPEG_COLOR)
target_compile_options(fluxrec-decode-colorchart PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)
