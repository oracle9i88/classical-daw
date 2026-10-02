#pragma once
#include "audio_unit_instrument.hpp"
#include "daw/parallel_render_graph.hpp"

namespace daw {
// One prepared AU, borrowed by the session graph. Creation/state/validation on
// main/control thread; exclusive audio consumer afterwards; destroy after join.
class AudioUnitTrackRenderer final : public PreparedTrackRenderer {
 public:
  AudioUnitTrackRenderer(InstrumentKind kind, const std::vector<std::uint8_t>& state,
                        const MidiSampleSequence& sequence) : au_(kind) {
    au_.restoreState(state); au_.prepareRealtime(sequence);
  }
  RendererLatency latency() const override {
    return {au_.realtimeLatencySeconds(),au_.preparedLatencyGeneration()};
  }
  std::uint64_t latencyGeneration() const noexcept override { return au_.realtimeLatencyGeneration(); }
  TrackRenderError render(const TimedMidiEvent* events, std::size_t count,
      std::uint64_t frame, float* output, std::uint32_t frames) noexcept override {
    if (frame != next_frame_ || !au_.renderRealtime(events,count,output,frames))
      return TrackRenderError::ReturnedError;
    next_frame_+=frames;
    return TrackRenderError::None;
  }
  std::uint32_t componentVersion() const { return au_.componentVersion(); }
 private:
  AudioUnitInstrument au_;
  std::uint64_t next_frame_=0;
};
}
