cmake_minimum_required(VERSION 3.20)

# This directory is self-configurable for local development and can also be
# added from the repository root with add_subdirectory(capture).
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
project(fluxrec_capture LANGUAGES C CXX)
endif()

include(CTest)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

execute_process(
    COMMAND "${PKG_CONFIG_EXECUTABLE}" --variable=pkgdatadir wayland-protocols
    OUTPUT_VARIABLE WAYLAND_PROTOCOLS_PKG_DATA_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)

set(CAPTURE_PROTOCOL_XML_DIR "${WAYLAND_PROTOCOLS_PKG_DATA_DIR}/staging")
set(CAPTURE_PROTOCOL_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${CAPTURE_PROTOCOL_GENERATED_DIR}")

set(CAPTURE_PROTOCOLS
    ext-image-capture-source-v1
    ext-image-copy-capture-v1
    ext-foreign-toplevel-list-v1
    linux-dmabuf-v1
)

set(CAPTURE_PROTOCOL_SOURCES)
foreach(protocol IN LISTS CAPTURE_PROTOCOLS)
    if(protocol STREQUAL "linux-dmabuf-v1")
        set(protocol_xml "${WAYLAND_PROTOCOLS_PKG_DATA_DIR}/stable/linux-dmabuf/${protocol}.xml")
    elseif(protocol STREQUAL "ext-image-capture-source-v1")
        set(protocol_xml "${CAPTURE_PROTOCOL_XML_DIR}/ext-image-capture-source/${protocol}.xml")
    elseif(protocol STREQUAL "ext-foreign-toplevel-list-v1")
        set(protocol_xml "${CAPTURE_PROTOCOL_XML_DIR}/ext-foreign-toplevel-list/${protocol}.xml")
    else()
        set(protocol_xml "${CAPTURE_PROTOCOL_XML_DIR}/ext-image-copy-capture/${protocol}.xml")
    endif()

    if(NOT EXISTS "${protocol_xml}")
        message(FATAL_ERROR "Required Wayland protocol XML was not found: ${protocol_xml}")
    endif()

    set(protocol_header "${CAPTURE_PROTOCOL_GENERATED_DIR}/${protocol}-client-protocol.h")
    set(protocol_code "${CAPTURE_PROTOCOL_GENERATED_DIR}/${protocol}-protocol.c")

    add_custom_command(
        OUTPUT "${protocol_header}"
        COMMAND "${WAYLAND_SCANNER}" client-header "${protocol_xml}" "${protocol_header}"
        DEPENDS "${protocol_xml}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${protocol_code}"
        COMMAND "${WAYLAND_SCANNER}" private-code "${protocol_xml}" "${protocol_code}"
        DEPENDS "${protocol_xml}" "${protocol_header}"
        VERBATIM
    )
    list(APPEND CAPTURE_PROTOCOL_SOURCES "${protocol_code}" "${protocol_header}")
endforeach()

add_library(fluxrec_capture STATIC
    capture.cpp
    ${CAPTURE_PROTOCOL_SOURCES}
)
add_library(Fluxrec::Capture ALIAS fluxrec_capture)

target_compile_features(fluxrec_capture PUBLIC cxx_std_23)
target_include_directories(fluxrec_capture
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CAPTURE_PROTOCOL_GENERATED_DIR}"
)
target_link_libraries(fluxrec_capture
    PUBLIC PkgConfig::WAYLAND_CLIENT
    PRIVATE Threads::Threads
)
target_compile_options(fluxrec_capture PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

install(TARGETS fluxrec_capture ARCHIVE DESTINATION lib)
install(FILES capture.hpp DESTINATION include/capture)

if(BUILD_TESTING)
    add_executable(fluxrec_capture_rate_test tests/capture_rate_limit_test.cpp)
    target_compile_features(fluxrec_capture_rate_test PRIVATE cxx_std_23)
    target_include_directories(fluxrec_capture_rate_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME fluxrec_capture_rate_test COMMAND fluxrec_capture_rate_test)

    add_executable(fluxrec_capture_pts_rate_test tests/capture_pts_rate_limit_test.cpp)
    target_compile_features(fluxrec_capture_pts_rate_test PRIVATE cxx_std_23)
    target_include_directories(fluxrec_capture_pts_rate_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME fluxrec_capture_pts_rate_test COMMAND fluxrec_capture_pts_rate_test)

    add_executable(fluxrec_capture_lifecycle_test tests/capture_lifecycle_test.cpp)
    target_compile_features(fluxrec_capture_lifecycle_test PRIVATE cxx_std_23)
    target_include_directories(fluxrec_capture_lifecycle_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME fluxrec_capture_lifecycle_test COMMAND fluxrec_capture_lifecycle_test)
endif()
