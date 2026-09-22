#pragma once

#include "daw/midi.hpp"
#include "daw/score.hpp"

#include <string>

namespace daw {

// Convert the score interchange model into a Standard MIDI File model. Each
// ordered ScorePart becomes one MIDI track; voices and staves remain independent
// note events within that track. Imported notes retain their explicit MIDI
// channels; newly authored notes use the part index. More than 16 parts require
// explicit channels for every sounding note and some routed playback content.
// Rests are omitted, while note start, duration, pitch, attack/release velocity,
// source event order, and part MIDI channel events are retained. Tuplet metadata
// has no separate MIDI representation and uses the resolved tick durations.
// Sounding notes require velocity 1..127; zero is MIDI note-off, not a silent
// note-on. Rests may use velocity zero because they emit no MIDI event.
// Exactly contiguous ties with the same part/staff/voice, sounding pitch, and
// output channel become one MIDI note with the first segment's attack velocity
// and source order, and the last segment's release velocity and source order.
// Dangling, discontinuous, ambiguous, or rest-attached ties fail without changing the
// caller's MIDI model; they are never silently exported as repeated attacks.
// The score's BPM becomes the tick-zero TempoMap entry; all later absolute
// tempo_changes are validated and retained, including changes during ties.
// The score's single time signature is serialized as the first MIDI meter
// event; meter changes and other notation maps remain outside this bridge.
bool scoreToMidiFile(const Score& score, MidiFile* midi, std::string* error = nullptr);

// Convert a Standard MIDI File model into the score interchange model. Each
// ordered non-empty MIDI track becomes one ScorePart. Tracks containing only
// channel events are retained with one empty measure; only tracks with neither
// notes nor channel events are omitted. Track names are retained; unnamed
// tracks receive deterministic "Part N" names. MIDI channels become score
// voices (channel + 1), their playback routes remain explicit, and notes use
// canonical sharp spellings. Channel event bytes and source order are retained.
// The import bridge deliberately accepts the engine's fixed 960-PPQ domain, carries the
// first MIDI meter event (defaulting to 4/4), and retains the initial tempo as
// Score::bpm plus every later entry in Score::tempo_changes. Notes crossing
// barlines are split into tied score segments while
// preserving their total sounding duration and velocity. The first segment
// carries the source note-on order, the last carries the note-off order and
// release velocity, and all segments retain their original MIDI channel.
// Import limits measures and resulting segments per part to one million each.
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
