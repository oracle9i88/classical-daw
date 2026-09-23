#pragma once

#include "daw/score.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace daw {

// UI/control-thread history for editable Score state.  The history owns
// immutable snapshots of scores and is deliberately separate from the realtime
// audio path; callers must not use it from an audio callback or other hard
// realtime thread.
class ScoreHistory {
 public:
  // The initial score starts the history; oldest states are evicted at the
  // configured bound. A zero limit is
  // normalized to one so the history remains usable and strictly bounded.
  explicit ScoreHistory(Score initial, std::size_t max_states = 64U);

  // Commit a new editable state.  A successful commit discards the redo
  // branch. Only the incoming Score is deeply copied; retaining old states
  // copies snapshot handles, not their notes. With N score data and K retained
  // states, copied/allocated data is O(N + K), while snapshot storage remains
  // O(N * K). Reclaiming discarded redo snapshots still visits their owned data.
  // This compatibility API is not the WorkEditor's incremental command stack.
  // On failure, including allocation failure, the history is left unchanged.
  bool commit(const Score& score, std::string* error = nullptr);

  // Move the current position and copy the selected state into score.  A
  // failed operation leaves both the output score and history position alone.
  bool undo(Score* score, std::string* error = nullptr);
  bool redo(Score* score, std::string* error = nullptr);

  // Copy the current editable state for persistence or UI inspection.  The
  // history itself is unchanged; this method is control-thread only and must
  // never be called from a realtime callback.
  bool snapshot(Score* score, std::string* error = nullptr) const;

  // Replace the history with one current state while retaining its configured
  // bound.  This is used after opening a project or recovery sidecar: the
  // loaded document becomes the new clean baseline and no stale undo/redo
  // branch survives the load.
  bool reset(const Score& score, std::string* error = nullptr);

  bool canUndo() const noexcept;
  bool canRedo() const noexcept;
  std::size_t size() const noexcept;

  // Drop undo/redo entries while retaining the current score as the sole
  // state.  This is useful after saving or opening a project.  The boolean
  // result reports allocation failure; a failed clear leaves history intact.
  bool clear(std::string* error = nullptr);

 private:
  std::vector<std::shared_ptr<const Score>> states_;
  std::size_t cursor_ = 0U;
  std::size_t max_states_ = 1U;
  std::uint64_t note_id_high_water_ = 0;
};

}  // namespace daw
