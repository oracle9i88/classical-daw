cmake_minimum_required(VERSION 3.20)
project(classical_daw VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(Threads REQUIRED)
enable_testing()

add_library(daw_engine STATIC
  src/engine/midi.cpp
  src/engine/midi_sequence.cpp
  src/engine/meter_map.cpp
  src/engine/history.cpp
  src/engine/frozen_track.cpp
  src/engine/project.cpp
  src/engine/render.cpp
  src/engine/score.cpp
  src/engine/score_midi.cpp
  src/engine/score_tempo.cpp
  src/engine/session.cpp
  src/engine/session_mix.cpp
  src/engine/session_recovery.cpp
  src/engine/streaming_audio.cpp
  src/engine/session_player.cpp
  src/engine/tempo_map.cpp
  src/engine/timeline.cpp
  src/engine/wav.cpp
)
target_link_libraries(daw_engine PUBLIC Threads::Threads)
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

add_executable(daw_midi_import_tests tests/midi_import_tests.cpp)
target_link_libraries(daw_midi_import_tests PRIVATE daw_engine)
add_test(NAME daw_midi_import_tests COMMAND daw_midi_import_tests)

add_executable(daw_score_tie_tests tests/score_tie_tests.cpp)
target_link_libraries(daw_score_tie_tests PRIVATE daw_engine)
add_test(NAME daw_score_tie_tests COMMAND daw_score_tie_tests)

add_executable(daw_musicxml_dynamics_tests tests/musicxml_dynamics_tests.cpp)
target_link_libraries(daw_musicxml_dynamics_tests PRIVATE daw_engine)
add_test(NAME daw_musicxml_dynamics_tests COMMAND daw_musicxml_dynamics_tests)

add_executable(daw_musicxml_timing_tests tests/musicxml_timing_tests.cpp)
target_link_libraries(daw_musicxml_timing_tests PRIVATE daw_engine)
add_test(NAME daw_musicxml_timing_tests COMMAND daw_musicxml_timing_tests)

add_executable(daw_midi_channel_events_tests tests/midi_channel_events_tests.cpp)
target_link_libraries(daw_midi_channel_events_tests PRIVATE daw_engine)
add_test(NAME daw_midi_channel_events_tests COMMAND daw_midi_channel_events_tests)

add_executable(daw_score_midi_events_tests tests/score_midi_events_tests.cpp)
target_link_libraries(daw_score_midi_events_tests PRIVATE daw_engine)
add_test(NAME daw_score_midi_events_tests COMMAND daw_score_midi_events_tests)

add_executable(daw_project_midi_events_tests tests/project_midi_events_tests.cpp)
target_link_libraries(daw_project_midi_events_tests PRIVATE daw_engine)
add_test(NAME daw_project_midi_events_tests COMMAND daw_project_midi_events_tests)

add_executable(daw_midi_render_controls_tests tests/midi_render_controls_tests.cpp)
target_link_libraries(daw_midi_render_controls_tests PRIVATE daw_engine)
add_test(NAME daw_midi_render_controls_tests COMMAND daw_midi_render_controls_tests)

add_executable(daw_score_tempo_tests tests/score_tempo_tests.cpp)
target_link_libraries(daw_score_tempo_tests PRIVATE daw_engine)
add_test(NAME daw_score_tempo_tests COMMAND daw_score_tempo_tests)

add_executable(daw_project_tempo_tests tests/project_tempo_tests.cpp)
target_link_libraries(daw_project_tempo_tests PRIVATE daw_engine)
add_test(NAME daw_project_tempo_tests COMMAND daw_project_tempo_tests)

add_executable(daw_midi_meter_tests tests/midi_meter_tests.cpp)
target_link_libraries(daw_midi_meter_tests PRIVATE daw_engine)
add_test(NAME daw_midi_meter_tests COMMAND daw_midi_meter_tests)

add_executable(daw_score_meter_tests tests/score_meter_tests.cpp)
target_link_libraries(daw_score_meter_tests PRIVATE daw_engine)
add_test(NAME daw_score_meter_tests COMMAND daw_score_meter_tests)

add_executable(daw_project_meter_tests tests/project_meter_tests.cpp)
target_link_libraries(daw_project_meter_tests PRIVATE daw_engine)
add_test(NAME daw_project_meter_tests COMMAND daw_project_meter_tests)

add_executable(daw_musicxml_fractional_tests tests/musicxml_fractional_tests.cpp)
target_link_libraries(daw_musicxml_fractional_tests PRIVATE daw_engine)
add_test(NAME daw_musicxml_fractional_tests COMMAND daw_musicxml_fractional_tests)

add_executable(daw_musicxml_divisions_tests tests/musicxml_divisions_tests.cpp)
target_link_libraries(daw_musicxml_divisions_tests PRIVATE daw_engine)
add_test(NAME daw_musicxml_divisions_tests COMMAND daw_musicxml_divisions_tests)

add_executable(daw_musicxml_tempo_tests tests/musicxml_tempo_tests.cpp)
target_link_libraries(daw_musicxml_tempo_tests PRIVATE daw_engine)
add_test(NAME daw_musicxml_tempo_tests COMMAND daw_musicxml_tempo_tests)

add_executable(daw_musicxml_meter_tests tests/musicxml_meter_tests.cpp)
target_link_libraries(daw_musicxml_meter_tests PRIVATE daw_engine)
add_test(NAME daw_musicxml_meter_tests COMMAND daw_musicxml_meter_tests)

add_executable(daw_project_measure_tests tests/project_measure_tests.cpp)
target_link_libraries(daw_project_measure_tests PRIVATE daw_engine)
add_test(NAME daw_project_measure_tests COMMAND daw_project_measure_tests)

# Local import diagnostics; requires no audio device, UI, or network access.
add_executable(daw_midi_inspect tools/midi_inspect.cpp)
target_link_libraries(daw_midi_inspect PRIVATE daw_engine)

add_executable(daw_midi_sequence_tests tests/midi_sequence_tests.cpp)
target_link_libraries(daw_midi_sequence_tests PRIVATE daw_engine)
add_test(NAME daw_midi_sequence_tests COMMAND daw_midi_sequence_tests)

add_executable(daw_session_tests tests/session_tests.cpp)
target_link_libraries(daw_session_tests PRIVATE daw_engine)
add_test(NAME daw_session_tests COMMAND daw_session_tests)
add_executable(daw_session_player_tests tests/session_player_tests.cpp)
target_link_libraries(daw_session_player_tests PRIVATE daw_engine)
add_test(NAME daw_session_player_tests COMMAND daw_session_player_tests)
add_executable(daw_output_health_tests tests/output_health_tests.cpp)
target_link_libraries(daw_output_health_tests PRIVATE daw_engine)
add_test(NAME daw_output_health_tests COMMAND daw_output_health_tests)
add_executable(daw_session_mix_tests tests/session_mix_tests.cpp)
target_link_libraries(daw_session_mix_tests PRIVATE daw_engine Threads::Threads)
add_test(NAME daw_session_mix_tests COMMAND daw_session_mix_tests)
add_executable(daw_session_mix_history_tests tests/session_mix_history_tests.cpp)
target_link_libraries(daw_session_mix_history_tests PRIVATE daw_engine)
add_test(NAME daw_session_mix_history_tests COMMAND daw_session_mix_history_tests)
add_executable(daw_session_recovery_tests tests/session_recovery_tests.cpp)
target_link_libraries(daw_session_recovery_tests PRIVATE daw_engine)
add_test(NAME daw_session_recovery_tests COMMAND daw_session_recovery_tests)
add_executable(daw_streaming_audio_tests tests/streaming_audio_tests.cpp)
target_link_libraries(daw_streaming_audio_tests PRIVATE daw_engine)
add_test(NAME daw_streaming_audio_tests COMMAND daw_streaming_audio_tests)
add_executable(daw_frozen_stream_writer_tests tests/frozen_stream_writer_tests.cpp)
target_link_libraries(daw_frozen_stream_writer_tests PRIVATE daw_engine)
add_test(NAME daw_frozen_stream_writer_tests COMMAND daw_frozen_stream_writer_tests)
add_executable(daw_wav_stream_writer_tests tests/wav_stream_writer_tests.cpp)
target_link_libraries(daw_wav_stream_writer_tests PRIVATE daw_engine)
add_test(NAME daw_wav_stream_writer_tests COMMAND daw_wav_stream_writer_tests)
add_executable(daw_frozen_track_tests tests/frozen_track_tests.cpp)
target_link_libraries(daw_frozen_track_tests PRIVATE daw_engine)
add_test(NAME daw_frozen_track_tests COMMAND daw_frozen_track_tests)

add_executable(daw_session_recover tools/session_recover.cpp)
target_link_libraries(daw_session_recover PRIVATE daw_engine)

add_executable(daw_session_fixture tools/session_fixture.cpp)
target_link_libraries(daw_session_fixture PRIVATE daw_engine)
add_executable(daw_session_edit tools/session_edit.cpp)
target_link_libraries(daw_session_edit PRIVATE daw_engine)

add_executable(daw_midi_roundtrip tools/midi_roundtrip.cpp)
target_link_libraries(daw_midi_roundtrip PRIVATE daw_engine)

add_executable(daw_midi_render tools/midi_render.cpp)
target_link_libraries(daw_midi_render PRIVATE daw_engine)

if(APPLE)
  enable_language(OBJCXX)
  add_library(daw_au_host STATIC src/platform/macos/audio_unit_instrument.cpp src/platform/macos/audio_unit_runtime.mm)
  target_include_directories(daw_au_host PUBLIC src/platform/macos)
  target_link_libraries(daw_au_host PUBLIC daw_engine "-framework AudioToolbox" "-framework CoreFoundation" "-framework AppKit")
  target_compile_options(daw_au_host PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
  add_executable(daw_swam_state_tests tests/swam_state_tests.mm)
  target_link_libraries(daw_swam_state_tests PRIVATE daw_au_host)
  add_test(NAME daw_swam_state_tests COMMAND daw_swam_state_tests)
  add_library(daw_pianoteq INTERFACE)
  target_link_libraries(daw_pianoteq INTERFACE daw_au_host)

  add_executable(daw_piano_render tools/piano_render.cpp)
  target_link_libraries(daw_piano_render PRIVATE daw_pianoteq)

  add_executable(daw_instrument_render tools/piano_render.cpp)
  target_compile_definitions(daw_instrument_render PRIVATE DAW_GENERIC_INSTRUMENT_CLI=1)
  target_link_libraries(daw_instrument_render PRIVATE daw_au_host)

  add_executable(daw_session_render tools/session_render.cpp)
  target_link_libraries(daw_session_render PRIVATE daw_au_host)

  # Explicit installed/licensed Pianoteq check; never part of CTest/CI.
  add_executable(daw_au_chunk_probe tools/au_chunk_probe.cpp)
  target_link_libraries(daw_au_chunk_probe PRIVATE daw_au_host)

  add_library(daw_coreaudio STATIC
    src/platform/macos/coreaudio_output.cpp
  )
  target_include_directories(daw_coreaudio PUBLIC src/platform/macos)
  target_link_libraries(daw_coreaudio PUBLIC daw_engine "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
  target_compile_features(daw_coreaudio PUBLIC cxx_std_17)
  target_compile_options(daw_coreaudio PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
  add_executable(daw_session_play tools/session_play.cpp)
  target_link_libraries(daw_session_play PRIVATE daw_coreaudio daw_au_host)
  # Explicit local hardware check, intentionally excluded from CTest/CI.
  add_executable(daw_output_probe tools/output_probe.cpp)
  target_link_libraries(daw_output_probe PRIVATE daw_coreaudio)

  add_library(daw_coremidi STATIC
    src/platform/macos/coremidi_host.cpp
  )
  target_include_directories(daw_coremidi PUBLIC src/platform/macos)
  target_link_libraries(daw_coremidi PUBLIC "-framework CoreMIDI" "-framework CoreFoundation")
  target_compile_features(daw_coremidi PUBLIC cxx_std_17)
  target_compile_options(daw_coremidi PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()
