cmake_minimum_required(VERSION 3.25)
project(fluxrec_stimulus VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_EGL REQUIRED IMPORTED_TARGET
  wayland-client
  wayland-egl
  egl
  glesv2
)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_PKGDATADIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(PROTOCOL_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PROTOCOL_DIR}")

set(XDG_SHELL_XML
  "${WAYLAND_PROTOCOLS_PKGDATADIR}/stable/xdg-shell/xdg-shell.xml")
set(PRESENTATION_TIME_XML
  "${WAYLAND_PROTOCOLS_PKGDATADIR}/stable/presentation-time/presentation-time.xml")

foreach(protocol IN ITEMS xdg-shell presentation-time)
  if(protocol STREQUAL "xdg-shell")
    set(protocol_xml "${XDG_SHELL_XML}")
  else()
    set(protocol_xml "${PRESENTATION_TIME_XML}")
  endif()

  add_custom_command(
    OUTPUT "${PROTOCOL_DIR}/${protocol}-client-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" client-header
            "${protocol_xml}"
            "${PROTOCOL_DIR}/${protocol}-client-protocol.h"
    DEPENDS "${protocol_xml}"
    VERBATIM
  )
  add_custom_command(
    OUTPUT "${PROTOCOL_DIR}/${protocol}-protocol.c"
    COMMAND "${WAYLAND_SCANNER}" private-code
            "${protocol_xml}"
            "${PROTOCOL_DIR}/${protocol}-protocol.c"
    DEPENDS "${protocol_xml}"
    VERBATIM
  )
  list(APPEND GENERATED_PROTOCOL_SOURCES
    "${PROTOCOL_DIR}/${protocol}-protocol.c")
  list(APPEND GENERATED_PROTOCOL_HEADERS
    "${PROTOCOL_DIR}/${protocol}-client-protocol.h")
endforeach()

add_executable(fluxrec-stimulus
  src/main.cpp
  ${GENERATED_PROTOCOL_SOURCES}
  ${GENERATED_PROTOCOL_HEADERS}
)
target_include_directories(fluxrec-stimulus PRIVATE "${PROTOCOL_DIR}")
target_link_libraries(fluxrec-stimulus PRIVATE PkgConfig::WAYLAND_EGL)
target_compile_options(fluxrec-stimulus PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Wconversion>
)

install(TARGETS fluxrec-stimulus RUNTIME DESTINATION bin)
