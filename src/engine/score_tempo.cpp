#include "daw/score.hpp"

#include <cmath>
#include <stdexcept>

namespace daw {

TempoMap scoreTempoMap(const Score& score) {
  const auto validBpm = [](double bpm) {
    return std::isfinite(bpm) && bpm > 0.0 && bpm <= 1'000'000.0;
  };
  if (!validBpm(score.bpm)) throw std::invalid_argument("score initial tempo is out of range");
  if (score.tempo_changes.size() > kMaxScoreTempoChanges) {
    throw std::invalid_argument("score has too many tempo changes");
  }
  // Validate the whole sequence before constructing another allocation.
  Tick previous = 0;
  for (const auto& change : score.tempo_changes) {
    if (change.tick <= previous || !validBpm(change.bpm)) {
      throw std::invalid_argument("score tempo changes require increasing positive ticks and finite positive BPM within range");
    }
    previous = change.tick;
  }
  TempoMap result(score.bpm);
  for (const auto& change : score.tempo_changes) result.addChange(change.tick, change.bpm);
  return result;
}

}  // namespace daw
