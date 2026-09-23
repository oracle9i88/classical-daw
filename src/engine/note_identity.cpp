#include "daw/score.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace daw {
void validateNoteIds(const Score& score) {
  std::unordered_set<std::uint64_t> ids;
  for (const auto& p : score.parts) for (const auto& m : p.measures) for (const auto& n : m.notes) {
    if (!score.next_note_id) {
      if (n.id) throw std::invalid_argument("note ID requires an allocator high water");
    } else if (!n.id || n.id >= score.next_note_id || !ids.insert(n.id).second)
      throw std::invalid_argument("zero, duplicate or out-of-range note ID");
  }
}
void assignNoteIds(Score& score) {
  Score next = score;
  std::unordered_set<std::uint64_t> ids;
  std::uint64_t cursor = std::max<std::uint64_t>(1, next.next_note_id);
  for (const auto& p : next.parts) for (const auto& m : p.measures) for (const auto& n : m.notes) if (n.id) {
    if (n.id == std::numeric_limits<std::uint64_t>::max() || !ids.insert(n.id).second)
      throw std::invalid_argument("duplicate or exhausted note identity");
    cursor = std::max(cursor, n.id+1);
  }
  for (auto& p : next.parts) for (auto& m : p.measures) for (auto& n : m.notes) if (!n.id) {
    if (cursor == std::numeric_limits<std::uint64_t>::max()) throw std::length_error("note IDs exhausted");
    n.id = cursor++;
  }
  next.next_note_id = cursor;
  score = std::move(next);
}
}
