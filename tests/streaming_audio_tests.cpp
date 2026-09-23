#include "daw/session_player.hpp"
#include "daw/session_mix.hpp"
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace {
thread_local bool in_render = false;
thread_local unsigned allocations = 0, releases = 0;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template <class F> void rejects(F f) {
  bool failed = false; try { f(); } catch (const std::exception&) { failed = true; }
  require(failed, "invalid stream operation accepted");
}
struct Temp {
  fs::path path = fs::temp_directory_path() / ("daw-stream-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Temp() { require(fs::create_directory(path), "cannot reserve test directory"); }
  ~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
void wait(daw::StreamingAudio& source, std::size_t frame) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!source.frame(frame)) {
    require(!source.failed() && std::chrono::steady_clock::now() < deadline, "prefetch failed/timed out");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
void render(daw::SessionPlayer& p, float* out, unsigned frames) {
  in_render = true; p.render(out, frames); in_render = false;
}
int capacity(const fs::path& directory) {
  constexpr std::size_t frames = 1100000, count = 64;
  const auto path = (directory / "capacity.dawfreeze").string();
  {
    daw::AudioBuffer audio{48000, 2, std::vector<float>(frames * 2, .001F)};
    daw::writeFrozenTrack(audio, "capacity fixture", "", 0, path);
  }
  daw::Session session{"score.dawproj", 0, {}};
  std::vector<std::unique_ptr<daw::FrozenTrackReader>> readers;
  for (std::size_t i = 0; i < count; ++i) {
    session.routes.push_back({"part-" + std::to_string(i), "pianoteq", 0, 0, "", ""});
    readers.push_back(std::make_unique<daw::FrozenTrackReader>(path, "capacity fixture", frames));
  }
  auto source = std::make_unique<daw::StreamingAudio>(std::move(readers));
  auto* view = source.get();
  const auto bytes = source->bufferBytes(), resident = frames * count * 2 * sizeof(float);
  require(resident > daw::SessionPlayer::kMaxAudioBytes && bytes == 8U * 1024U * 1024U, "capacity boundary wrong");
  daw::SessionPlayer player(std::move(source), session);
  std::array<float, 512> output{};
  player.enqueue({daw::PlaybackAction::Play});
  // Complete sequential playback: every page traversed for all 64 readers.
  while (player.status().frame < frames) {
    const auto f = player.status().frame;
    wait(*view, f);
    render(player, output.data(), 256);
    if (f + 256 <= frames) require(std::abs(output.back() - .064) < 1e-6, "capacity playback mixed wrong tracks");
  }
  require(!player.status().playing && !player.status().stream_failed && player.status().buffering_frames == 0,
          "capacity playback incomplete or starved");
  require(allocations == 0 && releases == 0, "capacity render allocated");
  std::cout << "PASS capacity: tracks=" << count << " frames=" << frames << " resident_bytes=" << resident
            << " page_buffer_bytes=" << bytes << " buffering_frames=0\n";
  return 0;
}

}
void* operator new(std::size_t n) {
  if (in_render) ++allocations;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if (in_render && p) ++releases; std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
int main(int argc, char** argv) {
  try {
    Temp temp;
    if (argc == 2 && std::string(argv[1]) == "--capacity") return capacity(temp.path);
    require(argc == 1, "usage: daw_streaming_audio_tests [--capacity]");
    constexpr std::size_t frames = 100003;
    const std::string identity = "source identity";
    daw::Session session{"score.dawproj", -3, {{"p", "pianoteq", 0, -.3, "", ""}, {"c", "swam-cello", -2, .4, "", ""}}};
    std::vector<daw::AudioBuffer> audio(2, {48000, 2, {}});
    std::array<std::string, 2> paths;
    for (std::size_t t = 0; t < 2; ++t) {
      audio[t].samples.resize(frames * 2);
      for (std::size_t f = 0; f < frames; ++f) {
        audio[t].samples[f * 2] = static_cast<float>(static_cast<int>(f % 133) - 60) * .001F * static_cast<float>(t + 1);
        audio[t].samples[f * 2 + 1] = -audio[t].samples[f * 2] * .5F;
      }
      paths[t] = (temp.path / (std::to_string(t) + ".dawfreeze")).string();
      daw::writeFrozenTrack(audio[t], identity, "test", 1, paths[t]);
    }
    // Block reader preserves exact samples across arbitrary seeks and EOF.
    daw::FrozenTrackReader reader(paths[0], identity, frames);
    std::array<float, daw::FrozenTrackReader::kReadFrames * 2> samples{};
    for (const std::size_t start : {0U, 1U, 4095U, 8191U, 99000U}) {
      const auto count = std::min<std::size_t>(8192, frames - start);
      reader.readFrames(start, count, samples.data());
      require(std::memcmp(samples.data(), audio[0].samples.data() + start * 2, count * 8) == 0, "disk seek changed samples");
    }
    reader.readFrames(frames, 0, nullptr);
    rejects([&] { reader.readFrames(frames, 1, samples.data()); });
    rejects([&] { reader.readFrames(0, 8193, samples.data()); });
    rejects([&] { reader.readFrames(0, 1, nullptr); });
    rejects([&] { daw::FrozenTrackReader bad(paths[0], "stale", frames); });
    rejects([&] { daw::FrozenTrackReader bad(paths[0], identity, frames - 1); });
    const auto link = temp.path / "link"; fs::create_symlink(paths[0], link);
    rejects([&] { daw::FrozenTrackReader bad(link.string(), identity, frames); });
    auto make = [&] {
      std::vector<std::unique_ptr<daw::FrozenTrackReader>> readers;
      for (const auto& path : paths) readers.push_back(std::make_unique<daw::FrozenTrackReader>(path, identity, frames));
      return std::make_unique<daw::StreamingAudio>(std::move(readers));
    };
    auto source = make(); auto* view = source.get();
    require(view->bufferBytes() == 262144 && view->trackCount() == 2 && view->frameCount() == frames, "unbounded/wrong page storage");
    daw::SessionPlayer streamed(std::move(source), session), resident(session, audio);
    daw::SessionMixState streaming_mix(session), resident_mix(session);
    auto command = [&](daw::PlaybackCommand c) { require(streamed.enqueue(c) && resident.enqueue(c), "command rejected"); };
    auto same = [&](unsigned count) {
      while (count) {
        const auto position = resident.status().frame;
        if (position < frames) wait(*view, position);
        const auto n = static_cast<unsigned>(std::min<std::size_t>(count, daw::StreamingAudio::kPageFrames - position % daw::StreamingAudio::kPageFrames));
        std::array<float, 512> a{}, b{};
        render(streamed, a.data(), n); render(resident, b.data(), n);
        require(std::memcmp(a.data(), b.data(), n * 8) == 0, "streamed/resident output differs");
        require(streamed.status().frame == resident.status().frame && !streamed.status().buffering, "stream position drifted");
        count -= n;
      }
    };
    command({daw::PlaybackAction::Play});
    // Align blocks to pages so the control-thread wait covers the entire block.
    for (unsigned i = 0; i < 100; ++i) {
      if (i == 30) {
        streaming_mix.apply({daw::MixParameter::Solo, "c", 1}, &streamed);
        resident_mix.apply({daw::MixParameter::Solo, "c", 1}, &resident);
      }
      if (i == 45) { streaming_mix.undo(&streamed); resident_mix.undo(&resident); }
      same(256);
    }
    command({daw::PlaybackAction::Pause}); same(256); same(256);
    // Rapid arbitrary seeks, including backwards seeks, replace prefetched pages.
    for (std::size_t target : {90000U, 4096U, 80000U, 0U, 70000U}) {
      wait(*view, target);
      command({daw::PlaybackAction::Seek, 0, 0, target});
      std::array<float, 256> a{}, b{};
      render(streamed, a.data(), 128); render(resident, b.data(), 128);
      require(a == b && streamed.status().frame == target, "paused seek drifted");
      command({daw::PlaybackAction::Play}); same(128);
      command({daw::PlaybackAction::Pause}); same(128);
    }
    // EOF / terminal partial page / stop and restart use the shared transport.
    wait(*view, frames - 100);
    command({daw::PlaybackAction::Seek, 0, 0, frames - 100});
    command({daw::PlaybackAction::Play});
    std::array<float, 512> a{}, b{};
    render(streamed, a.data(), 256); render(resident, b.data(), 256);
    require(a == b && !streamed.status().playing && streamed.status().frame == frames, "EOF/tail mismatch");
    wait(*view, 0); command({daw::PlaybackAction::Stop});
    render(streamed, a.data(), 256); render(resident, b.data(), 256); require(a == b, "stop mismatch");
    command({daw::PlaybackAction::Play}); same(256);
    require(streamed.status().buffering_frames == 0, "prepared stream unexpectedly starved");

    // Stress page ownership while worker runs; only one consumer ever accesses frame().
    for (std::size_t i = 0; i < 300; ++i) {
      const auto f = (i * 7919) % frames;
      wait(*view, f); const auto* values = view->frame(f);
      require(values && values[0] == audio[0].samples[f * 2] && values[2] == audio[1].samples[f * 2], "page reuse mixed tracks/positions");
    }
    // Simulate a post-validation disk failure. No stale/partial block is exposed,
    // all tracks stop advancing together and the worker error becomes visible.
    auto failing = make(); auto* failure_view = failing.get();
    daw::SessionPlayer broken(std::move(failing), session);
    fs::resize_file(paths[1], 1);
    broken.enqueue({daw::PlaybackAction::Seek, 0, 0, 90000}); broken.enqueue({daw::PlaybackAction::Play});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do { render(broken, a.data(), 256); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    while (!failure_view->failed() && std::chrono::steady_clock::now() < deadline);
    require(failure_view->failed(), "disk failure not surfaced");
    render(broken, a.data(), 256);
    require(broken.status().stream_failed && broken.status().buffering && broken.status().frame == 90000 &&
            broken.status().buffering_frames > 0, "missing disk data advanced/desynchronized transport");
    for (float value : a) require(value == 0, "disk failure leaked partial/stale samples");
    broken.enqueue({daw::PlaybackAction::Pause}); render(broken, a.data(), 256);
    require(!broken.status().playing && !broken.status().buffering, "pause ignored during starvation");
    require(allocations == 0 && releases == 0, "allocation/deallocation in streamed render");
    rejects([&] { daw::FrozenTrackReader bad(paths[1], identity, frames); });
    const auto corrupt = temp.path / "corrupt"; fs::copy_file(paths[0], corrupt);
    { std::fstream file(corrupt, std::ios::binary | std::ios::in | std::ios::out); file.seekp(-5, std::ios::end); file.put('x'); }
    rejects([&] { daw::FrozenTrackReader bad(corrupt.string(), identity, frames); });
    std::cout << "Streaming samples/transport/mix parity, bounded pages, seek stress, disk failure and callback allocation tests passed\n";
    return 0;
  } catch (const std::exception& e) { in_render = false; std::cerr << e.what() << '\n'; return 1; }
}
