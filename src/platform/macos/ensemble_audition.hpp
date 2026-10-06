#pragma once
#include "audio_unit_track_renderer.hpp"
#include "audio_unit_runtime.hpp"
#include "daw/ensemble_audition.hpp"

namespace daw {
// Platform-specific policy only. The portable adapter executes the same graph,
// whole-session admission, output gate, metering and lifetime in tests and CLI.
struct AudioUnitAuditionFactory {
  using Renderer = AudioUnitTrackRenderer;
  using Statistics = TrackRenderStatistics;
  static InstrumentKind kind(const PerformanceRoute &route) {
    if (route.instrument == "pianoteq")
      return InstrumentKind::Pianoteq9;
    if (route.instrument == "swam-cello")
      return InstrumentKind::SwamCello3;
    throw std::invalid_argument("unsupported realtime instrument");
  }
  void validateSequence(const PerformanceRoute &route, const MidiSampleSequence &sequence) const {
    if (kind(route) != InstrumentKind::SwamCello3)
      return;
    requireInitialExpression(sequence);
    const auto transpose = swamCelloStateTranspose(route.state);
    for (const auto &event : sequence.events) {
      if ((event.status & 0xf0) != 0x90 || !event.data2)
        continue;
      const auto pitch = static_cast<int>(event.data1) + transpose;
      if (pitch < 36 || pitch > 89)
        throw std::invalid_argument("SWAM note outside saved-state range; edit was not saved");
    }
  }
  std::unique_ptr<Renderer> create(const PerformanceRoute &route,
                                   const MidiSampleSequence &sequence) const {
    return std::make_unique<Renderer>(kind(route), route.state, sequence);
  }
};
using EnsembleAudition = BasicEnsembleAudition<AudioUnitAuditionFactory>;
} // namespace daw
