cmake_minimum_required(VERSION 3.25)
project(FluxrecPreviewConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Qml Quick Widgets)
find_package(Vulkan REQUIRED)

if(NOT TARGET fluxrec_preview)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../preview" preview_protocol)
endif()

add_library(fluxrec_preview_consumer STATIC
  preview_channel.cpp
  preview_channel.hpp
  preview_item.cpp
  preview_item.hpp
)
target_include_directories(fluxrec_preview_consumer PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../.."
)
target_link_libraries(fluxrec_preview_consumer PUBLIC
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Widgets
  Vulkan::Vulkan Fluxrec::Preview
)
target_compile_options(fluxrec_preview_consumer PRIVATE -Wall -Wextra -Wpedantic)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # Vulkan structs are zero-initialized with only the required sType/pNext and
  # explicitly set fields; suppress the aggregate warnings for that idiom.
  target_compile_options(fluxrec_preview_consumer PRIVATE -Wno-missing-field-initializers)
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
  add_executable(fluxrec_preview_consumer_test preview_channel_test.cpp)
  target_link_libraries(fluxrec_preview_consumer_test PRIVATE
    fluxrec_preview_consumer Qt6::Test
  )
  target_compile_options(fluxrec_preview_consumer_test PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME fluxrec_preview_consumer COMMAND fluxrec_preview_consumer_test)
endif()
