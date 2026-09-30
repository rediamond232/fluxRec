add_executable(fluxrec-probe probe.cpp)
target_link_libraries(fluxrec-probe PRIVATE fluxrec_capture)
target_include_directories(fluxrec-probe PRIVATE "${PROJECT_SOURCE_DIR}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

add_executable(fluxrec-capture-smoke capture_smoke.cpp)
target_link_libraries(fluxrec-capture-smoke PRIVATE
  fluxrec_capture PkgConfig::GBM PkgConfig::LIBDRM)
target_include_directories(fluxrec-capture-smoke PRIVATE "${PROJECT_SOURCE_DIR}")

if(TARGET fluxrec_gpu)
  add_executable(fluxrec-record
    fluxrec_main.cpp
    record.cpp
    packet_router.cpp
    record_options.cpp
    audio_session.cpp
    replay_export.cpp
    control_pipe.cpp
    control_service.cpp)
  set_target_properties(fluxrec-record PROPERTIES
    OUTPUT_NAME fluxrec
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON)
  add_custom_command(TARGET fluxrec-record POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -f
      "$<TARGET_FILE_DIR:fluxrec-record>/fluxrec-record"
      "${CMAKE_BINARY_DIR}/ui/fluxrec-ui"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/ui"
    COMMAND "${CMAKE_COMMAND}" -E create_symlink
      "$<TARGET_FILE_NAME:fluxrec-record>"
      "$<TARGET_FILE_DIR:fluxrec-record>/fluxrec-record"
    COMMAND "${CMAKE_COMMAND}" -E create_symlink
      "$<TARGET_FILE:fluxrec-record>"
      "${CMAKE_BINARY_DIR}/ui/fluxrec-ui"
    VERBATIM
  )
  target_link_libraries(fluxrec-record PRIVATE
    fluxrec_capture fluxrec_gpu fluxrec_mux fluxrec_replay fluxrec_telemetry fluxrec_audio)
  target_include_directories(fluxrec-record PRIVATE "${PROJECT_SOURCE_DIR}")
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
