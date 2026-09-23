#pragma once

#include "daw/tempo_map.hpp"
#include "daw/midi_event.hpp"

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
  std::uint8_t release_velocity = 0;
  // Source ordinals are valid for unchanged events. Editors moving/repitching
  // notes should clear these to zero; export rejects stale retrigger ordering.
  std::uint64_t on_order = 0;
  std::uint64_t off_order = 0;
  // Native score anchor only; SMF interchange does not preserve this identity.
  std::uint64_t source_note_id = 0;
  std::vector<std::uint64_t> source_notation_ids; // All tied segments, native only.

  [[nodiscard]] Tick end() const { return start + duration; }
};

struct MidiTrack {
  std::string name;
  std::vector<MidiNote> notes;
  std::vector<MidiChannelEvent> channel_events{};
};

struct MidiFile {
  std::int16_t format = 1;
  Tick ticks_per_quarter = kTicksPerQuarter;
  TimeSignature time_signature{};
  TempoMap tempo;
  std::vector<MidiTrack> tracks;
  // time_signature applies at tick zero; later entries use positive,
  // strictly increasing absolute 960-PPQ ticks. Default is 4/4, 24, 8.
  std::vector<TimeSignatureChange> meter_changes{};
};

// Boundary diagnostics are separate from project state. On a successful read,
// callers can report timing rounding and content this small reader omits.
// Neither this report nor the output file is changed on a failed import.
struct MidiImportReport {
  std::uint16_t source_ticks_per_quarter = 0;
  std::uint64_t rounded_note_boundaries = 0;
  std::uint64_t rounded_tempo_events = 0;
  std::uint64_t ignored_channel_events = 0;
  std::uint64_t preserved_channel_events = 0;
  std::uint64_t rounded_channel_events = 0;
  std::uint64_t ignored_meta_events = 0;
  std::uint64_t ignored_sysex_events = 0;
  std::uint64_t ignored_time_signature_events = 0;
  // Same channel/pitch overlaps have no note identity in SMF. The reader uses
  // its existing last-on/first-off pairing; report every ambiguous note-on.
  std::uint64_t overlapping_same_pitch_notes = 0;
  // Explicit source FF 58 messages, including ones coalesced at the same
  // normalized tick. The initial default does not count as an explicit event.
  std::uint64_t preserved_time_signature_events = 0;
  std::uint64_t rounded_time_signature_events = 0;
  std::uint64_t coalesced_time_signature_events = 0;
};

// A deliberately small Standard MIDI File (SMF) Type 0/1 reader and writer.
// It covers note/release events, all channel voice messages, track names,
// tempo events, and all four bytes of time signature events. Unknown meta and
// SysEx events are skipped; unsupported system-common events are rejected so
// malformed timing data is not hidden.
// Input accepts positive PPQ divisions (1..32767), normalizing absolute note
// starts/ends and tempo/channel/meter positions to 960 PPQ with nearest-tick
// rounding (half up). No delta rounding is accumulated. Notes that collapse to zero duration
// after normalization fail explicitly. SMPTE and Type 2 remain unsupported.
// The writer and in-memory engine model continue to require 960 PPQ.
bool writeMidiFile(const MidiFile& file, const std::string& path, std::string* error = nullptr);
bool readMidiFile(const std::string& path, MidiFile* file, std::string* error = nullptr,
                  MidiImportReport* report = nullptr);

}  // namespace daw
