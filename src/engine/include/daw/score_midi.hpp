#pragma once

#include "daw/midi.hpp"
#include "daw/score.hpp"

#include <string>

namespace daw {

// Convert the score interchange model into a Standard MIDI File model. Each
// ordered ScorePart becomes one MIDI track; voices and staves remain independent
// note events within that track. Parts use one stable MIDI channel each
// (part index modulo 16), and export rejects more than 16 parts so channels do
// not collide. Rests are omitted, while note start, duration, pitch, and
// velocity are retained. Tuplet metadata has no separate MIDI representation
// and is therefore represented by the already-resolved tick durations.
// The score's BPM becomes the tick-zero TempoMap entry. The existing MidiFile
// model has no time-signature field, so score meter is intentionally not
// serialized by this bridge.
bool scoreToMidiFile(const Score& score, MidiFile* midi, std::string* error = nullptr);

// Convert a Standard MIDI File model into the score interchange model. Each
// ordered MIDI track becomes one ScorePart. Track names are retained; unnamed
// tracks receive deterministic "Part N" names. MIDI channels become score
// voices (channel + 1) and notes use canonical sharp spellings. The import
// bridge deliberately accepts the engine's fixed 960-PPQ domain, uses the
// first valid tempo as Score::bpm, and supplies a default 4/4 meter.
bool midiToScore(const MidiFile& midi, Score* score, std::string* error = nullptr);

// Read an SMF and then convert it to the score model without exposing a
// partially parsed score when either operation fails.
bool readMidiScoreFile(const std::string& path, Score* score, std::string* error = nullptr);

// Convert first and then write through the existing SMF writer. Conversion is
// completed before touching the destination, and writing uses a temporary
// sibling file so conversion/write failures do not leave a partial result at
// the requested path.
bool writeScoreMidiFile(const Score& score, const std::string& path, std::string* error = nullptr);

// Descriptive aliases for callers that prefer the verb "convert" or an
// explicit "standard MIDI" name.
inline bool convertScoreToMidiFile(const Score& score, MidiFile* midi, std::string* error = nullptr) {
  return scoreToMidiFile(score, midi, error);
}

inline bool writeScoreAsMidi(const Score& score, const std::string& path, std::string* error = nullptr) {
  return writeScoreMidiFile(score, path, error);
}

}  // namespace daw
