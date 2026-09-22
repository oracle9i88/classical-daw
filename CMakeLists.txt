cmake_minimum_required(VERSION 3.20)
project(classical_daw VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
enable_testing()

add_library(daw_engine STATIC
  src/engine/midi.cpp
  src/engine/history.cpp
  src/engine/project.cpp
  src/engine/render.cpp
  src/engine/score.cpp
  src/engine/score_midi.cpp
  src/engine/tempo_map.cpp
  src/engine/timeline.cpp
  src/engine/wav.cpp
)
target_include_directories(daw_engine PUBLIC src/engine/include)
target_compile_features(daw_engine PUBLIC cxx_std_17)
if(MSVC)
  target_compile_options(daw_engine PRIVATE /W4 /permissive-)
else()
  target_compile_options(daw_engine PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(daw_tests tests/test_engine.cpp)
target_link_libraries(daw_tests PRIVATE daw_engine)
add_test(NAME daw_tests COMMAND daw_tests)

add_executable(daw_project_tests tests/project_tests.cpp)
target_link_libraries(daw_project_tests PRIVATE daw_engine)
add_test(NAME daw_project_tests COMMAND daw_project_tests)

add_executable(daw_project_recovery_tests tests/project_recovery_tests.cpp)
target_link_libraries(daw_project_recovery_tests PRIVATE daw_engine)
add_test(NAME daw_project_recovery_tests COMMAND daw_project_recovery_tests)

add_executable(daw_history_tests tests/history_tests.cpp)
target_link_libraries(daw_history_tests PRIVATE daw_engine)
add_test(NAME daw_history_tests COMMAND daw_history_tests)

add_executable(daw_project_history_tests tests/project_history_tests.cpp)
target_link_libraries(daw_project_history_tests PRIVATE daw_engine)
add_test(NAME daw_project_history_tests COMMAND daw_project_history_tests)

add_executable(daw_realtime_event_tests tests/realtime_event_tests.cpp)
target_link_libraries(daw_realtime_event_tests PRIVATE daw_engine)
add_test(NAME daw_realtime_event_tests COMMAND daw_realtime_event_tests)

add_executable(daw_score_render_tests tests/score_render_tests.cpp)
target_link_libraries(daw_score_render_tests PRIVATE daw_engine)
add_test(NAME daw_score_render_tests COMMAND daw_score_render_tests)

if(APPLE)
  add_library(daw_coreaudio STATIC
    src/platform/macos/coreaudio_output.cpp
  )
  target_include_directories(daw_coreaudio PUBLIC src/platform/macos)
  target_link_libraries(daw_coreaudio PUBLIC daw_engine "-framework AudioToolbox" "-framework CoreAudio")
  target_compile_features(daw_coreaudio PUBLIC cxx_std_17)
  target_compile_options(daw_coreaudio PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

  add_library(daw_coremidi STATIC
    src/platform/macos/coremidi_host.cpp
  )
  target_include_directories(daw_coremidi PUBLIC src/platform/macos)
  target_link_libraries(daw_coremidi PUBLIC "-framework CoreMIDI" "-framework CoreFoundation")
  target_compile_features(daw_coremidi PUBLIC cxx_std_17)
  target_compile_options(daw_coremidi PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()
