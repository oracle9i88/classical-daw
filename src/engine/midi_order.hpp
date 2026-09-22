#pragma once

#include <cstdint>

namespace daw::detail {

// Caller compares tick (and track, when merging) first. Priority is metadata
// -1, note-off 0, channel event 1, note-on 2. Stable ties retain vector order.
inline bool midiEventBefore(int a_priority, std::uint64_t a_order,
                            int b_priority, std::uint64_t b_order) {
  if ((a_priority < 0) != (b_priority < 0)) return a_priority < 0;
  if (a_priority < 0) return false;
  const bool a_new_release = a_order == 0 && a_priority == 0;
  const bool b_new_release = b_order == 0 && b_priority == 0;
  if (a_new_release != b_new_release) return a_new_release;
  if ((a_order == 0) != (b_order == 0)) return a_order != 0;
  if (a_order != b_order) return a_order < b_order;
  return a_priority < b_priority;
}

}  // namespace daw::detail
