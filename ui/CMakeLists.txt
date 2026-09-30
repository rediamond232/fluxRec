cmake_minimum_required(VERSION 3.25)
project(FluxrecUI LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET GLOBAL libpipewire-0.3)

add_subdirectory(preview_consumer)

target_sources(fluxrec-record PRIVATE
  src/audio_device_catalog.cpp
  src/audio_device_catalog.hpp
  src/audio_mixer_panel.cpp
  src/audio_mixer_panel.hpp
  src/main.cpp
  src/main_window.cpp
  src/main_window.hpp
  src/process_recorder_backend.cpp
  src/process_recorder_backend.hpp
  src/recorder_backend.hpp
  src/recorder_settings.cpp
  src/recorder_settings.hpp
  src/scene_profiles_panel.cpp
  src/scene_profiles_panel.hpp
  src/settings_dialog.cpp
  src/settings_dialog.hpp
  src/theme.cpp
  src/theme.hpp
  src/theme_tokens.hpp
  src/wayland_output_picker.cpp
  src/wayland_output_picker.hpp
)
target_link_libraries(fluxrec-record PRIVATE Qt6::Widgets fluxrec_preview_consumer
                      fluxrec_audio_tracks
                      PkgConfig::PIPEWIRE)
target_compile_options(fluxrec-record PRIVATE -Wall -Wextra -Wpedantic)

add_custom_target(fluxrec-ui-theme-check ALL
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_theme_tokens.py"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  COMMENT "Checking Qt theme tokens against DESIGN.md"
  VERBATIM
)
add_dependencies(fluxrec-record fluxrec-ui-theme-check)

if(BUILD_TESTING)
  add_test(NAME fluxrec_ui_smoke
    COMMAND "$<TARGET_FILE:fluxrec-record>" --smoke
  )
  set_tests_properties(fluxrec_ui_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  )
endif()
