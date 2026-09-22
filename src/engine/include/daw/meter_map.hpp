#pragma once

#include "daw/types.hpp"

#include <cstddef>
#include <vector>

namespace daw {

inline constexpr std::size_t kMaxMeterChanges = 1'000'000;

// Raw meter validation: positive numerator, power-of-two denominator, nonzero
// notation ratio, any metronome-click byte, sorted unique positive changes.
// It does not require a meter change to fall on an existing barline.
void validateMeterMap(const TimeSignature& initial,
                      const std::vector<TimeSignatureChange>& changes);

// Uses all notation-ratio fields: 960 * 32 * numerator / (denominator * bb).
// Fractional engine-tick measures are explicitly unsupported in Score grids.
Tick meterMeasureTicks(const TimeSignature& signature);

struct MeasureSpan {
  Tick start = 0;
  Tick end = 0;
};

// Covers [0,end_tick), with at least one measure for an empty score. Each
// numerator/denominator/notation-ratio change closes the old bar at its exact
// tick and begins a new one; the old bar may be partial. Redundant or click-
// only changes never move barlines. Final span may extend past end_tick.
// All maps and arithmetic are validated; count is bounded before each append.
std::vector<MeasureSpan> makeMeasureGrid(const TimeSignature& initial,
                                        const std::vector<TimeSignatureChange>& changes,
                                        Tick end_tick, std::size_t max_measures = 1'000'000);

}  // namespace daw
