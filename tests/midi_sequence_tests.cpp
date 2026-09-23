#include "daw/midi_sequence.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template <typename F> void rejects(F function) {
  bool failed = false;
  try { function(); } catch (const std::exception&) { failed = true; }
  require(failed, "invalid sequence was accepted");
}
daw::MidiFile fixture() {
  daw::MidiFile midi;
  midi.tracks.push_back({"Piano", {{960, 960, 60, 90, 3, 47}}, {}});
  return midi;
}
}
int main() {
  try {
    auto midi = fixture();
    midi.tempo.addChange(960, 60);
    auto sequence = daw::makeMidiSampleSequence(midi, 48000, 1);
    require(sequence.frames == 120000 && sequence.end_frame == 72000, "step tempo / tail was lost");
    require(sequence.events.size() == 6, "note plus final pedal release messages missing");
    require(sequence.events[0].frame == 24000 && sequence.events[0].status == 0x93, "note-on timing/channel wrong");
    require(sequence.events[1].frame == 72000 && sequence.events[1].status == 0x83 &&
            sequence.events[1].data2 == 47, "release velocity/timing lost");
    for (std::size_t i = 2; i < 6; ++i) require(sequence.events[i].frame == 72000 &&
        sequence.events[i].status == 0xb3 && sequence.events[i].data2 == 0, "end release wrong");

    midi = fixture();
    midi.tracks[0].notes = {{0, 960, 60, 90, 0, 0, 1, 4}, {960, 960, 60, 90, 0, 0, 6, 8}};
    midi.tracks[0].channel_events = {{960, daw::MidiChannelEventType::ControlChange, 0, 64, 127, 5}};
    sequence = daw::makeMidiSampleSequence(midi);
    require(sequence.events[1].status == 0x80 && sequence.events[2].data1 == 64 &&
            sequence.events[3].status == 0x90, "same-tick source ordering changed");
    midi.tracks[0].notes[0].off_order = 7;
    rejects([&] { daw::makeMidiSampleSequence(midi); });

    // Adjacent source ticks rounding to the same sample must remain chronological,
    // even where track index and source ordinal suggest the opposite order.
    midi = fixture();
    midi.tempo = daw::TempoMap(6000);
    midi.tracks[0].notes = {{2, 100, 64, 90}};
    midi.tracks.push_back({"Earlier", {{1, 100, 60, 90}}, {}});
    sequence = daw::makeMidiSampleSequence(midi, 8000, 1);
    require(sequence.events[0].frame == 0 && sequence.events[1].frame == 0 &&
            sequence.events[0].data1 == 60 && sequence.events[1].data1 == 64, "collapsed sample order changed");

    require(daw::isInstrumentSelection({0, 0xc4, 42, 0}), "program change not recognized");
    require(daw::isInstrumentSelection({0, 0xb7, 32, 2}), "bank LSB not recognized");
    require(daw::isInstrumentSelection({0, 0xb0, 0, 2}), "bank MSB not recognized");
    require(!daw::isInstrumentSelection({0, 0xb0, 64, 127}), "sustain misclassified");
    require(!daw::isInstrumentSelection({0, 0x90, 32, 90}), "note misclassified");
    midi = fixture();
    rejects([&] { daw::makeMidiSampleSequence(midi, 7999); });
    rejects([&] { daw::makeMidiSampleSequence(midi, 48000, 0); });
    rejects([&] { daw::makeMidiSampleSequence(midi, 48000, std::numeric_limits<double>::infinity()); });
    rejects([&] { daw::makeMidiSampleSequence(midi, 48000, 1, 95999); });
    require(daw::makeMidiSampleSequence(midi, 48000, 1, 96000).frames == 96000, "exact budget rejected");
    midi.tracks[0].notes[0].velocity = 0;
    rejects([&] { daw::makeMidiSampleSequence(midi); });
    midi = fixture(); midi.ticks_per_quarter = 480;
    rejects([&] { daw::makeMidiSampleSequence(midi); });
    midi = fixture(); midi.tracks[0].notes[0].duration = std::numeric_limits<daw::Tick>::max();
    rejects([&] { daw::makeMidiSampleSequence(midi); });
    midi = fixture(); midi.tracks[0].channel_events = {{0, daw::MidiChannelEventType::ChannelPressure, 0, 1, 1}};
    rejects([&] { daw::makeMidiSampleSequence(midi); });
    midi = fixture(); midi.tracks[0].notes.clear();
    sequence = daw::makeMidiSampleSequence(midi);
    require(sequence.events.empty() && sequence.frames == 240000, "empty sequence is not bounded silence");
    daw::requireInitialExpression(sequence);
    midi = fixture();
    rejects([&] { daw::requireInitialExpression(daw::makeMidiSampleSequence(midi)); });
    midi.tracks[0].channel_events = {{0, daw::MidiChannelEventType::ControlChange, 3, 11, 0}};
    daw::requireInitialExpression(daw::makeMidiSampleSequence(midi));  // Deliberate silence before crescendo is valid.
    midi.tracks[0].channel_events[0].channel = 2;
    rejects([&] { daw::requireInitialExpression(daw::makeMidiSampleSequence(midi)); });
    midi.tracks[0].channel_events[0].channel = 3;
    midi.tracks[0].channel_events[0].tick = 961;
    rejects([&] { daw::requireInitialExpression(daw::makeMidiSampleSequence(midi)); });
    midi = fixture();
    midi.tracks[0].notes[0].pitch = 41;
    daw::requireNoteRange(midi, 36, 89, 0);
    rejects([&] { daw::requireNoteRange(midi, 36, 89, -12); });
    midi.tracks[0].notes[0].pitch = 48;
    daw::requireNoteRange(midi, 36, 89, -12);
    for (const auto key : {36, 89}) {
      midi.tracks[0].notes[0].pitch = static_cast<std::uint8_t>(key);
      daw::requireNoteRange(midi, 36, 89, 0);
    }
    for (const auto key : {35, 90}) {
      midi.tracks[0].notes[0].pitch = static_cast<std::uint8_t>(key);
      rejects([&] { daw::requireNoteRange(midi, 36, 89, 0); });
    }
    rejects([&] { daw::requireNoteRange(midi, 89, 36, 0); });
    rejects([&] { daw::requireNoteRange(midi, 36, 89, std::numeric_limits<int>::max()); });
    std::cout << "MIDI sample sequence and instrument pitch-range tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
