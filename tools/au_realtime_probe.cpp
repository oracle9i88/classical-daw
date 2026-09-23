// Opt-in real hardware / Pianoteq spike, deliberately independent of Score.
#include "audio_unit_instrument.hpp"
#include "coreaudio_output.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr std::size_t kRate = 48000;
constexpr std::size_t kSeconds = 30;
constexpr double kBudgetLimit = 0.5;

struct Workload {
  std::vector<daw::TimedMidiEvent> events;
  std::size_t max_pressed_keys = 0;
  std::size_t max_pedal_latched_keys = 0;
  std::size_t max_sounding_keys = 0;
};

Workload makeWorkload(bool polyphonic) {
  Workload result;
  // Alternating by a semitone keeps successive ten-note chords distinct.
  // These MIDI key counts are not the AU's internal voice counts: pedal-held
  // retriggers and resonance can consume additional DSP voices.
  constexpr std::array<std::uint8_t, 10> chord{36, 40, 43, 48, 52, 55, 60, 64, 67, 72};
  for (std::size_t second = 0; second < kSeconds; ++second) {
    const bool pedal = polyphonic ? second % 4 != 3 && second + 1 < kSeconds : second % 2 == 0;
    result.events.push_back({second * kRate, 0xb0, 64, static_cast<std::uint8_t>(pedal ? 127 : 0)});
    for (std::size_t i = 0; i < 20; ++i) {
      result.events.push_back({second * kRate + i * 2400, 0xb0, 11,
                              static_cast<std::uint8_t>(50 + (second * 20 + i) % 70)});
    }
    const std::size_t notes = polyphonic ? chord.size() : 1;
    for (std::size_t i = 0; i < notes; ++i) {
      const auto pitch = static_cast<std::uint8_t>(polyphonic ? chord[i] + second % 2 : 60 + second % 12);
      result.events.push_back({second * kRate + 137, 0x90, pitch, 80});
      result.events.push_back({second * kRate + 24013, 0x80, pitch, 32});
    }
  }
  std::stable_sort(result.events.begin(), result.events.end(), [](auto a, auto b) { return a.frame < b.frame; });

  std::array<bool, 128> pressed{}, latched{};
  bool pedal = false;
  for (const auto& event : result.events) {
    if (event.status == 0xb0 && event.data1 == 64) {
      pedal = event.data2 >= 64;
      if (!pedal) latched.fill(false);
    } else if (event.status == 0x90) {
      if (pressed[event.data1]) throw std::runtime_error("probe workload has overlapping key presses");
      pressed[event.data1] = true;
      latched[event.data1] = false;
    } else if (event.status == 0x80) {
      if (!pressed[event.data1]) throw std::runtime_error("probe workload has an unmatched release");
      pressed[event.data1] = false;
      latched[event.data1] = pedal;
    }
    const auto pressed_count = static_cast<std::size_t>(std::count(pressed.begin(), pressed.end(), true));
    const auto latched_count = static_cast<std::size_t>(std::count(latched.begin(), latched.end(), true));
    result.max_pressed_keys = std::max(result.max_pressed_keys, pressed_count);
    result.max_pedal_latched_keys = std::max(result.max_pedal_latched_keys, latched_count);
    result.max_sounding_keys = std::max(result.max_sounding_keys, pressed_count + latched_count);
  }
  const auto expected_messages = polyphonic ? 1230U : 690U;
  if (result.events.size() != expected_messages || result.max_pressed_keys != (polyphonic ? 10U : 1U) ||
      (polyphonic && result.max_pedal_latched_keys != 20) || pedal ||
      std::count(pressed.begin(), pressed.end(), true) || std::count(latched.begin(), latched.end(), true)) {
    throw std::runtime_error("probe workload contract failed");
  }
  return result;
}

void describe(const Workload& workload, bool polyphonic) {
  std::cout << "mode=" << (polyphonic ? "polyphonic" : "single-key-baseline")
            << " duration_seconds=" << kSeconds << " scheduled_events=" << workload.events.size()
            << " max_pressed_keys=" << workload.max_pressed_keys
            << " max_pedal_latched_keys=" << workload.max_pedal_latched_keys
            << " max_sounding_midi_keys=" << workload.max_sounding_keys
            << " pedal_cycle_seconds=" << (polyphonic ? 4 : 2)
            << " pedal_hold_seconds=" << (polyphonic ? 3 : 1)
            << " cc11_events_per_second=20 complete_callback_budget_limit=" << kBudgetLimit << '\n';
}

class Probe final : public daw::AudioOutputSource {
 public:
  Probe(daw::AudioUnitInstrument& au, const Workload& workload) : au_(au), events_(workload.events) {}
  bool acceptsFormat(double rate, std::uint32_t channels) const noexcept override { return rate == kRate && channels == 2; }

