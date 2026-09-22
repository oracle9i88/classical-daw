#include "daw/midi.hpp"

#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3 || (argc == 3 && std::string(argv[2]) != "--notes")) {
    std::cerr << "Usage: daw_midi_inspect input.mid [--notes]\n";
    return 2;
  }
  daw::MidiFile midi;
  daw::MidiImportReport report;
  std::string error;
  if (!daw::readMidiFile(argv[1], &midi, &error, &report)) {
    std::cerr << "MIDI import failed: " << error << '\n';
    return 1;
  }
  std::size_t note_count = 0;
  for (const auto& track : midi.tracks) note_count += track.notes.size();
  std::cout << std::setprecision(17)
            << "{\n  \"format\": " << midi.format
            << ",\n  \"source_ppq\": " << report.source_ticks_per_quarter
            << ",\n  \"engine_ppq\": " << midi.ticks_per_quarter
            << ",\n  \"note_count\": " << note_count
            << ",\n  \"rounded_note_boundaries\": " << report.rounded_note_boundaries
            << ",\n  \"rounded_tempo_events\": " << report.rounded_tempo_events
            << ",\n  \"ignored_channel_events\": " << report.ignored_channel_events
            << ",\n  \"preserved_channel_events\": " << report.preserved_channel_events
            << ",\n  \"rounded_channel_events\": " << report.rounded_channel_events
            << ",\n  \"ignored_meta_events\": " << report.ignored_meta_events
            << ",\n  \"ignored_sysex_events\": " << report.ignored_sysex_events
            << ",\n  \"ignored_time_signature_events\": " << report.ignored_time_signature_events
            << ",\n  \"preserved_time_signature_events\": " << report.preserved_time_signature_events
            << ",\n  \"rounded_time_signature_events\": " << report.rounded_time_signature_events
            << ",\n  \"coalesced_time_signature_events\": " << report.coalesced_time_signature_events
            << ",\n  \"overlapping_same_pitch_notes\": " << report.overlapping_same_pitch_notes
            << ",\n  \"meter\": [" << static_cast<int>(midi.time_signature.numerator)
            << ", " << static_cast<int>(midi.time_signature.denominator) << "]"
            << ",\n  \"meter_changes\": [";
  const auto printMeter = [](daw::Tick tick, const daw::TimeSignature& meter) {
    std::cout << '[' << tick << ", " << static_cast<int>(meter.numerator)
              << ", " << static_cast<int>(meter.denominator)
              << ", " << static_cast<int>(meter.clocks_per_click)
              << ", " << static_cast<int>(meter.notated_32nds_per_quarter) << ']';
  };
  printMeter(0, midi.time_signature);
  for (const auto& change : midi.meter_changes) {
    std::cout << ", ";
    printMeter(change.tick, change.signature);
  }
  std::cout << "],\n  \"tempo_changes\": [";
  bool first = true;
  for (const auto& change : midi.tempo.changes()) {
    if (!first) std::cout << ", ";
    first = false;
    std::cout << "[" << change.tick << ", " << change.bpm << "]";
  }
  std::cout << "],\n  \"tracks\": [";
  for (std::size_t index = 0; index < midi.tracks.size(); ++index) {
    if (index) std::cout << ",";
    const auto& track = midi.tracks[index];
    std::cout << "\n    {\"index\": " << index << ", \"note_count\": " << track.notes.size();
    if (argc == 3) {
      std::cout << ", \"notes\": [";
      for (std::size_t n = 0; n < track.notes.size(); ++n) {
        if (n) std::cout << ", ";
        const auto& note = track.notes[n];
        // [absolute start, absolute end, pitch, attack velocity, channel]
        std::cout << '[' << note.start << ", " << note.end() << ", " << static_cast<int>(note.pitch)
                  << ", " << static_cast<int>(note.velocity) << ", " << static_cast<int>(note.channel) << ']';
      }
      std::cout << ']';
      std::cout << ", \"channel_events\": [";
      for (std::size_t n = 0; n < track.channel_events.size(); ++n) {
        if (n) std::cout << ", ";
        const auto& event = track.channel_events[n];
        std::cout << '[' << event.tick << ", " << (static_cast<int>(event.type) | event.channel)
                  << ", " << static_cast<int>(event.data1) << ", " << static_cast<int>(event.data2) << ']';
      }
      std::cout << ']';
    }
    std::cout << '}';
  }
  std::cout << "\n  ]\n}\n";
}
