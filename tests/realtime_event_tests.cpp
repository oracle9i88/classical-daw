#include "daw/realtime.hpp"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

class RecordingRenderer final : public daw::InstrumentRenderer {
 public:
  bool prepare(const daw::InstrumentRenderConfig&) noexcept override { return true; }
  void reset() noexcept override { count = 0; }
  bool enqueue(const daw::TimedMidiEvent& event) noexcept override {
    if (count >= events.size()) return false;
    events[count++] = event;
    return true;
  }
  void render(float*, std::uint32_t, std::uint32_t, double) noexcept override {}

  std::array<daw::TimedMidiEvent, 16> events{};
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
  if (renderer.count != 1 || renderer.events[0].data1 != 60 ||
      renderer.events[0].frame != 100 || scheduler.pendingVoiceEventCount() != 2) {
    return fail("first block event dispatch");
  }
  // An event at the exact block end belongs to the following block.
  scheduler.processBlock(128);
  if (renderer.count != 2 || renderer.events[1].status != 0x80 ||
      renderer.events[1].frame != 128 || scheduler.pendingVoiceEventCount() != 1) {
    return fail("block-end event dispatch");
  }
  scheduler.processBlock(128);
  if (renderer.count != 3 || renderer.events[2].data1 != 64 || scheduler.pendingVoiceEventCount() != 0) {
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

  RecordingRenderer midi_renderer;
  BlockScheduler midi_scheduler;
  midi_scheduler.setInstrumentRenderer(&midi_renderer);
  midi_scheduler.enqueue({TransportCommandType::Start,0,120});
  if (!midi_scheduler.enqueueMidiEvent({7,0xb3,64,127}) ||
      !midi_scheduler.enqueueMidiEvent({8,0xb3,11,96}) ||
      !midi_scheduler.enqueueMidiEvent({9,0xe3,1,64}) ||
      !midi_scheduler.enqueueMidiEvent({10,0xd3,72,0})) return fail("full MIDI enqueue");
  midi_scheduler.processBlock(64);
  if (midi_renderer.count != 4 || midi_renderer.events[0].status != 0xb3 ||
      midi_renderer.events[1].data1 != 11 || midi_renderer.events[1].data2 != 96 ||
      midi_renderer.events[2].frame != 9 || midi_renderer.events[2].status != 0xe3 ||
      midi_renderer.events[3].status != 0xd3) return fail("controller/bend/pressure bytes lost");
  if (midi_scheduler.enqueueMidiEvent({0,0xf0,0,0}) ||
      midi_scheduler.enqueueMidiEvent({0,0xb0,128,0})) return fail("invalid MIDI accepted");
  return 0;
}
