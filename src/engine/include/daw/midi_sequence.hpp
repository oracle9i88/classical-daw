#pragma once

#include "daw/midi.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace daw {

struct MidiSampleEvent {
  std::size_t frame = 0;
  std::uint8_t status = 0;
  std::uint8_t data1 = 0;
  std::uint8_t data2 = 0;
};

struct MidiSampleSequence {
  std::uint32_t sample_rate = 48000;
  std::size_t frames = 0;
  std::size_t end_frame = 0;
  std::vector<MidiSampleEvent> events;
};

// Offline instrument-host sequence. Uses the same validated source ordering
// as the SMF/sine renderer, retaining all channel bytes and release velocity.
// Events at different ticks retain their order when rounded to the same frame.
// After the final source event, lifts sustain/sostenuto/hold-2 and sends all
// notes off on used channels. A positive tail is required to render that release.
// Default limit is 512 MiB of stereo float output. Throws before audio allocation.
// minimum_end_tick extends the shared release/tail boundary (never trims events),
// allowing independent instruments and explicit terminal rests to stay aligned.
MidiSampleSequence makeMidiSampleSequence(
    const MidiFile& midi, std::uint32_t rate = 48000, double tail_seconds = 5.0,
    std::size_t max_frames = 64U * 1024U * 1024U, Tick minimum_end_tick = 0);

// A fixed piano preset must not be replaced by imported GM bank/program data.
// The original project/MIDI still retains these messages.
bool isInstrumentSelection(const MidiSampleEvent& event) noexcept;

// Expressive instruments such as SWAM need CC11 before the first attack on
// each used MIDI channel. Fail clearly rather than silently invent automation.
void requireInitialExpression(const MidiSampleSequence& sequence);

}  // namespace daw
