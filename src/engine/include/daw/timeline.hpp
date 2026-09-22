#pragma once

#include "daw/tempo_map.hpp"

namespace daw {

class Timeline {
 public:
  Timeline(double sample_rate, TempoMap tempo = TempoMap{});

  [[nodiscard]] double sampleRate() const { return sample_rate_; }
  [[nodiscard]] double tickToSeconds(Tick tick) const { return tempo_.tickToSeconds(tick); }
  [[nodiscard]] Tick secondsToTick(double seconds) const { return tempo_.secondsToTick(seconds); }
  [[nodiscard]] SampleIndex tickToSample(Tick tick) const;
  [[nodiscard]] Tick sampleToTick(SampleIndex sample) const;
  [[nodiscard]] const TempoMap& tempo() const { return tempo_; }

 private:
  double sample_rate_;
  TempoMap tempo_;
};

}  // namespace daw
