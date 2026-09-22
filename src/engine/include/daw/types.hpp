#pragma once

#include <cstdint>

namespace daw {

// The transport uses a fixed PPQ resolution so score edits remain sample-rate
// independent. 960 ticks per quarter note gives enough resolution for common
// tuplets while keeping integer arithmetic practical.
using Tick = std::int64_t;
using SampleIndex = std::int64_t;
constexpr Tick kTicksPerQuarter = 960;

struct TimeSignature {
  std::uint8_t numerator = 4;
  std::uint8_t denominator = 4;
  // SMF FF 58's remaining bytes: MIDI clocks per metronome click and
  // notated 32nd notes per MIDI quarter. Kept independently of tempo.
  std::uint8_t clocks_per_click = 24;
  std::uint8_t notated_32nds_per_quarter = 8;
};

struct TimeSignatureChange {
  Tick tick = 0;
  TimeSignature signature{};
};

}  // namespace daw
