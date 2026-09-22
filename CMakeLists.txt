cmake_minimum_required(VERSION 3.20)
project(classical_daw VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
enable_testing()

add_library(daw_engine STATIC
  src/engine/midi.cpp
  src/engine/render.cpp
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
