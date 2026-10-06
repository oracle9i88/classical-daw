#include "daw/document_audition.hpp"
#include "daw/ensemble_audition.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void need(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
template <class Function> std::string rejects(Function function) {
  try {
    function();
  } catch (const std::exception &error) {
    return error.what();
  }
  throw std::runtime_error("expected rejection");
}
daw::ScoreNote note(daw::Tick start, char step, int octave) {
  daw::ScoreNote result;
  result.start = start;
  result.duration = 480;
  result.pitch = {step, 0, octave};
  result.midi_channel = 0;
  return result;
}
daw::PerformanceDocument fixture() {
  daw::PerformanceDocument d;
  d.gain_db = 0;
  d.score.bpm = 120;
  d.score.parts = {
      {"piano", "Piano", {{1, 0, {note(0, 'C', 4), note(1920, 'D', 4)}, 3840, {}}}, {}},
      {"cello",
       "Cello",
       {{1, 0, {note(0, 'C', 3), note(1920, 'D', 3)}, 3840, {}}},
       {{0, daw::MidiChannelEventType::ControlChange, 0, 11, 80}}}};
  daw::assignNoteIds(d.score);
  d.performances = {daw::makePerformance(d.score, "Adapter fixture")};
  d.routes = {{"piano", "pianoteq", {1}, 0, 0, false, false, 0},
              {"cello", "swam-cello", {2}, 0, 0, false, false, 0}};
  return d;
}
struct Statistics {
  std::uint64_t frames = 0, note_ons = 0, note_offs = 0;
};
struct Lane {
  std::atomic<std::uint64_t> generation{0};
  std::atomic<bool> fail{false};
  double latency = 0;
  float value = .25F;
  Statistics statistics;
};
struct Context {
  std::vector<std::string> lifecycle;
  std::array<Lane, 2> lanes;
  unsigned attempts = 0, created = 0, destroyed = 0, fail_creation = 0;
  bool null_creation = false;
};
struct Renderer final : daw::PreparedTrackRenderer {
  std::shared_ptr<Context> context;
  std::string name;
  std::size_t index;
  Renderer(std::shared_ptr<Context> c, std::string n, std::size_t i)
      : context(std::move(c)), name(std::move(n)), index(i) {
    ++context->created;
  }
  ~Renderer() override {
    context->lifecycle.push_back("destroy:" + name);
    ++context->destroyed;
  }
  daw::RendererLatency latency() const override {
    return {context->lanes[index].latency, context->lanes[index].generation.load()};
  }
  std::uint64_t latencyGeneration() const noexcept override {
    return context->lanes[index].generation.load();
  }
  daw::TrackRenderError render(const daw::TimedMidiEvent *events, std::size_t count,
                               std::uint64_t frame, float *out,
                               std::uint32_t frames) noexcept override {
    auto &lane = context->lanes[index];
    if (lane.fail.load() || frame != lane.statistics.frames)
      return daw::TrackRenderError::ReturnedError;
    for (std::size_t i = 0; i < count; ++i) {
      lane.statistics.note_ons += (events[i].status & 0xf0) == 0x90 && events[i].data2;
      lane.statistics.note_offs += (events[i].status & 0xf0) == 0x80;
    }
    lane.statistics.frames += frames;
    std::fill_n(out, frames * 2, lane.value);
    return daw::TrackRenderError::None;
  }
  Statistics statisticsAfterStop() const noexcept { return context->lanes[index].statistics; }
};
struct Factory {
  using Renderer = ::Renderer;
  using Statistics = ::Statistics;
  std::shared_ptr<Context> context = std::make_shared<Context>();
  void validateSequence(const daw::PerformanceRoute &route,
                        const daw::MidiSampleSequence &sequence) {
    context->lifecycle.push_back("validate:" + route.part_id);
    if (route.state.front() == 0)
      throw std::invalid_argument("fake saved-state validation failed");
    need(!sequence.events.empty(), "adapter supplied empty schedule");
  }
  std::unique_ptr<Renderer> create(const daw::PerformanceRoute &route,
                                   const daw::MidiSampleSequence &) {
    ++context->attempts;
    context->lifecycle.push_back("create:" + route.part_id);
    if (context->attempts == context->fail_creation)
      throw std::runtime_error("fake plugin creation failed");
    if (context->null_creation)
      return {};
    return std::make_unique<Renderer>(context, route.part_id, route.part_id == "piano" ? 0 : 1);
  }
};
using Ensemble = daw::BasicEnsembleAudition<Factory>;

