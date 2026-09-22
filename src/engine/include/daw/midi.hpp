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
  std::uint8_t velocity = 100;  // Attack velocity 1..127; zero encodes note-off in SMF.
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

// Boundary diagnostics are separate from project state. On a successful read,
// callers can report timing rounding and content this small reader omits.
// Neither this report nor the output file is changed on a failed import.
struct MidiImportReport {
  std::uint16_t source_ticks_per_quarter = 0;
  std::uint64_t rounded_note_boundaries = 0;
  std::uint64_t rounded_tempo_events = 0;
  std::uint64_t ignored_channel_events = 0;
  std::uint64_t ignored_meta_events = 0;
  std::uint64_t ignored_sysex_events = 0;
  std::uint64_t ignored_time_signature_events = 0;
  // Same channel/pitch overlaps have no note identity in SMF. The reader uses
  // its existing last-on/first-off pairing; report every ambiguous note-on.
  std::uint64_t overlapping_same_pitch_notes = 0;
};

// A deliberately small Standard MIDI File (SMF) Type 0/1 reader and writer.
// It covers note events, track names, tempo events, and the first time
// signature event. Unknown meta and SysEx events are skipped; unsupported
// system-common events are rejected so malformed timing data is not hidden.
// Input accepts positive PPQ divisions (1..32767), normalizing absolute note
// starts/ends and tempo positions to 960 PPQ with nearest-tick rounding (half
// up). No delta rounding is accumulated. Notes that collapse to zero duration
// after normalization fail explicitly. SMPTE and Type 2 remain unsupported.
// The writer and in-memory engine model continue to require 960 PPQ.
bool writeMidiFile(const MidiFile& file, const std::string& path, std::string* error = nullptr);
bool readMidiFile(const std::string& path, MidiFile* file, std::string* error = nullptr,
                  MidiImportReport* report = nullptr);

}  // namespace daw
