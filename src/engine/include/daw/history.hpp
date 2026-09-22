#pragma once

#include "daw/score.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace daw {

// UI/control-thread history for editable Score state.  The history owns
// copies of scores and is deliberately separate from the realtime audio
// path; callers must not use it from an audio callback or other hard realtime
// thread.
class ScoreHistory {
 public:
  // The initial score is always retained as the first state.  A zero limit is
  // normalized to one so the history remains usable and strictly bounded.
  explicit ScoreHistory(Score initial, std::size_t max_states = 64U);

  // Commit a new editable state.  A successful commit discards the redo
  // branch.  On failure, including allocation failure, the history is left
  // unchanged.
  bool commit(const Score& score, std::string* error = nullptr);

  // Move the current position and copy the selected state into score.  A
  // failed operation leaves both the output score and history position alone.
  bool undo(Score* score, std::string* error = nullptr);
  bool redo(Score* score, std::string* error = nullptr);

  bool canUndo() const noexcept;
  bool canRedo() const noexcept;
  std::size_t size() const noexcept;

  // Drop undo/redo entries while retaining the current score as the sole
  // state.  This is useful after saving or opening a project.  The boolean
  // result reports allocation failure; a failed clear leaves history intact.
  bool clear(std::string* error = nullptr);

 private:
  std::vector<Score> states_;
  std::size_t cursor_ = 0U;
  std::size_t max_states_ = 1U;
};

}  // namespace daw
