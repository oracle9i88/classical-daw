#pragma once

#include "daw/midi.hpp"
#include "daw/wav.hpp"

namespace daw {

// Renders a deterministic, intentionally simple sine instrument. This is an
// offline engine smoke test, not a finished sampler or orchestral instrument.
AudioBuffer renderNotes(const MidiTrack& track, const TempoMap& tempo,
                        double sample_rate = 48000.0, double tail_seconds = 0.1);

}  // namespace daw
