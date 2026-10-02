#pragma once
#include "audio_unit_instrument.hpp"
#include "daw/parallel_render_graph.hpp"
#include <algorithm>
#include <cmath>

namespace daw {
// One prepared AU, borrowed by the session graph. Creation/state/validation on
// main/control thread; exclusive audio consumer afterwards; destroy after join.
struct TrackRenderStatistics { double peak=0; std::uint64_t note_ons=0,note_offs=0,frames=0; };
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
    for(std::size_t i=0;i<count;++i){statistics_.note_ons+=(events[i].status&0xf0)==0x90 && events[i].data2;statistics_.note_offs+=(events[i].status&0xf0)==0x80;}
    for(std::size_t i=0;i<frames*2;++i)statistics_.peak=std::max(statistics_.peak,std::abs(static_cast<double>(output[i])));
    statistics_.frames+=frames;next_frame_+=frames;
    return TrackRenderError::None;
  }
  TrackRenderStatistics statisticsAfterStop() const noexcept { return statistics_; }
  std::uint32_t componentVersion() const { return au_.componentVersion(); }
 private:
  AudioUnitInstrument au_;
  std::uint64_t next_frame_=0;
  TrackRenderStatistics statistics_;
};
}
