#include "daw/tempo_map.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace daw {
namespace {

double secondsPerTick(double bpm) {
  return 60.0 / (bpm * static_cast<double>(kTicksPerQuarter));
}

void validateBpm(double bpm) {
  if (!std::isfinite(bpm) || bpm <= 0.0) {
    throw std::invalid_argument("tempo must be a finite positive BPM value");
  }
}

}  // namespace

TempoMap::TempoMap(double initial_bpm) {
  validateBpm(initial_bpm);
  changes_.push_back({0, initial_bpm});
}

void TempoMap::addChange(Tick tick, double bpm) {
  if (tick < 0) {
    throw std::invalid_argument("tempo changes cannot be placed before tick zero");
  }
  validateBpm(bpm);

  auto it = std::lower_bound(changes_.begin(), changes_.end(), tick,
                             [](const TempoChange& change, Tick value) {
                               return change.tick < value;
                             });
  if (it != changes_.end() && it->tick == tick) {
    it->bpm = bpm;
  } else {
    changes_.insert(it, {tick, bpm});
  }
}

double TempoMap::tickToSeconds(Tick tick) const {
  if (tick <= 0) {
    return static_cast<double>(tick) * secondsPerTick(changes_.front().bpm);
  }

  double seconds = 0.0;
  for (std::size_t i = 0; i < changes_.size(); ++i) {
    const Tick segment_start = changes_[i].tick;
    const Tick segment_end = (i + 1 < changes_.size()) ? changes_[i + 1].tick : tick;
    if (tick <= segment_start) {
      break;
    }
    const Tick segment_ticks = std::min(tick, segment_end) - segment_start;
    if (segment_ticks > 0) {
      seconds += static_cast<double>(segment_ticks) * secondsPerTick(changes_[i].bpm);
    }
    if (tick <= segment_end) {
      break;
    }
  }
  return seconds;
}

Tick TempoMap::secondsToTick(double seconds) const {
  if (!std::isfinite(seconds)) {
    throw std::invalid_argument("seconds must be finite");
  }
  if (seconds <= 0.0) {
    return static_cast<Tick>(std::llround(seconds / secondsPerTick(changes_.front().bpm)));
  }

  double elapsed = 0.0;
  for (std::size_t i = 0; i < changes_.size(); ++i) {
    const bool last = (i + 1 == changes_.size());
    const Tick segment_ticks = last ? std::numeric_limits<Tick>::max()
                                    : changes_[i + 1].tick - changes_[i].tick;
    const double segment_seconds =
        last ? std::numeric_limits<double>::infinity()
             : static_cast<double>(segment_ticks) * secondsPerTick(changes_[i].bpm);
    if (seconds <= elapsed + segment_seconds) {
      const double offset = (seconds - elapsed) / secondsPerTick(changes_[i].bpm);
      return changes_[i].tick + static_cast<Tick>(std::llround(offset));
    }
    elapsed += segment_seconds;
  }
  return changes_.back().tick;
}

}  // namespace daw
