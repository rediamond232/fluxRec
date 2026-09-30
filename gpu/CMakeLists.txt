cmake_minimum_required(VERSION 3.20)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(fluxrec_gpu LANGUAGES C CXX)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavfilter libavutil)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm libdrm)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
set(FLUXREC_NVENC_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/nvenc_input_audit/deps/nv-codec-headers-13.1.15.0/include")
if(NOT EXISTS "${FLUXREC_NVENC_INCLUDE_DIR}/ffnvcodec/nvEncodeAPI.h")
    message(FATAL_ERROR "Fluxrec's pinned NVENC 13.1 headers are missing")
endif()
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
find_program(SPIRV_VAL spirv-val REQUIRED)

set(NV12_CONVERT_SPIRV "${CMAKE_CURRENT_BINARY_DIR}/shaders/convert_xrgb_to_nv12.comp.spv")
add_custom_command(
    OUTPUT "${NV12_CONVERT_SPIRV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 -S comp
            -o "${NV12_CONVERT_SPIRV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/convert_xrgb_to_nv12.comp"
    COMMAND "${SPIRV_VAL}" --target-env vulkan1.2 "${NV12_CONVERT_SPIRV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/convert_xrgb_to_nv12.comp"
    VERBATIM
)
add_custom_target(fluxrec_nv12_shader DEPENDS "${NV12_CONVERT_SPIRV}")

set(NV12_RB_SWAP_SPIRV "${CMAKE_CURRENT_BINARY_DIR}/shaders/swap_nv12_rb.comp.spv")
add_custom_command(
    OUTPUT "${NV12_RB_SWAP_SPIRV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 -S comp
            -o "${NV12_RB_SWAP_SPIRV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/swap_nv12_rb.comp"
    COMMAND "${SPIRV_VAL}" --target-env vulkan1.2 "${NV12_RB_SWAP_SPIRV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/swap_nv12_rb.comp"
    VERBATIM
)
add_custom_target(fluxrec_nv12_rb_swap_shader DEPENDS "${NV12_RB_SWAP_SPIRV}")

set(DMABUF_BLACK_PROBE_SPIRV "${CMAKE_CURRENT_BINARY_DIR}/shaders/dmabuf_black_count.comp.spv")
add_custom_command(
    OUTPUT "${DMABUF_BLACK_PROBE_SPIRV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 -S comp
            -o "${DMABUF_BLACK_PROBE_SPIRV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dmabuf_black_count.comp"
    COMMAND "${SPIRV_VAL}" --target-env vulkan1.2 "${DMABUF_BLACK_PROBE_SPIRV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/dmabuf_black_count.comp"
    VERBATIM
)
add_custom_target(fluxrec_dmabuf_black_probe_shader DEPENDS "${DMABUF_BLACK_PROBE_SPIRV}")

set(PREVIEW_DOWNSCALE_SPIRV "${CMAKE_CURRENT_BINARY_DIR}/shaders/preview_downscale.comp.spv")
add_custom_command(
    OUTPUT "${PREVIEW_DOWNSCALE_SPIRV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 -S comp
            -o "${PREVIEW_DOWNSCALE_SPIRV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/preview_downscale.comp"
    COMMAND "${SPIRV_VAL}" --target-env vulkan1.2 "${PREVIEW_DOWNSCALE_SPIRV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/preview_downscale.comp"
    VERBATIM
)
add_custom_target(fluxrec_preview_downscale_shader DEPENDS "${PREVIEW_DOWNSCALE_SPIRV}")

set(FRAME_DIGEST_SPIRV "${CMAKE_CURRENT_BINARY_DIR}/shaders/frame_digest.comp.spv")
set(FRAME_DIGEST_REDUCE_SPIRV "${CMAKE_CURRENT_BINARY_DIR}/shaders/frame_digest_reduce.comp.spv")
set(FRAME_UNRELATED_SPIRV "${CMAKE_CURRENT_BINARY_DIR}/shaders/frame_identity_unrelated.comp.spv")
add_custom_command(
    OUTPUT "${FRAME_DIGEST_SPIRV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 -S comp
            -o "${FRAME_DIGEST_SPIRV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/frame_digest.comp"
    COMMAND "${SPIRV_VAL}" --target-env vulkan1.2 "${FRAME_DIGEST_SPIRV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/frame_digest.comp"
    VERBATIM
)
add_custom_command(
    OUTPUT "${FRAME_DIGEST_REDUCE_SPIRV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 -S comp
            -o "${FRAME_DIGEST_REDUCE_SPIRV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/frame_digest_reduce.comp"
    COMMAND "${SPIRV_VAL}" --target-env vulkan1.2 "${FRAME_DIGEST_REDUCE_SPIRV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/frame_digest_reduce.comp"
    VERBATIM
)
add_custom_command(
    OUTPUT "${FRAME_UNRELATED_SPIRV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 -S comp
            -o "${FRAME_UNRELATED_SPIRV}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/frame_identity_unrelated.comp"
    COMMAND "${SPIRV_VAL}" --target-env vulkan1.2 "${FRAME_UNRELATED_SPIRV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/frame_identity_unrelated.comp"
    VERBATIM
)
add_custom_target(fluxrec_frame_identity_shaders
    DEPENDS "${FRAME_DIGEST_SPIRV}" "${FRAME_DIGEST_REDUCE_SPIRV}"
            "${FRAME_UNRELATED_SPIRV}")

add_library(fluxrec_gpu STATIC gpu.cpp direct_cuda_egl_encoder.cpp direct_nv12.cpp nv12_color_corrector.cpp dmabuf_black_probe.cpp)
add_library(Fluxrec::Gpu ALIAS fluxrec_gpu)
add_dependencies(fluxrec_gpu fluxrec_nv12_shader)
add_dependencies(fluxrec_gpu fluxrec_nv12_rb_swap_shader)
add_dependencies(fluxrec_gpu fluxrec_dmabuf_black_probe_shader)

add_library(fluxrec_frame_identity STATIC frame_identity.cpp)
add_library(Fluxrec::FrameIdentity ALIAS fluxrec_frame_identity)
add_dependencies(fluxrec_frame_identity fluxrec_frame_identity_shaders)

target_compile_features(fluxrec_gpu PUBLIC cxx_std_23)
target_include_directories(fluxrec_gpu
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE "${FLUXREC_NVENC_INCLUDE_DIR}"
)
target_link_libraries(fluxrec_gpu
    PUBLIC
        Fluxrec::Capture
        PkgConfig::FFMPEG
    PRIVATE
        PkgConfig::GBM
        PkgConfig::EGL
        Threads::Threads
        Vulkan::Vulkan
        Fluxrec::FrameIdentity
        ${CMAKE_DL_LIBS}
)
target_compile_options(fluxrec_gpu PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)
target_compile_features(fluxrec_frame_identity PUBLIC cxx_std_23)
target_include_directories(fluxrec_frame_identity
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
        "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(fluxrec_frame_identity
    PUBLIC PkgConfig::FFMPEG
    PRIVATE Vulkan::Vulkan
)
target_compile_options(fluxrec_frame_identity PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

add_library(fluxrec_gpu_preview STATIC preview_producer.cpp)
add_library(Fluxrec::GpuPreview ALIAS fluxrec_gpu_preview)
add_dependencies(fluxrec_gpu_preview fluxrec_preview_downscale_shader)
target_compile_features(fluxrec_gpu_preview PUBLIC cxx_std_23)
target_include_directories(fluxrec_gpu_preview
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
        "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(fluxrec_gpu_preview
    PUBLIC
        Fluxrec::Preview
    PRIVATE
        PkgConfig::FFMPEG
        Threads::Threads
        Vulkan::Vulkan
)
target_compile_options(fluxrec_gpu_preview PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion;-Wno-deprecated-declarations>
)
target_compile_definitions(fluxrec_gpu PRIVATE
    FLUXREC_GPU_NV12_SHADER_PATH="${NV12_CONVERT_SPIRV}"
    FLUXREC_GPU_NV12_RB_SWAP_SHADER_PATH="${NV12_RB_SWAP_SPIRV}"
    FLUXREC_GPU_DMABUF_BLACK_SHADER_PATH="${DMABUF_BLACK_PROBE_SPIRV}"
    FLUXREC_FRAME_IDENTITY_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders"
)
target_compile_definitions(fluxrec_gpu_preview PRIVATE
    FLUXREC_PREVIEW_SHADER_PATH="${PREVIEW_DOWNSCALE_SPIRV}"
)

# Keep the producer in its own library, but link it into the encoder. The
# producer needs only FFmpeg/Vulkan/Preview APIs, so this avoids a static
# library cycle through Fluxrec::Gpu.
target_link_libraries(fluxrec_gpu PRIVATE Fluxrec::GpuPreview)

install(TARGETS fluxrec_gpu fluxrec_frame_identity fluxrec_gpu_preview ARCHIVE DESTINATION lib)
install(FILES gpu.hpp direct_cuda_egl_encoder.hpp frame_identity.hpp preview_producer.hpp DESTINATION include/gpu)

if(BUILD_TESTING)
    add_executable(fluxrec_dmabuf_black_probe_metrics_test
        tests/dmabuf_black_probe_metrics_test.cpp
    )
    target_compile_features(fluxrec_dmabuf_black_probe_metrics_test PRIVATE cxx_std_23)
    target_include_directories(fluxrec_dmabuf_black_probe_metrics_test PRIVATE
        "${PROJECT_SOURCE_DIR}"
    )
    target_compile_options(fluxrec_dmabuf_black_probe_metrics_test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
    add_test(NAME fluxrec_dmabuf_black_probe_metrics
        COMMAND fluxrec_dmabuf_black_probe_metrics_test
    )

    add_executable(fluxrec_gpu_rate_control_mode_test
        tests/rate_control_mode_test.cpp
    )
    target_compile_features(fluxrec_gpu_rate_control_mode_test PRIVATE cxx_std_23)
    target_include_directories(fluxrec_gpu_rate_control_mode_test PRIVATE
        "${PROJECT_SOURCE_DIR}"
    )
    target_compile_options(fluxrec_gpu_rate_control_mode_test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
    add_test(NAME fluxrec_gpu_rate_control_mode
        COMMAND fluxrec_gpu_rate_control_mode_test
    )
endif()
