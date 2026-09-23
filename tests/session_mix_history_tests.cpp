#include "daw/session_mix.hpp"
#include "daw/session_player.hpp"
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
thread_local long fail_after = -1;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template <typename F> void rejects(F f) {
  bool failed = false; try { f(); } catch (const std::exception&) { failed = true; }
  require(failed, "invalid history operation accepted");
}
daw::Session source() {
  return {"score.dawproj", 0, {{"piano", "pianoteq", 0, 0, "", ""}, {"cello", "swam-cello", 0, 0, "", ""}}};
}
std::vector<daw::AudioBuffer> audio() {
  return {{48000, 2, std::vector<float>(65536, .1F)}, {48000, 2, std::vector<float>(65536, .2F)}};
}
void level(daw::SessionPlayer& player, double left, double right) {
  std::array<float, 512> block{};
  player.render(block.data(), 256);
  require(std::abs(block[510] - left) < 1e-6 && std::abs(block[511] - right) < 1e-6,
          "history changed document but not audible target");
}
void failureGuarantees(int operation) {
  using P = daw::MixParameter;
  unsigned failures = 0;
  bool succeeded = false;
  for (long n = 0; n < 128 && !succeeded; ++n) {
    daw::SessionMixState state(source());
    state.apply({P::Gain, "piano", -6}); state.apply({P::Master, "", -3});
    if (operation == 2) require(state.undo(), "redo fixture preparation");
    daw::SessionPlayer player(state.current(), audio());
    const auto before = daw::serializeSession(state.current());
    const auto revision = state.revision();
    const auto size = state.historySize();
    const bool undo = state.canUndo(), redo = state.canRedo();
    bool failed = false;
    fail_after = n;
    try {
      succeeded = operation == 0 ? state.apply({P::Balance, "cello", -1}, &player) :
                  operation == 1 ? state.undo(&player) : state.redo(&player);
    } catch (const std::bad_alloc&) { failed = true; }
    fail_after = -1;
    if (!failed) continue;
    ++failures;
    require(daw::serializeSession(state.current()) == before && state.revision() == revision &&
        state.historySize() == size && state.canUndo() == undo && state.canRedo() == redo,
        "allocation failure changed mix/history/revision");
    require(player.enqueue({daw::PlaybackAction::Play}), "allocation failure blocked queue");
    const double expected = (.1 * std::pow(10., -6. / 20.) + .2) * (operation == 2 ? 1 : std::pow(10., -3. / 20.));
    level(player, expected, expected); // No command escaped before the failure.
  }
  require(succeeded && failures > 0, "allocation failure injection did not cover operation");
}
}
void* operator new(std::size_t n) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

