#include "daw/audio_limits.hpp"
#include "daw/frozen_track.hpp"
#include "daw/session_player.hpp"
#include "daw/midi_sequence.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
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
template <class F> void rejects(F f) {
  bool failed = false; try { f(); } catch (const std::exception&) { failed = true; }
  require(failed, "invalid long-stream operation accepted");
}
struct Temp {
  fs::path path = fs::temp_directory_path() / ("daw-long-stream-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Temp() { require(fs::create_directory(path), "cannot reserve test directory"); }
  ~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
std::string read(const fs::path& path) { std::ifstream in(path, std::ios::binary); std::ostringstream out; out << in.rdbuf(); return out.str(); }
void write(const fs::path& path, const std::string& bytes) { std::ofstream out(path, std::ios::binary); out << bytes; }
daw::Session session() {
  return {"score.dawproj", 0, {{"p", "pianoteq", -6, 0, "", ""}, {"c", "swam-cello", -6, 0, "", ""}}};
}
daw::Score score(daw::Tick end) {
  daw::Score score;
  daw::ScoreNote note;
  note.duration = 960;
  score.parts = {{"p", "Piano", {{1, 0, {note}, end}}}, {"c", "Cello", {{1, 0, {note}, end}},
      {{0, daw::MidiChannelEventType::ControlChange, 1, 11, 100}}}};
  return score;
}
float value(std::size_t frame) { return static_cast<float>(static_cast<int>(frame % 997) - 498) * .0002F; }
void wait(daw::StreamingAudio& audio, std::size_t frame) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!audio.frame(frame)) {
    require(!audio.failed() && std::chrono::steady_clock::now() < deadline, "long prefetch failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
void longFixture(const fs::path& root) {
  constexpr std::size_t frames = 48000U * 60U * 45U;
  const auto plan = daw::planSession(session(), score((45 * 60 - 5) * 1920), 48000, 5, daw::SessionPlanMode::Streaming);
  require(plan.frames == frames, "45 minute plan duration incorrect");
  const auto path = (root / "45-minute.dawfreeze").string();
  {
    daw::FrozenTrackWriter writer(path, "synthetic long fixture", "diagnostic", 1, frames);
    std::array<float, 16384> block{};
    for (std::size_t start = 0; start < frames;) {
      const auto count = std::min<std::size_t>(8192, frames - start);
      for (std::size_t i = 0; i < count; ++i) { block[2*i] = value(start+i); block[2*i+1] = -value(start+i); }
      writer.appendFrames(block.data(), count); start += count;
    }
    writer.finish();
  }
  rejects([&] { daw::readFrozenTrack(path, "synthetic long fixture", frames); }); // Never allocate 1 GiB in legacy API.
  std::vector<std::unique_ptr<daw::FrozenTrackReader>> readers;
  for (int i = 0; i < 2; ++i) readers.push_back(std::make_unique<daw::FrozenTrackReader>(path, "synthetic long fixture", frames));
  auto streaming = std::make_unique<daw::StreamingAudio>(std::move(readers)); auto* view = streaming.get();
  require(view->bufferBytes() == 262144, "long duration grew audio memory");
  daw::SessionPlayer player(std::move(streaming), session());
  std::array<float, 512> out{};
  for (const auto target : std::array<std::size_t, 5>{0, 48000U*60U*12U+3U, 48000U*60U*30U+5U, frames-512, 4096}) {
    wait(*view, target);
    player.enqueue({daw::PlaybackAction::Seek, 0, 0, target}); player.enqueue({daw::PlaybackAction::Play});
    // Render 256 frames in page-aligned spans, warming only outside render.
    std::size_t consumed = 0;
    while (consumed < 256) {
      wait(*view, target + consumed);
      const auto count = std::min<std::size_t>(256 - consumed, 4096 - (target + consumed) % 4096);
      player.render(out.data() + consumed * 2, static_cast<std::uint32_t>(count)); consumed += count;
    }
    const double expected = value(target + 255) * 2. * std::pow(10., -6. / 20.);
    require(std::abs(out[510] - expected) < 1e-7 && std::abs(out[511] + expected) < 1e-7 &&
            player.status().frame == target + 256, "long seek corrupted frame/channel alignment");
  }
  wait(*view, frames-16);
  player.enqueue({daw::PlaybackAction::Seek, 0, 0, frames-16});
  player.render(out.data(), 256);
  require(player.status().frame == frames && !player.status().playing && !player.status().stream_failed,
          "long EOF did not stop at common end");
  require(player.status().buffering_frames == 0, "prepared long stream starved");
  std::cout << "PASS long fixture: minutes=45 frames=" << frames << " file_bytes=" << fs::file_size(path)
            << " tracks=2 page_buffer_bytes=262144 seeks=12min/30min/end/backwards; checksum and EOF verified\n";
}
}
int main(int argc, char** argv) {
  try {
    Temp temp;
    if (argc == 2 && std::string(argv[1]) == "--long") { longFixture(temp.path); return 0; }
    require(argc == 1, "usage: daw_frozen_stream_writer_tests [--long]");
    const auto path = (temp.path / "stream.dawfreeze").string();
    const auto buffered = (temp.path / "buffered.dawfreeze").string();
    daw::AudioBuffer audio{48000, 2, {0.F, -0.F, 1.5F, -2.F, .1234567F, -.1F}};
    daw::writeFrozenTrack(audio, "identity", "preset", 123, buffered);
    {
      daw::FrozenTrackWriter writer(path, "identity", "preset", 123, 3);
      require(!fs::exists(path), "partial stream published");
      rejects([&] { daw::FrozenTrackWriter other(path, "identity", "preset", 123, 3); });
      rejects([&] { writer.finish(); });
      rejects([&] { writer.appendFrames(nullptr, 1); });
      rejects([&] { writer.appendFrames(audio.samples.data(), 4); });
      auto bad = audio.samples; bad.back() = std::numeric_limits<float>::quiet_NaN();
      rejects([&] { writer.appendFrames(bad.data(), 3); });
      require(writer.writtenFrames() == 0, "invalid chunk changed count");
      writer.appendFrames(audio.samples.data(), 1); writer.appendFrames(audio.samples.data()+2, 2);
      writer.appendFrames(nullptr, 0); writer.finish();
      rejects([&] { writer.finish(); }); rejects([&] { writer.appendFrames(nullptr, 0); });
    }
    require(read(path) == read(buffered), "chunked format differs from legacy writer");
    require(!fs::exists(path + ".writing"), "owned staging leaked");
    require(daw::readFrozenTrack(path, "identity", 3).audio.samples == audio.samples, "legacy reader lost streamed values");
    rejects([&] { daw::FrozenTrackWriter bad(path, "identity", "", 0, 3); });
    const auto partial = (temp.path / "partial").string();
    { daw::FrozenTrackWriter writer(partial, "identity", "", 0, 3); writer.appendFrames(audio.samples.data(), 1); }
    require(!fs::exists(partial) && !fs::exists(partial+".writing"), "aborted writer left published or owned data");
    fs::create_directory(partial+".writing"); write(fs::path(partial+".writing")/"audio.tmp", "old crash");
    rejects([&] { daw::FrozenTrackWriter writer(partial, "identity", "", 0, 3); });
    require(read(fs::path(partial+".writing")/"audio.tmp") == "old crash", "stale staging deleted");
    const auto competing = (temp.path / "competing").string();
    {
      daw::FrozenTrackWriter writer(competing, "identity", "", 0, 3); writer.appendFrames(audio.samples.data(), 3);
      write(competing, "other owner"); rejects([&] { writer.finish(); });
      rejects([&] { writer.appendFrames(nullptr, 0); });
    }
    require(read(competing) == "other owner", "competing output overwritten");
    const auto link = temp.path / "symlink"; fs::create_symlink(temp.path / "absent", link);
    rejects([&] { daw::FrozenTrackWriter writer(link.string(), "identity", "", 0, 3); });
    const auto invalid = (temp.path / "invalid").string();
    rejects([&] { daw::FrozenTrackWriter writer(invalid, "identity", "", 0, 0); });
    rejects([&] { daw::FrozenTrackWriter writer(invalid, "identity", "", 0, daw::kMaxStreamAudioFrames+1); });
    require(!fs::exists(invalid+".writing"), "bad metadata created staging");
    const auto end = (120*60-5)*1920;
    const auto full = daw::planSession(session(), score(end), 48000, 5, daw::SessionPlanMode::Streaming);
    require(full.frames == daw::kMaxStreamAudioFrames && full.end_tick == end, "two-hour plan limit wrong");
    for (const auto& track : full.tracks) {
      const auto seq = daw::makeMidiSampleSequence(track.midi, 48000, 5, daw::kMaxStreamAudioFrames, full.end_tick);
      require(seq.frames == full.frames && seq.end_frame == full.frames - 240000, "long parts have different release times");
    }
    rejects([&] { daw::planSession(session(), score(end)); });
    rejects([&] { daw::planSession(session(), score(end+1), 48000, 5, daw::SessionPlanMode::Streaming); });
    rejects([&] { daw::planSession(session(), score(end), 44100, 5, daw::SessionPlanMode::Streaming); });
    rejects([&] { daw::planSession(session(), score(end), 48000, 5, static_cast<daw::SessionPlanMode>(99)); });
    std::cout << "Chunk writer compatibility, finite/bounds checks, atomic publication and two-hour planning tests passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
