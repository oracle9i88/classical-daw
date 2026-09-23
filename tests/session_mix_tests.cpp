#include "daw/session_mix.hpp"
#include "daw/session_player.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template <typename F> void rejects(F f) {
  bool failed = false; try { f(); } catch (const std::exception&) { failed = true; }
  require(failed, "invalid mix operation accepted");
}
std::string read(const fs::path& path) { std::ifstream in(path); std::ostringstream text; text << in.rdbuf(); return text.str(); }
daw::Session initial() {
  return {"score.dawproj", -3, {{"piano", "pianoteq", -4.5, -.2, "", "p.aupreset", false, false, "p.dawfreeze"},
                              {"cello", "swam-cello", -6, .2, "", "c.aupreset", false, false, "c.dawfreeze"}}};
}
std::vector<daw::AudioBuffer> audio() {
  return {{48000, 2, std::vector<float>(4096, .1F)}, {48000, 2, std::vector<float>(4096, .2F)}};
}
struct Directory {
  fs::path path;
  Directory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int n = 0; n < 100; ++n) {
      auto candidate = fs::temp_directory_path() / ("daw-mix-" + std::to_string(stamp) + "-" + std::to_string(n));
      if (fs::create_directory(candidate)) { path = candidate; return; }
    }
    throw std::runtime_error("cannot create test directory");
  }
  ~Directory() { std::error_code ignored; fs::remove_all(path, ignored); }
};
}
int main() {
  using P = daw::MixParameter;
  try {
    const auto source = initial();
    daw::SessionMixState state(source);
    daw::SessionPlayer player(source, audio());
    require(state.apply({P::Solo, "cello", 1}, &player), "solo not submitted");
    require(state.apply({P::Gain, "cello", -2}, &player), "gain not submitted");
    require(state.apply({P::Balance, "cello", -.3}, &player), "balance not submitted");
    require(state.apply({P::Mute, "piano", 1}, &player), "mute not submitted");
    require(state.apply({P::Master, "", -1}, &player), "master not submitted");
    require(state.current().routes[0].gain_db == source.routes[0].gain_db && state.current().routes[1].gain_db == -2,
            "stable ID edit crossed tracks");
    const auto expected = daw::serializeSession(state.current());
    const auto before = daw::serializeSession(source);
    rejects([&] { state.apply({P::Gain, "missing", 0}, &player); });
    rejects([&] { state.apply({P::Master, "piano", 0}, &player); });
    rejects([&] { state.apply({P::Master, "", std::numeric_limits<double>::quiet_NaN()}, &player); });
    rejects([&] { state.apply({P::Balance, "cello", 2}, &player); });
    rejects([&] { state.apply({P::Mute, "cello", .5}, &player); });
    rejects([&] { state.apply({static_cast<P>(999), "cello", 0}, &player); });
    require(daw::serializeSession(state.current()) == expected, "invalid edit changed document");
    auto reordered = source; std::swap(reordered.routes[0], reordered.routes[1]);
    daw::SessionPlayer other(reordered, audio());
    rejects([&] { state.apply({P::Gain, "cello", 0}, &other); });
    require(daw::serializeSession(state.current()) == expected, "mismatched player changed document");

    std::array<float, 512> block{};
    player.render(block.data(), 256); // Consume edits while paused.
    for (std::size_t i = 0; i < daw::SessionPlayer::kCapacity; ++i)
      require(player.enqueue({daw::PlaybackAction::Pause}), "unexpected full queue");
    require(!state.apply({P::Gain, "cello", 0}, &player), "full queue accepted mix");
    require(daw::serializeSession(state.current()) == expected, "full queue changed saved targets");
    player.render(block.data(), 256);

    Directory temp;
    const auto input = temp.path / "source.dawsession", output = temp.path / "saved.dawsession";
    { std::ofstream file(input); file << before; }
    daw::saveNewSessionMix(state.current(), input.string(), output.string());
    require(read(output) == expected && read(input) == before, "save bytes wrong or original changed");
    require(!fs::exists(output.string() + ".saving"), "save staging not cleaned");
    const auto reopened = daw::parseSession(read(output));
    require(reopened.routes[1].state_file == "c.aupreset" && reopened.routes[1].frozen_file == "c.dawfreeze",
            "mix save changed sound references");
    daw::SessionPlayer restored(reopened, audio());
    require(player.enqueue({daw::PlaybackAction::Play}) && restored.enqueue({daw::PlaybackAction::Play}), "play failed");
    std::array<float, 512> reload_block{};
    player.render(block.data(), 256); restored.render(reload_block.data(), 256);
    for (std::size_t i = 0; i < block.size(); ++i)
      require(std::abs(block[i] - reload_block[i]) < 1e-7F, "saved/reopened mix differs from live accepted state");
    rejects([&] { daw::saveNewSessionMix(source, input.string(), output.string()); });
    require(read(output) == expected, "existing save overwritten");
    const auto dangling = temp.path / "dangling";
    fs::create_symlink(temp.path / "absent", dangling);
    rejects([&] { daw::saveNewSessionMix(source, input.string(), dangling.string()); });
    require(fs::is_symlink(fs::symlink_status(dangling)), "dangling symlink changed");
    fs::create_directory(temp.path / "other");
    rejects([&] { daw::saveNewSessionMix(source, input.string(), (temp.path / "other/x").string()); });
    const auto blocked = temp.path / "blocked";
    fs::create_directory(blocked.string() + ".saving");
    { std::ofstream file(blocked.string() + ".saving/keep"); file << "original"; }
    rejects([&] { daw::saveNewSessionMix(source, input.string(), blocked.string()); });
    require(read(blocked.string() + ".saving/keep") == "original" && !fs::exists(blocked), "stale staging modified");
    auto invalid = source; invalid.routes[0].gain_db = 99;
    rejects([&] { daw::saveNewSessionMix(invalid, input.string(), (temp.path / "invalid").string()); });
    require(!fs::exists(temp.path / "invalid.saving"), "invalid save created staging");
    // Simultaneous same-path saves must publish exactly one whole document.
    std::atomic<unsigned> successes{0};
    auto save = [&](daw::Session s) {
      try { daw::saveNewSessionMix(s, input.string(), (temp.path / "race").string()); ++successes; }
      catch (const std::exception&) {}
    };
    std::thread one(save, source), two(save, reopened); one.join(); two.join();
    const auto winner = read(temp.path / "race");
    require(successes == 1 && (winner == before || winner == expected), "concurrent save replaced or partially wrote file");
    require(!fs::exists(temp.path / "race.saving"), "concurrent save leaked staging");
    std::cout << "Mix command/document parity, queue failure, live/save/reopen and exclusive publication passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
