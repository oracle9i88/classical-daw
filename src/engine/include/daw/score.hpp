#pragma once

#include "daw/types.hpp"
#include "daw/midi_event.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace daw {

struct ScorePitch {
  char step = 'C';
  int alter = 0;
  int octave = 4;
};

struct ScoreNote {
  Tick start = 0;
  Tick duration = kTicksPerQuarter;
  ScorePitch pitch{};
  bool rest = false;
  bool chord = false;
  bool tie_start = false;
  bool tie_stop = false;
  std::uint8_t velocity = 100;
  std::uint16_t voice = 1;
  std::uint16_t staff = 1;
  std::uint16_t tuplet_actual = 0;
  std::uint16_t tuplet_normal = 0;
  // Optional single lyric syllable attached to this note.  The current
  // interchange slice keeps one <lyric><text> value per note; richer
  // syllabic/verse metadata can be added without changing note timing.
  std::string lyric;
  // Imported playback identity survives Score/project edits. -1 uses the
  // score exporter's default part channel; 0..15 retains an explicit channel.
  std::int16_t midi_channel = -1;
  // Clear ordinals when moving/repitching an imported note or changing its
  // endpoints, so newly authored event ordering is used at the edited boundary.
  std::uint64_t midi_on_order = 0;
  std::uint64_t midi_off_order = 0;
  std::uint8_t midi_release_velocity = 0;
};

struct ScoreMeasure {
  int number = 1;
  Tick start = 0;
  std::vector<ScoreNote> notes;
};

struct ScorePart {
  std::string id = "P1";
  std::string name = "Part 1";
  std::vector<ScoreMeasure> measures;
  std::vector<MidiChannelEvent> midi_events{};
};

// Internal score state uses the engine's fixed 960 PPQ tick domain. MusicXML
// is an interchange format at the boundary; it is not the realtime state.
struct Score {
  Tick divisions = kTicksPerQuarter;
  TimeSignature time_signature{};
  double bpm = 120.0;
  std::vector<ScorePart> parts;
};

// This first vertical slice supports ordered parts with multiple voices/staves.
// It preserves pitch, integer duration, rests, chords, ties, tuplets, meter,
// tempo, and MIDI velocity via the standard note dynamics percentage.
// Full engraving metadata remains outside this model.
struct MusicXmlExportReport {
  // The native project and SMF bridge retain these fields; the current
  // MusicXML notation slice omits them. Unchanged on failed export.
  std::uint64_t omitted_midi_events = 0;
  std::uint64_t omitted_note_midi_metadata = 0;
};
bool writeMusicXmlFile(const Score& score, const std::string& path, std::string* error = nullptr,
                       MusicXmlExportReport* report = nullptr);
bool readMusicXmlFile(const std::string& path, Score* score, std::string* error = nullptr);

}  // namespace daw
