#include "daw/history.hpp"

#include <iostream>
#include <string>

namespace {

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

}  // namespace

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

  std::cout << "classical-daw score history tests passed\n";
  return 0;
}
