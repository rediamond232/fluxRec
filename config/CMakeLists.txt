cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(fluxrec_config LANGUAGES CXX)
endif()

find_package(nlohmann_json 3.2.0 REQUIRED)
include(CTest)

add_library(fluxrec_config settings.cpp)
target_compile_features(fluxrec_config PUBLIC cxx_std_23)
target_link_libraries(fluxrec_config PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(fluxrec_config PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/..>
)

if(BUILD_TESTING)
    add_executable(fluxrec_config_test tests/settings_store_test.cpp)
    target_compile_features(fluxrec_config_test PRIVATE cxx_std_23)
    target_link_libraries(fluxrec_config_test PRIVATE fluxrec_config)
    add_test(NAME fluxrec_config_test COMMAND fluxrec_config_test)
endif()
