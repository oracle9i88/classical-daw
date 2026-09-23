// Opt-in engine-defense probe, deliberately bypassing product edit admission.
// The public editor rejects retiming an already-started attack. This probe
// injects that forbidden transition directly into LivePerformanceStream to
// exercise its last-line same-key ownership defense against a real piano AU.
// Captured WAVs are callback evidence, never sources of playback.
#include "audio_unit_instrument.hpp"
#include "coreaudio_output.hpp"
#include "daw/live_performance.hpp"
#include "daw/performance.hpp"
#include "daw/wav.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace {
constexpr std::size_t kRate = 48000, kFrames = 7 * kRate;
constexpr double kGain = .251188643150958; // -12 dB, no system volume changes.
void require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}
bool noteOn(const daw::TimedMidiEvent& e) { return (e.status & 0xf0) == 0x90 && e.data2; }
bool noteOff(const daw::TimedMidiEvent& e) {
  return (e.status & 0xf0) == 0x80 || ((e.status & 0xf0) == 0x90 && !e.data2);
}
daw::MidiSampleSequence sequence(int kind) {
  daw::MidiSampleSequence s; s.frames = kFrames; s.end_frame = 5 * kRate;
  s.events = {{0, 0xb0, 64, 0}, {0, 0xb0, 11, 127}};
  auto note = [&](std::uint64_t id, std::size_t on, std::size_t off) {
    s.events.push_back({on, 0x90, 60, 96, id});
    s.events.push_back({off, 0x80, 60, 0, id});
  };
  if (kind == 0) note(1, 0, 3 * kRate); // Reference: hold, release, later reattack.
  else if (kind == 1) {
    note(1, 0, 3 * kRate / 2);
    note(2, 2 * kRate, 12 * kRate / 5);
  } else {
    // Each plan separately has no same-pitch overlap. At publication (0.5 s),
    // ID 1 is actually held. Its future attack cannot undo the past attack;
    // ID 2 at 0.8 s collides with this actual owner, despite this legal plan.
    note(1, 2 * kRate, 3 * kRate);
    note(2, 4 * kRate / 5, 6 * kRate / 5);
  }
  note(3, 4 * kRate, 9 * kRate / 2);
  for (const auto cc : {64, 66, 69, 123})
    s.events.push_back({s.end_frame, 0xb0, static_cast<std::uint8_t>(cc), 0, 0, true});
  std::stable_sort(s.events.begin(), s.events.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
  return s;
}
void checkNotes(const std::vector<daw::TimedMidiEvent>& observed) {
  std::vector<daw::TimedMidiEvent> notes;
  for (const auto& e : observed) if (noteOn(e) || noteOff(e)) notes.push_back(e);
  require(notes.size() == 4, "actual MIDI must contain only two attacks and their two owned releases");
  require(noteOn(notes[0]) && notes[0].note_id == 1 && notes[0].frame == 0 &&
          noteOff(notes[1]) && notes[1].note_id == 1 && notes[1].frame == 3 * kRate &&
          noteOn(notes[2]) && notes[2].note_id == 3 && notes[2].frame == 4 * kRate &&
          noteOff(notes[3]) && notes[3].note_id == 3 && notes[3].frame == 9 * kRate / 2,
          "collision changed the old release, retriggered it, or blocked the later independent attack");
  for (const auto& e : notes) require(e.data1 == 60, "probe unexpectedly changed pitch");
}
void checkSequence() {
  for (const bool fault : {false, true}) {
    daw::LivePerformanceStream stream(sequence(fault ? 1 : 0), kGain);
    std::vector<daw::TimedMidiEvent> actual;
    bool submitted = false;
    while (stream.frame() < kFrames) {
      if (fault && !submitted && stream.frame() >= kRate / 2) {
        stream.submit(sequence(2), kGain, 1); submitted = true;
      }
      const auto b = stream.nextBlock(256);
      require(!stream.failed() && b.frames, "pure sequence simulation failed");
      for (std::size_t i = 0; i < b.count; ++i) {
        auto e = b.events[i]; e.frame += b.frame; actual.push_back(e);
      }
    }
    checkNotes(actual);
    require(stream.suppressedConflictsAfterStop() == static_cast<unsigned>(fault) &&
            stream.liveUpdateCountAfterStop() == static_cast<unsigned>(fault),
            "fixture did not force exactly one conflicting trigger and one plan exchange");
  }
}
class Source final : public daw::AudioOutputSource {
 public:
  Source(const std::vector<std::uint8_t>& state, bool fault, bool audible)
      : au_(daw::InstrumentKind::Pianoteq9), stream_(sequence(fault ? 1 : 0), kGain),
        audible_(audible), captured_(kFrames * 2), audit_(1024) {
    au_.restoreState(state); au_.prepareRealtime();
  }
  bool acceptsFormat(double rate, std::uint32_t channels) const noexcept override { return rate == kRate && channels == 2; }
  void start() noexcept { armed_.store(true, std::memory_order_release); }
  void inject() { stream_.submit(sequence(2), kGain, 1); } // Intentionally bypasses product admission.
  void render(float* out, std::uint32_t frames) noexcept override {
    std::fill(out, out + frames * 2, 0.F);
    if (!armed_.load(std::memory_order_acquire) || failed_.load()) return;
    const auto b = stream_.nextBlock(frames);
    if (stream_.failed() || b.count > audit_.size() - audit_count_ || b.frame + b.frames > kFrames ||
        b.frame != published_.load(std::memory_order_relaxed)) { failed_.store(true); return; }
    if (!b.frames) return;
    if (!au_.renderRealtime(b.events, b.count, out, b.frames)) { failed_.store(true); return; }
    // Record messages only after their real AU delivery succeeded.
    for (std::size_t i = 0; i < b.count; ++i) { auto e = b.events[i]; e.frame += b.frame; audit_[audit_count_++] = e; }
    for (std::size_t i = 0; i < b.frames * 2; ++i) {
      const auto value = static_cast<double>(out[i]) * kGain;
      if (!std::isfinite(value)) { failed_.store(true); out[i] = 0; continue; }
      peak_ = std::max(peak_, std::abs(value));
      if (std::abs(value) > 1) ++clipped_;
      captured_[b.frame * 2 + i] = static_cast<float>(value);
      out[i] = audible_ ? static_cast<float>(std::clamp(value, -1., 1.)) : 0.F;
    }
    published_.store(b.frame + b.frames, std::memory_order_release);
  }
  bool failed() const noexcept { return failed_.load() || stream_.failed(); }
  std::size_t frame() const noexcept { return published_.load(std::memory_order_acquire); }
  std::size_t appliedFrame() const noexcept { return stream_.appliedFrame(); }
  std::uint64_t revision() const noexcept { return stream_.appliedRevision(); }
  std::uint64_t conflicts() const noexcept { return stream_.suppressedConflictsAfterStop(); }
  std::uint64_t updates() const noexcept { return stream_.liveUpdateCountAfterStop(); }
  double latency() const noexcept { return au_.realtimeLatencySeconds(); }
  double peak() const noexcept { return peak_; }
  std::uint64_t clipped() const noexcept { return clipped_; }
  const std::vector<float>& audio() const noexcept { return captured_; }
  std::vector<daw::TimedMidiEvent> events() const { return {audit_.begin(), audit_.begin() + static_cast<std::ptrdiff_t>(audit_count_)}; }
 private:
  daw::AudioUnitInstrument au_; daw::LivePerformanceStream stream_;
  bool audible_; std::vector<float> captured_; std::vector<daw::TimedMidiEvent> audit_;
  std::size_t audit_count_ = 0; double peak_ = 0; std::uint64_t clipped_ = 0;
  std::atomic<bool> armed_{false}, failed_{false}; std::atomic<std::size_t> published_{0};
};
double epochSeconds() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
double rms(const std::vector<float>& audio, std::size_t begin, std::size_t end) {
  require(begin < end && end <= audio.size() / 2, "RMS window is outside capture");
  double sum = 0;
  for (auto i = begin * 2; i < end * 2; ++i) {
    require(std::isfinite(audio[i]), "nonfinite callback capture"); sum += static_cast<double>(audio[i]) * audio[i];
  }
  return std::sqrt(sum / static_cast<double>((end - begin) * 2));
}
struct Window { double before, across, after, minimum; std::size_t silence; };
Window window(const std::vector<float>& audio, std::size_t center) {
  Window w{rms(audio, center - 960, center), rms(audio, center - 240, center + 240),
           rms(audio, center, center + 960), 1, 0};
  for (auto f = center - 960; f + 480 <= center + 960; f += 120) w.minimum = std::min(w.minimum, rms(audio, f, f + 480));
  std::size_t length = 0;
  for (auto f = center - 960; f < center + 960; ++f) {
    length = std::max(std::abs(audio[f * 2]), std::abs(audio[f * 2 + 1])) <= 1e-10F ? length + 1 : 0;
    w.silence = std::max(w.silence, length);
  }
  return w;
}
struct Run { std::vector<float> audio; std::vector<daw::TimedMidiEvent> events; bool passed = false; };
Run run(const std::vector<std::uint8_t>& state, bool fault, bool audible, const fs::path& root, std::ostream& report) {
  const std::string name = fault ? "injected-conflict" : "reference";
  std::cout << "Starting " << name << ": C4 held until 3 s; independent C4 at 4 s; "
            << (audible ? "speaker output enabled" : "speaker output silenced") << '\n' << std::flush;
  report << "run_attempt=" << name << " initialization_epoch_seconds=" << epochSeconds() << '\n'; report.flush();
  Source source(state, fault, audible); daw::CoreAudioOutput output; std::string error;
  require(output.setAudioSource(&source, &error), "cannot attach source");
  if (!output.start(&error)) throw std::runtime_error(error);
  const auto device = output.diagnostics();
  const auto started = epochSeconds(); source.start();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  bool injected = false, healthy = true; std::size_t before = 0, after = 0;
  try {
    while (source.frame() < kFrames && !source.failed()) {
      if (fault && !injected && source.frame() >= kRate / 2) {
        before = source.frame(); source.inject(); after = source.frame(); injected = true;
      }
      if (std::chrono::steady_clock::now() > deadline || !output.checkHealth(&error)) { healthy = false; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } catch (const std::exception& e) {
    healthy = false; error = std::string("runtime exception: ") + e.what();
  }
  output.stop(); const auto ended = epochSeconds(); const auto timing = output.callbackTimingAfterStop();
  Run result{source.audio(), source.events(), false};
  report << "run=" << name << " start_epoch_seconds=" << started << " stop_epoch_seconds=" << ended << '\n'
         << "au_instances=1 output_starts=1 output_stops=1 speaker_output=" << (audible ? "audible" : "silenced") << '\n'
         << "device=" << device << '\n' << daw::formatCallbackTiming(timing) << '\n'
         << "callback_frames=" << source.frame() << " callback_errors=" << output.xrunCount()
         << " source_failed=" << source.failed() << " output_healthy=" << healthy << " health_error=" << std::quoted(error)
         << " peak=" << source.peak() << " clipped_samples=" << source.clipped() << " plugin_latency_seconds=" << source.latency() << '\n'
         << "applied_live_updates=" << source.updates() << " applied_revision=" << source.revision()
         << " suppressed_conflicts=" << source.conflicts() << " submit_begin_frame=" << before
         << " submit_return_frame=" << after << " applied_frame=" << source.appliedFrame() << '\n';
  bool continuity = true;
  for (const auto at : {4 * kRate / 5, 6 * kRate / 5}) {
    const auto w = window(result.audio, at);
    report << "guarded_boundary_frame=" << at << " before_rms=" << w.before << " across_rms=" << w.across
           << " after_rms=" << w.after << " minimum_10ms_rms=" << w.minimum << " longest_silent_frames=" << w.silence << '\n';
    continuity = continuity && w.minimum > 1e-6 && w.silence < 48;
  }
  const auto held = rms(result.audio, 13 * kRate / 10, 14 * kRate / 10);
  const auto preattack = rms(result.audio, 38 * kRate / 10, 39 * kRate / 10);
  const auto reattack = rms(result.audio, 402 * kRate / 100, 412 * kRate / 100);
  report << "held_after_suppressed_release_rms=" << held << " before_later_reattack_rms=" << preattack
         << " later_reattack_rms=" << reattack << '\n';
  std::ofstream midi(root / (name + "-actual-midi.tsv"));
  midi << "frame\tseconds\tstatus\tdata1\tdata2\tperformed_id\tterminal_reset\n";
  for (const auto& e : result.events) midi << e.frame << '\t' << static_cast<double>(e.frame) / kRate << '\t'
      << static_cast<unsigned>(e.status) << '\t' << static_cast<unsigned>(e.data1) << '\t'
      << static_cast<unsigned>(e.data2) << '\t' << e.note_id << '\t' << e.terminal_reset << '\n';
  midi.close(); require(static_cast<bool>(midi), "cannot write actual MIDI audit");
  require(daw::writeWavPcm16({48000, 2, result.audio}, (root / (name + "-callback.wav")).string(), &error), "cannot write callback WAV evidence");
  bool notes_ok = true;
  try { checkNotes(result.events); } catch (const std::exception& e) { notes_ok = false; report << "note_gate_error=" << std::quoted(e.what()) << '\n'; }
  const auto expected = static_cast<unsigned>(fault);
  const bool swap_ok = !fault || (source.appliedFrame() >= before && source.appliedFrame() <= after + 256 &&
                                 source.appliedFrame() < 4 * kRate / 5);
  result.passed = healthy && !source.failed() && source.frame() == kFrames && !output.xrunCount() &&
      source.peak() > 1e-5 && !source.clipped() && source.updates() == expected && source.conflicts() == expected &&
      source.revision() == expected && swap_ok && notes_ok && continuity && held > 1e-5 &&
      reattack > 1e-5 && reattack > preattack * 1.5 && timing.callback_count > 0 &&
      !timing.invalid_budget_callbacks && !timing.deadline_misses && timing.max_callback_budget_ratio < .5;
  report << "actual_note_on_count=" << std::count_if(result.events.begin(), result.events.end(), noteOn)
         << " actual_note_off_count=" << std::count_if(result.events.begin(), result.events.end(), noteOff)
         << " note_ownership_gate=" << notes_ok << " continuity_gate=" << continuity
         << " full_callback_budget_limit=0.5 run_gate=" << (result.passed ? "PASS" : "FAIL") << '\n';
  for (const auto id : {1ULL, 2ULL, 3ULL}) {
    report << "performed_id=" << id << " actual_note_on_count="
           << std::count_if(result.events.begin(), result.events.end(), [id](const auto& e) { return e.note_id == id && noteOn(e); })
           << " actual_note_off_count="
           << std::count_if(result.events.begin(), result.events.end(), [id](const auto& e) { return e.note_id == id && noteOff(e); }) << '\n';
  }
  report.flush(); return result;
}
} // namespace
int main(int argc, char** argv) {
  std::ofstream report;
  try {
    if (argc == 2 && std::string(argv[1]) == "--check-sequence") {
      checkSequence(); std::cout << "PASS engine-defense fixture: one forced collision, no stolen release, one later reattack; no AU or device opened\n"; return 0;
    }
    require(argc == 3 || (argc == 4 && std::string(argv[3]) == "--audible"),
            "usage: daw_performance_conflict_probe DOCUMENT NEW_DIRECTORY [--audible], or --check-sequence");
    checkSequence(); const auto d = daw::loadPerformanceDocument(argv[1]); const fs::path root(argv[2]);
    require(!fs::exists(fs::symlink_status(root)) && fs::create_directory(root), "output directory must be new");
    report.open(root / "evidence.txt"); require(static_cast<bool>(report), "cannot create evidence file"); report.precision(15);
    report << "test_kind=engine_defense_fault_injection product_admission=bypassed_by_design\n"
           << "listening_confirmation=not_automated audible_mode=" << (argc == 4) << '\n'
           << "audio_relative_rms_limit=0.02 audio_max_sample_delta_limit=0.01\n";
    const auto reference = run(d.piano_state, false, argc == 4, root, report);
    const auto fault = run(d.piano_state, true, argc == 4, root, report);
    double error_energy = 0, reference_energy = 0, max_delta = 0;
    for (std::size_t i = 0; i < reference.audio.size(); ++i) {
      const auto delta = static_cast<double>(fault.audio[i]) - reference.audio[i];
      error_energy += delta * delta; reference_energy += static_cast<double>(reference.audio[i]) * reference.audio[i];
      max_delta = std::max(max_delta, std::abs(delta));
    }
    const auto relative = std::sqrt(error_energy / std::max(reference_energy, 1e-30));
    const bool same_midi = reference.events.size() == fault.events.size() &&
        std::equal(reference.events.begin(), reference.events.end(), fault.events.begin(), [](const auto& a, const auto& b) {
          return a.frame == b.frame && a.status == b.status && a.data1 == b.data1 && a.data2 == b.data2 &&
                 a.note_id == b.note_id && a.terminal_reset == b.terminal_reset;
        });
    const bool passed = reference.passed && fault.passed && same_midi && relative <= .02 && max_delta <= .01;
    report << "reference_vs_injected_relative_rms_error=" << relative << " max_sample_delta=" << max_delta << '\n'
           << "actual_delivered_midi_equal=" << same_midi << '\n'
           << (passed ? "PASS" : "FAIL") << " callback audio A/B, actual same-key ownership and complete callback headroom\n";
    report.close(); require(static_cast<bool>(report), "cannot finish evidence file");
    std::ifstream result(root / "evidence.txt"); std::cout << result.rdbuf() << std::flush;
    require(passed, "same-key conflict hardware gate failed; full measurements and captures retained");
  } catch (const std::exception& e) {
    if (report.is_open()) { report << "FAIL exception=" << std::quoted(e.what()) << '\n'; report.flush(); }
    std::cerr << e.what() << '\n'; return 1;
  }
}
