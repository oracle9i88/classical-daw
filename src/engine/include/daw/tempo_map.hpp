#pragma once

#include "daw/types.hpp"

#include <vector>

namespace daw {

struct TempoChange {
  Tick tick = 0;
  double bpm = 120.0;
};

// Piecewise-constant tempo map. A map always has a tempo at tick zero.
class TempoMap {
 public:
  explicit TempoMap(double initial_bpm = 120.0);

  void addChange(Tick tick, double bpm);
  [[nodiscard]] double tickToSeconds(Tick tick) const;
  [[nodiscard]] Tick secondsToTick(double seconds) const;
  [[nodiscard]] const std::vector<TempoChange>& changes() const { return changes_; }

 private:
  std::vector<TempoChange> changes_;
};

}  // namespace daw
