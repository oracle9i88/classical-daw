#pragma once
#include "audio_unit_instrument.hpp"

namespace daw {
using PianoRenderReport = InstrumentRenderReport;
// Source-compatible convenience adapter for existing piano clients.
class PianoteqAU final : public AudioUnitInstrument {
 public:
  PianoteqAU() : AudioUnitInstrument(InstrumentKind::Pianoteq9) {}
};
}  // namespace daw
