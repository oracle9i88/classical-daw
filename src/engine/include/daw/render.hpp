#pragma once

#include "daw/midi.hpp"
#include "daw/score.hpp"
#include "daw/wav.hpp"

namespace daw {

// Renders a deterministic, intentionally simple sine instrument. This is an
// offline engine smoke test, not a finished sampler or orchestral instrument.
// Channel events are retained for interchange but are not interpreted here:
// CC/pedal, program changes, pressure, bend, and release velocity have no effect.
AudioBuffer renderNotes(const MidiTrack& track, const TempoMap& tempo,
                        double sample_rate = 48000.0, double tail_seconds = 0.1);

// Convert the score through the strict score-to-MIDI bridge and render every
// part into one deterministic mono diagnostic mix. This uses the sine
// renderer behind renderNotes, not a production sampler or orchestral
// instrument. Returning by value gives callers a strong failure guarantee:
// invalid input throws before caller-owned audio state is changed.
AudioBuffer renderScore(const Score& score, double sample_rate = 48000.0,
                        double tail_seconds = 0.1);

}  // namespace daw
