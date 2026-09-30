cmake_minimum_required(VERSION 3.20)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(fluxrec_preview LANGUAGES CXX)
    include(CTest)
endif()

add_library(fluxrec_preview STATIC preview.cpp)
add_library(Fluxrec::Preview ALIAS fluxrec_preview)
target_compile_features(fluxrec_preview PUBLIC cxx_std_23)
target_include_directories(fluxrec_preview
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
        "$<INSTALL_INTERFACE:include>"
)
target_compile_options(fluxrec_preview PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

if(BUILD_TESTING)
    add_executable(fluxrec_preview_protocol_test tests/preview_protocol_test.cpp)
    target_link_libraries(fluxrec_preview_protocol_test PRIVATE Fluxrec::Preview)
    target_compile_options(fluxrec_preview_protocol_test PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
    )
    add_test(NAME fluxrec_preview_protocol_test COMMAND fluxrec_preview_protocol_test)
endif()

install(TARGETS fluxrec_preview ARCHIVE DESTINATION lib)
install(FILES preview.hpp DESTINATION include/preview)