  void render(float* out, std::uint32_t frames) noexcept override {
    if (!frames) return;
    if (!armed.load()) { std::fill(out, out + frames * 2, 0.F); return; }
    const auto start = std::chrono::steady_clock::now();
    std::array<daw::TimedMidiEvent, 256> block{};
    std::size_t n = 0;
    while (next < events_.size() && events_[next].frame < position + frames) {
      if (n == block.size() || events_[next].frame < position) { failed.store(true); break; }
      block[n] = events_[next++];
      block[n++].frame -= position;
    }
    if (!au_.renderRealtime(block.data(), n, out, frames)) failed.store(true);
    for (std::size_t i = 0; i < frames * 2; ++i) {
      if (!std::isfinite(out[i])) failed.store(true);
      peak = std::max(peak, std::abs(static_cast<double>(out[i])));
      out[i] = 0;  // Always silence the speakers, including the heavy workload.
    }
    position += frames;
    sent += n;
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    // Subdivision ratios are diagnostic only. Fixed per-call cost exaggerates
    // a short tail's ratio; only CoreAudioOutput's COMPLETE callback timer is
    // used for the headroom/deadline acceptance gate.
    const auto budget_ratio = seconds / (static_cast<double>(frames) / kRate);
    max_seconds = std::max(max_seconds, seconds);
    max_budget_ratio = std::max(max_budget_ratio, budget_ratio);
    if (budget_ratio >= 1.0) ++deadline_misses;
    if (budget_ratio >= kBudgetLimit) ++headroom_misses;
    min_block_frames = std::min(min_block_frames, frames);
    max_block_frames = std::max(max_block_frames, frames);
    ++blocks;
    frame.store(position);
  }

  std::atomic<bool> armed{false}, failed{false};
  std::atomic<std::size_t> frame{0};
  std::size_t position = 0, next = 0, sent = 0, deadline_misses = 0, headroom_misses = 0, blocks = 0;
  std::uint32_t min_block_frames = std::numeric_limits<std::uint32_t>::max(), max_block_frames = 0;
  double peak = 0, max_seconds = 0, max_budget_ratio = 0;

 private:
  daw::AudioUnitInstrument& au_;
  const std::vector<daw::TimedMidiEvent>& events_;
};
}  // namespace

int main(int argc, char** argv) {
  try {
    bool polyphonic = false, sequence_only = false;
    std::string state_path;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--polyphonic" && !polyphonic) polyphonic = true;
      else if (arg == "--check-sequence" && !sequence_only) sequence_only = true;
      else if (arg.empty() || arg.front() == '-' || !state_path.empty()) {
        throw std::runtime_error("usage: daw_au_realtime_probe PIANO.aupreset [--polyphonic], or --check-sequence [--polyphonic]");
      } else state_path = arg;
    }
    if (sequence_only ? !state_path.empty() : state_path.empty()) {
      throw std::runtime_error("usage: daw_au_realtime_probe PIANO.aupreset [--polyphonic], or --check-sequence [--polyphonic]");
    }
    const auto workload = makeWorkload(polyphonic);
    describe(workload, polyphonic);
    if (sequence_only) {
      std::cout << "PASS MIDI workload counts, paired releases and final pedal release; no AU or device opened\n";
      return 0;
    }
    const auto size = std::filesystem::file_size(state_path);
    if (!size || size > 16U * 1024 * 1024) throw std::runtime_error("invalid state size");
    std::ifstream in(state_path, std::ios::binary);
    const std::vector<std::uint8_t> state{std::istreambuf_iterator<char>(in), {}};
    if (in.bad() || state.size() != size) throw std::runtime_error("failed to read complete state");
    daw::AudioUnitInstrument au(daw::InstrumentKind::Pianoteq9);
    au.restoreState(state);
    au.prepareRealtime();
    Probe probe(au, workload);
    daw::CoreAudioOutput output;
    std::string error;
    std::exception_ptr run_failure;
    try {
      if (!output.setAudioSource(&probe, &error) || !output.start(&error)) throw std::runtime_error(error);
      probe.armed.store(true);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
      while (probe.frame.load() < kSeconds * kRate && !probe.failed.load()) {
        if (std::chrono::steady_clock::now() > deadline || !output.checkHealth(&error)) {
          throw std::runtime_error("output failure/timeout: " + error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    } catch (...) { run_failure = std::current_exception(); }
    const auto output_diagnostics = output.diagnostics(); // device properties, control thread, before stop.
    output.stop();
    const auto timing = output.callbackTimingAfterStop();
    // Emit every measured field BEFORE either a workload or timing gate can
    // fail. Preserve the original failed run's log; do not keep only PASS.
    std::cout << output_diagnostics << '\n' << daw::formatCallbackTiming(timing) << '\n';
    std::cout << "frames=" << probe.position << " sent=" << probe.sent << " peak=" << probe.peak
              << " callback_errors=" << output.xrunCount()
              << " render_block_ratio_ge_1_count=" << probe.deadline_misses
              << " render_block_ratio_ge_0_5_count=" << probe.headroom_misses << " render_blocks=" << probe.blocks
              << " min_render_block_frames=" << probe.min_block_frames << " max_render_block_frames=" << probe.max_block_frames
              << " max_render_block_seconds=" << probe.max_seconds
              << " max_render_block_budget_ratio=" << probe.max_budget_ratio
              << " reported_plugin_latency_seconds=" << au.realtimeLatencySeconds() << '\n' << std::flush;
    if (run_failure) std::rethrow_exception(run_failure);
    if (probe.failed.load() || probe.peak <= 0 || probe.sent != workload.events.size() || output.xrunCount()) {
      throw std::runtime_error("real AU callback acceptance failed (workload, audio or callback error)");
    }
    if (!timing.callback_count || timing.invalid_budget_callbacks || timing.deadline_misses ||
        timing.max_callback_budget_ratio >= kBudgetLimit) {
      throw std::runtime_error("real AU callback acceptance failed (requires every complete client callback below 50% of its frame budget; excludes surrounding HAL/driver work)");
    }
    std::cout << "PASS 30 seconds real Pianoteq in CoreAudio callbacks, note/release offsets, CC64 and CC11; speaker output silenced\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
