#include "daw/realtime.hpp"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

class RecordingRenderer final : public daw::InstrumentRenderer {
 public:
  bool prepare(const daw::InstrumentRenderConfig&) noexcept override { return true; }
  void reset() noexcept override { count = 0; }
  bool enqueue(const daw::VoiceEvent& event) noexcept override {
    if (count >= events.size()) return false;
    events[count++] = event;
    return true;
  }
  void render(float*, std::uint32_t, std::uint32_t, double) noexcept override {}

  std::array<daw::VoiceEvent, 16> events{};
  std::size_t count = 0;
};

int fail(const char* message) {
  std::cerr << "FAIL: " << message << '\n';
  return 1;
}

}  // namespace

int main() {
  using namespace daw;

  RecordingRenderer renderer;
  BlockScheduler scheduler;
  scheduler.setInstrumentRenderer(&renderer);
  if (!scheduler.enqueue({TransportCommandType::Start, 0, 120.0})) {
    return fail("start enqueue");
  }
  if (!scheduler.enqueueVoiceEvent({VoiceEventType::NoteOn, 60, 100, 100}) ||
      !scheduler.enqueueVoiceEvent({VoiceEventType::NoteOff, 60, 0, 128}) ||
      !scheduler.enqueueVoiceEvent({VoiceEventType::NoteOn, 64, 90, 300})) {
    return fail("timestamped event enqueue");
  }

  scheduler.processBlock(128);
  if (renderer.count != 1 || renderer.events[0].pitch != 60 ||
      renderer.events[0].sample_position != 100 || scheduler.pendingVoiceEventCount() != 2) {
    return fail("first block event dispatch");
  }
  // An event at the exact block end belongs to the following block.
  scheduler.processBlock(128);
  if (renderer.count != 2 || renderer.events[1].type != VoiceEventType::NoteOff ||
      renderer.events[1].sample_position != 128 || scheduler.pendingVoiceEventCount() != 1) {
    return fail("block-end event dispatch");
  }
  scheduler.processBlock(128);
  if (renderer.count != 3 || renderer.events[2].pitch != 64 || scheduler.pendingVoiceEventCount() != 0) {
    return fail("future event retention");
  }

  // A timestamp behind the transport is dispatched at the next callback and
  // counted as late rather than being silently lost.
  if (!scheduler.enqueueVoiceEvent({VoiceEventType::NoteOn, 67, 80, 1})) {
    return fail("late event enqueue");
  }
  scheduler.processBlock(128);
  if (scheduler.snapshot().voice_event_late_count != 1 || renderer.count != 4) {
    return fail("late event accounting");
  }

  // The producer-facing queue is bounded and reports overflow without
  // allocating or blocking.
  BlockScheduler bounded;
  for (std::size_t index = 0; index < BlockScheduler::kVoiceEventCapacity; ++index) {
    if (!bounded.enqueueVoiceEvent({VoiceEventType::NoteOn, 60, 1, 1000000 + static_cast<SampleIndex>(index)})) {
      return fail("bounded queue unexpectedly rejected event");
    }
  }
  if (bounded.enqueueVoiceEvent({VoiceEventType::NoteOn, 60, 1, 1000000})) {
    return fail("bounded queue overflow accounting");
  }
  bounded.processBlock(64);
  if (bounded.pendingVoiceEventCount() != BlockScheduler::kVoiceEventCapacity ||
      bounded.snapshot().voice_event_drop_count != 1) {
    return fail("future event queue retention");
  }

  return 0;
}
