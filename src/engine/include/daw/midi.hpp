#pragma once

#include "daw/tempo_map.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace daw {

struct MidiNote {
  Tick start = 0;
  Tick duration = kTicksPerQuarter;
  std::uint8_t pitch = 60;
  std::uint8_t velocity = 100;
  std::uint8_t channel = 0;

  [[nodiscard]] Tick end() const { return start + duration; }
};

struct MidiTrack {
  std::string name;
  std::vector<MidiNote> notes;
};

struct MidiFile {
  std::int16_t format = 1;
  Tick ticks_per_quarter = kTicksPerQuarter;
  TimeSignature time_signature{};
  TempoMap tempo;
  std::vector<MidiTrack> tracks;
};

// A deliberately small Standard MIDI File (SMF) Type 0/1 reader and writer.
// It covers note events, track names, tempo events, and the first time
// signature event; unknown events are skipped so files from a full DAW can
// still be inspected.
bool writeMidiFile(const MidiFile& file, const std::string& path, std::string* error = nullptr);
bool readMidiFile(const std::string& path, MidiFile* file, std::string* error = nullptr);

}  // namespace daw
