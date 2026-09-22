#pragma once

#include "daw/types.hpp"

#include <cstdint>

namespace daw {

enum class MidiChannelEventType : std::uint8_t {
  PolyPressure = 0xa0,
  ControlChange = 0xb0,
  ProgramChange = 0xc0,
  ChannelPressure = 0xd0,
  PitchBend = 0xe0,
};

// Stores the original 7-bit data bytes (pitch bend is LSB, then MSB).
// order is a per-track source-event ordinal. Zero means newly authored data.
// At a shared tick: new note-offs, source-ordered events, new channel events
// in vector order, then new note-ons. Releasing new notes first protects
// imported same-pitch retriggers; relative order within imported events stays.
struct MidiChannelEvent {
  Tick tick = 0;
  MidiChannelEventType type = MidiChannelEventType::ControlChange;
  std::uint8_t channel = 0;
  std::uint8_t data1 = 0;
  std::uint8_t data2 = 0;  // Must be zero for one-byte messages.
  std::uint64_t order = 0;
};

inline bool validMidiChannelEvent(const MidiChannelEvent& event) noexcept {
  if (event.tick < 0 || event.channel > 15 || event.data1 > 127 || event.data2 > 127) return false;
  switch (event.type) {
    case MidiChannelEventType::ProgramChange:
    case MidiChannelEventType::ChannelPressure: return event.data2 == 0;
    case MidiChannelEventType::ControlChange:
    case MidiChannelEventType::PolyPressure:
    case MidiChannelEventType::PitchBend: return true;
  }
  return false;
}

}  // namespace daw
