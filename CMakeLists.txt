cmake_minimum_required(VERSION 3.25)
project(Fluxrec VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

# The capture path requires a source-patched libavutil so FFmpeg imports the
# compositor's little-endian AR24/BGRA buffers with a B8 Vulkan image view.
# The private runtime also enables CUDA hwframes and Vulkan-to-CUDA interop
# for the default NVENC encoder path. Its opt-in scale_vulkan timing probe is
# disabled unless FLUXREC_FF_VK_DEVICE_TIMING=1 is set for the recorder.
# Bootstrap the repo default as part of the build; validate an explicitly
# selected private library at configure time instead of using the stock one.
set(FLUXREC_DEFAULT_PRIVATE_FFMPEG_LIBDIR
    "${CMAKE_CURRENT_SOURCE_DIR}/.deps/ffmpeg-bgra-b8/lib")
set(FLUXREC_PRIVATE_FFMPEG_LIBDIR
    "${FLUXREC_DEFAULT_PRIVATE_FFMPEG_LIBDIR}"
    CACHE PATH "Directory containing Fluxrec's private FFmpeg avutil, avfilter, and swscale libraries")
if(IS_DIRECTORY "${FLUXREC_PRIVATE_FFMPEG_LIBDIR}")
  file(REAL_PATH "${FLUXREC_PRIVATE_FFMPEG_LIBDIR}" FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL)
else()
  get_filename_component(FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL
    "${FLUXREC_PRIVATE_FFMPEG_LIBDIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
set(FLUXREC_PRIVATE_FFMPEG_MISSING_LIBS)
foreach(soname IN ITEMS libavutil.so.61 libavfilter.so.12 libswscale.so.10)
  if(NOT EXISTS "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}/${soname}")
    list(APPEND FLUXREC_PRIVATE_FFMPEG_MISSING_LIBS "${soname}")
  endif()
endforeach()
set(FLUXREC_BOOTSTRAP_PRIVATE_FFMPEG OFF)
if(FLUXREC_PRIVATE_FFMPEG_MISSING_LIBS)
  if("${FLUXREC_PRIVATE_FFMPEG_LIBDIR}" STREQUAL "${FLUXREC_DEFAULT_PRIVATE_FFMPEG_LIBDIR}")
    set(FLUXREC_BOOTSTRAP_PRIVATE_FFMPEG ON)
    file(REMOVE "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}/BUILD-MANIFEST")
    message(STATUS "Private B8/CUDA FFmpeg runtime stack will be built by the default Fluxrec build")
  else()
    message(FATAL_ERROR
      "The selected private FFmpeg runtime is missing: ${FLUXREC_PRIVATE_FFMPEG_MISSING_LIBS}\n"
      "Build the repo default with ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-ffmpeg-bgra-b8.sh,\n"
      "or select a complete private library directory with -DFLUXREC_PRIVATE_FFMPEG_LIBDIR=....")
  endif()
else()
  execute_process(
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-ffmpeg-runtime.sh"
            --library-only "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}"
    RESULT_VARIABLE FLUXREC_FFMPEG_LIBRARY_CHECK_RESULT
    OUTPUT_VARIABLE FLUXREC_FFMPEG_LIBRARY_CHECK_OUTPUT
    ERROR_VARIABLE FLUXREC_FFMPEG_LIBRARY_CHECK_ERROR
  )
  if(NOT FLUXREC_FFMPEG_LIBRARY_CHECK_RESULT EQUAL 0)
    if("${FLUXREC_PRIVATE_FFMPEG_LIBDIR}" STREQUAL "${FLUXREC_DEFAULT_PRIVATE_FFMPEG_LIBDIR}")
      set(FLUXREC_BOOTSTRAP_PRIVATE_FFMPEG ON)
      file(REMOVE "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}/BUILD-MANIFEST")
      message(WARNING
        "The repo-default private FFmpeg runtime failed validation and will be rebuilt by the default build.\n"
        "${FLUXREC_FFMPEG_LIBRARY_CHECK_OUTPUT}${FLUXREC_FFMPEG_LIBRARY_CHECK_ERROR}")
    else()
      message(FATAL_ERROR
        "The selected private FFmpeg runtime failed its version, dependency, filter, or BGRA/Vulkan checks.\n"
        "${FLUXREC_FFMPEG_LIBRARY_CHECK_OUTPUT}${FLUXREC_FFMPEG_LIBRARY_CHECK_ERROR}")
    endif()
  else()
    message(STATUS "${FLUXREC_FFMPEG_LIBRARY_CHECK_OUTPUT}")
  endif()
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)

# Components are added as their independent prototypes become available.
foreach(component IN ITEMS capture preview gpu telemetry replay mux audio config engine tools stimulus)
  if(component STREQUAL "gpu" AND
     NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gpu/gpu.cpp")
    continue()
  endif()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${component}/CMakeLists.txt")
    add_subdirectory("${component}")
  endif()
endforeach()

if(TARGET fluxrec-record AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ui/CMakeLists.txt")
  add_subdirectory(ui)
endif()

if(TARGET fluxrec-record)
  if(FLUXREC_BOOTSTRAP_PRIVATE_FFMPEG)
    add_custom_command(
      OUTPUT
        "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}/libavutil.so.61"
        "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}/libavfilter.so.12"
        "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}/libswscale.so.10"
        "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}/BUILD-MANIFEST"
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-ffmpeg-bgra-b8.sh"
      DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-ffmpeg-bgra-b8.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/ffmpeg-n9.0.2-vulkan-bgra-b8.patch"
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/ffmpeg-n9.0.2-vulkan-filter-wait-env.patch"
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/ffmpeg-n9.0.2-vulkan-timestamp-query.patch"
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/ffmpeg-n9.0.2-vulkan-filter-timing.patch"
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/ffmpeg-n9.0.2-vulkan-filter-timing-callback.patch"
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/ffmpeg-n9.0.2-vulkan-encode-timing.patch"
      COMMENT "Building private FFmpeg 9.0.2 runtime libraries"
      VERBATIM
    )
    add_custom_target(fluxrec-ffmpeg-bootstrap
      DEPENDS
        "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}/libavutil.so.61"
        "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}/libavfilter.so.12"
        "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}/libswscale.so.10"
        "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}/BUILD-MANIFEST")
    add_dependencies(fluxrec-record fluxrec-ffmpeg-bootstrap)
  endif()

  # The UI starts fluxrec-record as a child process. Embed the private library
  # directory in the recorder itself so this works when the UI is launched
  # directly from the build tree, without relying on an inherited shell path.
  set_property(TARGET fluxrec-record APPEND PROPERTY
    BUILD_RPATH "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}")
  set_property(TARGET fluxrec-record PROPERTY
    INSTALL_RPATH "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}")

  # Use DT_RPATH for this executable so an unrelated LD_LIBRARY_PATH cannot
  # silently replace the verified private FFmpeg runtime at launch.
  if(UNIX AND NOT APPLE)
    target_link_options(fluxrec-record PRIVATE "LINKER:--disable-new-dtags")
  endif()

  add_custom_target(fluxrec-ffmpeg-runtime-check ALL
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-ffmpeg-runtime.sh"
      "$<TARGET_FILE:fluxrec-record>" "${FLUXREC_PRIVATE_FFMPEG_LIBDIR_REAL}"
    COMMENT "Verifying fluxrec-record resolves the private FFmpeg runtime"
    DEPENDS fluxrec-record
    VERBATIM
  )
endif()