void prepareAndLifetime() {
  auto d = fixture();
  Factory factory;
  auto bad = d;
  bad.routes[1].state = {0};
  const auto preflight_error = rejects([&] { Ensemble invalid(bad, false, 0, factory); });
  need(preflight_error == "fake saved-state validation failed", preflight_error.c_str());
  need(factory.context->attempts == 0 &&
           factory.context->lifecycle ==
               std::vector<std::string>({"validate:piano", "validate:cello"}),
       "a plugin opened before later-route validation finished");

  factory = Factory{};
  factory.context->fail_creation = 2;
  rejects([&] { Ensemble invalid(d, false, 0, factory); });
  need(factory.context->created == 1 && factory.context->destroyed == 1 &&
           factory.context->lifecycle ==
               std::vector<std::string>({"validate:piano", "validate:cello", "create:piano",
                                         "create:cello", "destroy:piano"}),
       "partial construction leaked a renderer");

  factory = Factory{};
  factory.context->null_creation = true;
  rejects([&] { Ensemble invalid(d, false, 0, factory); });
  need(factory.context->created == 0, "null factory result accepted");

  factory = Factory{};
  bad = d;
  bad.routes[1].track_delay_us = -1000;
  rejects([&] { Ensemble invalid(bad, false, 0, factory); });
  need(factory.context->attempts == 0, "unsupported musical delay opened a plugin");
  bad = d;
  bad.routes[1].gain_db = 13;
  rejects([&] { Ensemble invalid(bad, false, 0, factory); });
  need(factory.context->attempts == 0, "invalid mix opened a plugin");
  {
    Ensemble healthy(d, false, 0, factory);
    need(factory.context->created == 2 && factory.context->destroyed == 0,
         "healthy preparation did not own both renderers");
  }
  need(factory.context->destroyed == 2, "healthy teardown leaked renderers");
}
void gatingAndMeters() {
  auto d = fixture();
  Factory factory;
  factory.context->lanes[0].value = 1.25F;
  factory.context->lanes[1].value = .5F;
  Ensemble silent(d, true, 7, factory);
  float out[512];
  std::fill_n(out, 512, 123.F);
  silent.render(out, 256);
  need(silent.frame() == 0 && silent.peakAfterStop() == 0 &&
           silent.trackStatisticsAfterStop()[0].frames == 0 &&
           std::all_of(out, out + 512, [](float v) { return v == 0; }),
       "unarmed audition advanced or emitted audio");
  need(silent.acceptsFormat(48000, 2) && !silent.acceptsFormat(44100, 2) &&
           !silent.acceptsFormat(48000, 1),
       "adapter admitted wrong format");
  silent.start();
  silent.render(out, 256);
  const auto stats = silent.trackStatisticsAfterStop();
  need(silent.frame() == 256 && stats[0].frames == 256 && stats[1].frames == 256 &&
           stats[0].note_ons == 1 && stats[1].note_ons == 1 && silent.peakAfterStop() == 1.75 &&
           silent.clippedAfterStop() == 512 &&
           std::all_of(out, out + 512, [](float v) { return v == 0; }),
       "silent mode skipped DSP, clipping accounting or MIDI delivery");
  need(silent.appliedRevision() == 7, "initial revision discarded");
  Ensemble audible(d, false, 0, Factory{});
  audible.start();
  audible.render(out, 256);
  need(out[0] == .5F && out[511] == .5F && audible.peakAfterStop() == .5,
       "audible adapter changed static mix");
}
void quarantineAndLatency() {
  auto d = fixture();
  Factory factory;
  Ensemble audition(d, false, 0, factory);
  float out[512]{};
  audition.start();
  audition.render(out, 256);
  factory.context->lanes[0].fail.store(true);
  audition.render(out, 256);
  need(!audition.failed() && audition.trackStatus(0).quarantined() &&
           audition.trackStatus(0).failure_frame == 256 && !audition.trackStatus(1).quarantined() &&
           out[0] == .25F && audition.statusText().find("quarantined") != std::string::npos,
       "a renderer failure was not visibly isolated");
  audition.render(out, 256);
  const auto stats = audition.trackStatisticsAfterStop();
  need(stats[0].frames == 256 && stats[1].frames == 768 && audition.frame() == 768,
       "quarantined track stopped its healthy neighbour");

  Factory changing;
  changing.context->lanes[1].latency = .02;
  Ensemble latency(d, false, 0, changing);
  need(latency.latency() == .02, "common reported latency not exposed");
  latency.start();
  latency.render(out, 256);
  changing.context->lanes[1].generation.store(1);
  latency.render(out, 256);
  need(latency.failed() && latency.fault() == daw::RenderGraphFault::LatencyChanged &&
           std::all_of(out, out + 512, [](float v) { return v == 0; }),
       "changed plugin latency did not stop safely");
  need(rejects([&] { latency.submit(d, 1); }).find("stop and rebuild") != std::string::npos,
       "failed audition accepted another edit");
}
void atomicAdmission() {
  const auto d = fixture();
  Factory factory;
  Ensemble audition(d, false, 0, factory);
  float first[512]{};
  audition.start();
  audition.render(first, 256); // Lock both opening attacks.
  std::atomic<bool> stop{false};
  std::thread audio([&] {
    float output[512]{};
    while (!stop.load()) {
      // Keep this synthetic callback before the later note, independent of
      // control-thread scheduling load. No fake admission/receipt is involved.
      if (audition.frame() < 16384)
        audition.render(output, 256);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  struct Join {
    std::atomic<bool> &stop;
    std::thread &thread;
    ~Join() {
      stop.store(true);
      thread.join();
    }
  } join{stop, audio};
  daw::WorkEditor editor(d);
  editor.setCommitAdmission(
      [&](const auto &candidate, auto revision) { audition.submit(candidate, revision); });
  auto reason = rejects([&] { editor.set({1, .1, 1, -1}); });
  need(reason.find("onset has already been processed") != std::string::npos &&
           editor.revision() == 0 && editor.document().performances[0].notes.empty() &&
           audition.appliedRevision() == 0,
       "rejected started-onset edit changed playing plan or history");
  need(editor.set({2, .02, .9, 88}) && editor.revision() == 1 && audition.appliedRevision() == 1,
       "real mailbox did not accept a legal edit after rejection");
  need(editor.undo() && audition.appliedRevision() == 2 &&
           editor.document().performances[0].notes.empty(),
       "accepted-note bookkeeping was poisoned by rejection/undo");
  auto invalid = editor.document();
  invalid.routes[1].state = {9};
  rejects([&] { audition.submit(invalid, 3); });
  need(audition.appliedRevision() == 2, "saved-state topology edit reached the mailbox");
}

struct FacadeCalls {
  int made = 0, renders = 0, starts = 0, submits = 0, ranges = 0;
  bool silent = false, capture = false;
  std::uint64_t revision = 0;
  std::size_t from = 0, until = 0;
};
FacadeCalls piano_calls, ensemble_calls;
template <bool Piano> struct FacadeInstrument {
  static FacadeCalls &calls() {
    if constexpr (Piano)
      return piano_calls;
    else
      return ensemble_calls;
  }
  FacadeInstrument(const daw::PerformanceDocument &, bool silent, bool capture,
                   std::uint64_t revision) {
    auto &c = calls();
    ++c.made;
    c.silent = silent;
    c.capture = capture;
    c.revision = revision;
  }
  FacadeInstrument(const daw::PerformanceDocument &d, bool silent, std::uint64_t revision)
      : FacadeInstrument(d, silent, false, revision) {}
  void render(float *out, std::uint32_t frames) noexcept {
    ++calls().renders;
    std::fill_n(out, frames * 2, Piano ? .1F : .2F);
  }
  void start() noexcept { ++calls().starts; }
  void prepareRange(std::size_t from, std::size_t until) {
    auto &c = calls();
    ++c.ranges;
    c.from = from;
    c.until = until;
  }
  daw::LiveSessionStream::Receipt submit(const daw::PerformanceDocument &, std::uint64_t revision) {
    ++calls().submits;
    calls().revision = revision;
    daw::LiveSessionStream::Receipt r;
    r.decision = daw::LiveSessionStream::Decision::Applied;
    return r;
  }
  bool done() const noexcept { return false; }
  bool failed() const noexcept { return false; }
  std::size_t frame() const noexcept { return Piano ? 11 : 22; }
  double latency() const noexcept { return Piano ? 0 : .02; }
  std::uint64_t appliedRevision() const noexcept { return calls().revision; }
  std::size_t appliedFrame() const noexcept { return frame(); }
  std::uint64_t suppressedConflictsAfterStop() const noexcept { return Piano ? 3 : 4; }
  std::string statusText() const { return "ensemble status"; }
};
void facadeRouting() {
  using Facade = daw::BasicDocumentAudition<FacadeInstrument<true>, FacadeInstrument<false>>;
  auto d = fixture();
  auto legacy = d;
  legacy.routes.clear();
  piano_calls = {};
  ensemble_calls = {};
  float out[4]{};
  Facade piano(legacy, true, true, 9);
  need(piano_calls.made == 1 && ensemble_calls.made == 0 && piano_calls.silent &&
           piano_calls.capture && piano.appliedRevision() == 9,
       "legacy document did not select the piano path");
  piano.prepareRange(12, 34);
  piano.start();
  piano.render(out, 2);
  piano.submit(legacy, 10);
  need(piano_calls.ranges == 1 && piano_calls.from == 12 && piano_calls.until == 34 &&
           piano_calls.starts == 1 && piano_calls.renders == 1 && piano_calls.submits == 1 &&
           out[0] == .1F && piano.frame() == 11 && piano.appliedFrame() == 11 &&
           piano.latency() == 0 && piano.suppressedConflictsAfterStop() == 3 &&
           piano.statusText().empty(),
       "piano facade forwarding failed");
  rejects([&] { Facade capture(d, false, true, 0); });
  need(ensemble_calls.made == 0, "unsupported ensemble capture created a renderer");
  Facade ensemble(d, true, false, 17);
  ensemble.prepareRange(0);
  rejects([&] { ensemble.prepareRange(1); });
  rejects([&] { ensemble.prepareRange(0, 100); });
  ensemble.start();
  ensemble.render(out, 2);
  ensemble.submit(d, 18);
  need(ensemble_calls.made == 1 && ensemble_calls.silent && !ensemble_calls.capture &&
           ensemble_calls.ranges == 0 && ensemble_calls.starts == 1 &&
           ensemble_calls.renders == 1 && ensemble_calls.submits == 1 && out[0] == .2F &&
           ensemble.frame() == 22 && ensemble.appliedFrame() == 22 && ensemble.latency() == .02 &&
           ensemble.appliedRevision() == 18 && ensemble.suppressedConflictsAfterStop() == 4 &&
           ensemble.statusText() == "ensemble status",
       "ensemble facade forwarding failed");
  need(!ensemble.done() && !ensemble.failed() && ensemble.acceptsFormat(48000, 2) &&
           !ensemble.acceptsFormat(44100, 2),
       "facade status/format forwarding failed");
  // The production facade and production ensemble adapter also compose with
  // the fake renderer factory, not only with the dispatch spies above.
  using FullFacade = daw::BasicDocumentAudition<FacadeInstrument<true>, Ensemble>;
  FullFacade full(d, false, false, 23);
  full.render(out, 2);
  need(full.frame() == 0 && out[0] == 0, "facade armed an ensemble prematurely");
  full.start();
  full.render(out, 2);
  need(full.frame() == 2 && out[0] == .5F && full.appliedRevision() == 23,
       "facade did not reach the real session renderer");
}
} // namespace
int main() {
  try {
    prepareAndLifetime();
    gatingAndMeters();
    quarantineAndLatency();
    atomicAdmission();
    facadeRouting();
    std::cout << "audition adapters: preflight/lifetime, output gate/meters, per-track quarantine, "
                 "latency fault, real mailbox rejection/recovery and document dispatch passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
