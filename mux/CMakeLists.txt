cmake_minimum_required(VERSION 3.20)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(fluxrec_mux LANGUAGES CXX)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
)

add_library(fluxrec_mux mux_writer.cpp)
add_library(Fluxrec::Mux ALIAS fluxrec_mux)
target_compile_features(fluxrec_mux PUBLIC cxx_std_23)
target_include_directories(fluxrec_mux PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(fluxrec_mux PUBLIC PkgConfig::FFMPEG)
target_compile_options(fluxrec_mux PRIVATE -Wall -Wextra -Wpedantic)

option(FLUXREC_MUX_BUILD_TESTS "Build the synthetic packet mux integration test" ON)
if(FLUXREC_MUX_BUILD_TESTS)
    enable_testing()
    add_executable(fluxrec_mux_integration_test tests/mux_writer_integration.cpp)
    target_compile_features(fluxrec_mux_integration_test PRIVATE cxx_std_23)
    target_link_libraries(fluxrec_mux_integration_test PRIVATE fluxrec_mux PkgConfig::FFMPEG)
    target_compile_options(fluxrec_mux_integration_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME fluxrec_mux_integration COMMAND fluxrec_mux_integration_test)

    add_executable(fluxrec_mux_live_reorder_audio_sync_test
        tests/live_reorder_audio_sync.cpp)
    target_compile_features(fluxrec_mux_live_reorder_audio_sync_test PRIVATE cxx_std_23)
    target_link_libraries(fluxrec_mux_live_reorder_audio_sync_test PRIVATE
        fluxrec_mux PkgConfig::FFMPEG)
    target_compile_options(fluxrec_mux_live_reorder_audio_sync_test PRIVATE
        -Wall -Wextra -Wpedantic)
    add_test(NAME fluxrec_mux_live_reorder_audio_sync
        COMMAND fluxrec_mux_live_reorder_audio_sync_test)
    set_tests_properties(fluxrec_mux_live_reorder_audio_sync PROPERTIES TIMEOUT 10)
endif()
