#include "daw/timeline.hpp"

#include <cmath>
#include <stdexcept>

namespace daw {

Timeline::Timeline(double sample_rate, TempoMap tempo)
    : sample_rate_(sample_rate), tempo_(std::move(tempo)) {
  if (!std::isfinite(sample_rate) || sample_rate <= 0.0) {
    throw std::invalid_argument("sample rate must be a finite positive value");
  }
}

SampleIndex Timeline::tickToSample(Tick tick) const {
  return static_cast<SampleIndex>(std::llround(tickToSeconds(tick) * sample_rate_));
}

Tick Timeline::sampleToTick(SampleIndex sample) const {
  return secondsToTick(static_cast<double>(sample) / sample_rate_);
}

}  // namespace daw