int main() {
  using P = daw::MixParameter;
  try {
    daw::SessionMixState state(source());
    daw::SessionPlayer player(source(), audio());
    require(!state.undo(&player) && !state.redo(&player) && state.revision() == 0, "empty history changed state");
    require(player.enqueue({daw::PlaybackAction::Play}), "play rejected");
    level(player, .3, .3);
    const std::array<daw::MixEdit, 5> edits{{{P::Solo, "cello", 1}, {P::Gain, "cello", -20 * std::log10(2.)},
        {P::Balance, "cello", -1}, {P::Master, "", -20 * std::log10(2.)}, {P::Mute, "cello", 1}}};
    const std::array<std::array<double, 2>, 6> levels{{{.3,.3}, {.2,.2}, {.1,.1}, {.1,0}, {.05,0}, {0,0}}};
    std::vector<std::string> snapshots{daw::serializeSession(state.current())};
    for (std::size_t i = 0; i < edits.size(); ++i) {
      require(state.apply(edits[i], &player), "edit rejected"); level(player, levels[i+1][0], levels[i+1][1]);
      snapshots.push_back(daw::serializeSession(state.current()));
    }
    for (std::size_t i = edits.size(); i > 0; --i) {
      const auto frame = player.status().frame;
      require(state.undo(&player), "undo rejected"); level(player, levels[i-1][0], levels[i-1][1]);
      require(daw::serializeSession(state.current()) == snapshots[i-1], "undo did not restore exact saved fields");
      require(player.status().playing && player.status().frame == frame + 256, "mix undo moved transport");
    }
    for (std::size_t i = 1; i < snapshots.size(); ++i) {
      require(state.redo(&player), "redo rejected"); level(player, levels[i][0], levels[i][1]);
      require(daw::serializeSession(state.current()) == snapshots[i], "redo did not restore exact saved fields");
    }
    require(state.revision() == 15 && state.historySize() == 5, "revision/history accounting");
    require(state.undo(&player), "branch undo failed"); level(player, .05, 0);
    const auto rev = state.revision();
    require(state.apply({P::Mute, "cello", 0}, &player) && state.revision() == rev && state.canRedo(), "no-op destroyed redo");
    rejects([&] { state.apply({P::Balance, "cello", 2}, &player); });
    require(state.canRedo() && state.revision() == rev, "invalid edit destroyed redo");
    auto reordered = source(); std::swap(reordered.routes[0], reordered.routes[1]);
    daw::SessionPlayer wrong(reordered, audio());
    rejects([&] { state.undo(&wrong); }); rejects([&] { state.redo(&wrong); });
    require(state.revision() == rev && state.canRedo(), "mismatched player moved history");
    // Failed apply/undo/redo while the queue is full preserve the same branch.
    for (std::size_t i = 0; i < daw::SessionPlayer::kCapacity; ++i)
      require(player.enqueue({daw::PlaybackAction::Play}), "queue fill failed");
    require(!state.undo(&player) && !state.redo(&player) && !state.apply({P::Gain, "piano", -4}, &player), "full queue history mutation");
    require(state.revision() == rev && state.historySize() == 5 && state.canRedo() &&
        daw::serializeSession(state.current()) == snapshots[4], "failed submission changed history");
    level(player, .05, 0);
    require(state.apply({P::Gain, "cello", 0}, &player), "branch edit failed"); level(player, .1, 0);
    require(!state.canRedo() && !state.redo(&player), "new branch retained abandoned redo");
    require(state.undo(&player), "branch inverse failed"); level(player, .05, 0);

    // Small bound evicts only the oldest edit, preserving the remaining inverses.
    daw::SessionMixState bounded(source(), 2);
    bounded.apply({P::Gain, "piano", -1}); bounded.apply({P::Gain, "piano", -2}); bounded.apply({P::Gain, "piano", -3});
    require(bounded.historySize() == 2 && bounded.undo() && bounded.undo() && !bounded.undo() &&
            bounded.current().routes[0].gain_db == -1, "bounded history lost eviction baseline");
    require(bounded.redo() && bounded.redo() && bounded.current().routes[0].gain_db == -3, "bounded redo failed");
    daw::SessionMixState one(source(), 1);
    one.apply({P::Master, "", -1}); one.apply({P::Master, "", -2});
    require(one.undo() && !one.undo() && one.current().master_gain_db == -1, "one-entry bound failed");
    rejects([] { daw::SessionMixState invalid(source(), 0); });
    rejects([] { daw::SessionMixState invalid(source(), 1025); });
    daw::SessionMixState reopened(daw::parseSession(daw::serializeSession(state.current())));
    require(!reopened.canUndo() && !reopened.canRedo() && reopened.revision() == 0 &&
        daw::serializeSession(reopened.current()) == daw::serializeSession(state.current()), "reload history baseline incorrect");
    failureGuarantees(0); failureGuarantees(1); failureGuarantees(2);
    std::cout << "Mix undo/redo audio parity, history bounds, queue/allocation failure and branch tests passed\n";
    return 0;
  } catch (const std::exception& e) { fail_after = -1; std::cerr << e.what() << '\n'; return 1; }
}
