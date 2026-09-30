cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(fluxrec_telemetry LANGUAGES CXX)
endif()

add_library(fluxrec_telemetry telemetry.cpp system_metrics.cpp)
target_compile_features(fluxrec_telemetry PUBLIC cxx_std_23)
find_package(Threads REQUIRED)
target_link_libraries(fluxrec_telemetry PUBLIC Threads::Threads)
target_link_libraries(fluxrec_telemetry PRIVATE ${CMAKE_DL_LIBS})
target_include_directories(fluxrec_telemetry PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>
)

option(FLUXREC_TELEMETRY_BUILD_TESTS "Build the self-contained telemetry tests" OFF)
if(BUILD_TESTING OR FLUXREC_TELEMETRY_BUILD_TESTS)
    enable_testing()
    add_executable(fluxrec_telemetry_test tests/telemetry_test.cpp)
    target_link_libraries(fluxrec_telemetry_test PRIVATE fluxrec_telemetry)
    add_test(NAME fluxrec_telemetry_test COMMAND fluxrec_telemetry_test)
endif()
