#pragma once

#include "daw/midi.hpp"
#include "daw/wav.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace daw {

struct PianoRenderReport {
  std::uint64_t sent_messages = 0;
  std::uint64_t skipped_instrument_selection = 0;
  std::uint64_t clipped_samples = 0;
  double peak = 0.0;
  double rms = 0.0;
  double last_second_rms = 0.0;
};

// Local, offline AUv2 host for the separately installed Pianoteq 9 instrument.
// No plugin UI, audio device, network, license activation or preference writing
// is requested by this host. Plugin code runs in-process, outside the realtime
// engine. Each instance renders once; reload uses a new instance and saved state.
class PianoteqAU {
 public:
  PianoteqAU();
  ~PianoteqAU();
  PianoteqAU(const PianoteqAU&) = delete;
  PianoteqAU& operator=(const PianoteqAU&) = delete;

  std::vector<std::string> factoryPresets() const;
  void selectFactoryPreset(const std::string& name);
  std::string presetName() const;
  std::uint32_t componentVersion() const;
  std::vector<std::uint8_t> state() const;
  void restoreState(const std::vector<std::uint8_t>& bytes);

  // Fixed-preset piano: bank select and program changes are counted and omitted
  // from playback. All other channel voice bytes and original channels are sent
  // unchanged. Interpretation depends on the plugin's MIDI mapping. No GM drum
  // routing, host tempo callbacks, latency compensation or automatic mastering.
  // Out-of-range samples are counted and clipped for the PCM16 export boundary.
  AudioBuffer render(const MidiFile& midi, PianoRenderReport* report = nullptr,
                     std::uint32_t rate = 48000, double tail_seconds = 5.0);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace daw
