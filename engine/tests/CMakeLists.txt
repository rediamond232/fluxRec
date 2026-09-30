# Include with add_subdirectory(tests) from engine/CMakeLists.txt after the
# fluxrec_mux and fluxrec_replay component targets have been defined.
if(BUILD_TESTING)
  find_package(Threads REQUIRED)

  add_executable(fluxrec_record_options_test
    record_options_test.cpp
    ../record_options.cpp
    ../../audio/track_config.cpp
  )
  target_compile_features(fluxrec_record_options_test PRIVATE cxx_std_23)
  target_include_directories(fluxrec_record_options_test PRIVATE
    "${PROJECT_SOURCE_DIR}")
  target_compile_options(fluxrec_record_options_test PRIVATE
    -Wall -Wextra -Wpedantic)
  add_test(NAME fluxrec_record_options COMMAND fluxrec_record_options_test)

  add_executable(fluxrec_encoder_stats_telemetry_test
    encoder_stats_telemetry_test.cpp
  )
  target_compile_features(fluxrec_encoder_stats_telemetry_test PRIVATE cxx_std_23)
  target_include_directories(fluxrec_encoder_stats_telemetry_test PRIVATE
    "${PROJECT_SOURCE_DIR}")
  target_link_libraries(fluxrec_encoder_stats_telemetry_test PRIVATE
    fluxrec_telemetry)
  target_compile_options(fluxrec_encoder_stats_telemetry_test PRIVATE
    -Wall -Wextra -Wpedantic)
  add_test(NAME fluxrec_encoder_stats_telemetry
    COMMAND fluxrec_encoder_stats_telemetry_test)

  add_executable(fluxrec_packet_router_test
    packet_router_test.cpp
    ../packet_router.cpp
  )
  target_compile_features(fluxrec_packet_router_test PRIVATE cxx_std_23)
  target_include_directories(fluxrec_packet_router_test PRIVATE
    "${PROJECT_SOURCE_DIR}")
  target_link_libraries(fluxrec_packet_router_test PRIVATE
    fluxrec_mux fluxrec_replay fluxrec_telemetry Threads::Threads)
  target_compile_options(fluxrec_packet_router_test PRIVATE
    -Wall -Wextra -Wpedantic)
  add_test(NAME fluxrec_packet_router COMMAND fluxrec_packet_router_test)

  add_executable(fluxrec_replay_export_integration_test
    replay_export_integration.cpp
    ../replay_export.cpp
  )
  target_compile_features(fluxrec_replay_export_integration_test PRIVATE cxx_std_23)
  target_include_directories(fluxrec_replay_export_integration_test PRIVATE
    "${PROJECT_SOURCE_DIR}")
  target_link_libraries(fluxrec_replay_export_integration_test PRIVATE
    fluxrec_mux fluxrec_replay Threads::Threads)
  target_compile_options(fluxrec_replay_export_integration_test PRIVATE
    -Wall -Wextra -Wpedantic)
  add_test(NAME fluxrec_replay_export_integration
    COMMAND fluxrec_replay_export_integration_test)

  add_executable(fluxrec_control_service_integration_test
    control_service_integration.cpp
    ../control_pipe.cpp
    ../control_service.cpp
    ../replay_export.cpp
  )
  target_compile_features(fluxrec_control_service_integration_test PRIVATE cxx_std_23)
  target_include_directories(fluxrec_control_service_integration_test PRIVATE
    "${PROJECT_SOURCE_DIR}")
  target_link_libraries(fluxrec_control_service_integration_test PRIVATE
    fluxrec_mux fluxrec_replay fluxrec_audio_tracks Threads::Threads)
  target_compile_options(fluxrec_control_service_integration_test PRIVATE
    -Wall -Wextra -Wpedantic)
  add_test(NAME fluxrec_control_service_integration
    COMMAND fluxrec_control_service_integration_test)

  add_executable(fluxrec_audio_control_integration_test
    audio_control_integration.cpp
    ../control_pipe.cpp
    ../control_service.cpp
    ../replay_export.cpp
  )
  target_compile_features(fluxrec_audio_control_integration_test PRIVATE cxx_std_23)
  target_include_directories(fluxrec_audio_control_integration_test PRIVATE
    "${PROJECT_SOURCE_DIR}")
  target_link_libraries(fluxrec_audio_control_integration_test PRIVATE
    fluxrec_mux fluxrec_replay fluxrec_audio_tracks Threads::Threads)
  target_compile_options(fluxrec_audio_control_integration_test PRIVATE
    -Wall -Wextra -Wpedantic)
  add_test(NAME fluxrec_audio_control_integration
    COMMAND fluxrec_audio_control_integration_test)

  add_executable(fluxrec_audio_telemetry_shape_test
    audio_telemetry_shape_test.cpp
    ../audio_session.cpp
  )
  target_compile_features(fluxrec_audio_telemetry_shape_test PRIVATE cxx_std_23)
  target_include_directories(fluxrec_audio_telemetry_shape_test PRIVATE
    "${PROJECT_SOURCE_DIR}")
  target_link_libraries(fluxrec_audio_telemetry_shape_test PRIVATE
    fluxrec_mux fluxrec_replay fluxrec_audio Threads::Threads)
  target_compile_options(fluxrec_audio_telemetry_shape_test PRIVATE
    -Wall -Wextra -Wpedantic)
  add_test(NAME fluxrec_audio_telemetry_shape
    COMMAND fluxrec_audio_telemetry_shape_test)
endif()
