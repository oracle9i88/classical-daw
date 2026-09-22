#include "daw/history.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace daw {
namespace {

bool fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

void clearError(std::string* error) {
  if (error != nullptr) error->clear();
}

// Score's vector members are swapped rather than assigned so replacing an
// output score is non-throwing after the source copy has succeeded.
void replaceScore(Score* destination, Score&& source) noexcept {
  destination->divisions = source.divisions;
  destination->time_signature = source.time_signature;
  destination->bpm = source.bpm;
  destination->parts.swap(source.parts);
}

bool reportException(std::string* error, const char* operation) {
  try {
    throw;
  } catch (const std::exception& exception) {
    if (error != nullptr) *error = std::string(operation) + ": " + exception.what();
  } catch (...) {
    if (error != nullptr) *error = std::string(operation) + ": unknown failure";
  }
  return false;
}

}  // namespace

ScoreHistory::ScoreHistory(Score initial, std::size_t max_states)
    : states_{std::move(initial)}, max_states_(std::max<std::size_t>(1U, max_states)) {}

bool ScoreHistory::commit(const Score& score, std::string* error) {
  clearError(error);
  try {
    // Build the complete replacement first.  This gives commit a strong
    // exception guarantee and makes redo-branch removal atomic from the
    // caller's perspective.
    std::vector<Score> next;
    const std::size_t retained = cursor_ + 1U;
    next.reserve(std::min(max_states_, retained + 1U));
    next.insert(next.end(), states_.begin(), states_.begin() + static_cast<std::ptrdiff_t>(retained));
    next.push_back(score);
    if (next.size() > max_states_) {
      const std::size_t drop = next.size() - max_states_;
      next.erase(next.begin(), next.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    states_.swap(next);
    cursor_ = states_.size() - 1U;
    return true;
  } catch (...) {
    return reportException(error, "commit failed");
  }
}

bool ScoreHistory::undo(Score* score, std::string* error) {
  clearError(error);
  if (score == nullptr) return fail(error, "undo output score is null");
  if (!canUndo()) return fail(error, "no undo state available");
  try {
    Score previous = states_[cursor_ - 1U];
    replaceScore(score, std::move(previous));
    --cursor_;
    return true;
  } catch (...) {
    return reportException(error, "undo failed");
  }
}

bool ScoreHistory::redo(Score* score, std::string* error) {
  clearError(error);
  if (score == nullptr) return fail(error, "redo output score is null");
  if (!canRedo()) return fail(error, "no redo state available");
  try {
    Score next = states_[cursor_ + 1U];
    replaceScore(score, std::move(next));
    ++cursor_;
    return true;
  } catch (...) {
    return reportException(error, "redo failed");
  }
}

bool ScoreHistory::snapshot(Score* score, std::string* error) const {
  clearError(error);
  if (score == nullptr) return fail(error, "snapshot output score is null");
  try {
    // Copy before touching the destination so allocation failure leaves both
    // the caller's output and this history unchanged.
    Score copy = states_[cursor_];
    replaceScore(score, std::move(copy));
    return true;
  } catch (...) {
    return reportException(error, "snapshot failed");
  }
}

bool ScoreHistory::reset(const Score& score, std::string* error) {
  clearError(error);
  try {
    // Build a replacement vector first.  Swapping only after the copy has
    // succeeded gives reset a strong exception guarantee.
    std::vector<Score> next;
    next.reserve(1U);
    next.push_back(score);
    states_.swap(next);
    cursor_ = 0U;
    return true;
  } catch (...) {
    return reportException(error, "reset failed");
  }
}

bool ScoreHistory::canUndo() const noexcept { return cursor_ > 0U; }

bool ScoreHistory::canRedo() const noexcept { return cursor_ + 1U < states_.size(); }

std::size_t ScoreHistory::size() const noexcept { return states_.size(); }

bool ScoreHistory::clear(std::string* error) {
  clearError(error);
  try {
    std::vector<Score> next;
    next.reserve(1U);
    next.push_back(states_[cursor_]);
    states_.swap(next);
    cursor_ = 0U;
    return true;
  } catch (...) {
    return reportException(error, "clear failed");
  }
}

}  // namespace daw
