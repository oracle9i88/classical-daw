#pragma once

#include "daw/midi.hpp"
#include "daw/midi_sequence.hpp"
#include <functional>
#include "daw/wav.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace daw {

enum class InstrumentKind { Pianoteq9, SwamCello3 };
struct InstrumentDescriptor {
  std::uint32_t subtype;
  std::uint32_t manufacturer;
  const char* id;
  const char* name;
  const char* default_preset;
  const char* component_id;
  double startup_seconds;
};
const InstrumentDescriptor& instrumentDescriptor(InstrumentKind kind);
// No plugin loading. Saved SWAM state is checked even for frozen audio reuse.
void validateInstrumentStatePerformance(InstrumentKind kind, const std::vector<std::uint8_t>& state, const MidiFile& midi);


struct InstrumentRenderReport {
  std::uint64_t sent_messages = 0;
  std::uint64_t skipped_instrument_selection = 0;
  std::uint64_t clipped_samples = 0;
  std::uint64_t over_unity_samples = 0;
  double peak = 0.0;
  double rms = 0.0;
  double last_second_rms = 0.0;
};

// Local, offline AUv2 host for explicitly supported, separately installed instruments.
// No plugin UI, audio device, network, license activation or preference writing
// is requested by this host. Cocoa startup runs on the main thread. Plugin code
// runs in-process, outside the realtime
// engine. Each instance renders once; reload uses a new instance and saved state.
class AudioUnitInstrument {
 public:
  explicit AudioUnitInstrument(InstrumentKind kind);
  ~AudioUnitInstrument();
  AudioUnitInstrument(const AudioUnitInstrument&) = delete;
  AudioUnitInstrument& operator=(const AudioUnitInstrument&) = delete;

  const InstrumentDescriptor& descriptor() const;
  std::vector<std::string> factoryPresets() const;
  // New SWAM factory selections explicitly use concert pitch (transpose 0).
  // Restoring user state preserves its transposition, with note-range validation.
  void selectFactoryPreset(const std::string& name);
  std::string presetName() const;
  std::uint32_t componentVersion() const;
  std::vector<std::uint8_t> state() const;
  void restoreState(const std::vector<std::uint8_t>& bytes);

  // Fixed instrument preset: bank select and program changes are counted and omitted
  // from playback. All other channel voice bytes and original channels are sent
  // unchanged. Interpretation depends on the plugin's MIDI mapping. No GM drum
  // routing, host tempo callbacks, latency compensation or automatic mastering.
  // Out-of-range samples are counted and clipped for the PCM16 export boundary.
  // Sessions set clip_output=false to preserve float headroom for track/master
  // gain, and minimum_end_tick to use a common ending across instrument instances.
  AudioBuffer render(const MidiFile& midi, InstrumentRenderReport* report = nullptr,
                     std::uint32_t rate = 48000, double tail_seconds = 5.0,
                     bool clip_output = true, Tick minimum_end_tick = 0);

  using ChunkSink = std::function<void(std::size_t, const float*, std::uint32_t)>;
  // Offline fixed 48 kHz stereo, up to two hours including tail. The same AU
  // instance/clock/controller state spans every chunk. Sink runs synchronously,
  // borrows samples only until return, and may throw to abort; a used instance
  // cannot be retried. Report is published only after every sink call succeeds.
  void renderChunks(const MidiFile& midi, const ChunkSink& sink,
                    InstrumentRenderReport* report = nullptr, double tail_seconds = 5.0,
                    Tick minimum_end_tick = 0);

 private:
  void renderSequence(const MidiSampleSequence& sequence, const ChunkSink& sink,
                      InstrumentRenderReport* report, bool clip_output);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace daw
