#pragma once

#include "daw/midi.hpp"
#include "daw/score.hpp"

#include <string>

namespace daw {

// Convert the current score interchange model into a Standard MIDI File
// model. The alpha score is one part, so the result contains one MIDI track;
// voices and staves remain independent note events and use channel 0. Rests
// are omitted, while note start, duration, pitch, and velocity are retained.
// The score's BPM becomes the tick-zero TempoMap entry. The existing MidiFile
// model has no time-signature field, so score meter is intentionally not
// serialized by this bridge.
bool scoreToMidiFile(const Score& score, MidiFile* midi, std::string* error = nullptr);

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
