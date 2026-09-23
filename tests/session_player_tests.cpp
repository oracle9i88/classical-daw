#include "daw/session_player.hpp"
#include "daw/output_blocks.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
thread_local bool in_audio = false;
thread_local unsigned allocations = 0, frees = 0;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void near(double got, double expected, const char* message) { require(std::abs(got - expected) < 1e-6, message); }
template <typename F> void rejects(F f) {
  bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; }
  require(rejected, "invalid preparation accepted");
}
daw::Session settings() {
  return {"score.dawproj", 0, {{"piano", "pianoteq", 0, 0, "", ""},
                             {"cello", "swam-cello", 0, 0, "", ""}}};
}
std::vector<daw::AudioBuffer> sources(std::size_t frames = 8192) {
  std::vector<daw::AudioBuffer> result(2, daw::AudioBuffer{48000, 2, {}});
  for (auto& a : result) a.samples.resize(frames * 2);
  for (std::size_t i = 0; i < frames; ++i) {
    result[0].samples[i * 2] = .1F; result[0].samples[i * 2 + 1] = .2F;
    result[1].samples[i * 2] = .3F; result[1].samples[i * 2 + 1] = -.1F;
  }
  return result;
}
void command(daw::SessionPlayer& p, daw::PlaybackAction a, std::size_t track = 0, double value = 0, std::uint64_t frame = 0) {
  require(p.enqueue({a, track, value, frame}), "command rejected");
}
std::array<float, 512> block(daw::SessionPlayer& p) {
  std::array<float, 512> output{};
  in_audio = true; p.render(output.data(), 256); in_audio = false;
  return output;
}
}
void* operator new(std::size_t n) {
  if (in_audio) ++allocations;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if (in_audio && p) ++frees; std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

int main() {
  using A = daw::PlaybackAction;
  try {
    require(daw::outputSliceCapacity(512, 44100, 48000, 256) >= 814, "resampling capacity too small");
    require(daw::outputSliceCapacity(4096, 44100, 192000, 256) >= 18089, "high rate capacity too small");
    rejects([] { daw::outputSliceCapacity(65536, 44100, 48000, 256); });
    rejects([] { daw::outputSliceCapacity(512, 0, 48000, 256); });
    rejects([] { daw::outputSliceCapacity(512, 44100, std::numeric_limits<double>::quiet_NaN(), 256); });
    rejects([] { daw::outputSliceCapacity(512, 44100, 48000, 0); });
    // Variable hardware slices must never truncate frames or desynchronize the
    // two channels. Balanced quanta avoid pathological one-frame remainders
    // while retaining the same minimum number of render calls and exact data.
    for (const unsigned frames : {1U, 255U, 256U, 257U, 279U, 512U, 557U, 558U, 4459U}) {
      std::vector<float> output(static_cast<std::size_t>(frames) * 2 + 2, -9);
      unsigned position = 0, calls = 0, smallest = 256, largest = 0;
      in_audio = true;
      daw::renderOutputBlocks(output.data() + 1, frames, 2, 256,
          [&](float* dest, unsigned count) noexcept {
            if (count > 256 || !count) std::abort();
            smallest = std::min(smallest,count); largest = std::max(largest,count);
            ++calls;
            for (unsigned i = 0; i < count; ++i) {
              dest[2 * i] = static_cast<float>(position);
              dest[2 * i + 1] = -static_cast<float>(position++);
            }
          });
      in_audio = false;
      require(position == frames && calls == (frames + 255) / 256, "hardware slice lost frames");
      require(largest-smallest <= 1 && smallest == frames/calls, "hardware split left a tiny tail quantum");
      require(output.front() == -9 && output.back() == -9, "hardware slice overwrote guard");
      for (unsigned i = 0; i < frames; ++i) {
        near(output[1 + 2 * i], i, "hardware slice left alignment");
        near(output[2 + 2 * i], -static_cast<double>(i), "hardware slice right alignment");
      }
    }
    daw::SessionPlayer player(settings(), sources());
    require(player.acceptsFormat(48000, 2) && !player.acceptsFormat(44100, 2) &&
            !player.acceptsFormat(48000, 1), "format validation");
    auto b = block(player);
    require(std::all_of(b.begin(), b.end(), [](float v) { return v == 0; }) && player.status().frame == 0,
            "player did not start paused/silent");
    command(player, A::Play); b = block(player);
    near(b[0], .4 / 240, "start not ramped");
    near(b[510], .4, "track sum left"); near(b[511], .1, "track sum right");
    require(player.status().frame == 256 && player.status().playing, "transport position");
    command(player, A::Solo, 1, 1); b = block(player);
    near(b[510], .3, "solo leaked other track"); near(b[511], -.1, "solo stereo changed");
    command(player, A::Mute, 1, 1); b = block(player);
    near(b[510], 0, "muted solo not silent"); near(b[511], 0, "muted solo not silent right");
    command(player, A::Solo, 0, 1); b = block(player);
    near(b[510], .1, "multiple solo/mute priority");
    command(player, A::Mute, 1, 0); b = block(player);
    near(b[510], .4, "unmute lost track");
    command(player, A::Balance, 0, -1); command(player, A::Balance, 1, 1); b = block(player);
    near(b[510], .1, "left balance leak"); near(b[511], -.1, "right balance leak");
    command(player, A::Gain, 0, -20 * std::log10(2.)); b = block(player);
    near(b[510], .05, "gain adjustment"); near(b[511], -.1, "gain crossed tracks");
    command(player, A::Master, 0, -20 * std::log10(2.)); b = block(player);
    near(b[510], .025, "master gain"); near(b[511], -.05, "master stereo gain");
    const auto paused_at = player.status().frame;
    command(player, A::Pause); b = block(player);
    require(player.status().frame == paused_at && !player.status().playing, "pause advanced transport");
    require(std::abs(b[0]) < .025F && std::abs(b[0]) > .02F && b[510] == 0, "pause transition");
    command(player, A::Seek, 0, 0, 1024); block(player);
    require(player.status().frame == 1024 && !player.status().playing, "paused seek played or moved");
    command(player, A::Play); block(player);
    require(player.status().frame == 1280, "resume did not retain seek");
    command(player, A::Seek, 0, 0, 2000); block(player);
    require(player.status().frame == 2256, "playing seek");
    command(player, A::Stop); block(player);
    require(player.status().frame == 0 && !player.status().playing, "stop not reset");
    command(player, A::Seek, 0, 0, 8192 - 256); command(player, A::Play); block(player);
    require(player.status().frame == 8192 && !player.status().playing, "EOF boundary not stopped");
    b = block(player); near(b.back(), 0, "EOF tail not faded");
    command(player, A::Play); block(player);
    require(player.status().frame == 8192 && !player.status().playing, "EOF implicitly restarted");
    command(player, A::Seek, 0, 0, 8192 - 12); command(player, A::Play); b = block(player);
    require(player.status().frame == 8192 && !player.status().playing && b.back() == 0, "partial EOF block");

    // Sample alignment after seek, checked against independently evaluated ramps.
    auto signal = sources();
    for (auto& a : signal) for (std::size_t i = 0; i < a.frameCount(); ++i) {
      a.samples[2 * i] = static_cast<float>(i) / 100000;
      a.samples[2 * i + 1] = -a.samples[2 * i];
    }
    daw::SessionPlayer aligned(settings(), std::move(signal));
    command(aligned, A::Seek, 0, 0, 1234); command(aligned, A::Play); b = block(aligned);
    near(b[510], 2. * (1234 + 255) / 100000, "seek misaligned left samples");
    near(b[511], -2. * (1234 + 255) / 100000, "seek misaligned right samples");

    // No automatic loudness normalization; settled playback matches offline mixing.
    auto mix_settings = settings(); mix_settings.master_gain_db = -3;
    mix_settings.routes[0].gain_db = -4.5; mix_settings.routes[0].balance = -.2;
    mix_settings.routes[1].gain_db = -6; mix_settings.routes[1].balance = .2;
    auto raw = sources(); daw::SessionPlayer parity(mix_settings, raw);
    daw::AudioBuffer mix{48000, 2, std::vector<float>(8192 * 2)};
    for (std::size_t i = 0; i < raw.size(); ++i) {
      daw::applyTrackMix(raw[i], mix_settings.routes[i].gain_db, mix_settings.routes[i].balance);
      daw::addStereoTrack(mix, raw[i]);
    }
    daw::applyMasterMix(mix, mix_settings.master_gain_db);
    command(parity, A::Play); block(parity); b = block(parity);
    for (std::size_t i = 0; i < b.size(); ++i) near(b[i], mix.samples[512 + i], "offline/realtime mix mismatch");

    // Stop/join the output before moving its sole consumer to the control thread.
    // Pending accepted edits survive a lost device even when the queue is full.
    daw::SessionPlayer disconnected(settings(), sources());
    command(disconnected, A::Play); block(disconnected);
    const auto device_position = disconnected.status().frame;
    command(disconnected, A::Gain, 0, -6);
    command(disconnected, A::Mute, 1, 1);
    command(disconnected, A::Master, 0, -3);
    for (std::size_t i = 3; i < daw::SessionPlayer::kCapacity; ++i) command(disconnected, A::Play);
    disconnected.suspendAfterOutputStopped();
    require(!disconnected.status().playing && disconnected.status().frame == device_position &&
            disconnected.status().block_peak == 0, "device suspension lost position or pause");
    b = block(disconnected);
    require(std::all_of(b.begin(), b.end(), [](float value) { return value == 0; }), "old transition leaked after device restart");
    require(disconnected.status().frame == device_position, "paused restart advanced clock");
    command(disconnected, A::Play); b = block(disconnected);
    near(b[510], .1 * std::pow(10., -9./20.), "pending mix edit lost across device interruption");
    command(disconnected, A::Seek, 0, 0, 1234); disconnected.suspendAfterOutputStopped();
    require(disconnected.status().frame == 1234 && !disconnected.status().playing, "disconnected seek lost");
    command(disconnected, A::Stop); disconnected.suspendAfterOutputStopped();
    require(disconnected.status().frame == 0, "disconnected stop lost");

    // Clamping is a counted monitoring safety guard, never hidden normalization.
    auto hot = sources(); for (auto& a : hot) std::fill(a.samples.begin(), a.samples.end(), 10.F);
    daw::SessionPlayer overload(settings(), std::move(hot)); command(overload, A::Play); block(overload); b = block(overload);
    require(overload.status().clipped_samples > 0 && overload.status().block_peak == 20, "overload not reported");
    require(std::all_of(b.begin(), b.end(), [](float v) { return v == 1; }), "unsafe output escaped clamp");

    require(!player.enqueue({A::Seek, 0, 0, 8193}) && !player.enqueue({A::Gain, 2, 0}) &&
            !player.enqueue({A::Gain, 0, 13}) && !player.enqueue({A::Balance, 0, 2}) &&
            !player.enqueue({A::Mute, 0, .5}) && !player.enqueue({A::Master, 0, std::numeric_limits<double>::infinity()}) &&
            !player.enqueue({static_cast<A>(999)}), "invalid command accepted");
    require(player.status().rejected_commands == 7, "reject count incorrect");
    for (std::size_t i = 0; i < daw::SessionPlayer::kCapacity; ++i) command(player, A::Pause);
    require(!player.enqueue({A::Play}), "queue overflow accepted");
    block(player); require(player.enqueue({A::Stop}), "queue did not drain"); block(player);
    require(allocations == 0 && frees == 0, "audio callback allocated or freed memory");
    auto invalid = sources(); invalid[0].samples.pop_back();
    rejects([&] { daw::SessionPlayer p(settings(), invalid); });
    invalid = sources(); invalid[0].sample_rate = 44100;
    rejects([&] { daw::SessionPlayer p(settings(), invalid); });
    invalid = sources(); invalid[0].samples[12] = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { daw::SessionPlayer p(settings(), invalid); });
    rejects([&] { daw::SessionPlayer p(settings(), {}); });
    rejects([&] { daw::SessionPlayer p(settings(), sources(0)); });
    std::cout << "Session playback controls, alignment, mix parity, bounds, ramps and allocation guard passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
