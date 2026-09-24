#pragma once

#include "daw/midi.hpp"
#include "daw/score.hpp"

#include <string>

namespace daw {

enum class ScoreMidiChannelPolicy {
  SharedOutput,
  // Only for hosts that send each part to a separate instrument instance.
  // Unspecified channels use part_index % 16 (preserving the first 16 defaults).
  // Explicit channels and all controller bytes remain unchanged. Up to 64 parts.
  // The result must be split by track before playback; it is NOT a multi-port SMF.
  IndependentParts
};

// Convert the score interchange model into a Standard MIDI File model. Each
// ordered ScorePart becomes one MIDI track; voices and staves remain independent
// note events within that track. Imported notes retain their explicit MIDI
// channels. Under the default SharedOutput policy, newly authored notes use the
// part index; more than 16 parts require explicit channels for every sounding
// note and some routed playback content. IndependentParts is for isolated hosts.
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
// The initial time signature and every later meter change retain all four
// SMF fields (numerator, denominator, clocks per click, and notation ratio).
bool scoreToMidiFile(const Score& score, MidiFile* midi, std::string* error = nullptr,
                     ScoreMidiChannelPolicy policy = ScoreMidiChannelPolicy::SharedOutput);

// Convert a Standard MIDI File model into the score interchange model. Each
// ordered non-empty MIDI track becomes one ScorePart. Tracks containing only
// channel events are retained with one empty measure; only tracks with neither
// notes nor channel events are omitted. Track names are retained; unnamed
// tracks receive deterministic "Part N" names. MIDI channels become score
// voices (channel + 1), their playback routes remain explicit, and notes use
// canonical sharp spellings. Channel event bytes and source order are retained.
// The import bridge deliberately accepts the engine's fixed 960-PPQ domain, carries the
// initial MIDI meter (defaulting to 4/4) plus all later changes, and retains the initial tempo as
// Score::bpm plus every later entry in Score::tempo_changes. Notes crossing
// variable barlines are split into tied score segments while
// preserving their total sounding duration and velocity. The first segment
// carries the source note-on order, the last carries the note-off order and
// release velocity, and all segments retain their original MIDI channel.
// Structural meter changes start a new bar, closing a partial old bar when
// needed. Repeated signatures and click-only changes do not restart bars.
// The notation ratio affects bar length; fractional 960-PPQ lengths fail
// rather than round the score's measure positions.
// Import limits measures and resulting segments per part to one million each.
// Sounding notes require velocity 1..127. Same-channel same-pitch overlapping
// notes within a track are rejected: this model has no independent tie
// identity for them. Adjacent reattacks and different-channel overlaps remain
// valid. Failures leave the caller's score unchanged.
// A recorded or exported MIDI track routinely sounds one pitch twice on one
// channel before the first release. One channel becomes one score voice here,
// which cannot hold that, so the conversion refuses it. Passing a repair report
// asks it to shorten the earlier note instead, and to say how often it did.
bool midiToScore(const MidiFile& midi, Score* score, std::string* error = nullptr,
                 ScoreRepairReport* report = nullptr);

// Read an SMF and then convert it to the score model without exposing a
// partially parsed score when either operation fails.
bool readMidiScoreFile(const std::string& path, Score* score, std::string* error = nullptr,
                       ScoreRepairReport* report = nullptr);

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
