// Opt-in hardware acceptance for a SINGLE uninterrupted AU/CoreAudio run.
// Audio comes from the live callbacks; the captured WAV is evidence, never a
// playback source. The default (and only) speaker policy is silence.
#include "performance_audition.hpp"
#include "coreaudio_output.hpp"
#include "daw/wav.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace {
constexpr std::size_t kRate = 48000;
constexpr std::uint64_t kTie = 8, kFutureNote = 10;
void require(bool condition, const char* why) {
  if (!condition) throw std::runtime_error(why);
}
bool noteOn(const daw::TimedMidiEvent& event) {
  return (event.status & 0xf0) == 0x90 && event.data2;
}
bool noteOff(const daw::TimedMidiEvent& event) {
  return (event.status & 0xf0) == 0x80 || ((event.status & 0xf0) == 0x90 && !event.data2);
}
struct NoteTimes { std::size_t on = 0, off = 0; };
NoteTimes times(const std::vector<daw::TimedMidiEvent>& events, std::uint64_t id) {
  NoteTimes result;
  std::size_t attacks = 0, releases = 0;
  for (const auto& event : events) if (event.note_id == id) {
    if (noteOn(event)) { result.on = event.frame; ++attacks; }
    if (noteOff(event)) { result.off = event.frame; ++releases; }
  }
  require(attacks == 1 && releases == 1 && result.off > result.on,
          "observed performed note must have exactly one attack and one release");
  return result;
}
void checkFixture(const daw::PerformanceDocument& document) {
  require(document.score.parts.size() == 1 && document.score.parts.front().measures.size() == 8,
          "probe requires the one-part eight-bar acceptance fixture");
  const auto& take = document.performances.at(document.active);
  const auto mapping = std::find_if(take.mapping.begin(), take.mapping.end(), [](const auto& item) { return item.id == kTie; });
  require(mapping != take.mapping.end() && mapping->notation_ids == std::vector<std::uint64_t>{8, 9},
          "probe requires performed note 8 mapped to tied notation segments 8 and 9");
  const auto plan = daw::compilePerformance(document.score, take);
  const auto tie = times(plan.events, kTie);
  const auto future = times(plan.events, kFutureNote);
  require(tie.on == 168000 && tie.off == 210000 && future.on == 240000,
          "probe fixture timing differs from the declared 3.5–4.375 second tie / 5 second future note");
  require(std::any_of(take.curves.begin(), take.curves.end(), [](const auto& curve) { return curve.controller == 64; }),
          "probe requires a pedal curve");
  const auto expression = std::find_if(take.curves.begin(), take.curves.end(), [](const auto& curve) {
    return curve.id == 2 && curve.controller == 11 && curve.channel == 0;
  });
  require(expression != take.curves.end() && std::any_of(expression->points.begin(), expression->points.end(), [](const auto& point) {
    return point.id == 2 && point.seconds == 4 && point.value == 120;
  }), "probe requires CC11 curve 2, point 2 at 4 seconds / value 120");
}
struct Swap {
  std::uint64_t revision = 0;
  std::size_t before_submit = 0, after_submit = 0, applied = 0;
};
void checkProgress(daw::PerformanceAudition& source, daw::CoreAudioOutput& output,
                   std::size_t& previous, const std::chrono::steady_clock::time_point& deadline) {
  std::string error;
  require(!source.failed(), "live AU source failed");
  if (std::chrono::steady_clock::now() > deadline || !output.checkHealth(&error)) {
    throw std::runtime_error("live output timeout/health failure: " + error);
  }
  const auto frame = source.frame();
  require(frame >= previous, "transport moved backwards during live editing");
  previous = frame;
}
double rms(const std::vector<float>& audio, std::size_t begin, std::size_t end) {
  require(begin < end && end <= audio.size() / 2, "RMS window exceeds callback capture");
  double energy = 0;
  for (auto i = begin * 2; i < end * 2; ++i) {
    require(std::isfinite(audio[i]), "nonfinite callback capture");
    energy += static_cast<double>(audio[i]) * audio[i];
  }
  return std::sqrt(energy / static_cast<double>((end - begin) * 2));
}
struct Continuity { double before = 0, across = 0, after = 0, minimum_window = 0; std::size_t silent_frames = 0; };
Continuity checkContinuity(const std::vector<float>& audio, std::size_t boundary) {
  // Fixed gates declared before running: every 10 ms window in the +/-20 ms
  // neighborhood must exceed 1e-6 RMS, with no >=1 ms silent run. This tests
  // gaps at swaps, not arbitrary clicklessness or timbral transparency.
  require(boundary >= 960 && boundary + 960 <= audio.size() / 2, "swap has no full continuity window");
  Continuity result;
  result.before = rms(audio, boundary - 960, boundary);
  result.across = rms(audio, boundary - 240, boundary + 240);
  result.after = rms(audio, boundary, boundary + 960);
  result.minimum_window = 1;
  for (std::size_t frame = boundary - 960; frame + 480 <= boundary + 960; frame += 120) {
    result.minimum_window = std::min(result.minimum_window, rms(audio, frame, frame + 480));
  }
  std::size_t silent_run = 0;
  for (std::size_t frame = boundary - 960; frame < boundary + 960; ++frame) {
    if (std::max(std::abs(audio[frame * 2]), std::abs(audio[frame * 2 + 1])) <= 1e-10F) ++silent_run;
    else silent_run = 0;
    result.silent_frames = std::max(result.silent_frames, silent_run);
  }
  return result;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const bool fixture_only = argc == 3 && std::string(argv[1]) == "--check-fixture";
    require(argc == 3, "usage: daw_performance_live_probe DOCUMENT NEW_DIRECTORY, or --check-fixture DOCUMENT");
    auto document = daw::loadPerformanceDocument(argv[fixture_only ? 2 : 1]);
    checkFixture(document);
    daw::WorkEditor editor(document);
    if (fixture_only) {
      require(editor.set({kFutureNote, .12, 1, -1}), "future-note edit was a no-op");
      require(editor.setCurvePoint(2, 2, 45), "expression edit was a no-op");
      require(editor.set({kTie, 0, 1.25, -1}), "tie-duration edit was a no-op");
      const auto expected = daw::compilePerformance(editor.document().score,
          editor.document().performances.at(editor.document().active));
      require(times(expected.events, kTie).off == 220500 && times(expected.events, kFutureNote).on == 245760,
              "edited fixture timing is wrong");
      std::cout << "PASS live fixture, tie identities, mandatory curves and three edits; no AU or device opened\n";
      return 0;
    }
    const fs::path root(argv[2]);
    require(!fs::exists(fs::symlink_status(root)) && fs::create_directory(root), "output directory must be new");
    // Exactly one source/AU and one output device lifetime; start/stop each once.
    daw::PerformanceAudition source(document, true, true);
    daw::CoreAudioOutput output;
    std::string error;
    require(output.setAudioSource(&source, &error), "cannot attach live source");
    if (!output.start(&error)) throw std::runtime_error(error);
    source.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    std::size_t previous = 0;
    const std::array<std::size_t, 3> targets{172800, 182400, 192000}; // 3.6, 3.8, 4.0 s, all inside original tie.
    std::array<Swap, 3> swaps{};
    Swap* current_swap = nullptr;
    editor.setCommitAdmission([&](const daw::PerformanceDocument& candidate, std::uint64_t revision) {
      require(current_swap != nullptr, "live command has no probe receipt");
      current_swap->revision = revision;
      current_swap->before_submit = source.frame();
      source.submit(candidate, revision);
      current_swap->after_submit = source.frame(); // no fallible work after publication
    });
    for (std::size_t edit = 0; edit < targets.size(); ++edit) {
      while (source.frame() < targets[edit]) {
        checkProgress(source, output, previous, deadline);
        require(!source.done(), "transport ended before a planned live edit");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      checkProgress(source, output, previous, deadline);
      auto& swap = swaps[edit];
      current_swap = &swap;
      if (edit == 0) require(editor.set({kFutureNote, .12, 1, -1}), "future-note edit failed");
      else if (edit == 1) require(editor.setCurvePoint(2, 2, 45), "expression edit failed");
      else require(editor.set({kTie, 0, 1.25, -1}), "tie-duration edit failed");
      current_swap = nullptr;
      require(editor.revision() == swap.revision, "document and playback revisions diverged");
      while (source.appliedRevision() < swap.revision) {
        checkProgress(source, output, previous, deadline);
        require(!source.done(), "transport ended with unapplied live edit");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      swap.applied = source.appliedFrame();
      require(source.appliedRevision() == swap.revision && swap.applied >= swap.before_submit &&
                  swap.applied <= swap.after_submit + 256,
              "live revision missed the next eligible render-block boundary");
      require(swap.applied > 168000 && swap.applied < 210000,
              "live edit was not applied while the original cross-bar tie was held");
    }
    while (!source.done()) {
      checkProgress(source, output, previous, deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    checkProgress(source, output, previous, deadline);
    output.stop();
    // Preserve the measured load in the console log even when a later gate
    // rejects it. An unsuccessful run must not hide its timing evidence.
    std::cout << "measured_callback_frames=" << source.frame() << " callback_errors=" << output.xrunCount()
              << " peak=" << source.peakAfterStop() << " deadline_misses=" << source.deadlineMissesAfterStop()
              << " worst_render_block_budget_ratio=" << source.worstBudgetRatioAfterStop() << '\n' << std::flush;
    require(!source.failed() && source.peakAfterStop() > 0 && !source.clippedAfterStop() && !output.xrunCount(),
            "uninterrupted callback playback failed, clipped or remained silent");
    require(source.deadlineMissesAfterStop() == 0 && source.worstBudgetRatioAfterStop() < .5,
            "live rendering including plan swaps exceeded the 50% block-budget headroom gate");
    require(source.appliedRevision() == 3 && source.frame() >= 21 * kRate,
            "live transport did not reach the final revision and tail");
    require(source.liveUpdateCountAfterStop() == 3 && source.suppressedConflictsAfterStop() == 0,
            "live revisions were skipped or required suppressing conflicting note ownership");
    const auto& capture = source.capturedAfterStop();
    require(capture.size() == source.frame() * 2, "callback capture does not match the uninterrupted transport");
    const auto& observed = source.eventsAfterStop();
    require(std::is_sorted(observed.begin(), observed.end(), [](const auto& left, const auto& right) {
      return left.frame < right.frame;
    }), "AU delivery timeline moved backwards");
    const auto expected = daw::compilePerformance(editor.document().score,
        editor.document().performances.at(editor.document().active));
    for (const auto& mapping : editor.document().performances.at(editor.document().active).mapping) {
      const auto actual_note = times(observed, mapping.id);
      const auto expected_note = times(expected.events, mapping.id);
      require(actual_note.on == expected_note.on && actual_note.off == expected_note.off,
              "actual AU note ownership/timing differs from the final performance");
    }
    const auto tie = times(observed, kTie);
    const auto future = times(observed, kFutureNote);
    require(tie.on == 168000 && tie.off == 220500,
            "cross-page tie was retriggered, cut short or released at the obsolete time");
    require(future.on == 245760, "future-note onset edit did not reach the same AU run");
    require(std::any_of(observed.begin(), observed.end(), [&](const auto& event) {
      return event.frame == swaps[1].applied && event.status == 0xb0 && event.data1 == 11 && event.data2 < 60;
    }), "updated CC11 was not chased into the first block of its revision");
    std::ostringstream evidence;
    evidence.precision(10);
    evidence << "au_instances=1 output_starts=1 output_stops=1 speaker_output=silenced\n"
             << "callback_frames=" << source.frame() << " callback_errors=" << output.xrunCount()
             << " peak=" << source.peakAfterStop() << " reported_plugin_latency_seconds=" << source.latency() << '\n'
             << "deadline_misses=" << source.deadlineMissesAfterStop()
             << " worst_render_block_budget_ratio=" << source.worstBudgetRatioAfterStop() << " budget_limit=0.5\n"
             << "applied_live_updates=" << source.liveUpdateCountAfterStop()
             << " suppressed_conflicts=" << source.suppressedConflictsAfterStop() << '\n'
             << "tie_performed_id=8 note_on_count=1 note_off_count=1 on_frame=" << tie.on << " off_frame=" << tie.off << '\n'
             << "future_performed_id=10 edited_on_frame=" << future.on << '\n';
    for (const auto& swap : swaps) {
      const auto continuity = checkContinuity(capture, swap.applied);
      evidence << "revision=" << swap.revision << " submit_begin_frame=" << swap.before_submit
               << " submit_return_frame=" << swap.after_submit << " applied_frame=" << swap.applied
               << " before_rms=" << continuity.before << " across_rms=" << continuity.across
               << " after_rms=" << continuity.after << " minimum_10ms_rms=" << continuity.minimum_window
               << " longest_silent_frames=" << continuity.silent_frames << '\n';
      if (!(continuity.minimum_window > 1e-6 && continuity.silent_frames < 48)) {
        std::cerr << evidence.str();
        throw std::runtime_error("callback audio contains a silence gap around a live plan swap");
      }
    }
    require(daw::writeWavPcm16({48000, 2, capture}, (root / "live-callback.wav").string(), &error),
            "cannot write actual callback capture");
    daw::savePerformanceDocument(editor.document(), (root / "edited").string());
    evidence << "PASS one uninterrupted AU/CoreAudio run, three live revisions, future onset and CC11 delivered, one sustained tie attack/release, nonzero callback audio across swaps\n";
    std::ofstream report(root / "evidence.txt", std::ios::binary);
    report << evidence.str();
    report.close();
    require(static_cast<bool>(report), "cannot write live evidence report");
    std::cout << evidence.str();
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
