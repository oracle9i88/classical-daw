#pragma once

#include "daw/midi.hpp"
#include "daw/score.hpp"

#include <string>

namespace daw {

// Convert the score interchange model into a Standard MIDI File model. Each
// ordered ScorePart becomes one MIDI track; voices and staves remain independent
// note events within that track. Parts use one stable MIDI channel each
// (part index), and export rejects more than 16 parts so channels do
// not collide. Rests are omitted, while note start, duration, pitch, and
// velocity are retained. Tuplet metadata has no separate MIDI representation
// and is therefore represented by the already-resolved tick durations.
// Sounding notes require velocity 1..127; zero is MIDI note-off, not a silent
// note-on. Rests may use velocity zero because they emit no MIDI event.
// Exactly contiguous ties with the same part/staff/voice and sounding pitch
// become one MIDI note with the first segment's attack velocity. Dangling,
// discontinuous, ambiguous, or rest-attached ties fail without changing the
// caller's MIDI model; they are never silently exported as repeated attacks.
// The score's BPM becomes the tick-zero TempoMap entry.
// The score's single time signature is serialized as the first MIDI meter
// event; meter changes and other notation maps remain outside this bridge.
bool scoreToMidiFile(const Score& score, MidiFile* midi, std::string* error = nullptr);

// Convert a Standard MIDI File model into the score interchange model. Each
// ordered MIDI track becomes one ScorePart. Track names are retained; unnamed
// tracks receive deterministic "Part N" names. MIDI channels become score
// voices (channel + 1) and notes use canonical sharp spellings. The import
// bridge deliberately accepts the engine's fixed 960-PPQ domain, carries the
// first MIDI meter event (defaulting to 4/4), and uses the first valid tempo as
// Score::bpm. Notes crossing barlines are split into tied score segments while
// preserving their total sounding duration and velocity. Import limits both
// the number of measures and resulting segments per part to one million.
// Sounding notes require velocity 1..127. Same-channel same-pitch overlapping
// notes within a track are rejected: this model has no independent tie
// identity for them. Adjacent reattacks and different-channel overlaps remain
// valid. Failures leave the caller's score unchanged.
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
