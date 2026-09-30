add_library(fluxrec_replay
  replay_buffer.cpp
)

target_compile_features(fluxrec_replay PUBLIC cxx_std_23)
target_include_directories(fluxrec_replay PUBLIC "${PROJECT_SOURCE_DIR}")

if(BUILD_TESTING)
  find_package(Threads REQUIRED)

  add_executable(fluxrec_replay_tests
    tests/replay_buffer_tests.cpp
  )
  target_link_libraries(fluxrec_replay_tests PRIVATE fluxrec_replay Threads::Threads)
  add_test(NAME fluxrec_replay_tests COMMAND fluxrec_replay_tests)
endif()
