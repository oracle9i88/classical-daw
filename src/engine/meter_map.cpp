#include "daw/meter_map.hpp"

#include <limits>
#include <stdexcept>

namespace daw {
namespace {

void validateSignature(const TimeSignature& signature) {
  const auto denominator = signature.denominator;
  if (signature.numerator == 0 || denominator == 0 ||
      (denominator & static_cast<std::uint8_t>(denominator - 1U)) != 0 ||
      signature.notated_32nds_per_quarter == 0) {
    throw std::invalid_argument("meter requires a positive numerator, power-of-two denominator and positive notation ratio");
  }
}

bool sameBarLengthDefinition(const TimeSignature& a, const TimeSignature& b) {
  return a.numerator == b.numerator && a.denominator == b.denominator &&
         a.notated_32nds_per_quarter == b.notated_32nds_per_quarter;
}

}  // namespace

void validateMeterMap(const TimeSignature& initial,
                      const std::vector<TimeSignatureChange>& changes) {
  validateSignature(initial);
  if (changes.size() > kMaxMeterChanges) throw std::invalid_argument("too many meter changes");
  Tick previous = 0;
  for (const auto& change : changes) {
    if (change.tick <= previous) throw std::invalid_argument("meter changes require increasing positive ticks");
    validateSignature(change.signature);
    previous = change.tick;
  }
}

Tick meterMeasureTicks(const TimeSignature& signature) {
  validateSignature(signature);
  const Tick numerator = kTicksPerQuarter * 32 * static_cast<Tick>(signature.numerator);
  const Tick denominator = static_cast<Tick>(signature.denominator) * signature.notated_32nds_per_quarter;
  if (numerator % denominator != 0) {
    throw std::invalid_argument("meter measure length is not representable by integer 960-PPQ ticks");
  }
  return numerator / denominator;
}

std::vector<MeasureSpan> makeMeasureGrid(const TimeSignature& initial,
                                        const std::vector<TimeSignatureChange>& changes,
                                        Tick end_tick, std::size_t max_measures) {
  validateMeterMap(initial, changes);
  if (end_tick < 0) throw std::invalid_argument("measure grid end cannot be negative");
  // Filter once, so distant click-only messages cannot cause rescanning the
  // full event list at every bar. All events remain in the original model.
  std::vector<TimeSignatureChange> structural;
  auto last = initial;
  (void)meterMeasureTicks(initial);
  for (const auto& change : changes) {
    (void)meterMeasureTicks(change.signature);
    if (!sameBarLengthDefinition(last, change.signature)) structural.push_back(change);
    last = change.signature;
  }
  std::vector<MeasureSpan> result;
  Tick start = 0;
  Tick length = meterMeasureTicks(initial);
  std::size_t next_change = 0;
  do {
    while (next_change < structural.size() && structural[next_change].tick == start) {
      length = meterMeasureTicks(structural[next_change].signature);
      ++next_change;
    }
    if (result.size() >= max_measures) throw std::length_error("measure grid exceeds the configured count limit");
    Tick next = 0;
    // A nearby structural change can bound a partial bar even where adding
    // the nominal full length would overflow Tick near the timeline limit.
    if (next_change < structural.size() && structural[next_change].tick - start < length) {
      next = structural[next_change].tick;
    } else {
      if (start > std::numeric_limits<Tick>::max() - length) throw std::length_error("measure grid tick overflow");
      next = start + length;
    }
    result.push_back({start, next});
    start = next;
  } while (start < end_tick);
  return result;
}

}  // namespace daw
