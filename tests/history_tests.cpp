#include "daw/history.hpp"

#include <cstdlib>
#include <iostream>
#include <new>
#include <string>

namespace {

thread_local bool count_allocations = false;
thread_local std::size_t allocated_bytes = 0;
thread_local long fail_after = -1;

int fail(const std::string& message) {
  std::cerr << "FAIL: " << message << '\n';
  return 1;
}

daw::Score score(char step, int octave = 4) {
  daw::Score value;
  value.parts = {daw::ScorePart{"P1", "Piano", {daw::ScoreMeasure{1, 0,
                                                                      {daw::ScoreNote{0, 960,
                                                                                      daw::ScorePitch{step, 0,
                                                                                                     octave}}}}}}};
  return value;
}

bool isScore(const daw::Score& value, char step, int octave = 4) {
  return value.parts.size() == 1U && value.parts[0].measures.size() == 1U &&
         value.parts[0].measures[0].notes.size() == 1U &&
         value.parts[0].measures[0].notes[0].pitch.step == step &&
         value.parts[0].measures[0].notes[0].pitch.octave == octave;
}

int allocationFailureGuarantees(unsigned operation) {
  bool succeeded = false;
  unsigned failures = 0;
  for (long n = 0; n < 128 && !succeeded; ++n) {
    daw::ScoreHistory history(score('C'), 3U);
    if (!history.commit(score('E')) || !history.commit(score('G'))) return fail("fault fixture");
    daw::Score output = score('X');
    // Branch commit and redo must preserve an existing redo state on failure.
    if (operation == 1 || operation == 5) {
      if (!history.undo(&output)) return fail("fault fixture undo");
      output = score('X');
    }
    auto edited = score('F');
    edited.parts[0].measures[0].notes[0].lyric.assign(128, 'f');
    const bool before_undo = history.canUndo(), before_redo = history.canRedo();
    const std::size_t before_size = history.size();
    fail_after = n;
    // Use the optional null diagnostic sink so failure reporting itself does
    // not allocate while every subsequent allocation is also being failed.
    switch (operation) {
      case 0: case 1: succeeded = history.commit(edited); break;
      case 2: succeeded = history.reset(edited); break;
      case 3: succeeded = history.clear(); break;
      case 4: succeeded = history.undo(&output); break;
      case 5: succeeded = history.redo(&output); break;
      default: succeeded = history.snapshot(&output); break;
    }
    fail_after = -1;
    if (succeeded) continue;
    ++failures;
    if (history.size() != before_size || history.canUndo() != before_undo ||
        history.canRedo() != before_redo || !isScore(output, 'X')) {
      return fail("allocation failure changed cursor, bound, branches or output");
    }
    if (!history.snapshot(&output) || !isScore(output, before_redo ? 'E' : 'G')) {
      return fail("allocation failure changed current score");
    }
    if (before_redo && (!history.redo(&output) || !isScore(output, 'G'))) {
      return fail("allocation failure lost old redo content");
    }
    if (!history.undo(&output) || !isScore(output, 'E') ||
        !history.undo(&output) || !isScore(output, 'C')) {
      return fail("allocation failure evicted old undo content");
    }
  }
  return succeeded && failures ? 0 : fail("fault injection did not cover operation");
}

std::size_t measuredCommit(daw::ScoreHistory& history, const daw::Score& edited) {
  allocated_bytes = 0;
  count_allocations = true;
  const bool ok = history.commit(edited);
  count_allocations = false;
  return ok ? allocated_bytes : 0;
}

}  // namespace

void* operator new(std::size_t n) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (count_allocations) allocated_bytes += n;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

