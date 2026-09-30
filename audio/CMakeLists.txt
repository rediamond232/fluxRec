cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(fluxrec_audio LANGUAGES CXX)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)

add_library(fluxrec_audio_tracks STATIC track_config.cpp)
target_compile_features(fluxrec_audio_tracks PUBLIC cxx_std_23)
target_include_directories(fluxrec_audio_tracks PUBLIC "${CMAKE_CURRENT_LIST_DIR}/..")
target_compile_options(fluxrec_audio_tracks PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)

add_library(fluxrec_audio STATIC audio.cpp)
target_compile_features(fluxrec_audio PUBLIC cxx_std_23)
target_include_directories(fluxrec_audio PUBLIC "${CMAKE_CURRENT_LIST_DIR}/..")
target_link_libraries(fluxrec_audio
    PUBLIC
        fluxrec_audio_tracks
        PkgConfig::PIPEWIRE
        PkgConfig::AVCODEC
        PkgConfig::AVUTIL
        Threads::Threads
)

if(BUILD_TESTING)
    add_executable(fluxrec_audio_track_config_test tests/track_config_test.cpp)
    target_compile_features(fluxrec_audio_track_config_test PRIVATE cxx_std_23)
    target_link_libraries(fluxrec_audio_track_config_test PRIVATE fluxrec_audio_tracks)
    target_compile_options(fluxrec_audio_track_config_test PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    add_test(NAME fluxrec_audio_track_config COMMAND fluxrec_audio_track_config_test)

    add_executable(fluxrec_audio_capture_track_api_test tests/capture_track_api_test.cpp)
    target_compile_features(fluxrec_audio_capture_track_api_test PRIVATE cxx_std_23)
    target_link_libraries(fluxrec_audio_capture_track_api_test PRIVATE fluxrec_audio)
    target_compile_options(fluxrec_audio_capture_track_api_test PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    add_test(NAME fluxrec_audio_capture_track_api
        COMMAND fluxrec_audio_capture_track_api_test)

    add_executable(fluxrec_audio_pipewire_clock_metrics_test
        tests/pipewire_clock_metrics_test.cpp)
    target_compile_features(fluxrec_audio_pipewire_clock_metrics_test PRIVATE cxx_std_23)
    target_include_directories(fluxrec_audio_pipewire_clock_metrics_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/..")
    target_compile_options(fluxrec_audio_pipewire_clock_metrics_test PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    add_test(NAME fluxrec_audio_pipewire_clock_metrics
        COMMAND fluxrec_audio_pipewire_clock_metrics_test)
endif()

# Live fixture target: it deliberately creates temporary PipeWire nodes and is
# not registered as a CTest. The runner never changes default audio routes.
if(BUILD_TESTING AND TARGET fluxrec_mux AND TARGET fluxrec_replay)
    add_executable(fluxrec-audio-av-sync-fixture
        tests/av_sync_fixture.cpp
        ../engine/audio_session.cpp
    )
    target_compile_features(fluxrec-audio-av-sync-fixture PRIVATE cxx_std_23)
    target_include_directories(fluxrec-audio-av-sync-fixture PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/..")
    target_link_libraries(fluxrec-audio-av-sync-fixture PRIVATE
        fluxrec_audio fluxrec_mux fluxrec_replay Threads::Threads)
    target_compile_options(fluxrec-audio-av-sync-fixture PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()