int main() {
  using namespace daw;
  std::string error;

  ScoreHistory history(score('C'));
  if (history.size() != 1U || history.canUndo() || history.canRedo()) return fail("initial history state");
  if (!history.commit(score('E'), &error) || !history.commit(score('G'), &error)) {
    return fail("basic commits: " + error);
  }
  if (history.size() != 3U || !history.canUndo() || history.canRedo()) return fail("commit cursor state");

  Score output = score('X');
  if (!history.undo(&output, &error) || !isScore(output, 'E') || !history.canRedo()) {
    return fail("undo to middle state: " + error);
  }
  if (!history.undo(&output, &error) || !isScore(output, 'C') || history.canUndo()) {
    return fail("undo to initial state: " + error);
  }
  if (history.undo(&output, &error) || !isScore(output, 'C') || history.size() != 3U) {
    return fail("failed undo mutated output or history");
  }
  if (history.redo(nullptr, &error) || error != "redo output score is null" || !isScore(output, 'C')) {
    return fail("null redo validation");
  }
  if (!history.redo(&output, &error) || !isScore(output, 'E')) return fail("redo: " + error);

  // Committing from the middle removes the redo branch.
  if (!history.commit(score('F'), &error) || history.canRedo() || history.size() != 3U) {
    return fail("redo branch was not cleared");
  }
  if (!history.undo(&output, &error) || !isScore(output, 'E')) return fail("branch undo: " + error);
  if (!history.redo(&output, &error) || !isScore(output, 'F')) return fail("branch redo: " + error);

  // The state count is strictly bounded and retains the newest states.
  ScoreHistory bounded(score('C'), 3U);
  if (!bounded.commit(score('D'), &error) || !bounded.commit(score('E'), &error) ||
      !bounded.commit(score('F'), &error) || bounded.size() != 3U) {
    return fail("bounded commits: " + error);
  }
  if (!bounded.undo(&output, &error) || !isScore(output, 'E') || !bounded.undo(&output, &error) ||
      !isScore(output, 'D') || bounded.canUndo()) {
    return fail("bounded oldest state policy: " + error);
  }

  // A zero limit still retains exactly one current state.
  ScoreHistory one(score('A'), 0U);
  if (one.size() != 1U || !one.commit(score('B'), &error) || one.size() != 1U || one.canUndo()) {
    return fail("zero capacity normalization");
  }

  if (!history.clear(&error) || history.size() != 1U || history.canUndo() || history.canRedo()) {
    return fail("clear history: " + error);
  }
  if (!history.commit(score('H'), &error) || !history.undo(&output, &error) || !isScore(output, 'F')) {
    return fail("clear did not retain current state: " + error);
  }
  if (history.clear(nullptr) == false) return fail("second clear");

  // Immutable snapshots must not alias either the caller's editable Score or
  // output copies, even though histories can now share retained snapshots.
  auto editable = score('D');
  ScoreHistory isolated(editable);
  if (!isolated.commit(editable)) return fail("isolation commit");
  editable.parts[0].measures[0].notes[0].pitch.step = 'A';
  if (!isolated.snapshot(&output) || !isScore(output, 'D')) return fail("input aliases history");
  output.parts[0].measures[0].notes[0].pitch.step = 'B';
  auto fork = isolated;
  if (!fork.commit(score('F')) || !isolated.snapshot(&output) || !isScore(output, 'D')) {
    return fail("output or copied history mutated an immutable snapshot");
  }

  // Measured allocation volume is the regression gate, not an unstable timing
  // benchmark. Each non-SSO lyric forces copying note content to allocate.
  // Increasing retained history from 1 to 64 may add handles, never 63 copies
  // of the full score. The previous vector<Score> algorithm fails this gate.
  auto large = score('C');
  large.parts[0].measures[0].notes.resize(4096);
  for (auto& note : large.parts[0].measures[0].notes) note.lyric.assign(96, 'x');
  ScoreHistory shallow(large, 64), deep(large, 64);
  for (unsigned i = 1; i < 64; ++i) if (!deep.commit(large)) return fail("large history fixture");
  const std::size_t shallow_bytes = measuredCommit(shallow, large);
  const std::size_t deep_bytes = measuredCommit(deep, large);
  if (!shallow_bytes || !deep_bytes || deep_bytes > shallow_bytes + 4096) {
    return fail("commit copy volume grows with retained full-score snapshots");
  }
  allocated_bytes = 0;
  count_allocations = true;
  const bool cleared = deep.clear();
  count_allocations = false;
  if (!cleared || allocated_bytes > 4096) return fail("clear recopied current full score");
  std::cout << "commit allocated bytes: retained=1 " << shallow_bytes
            << ", retained=64 " << deep_bytes << '\n';

  for (unsigned operation = 0; operation < 7; ++operation) {
    if (const int result = allocationFailureGuarantees(operation)) return result;
  }

  auto identified = score('C');
  assignNoteIds(identified);
  ScoreHistory identities(identified);
  auto extension = identified;
  extension.parts[0].measures[0].notes.push_back(ScoreNote{});
  assignNoteIds(extension);
  if (!identities.commit(extension) || !identities.undo(&output) || output.next_note_id != 3) {
    return fail("shared snapshots recycled allocator on undo");
  }
  if (identities.commit(identified) || identities.commit(score('F')) || !identities.canRedo()) {
    return fail("stale or mixed identity commit discarded redo");
  }
  if (!identities.clear() || !identities.snapshot(&output) || output.next_note_id != 3) {
    return fail("clear lost allocator high water");
  }
  output.parts[0].measures[0].notes.push_back(ScoreNote{});
  assignNoteIds(output);
  if (output.parts[0].measures[0].notes.back().id != 3 || !identities.commit(output)) {
    return fail("branch reused abandoned notation ID");
  }

  std::cout << "classical-daw score history tests passed\n";
  return 0;
}
