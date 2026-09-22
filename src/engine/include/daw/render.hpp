#pragma once

#include "daw/midi.hpp"
#include "daw/score.hpp"
#include "daw/wav.hpp"

namespace daw {

// 512 MiB of mono float audio by default. Hosts may explicitly choose another
// frame budget; streaming long-form bounce is not implemented yet.
inline constexpr std::size_t kDefaultRenderFrameLimit = 128U * 1024U * 1024U;

struct MidiRenderReport {
  std::uint64_t interpreted_channel_events = 0;
  std::uint64_t unsupported_channel_events = 0;
  std::uint64_t voices_released_at_end = 0;
  std::uint64_t ignored_release_velocities = 0;
  std::uint64_t clipped_samples = 0;
};

// Offline mono sine diagnostic, not a sampler or a realtime audio callback.
// All tracks share 16 channels. Supports CC7/11 (linear gain, default 127),
// CC64 (binary sustain), CC120/121/123 and fixed +/-2-semitone pitch bend.
// Other messages (including RPN/bend-range changes) are counted as unsupported.
// Release velocity is retained in the model but does not shape the sine sound.
//
// Events are rounded to the nearest output sample using the complete tempo
// map. Same-tick events use track index, then the SMF writer's within-track
// ordering rule. Different source ticks retain their order even if rounded
// to one sample. Sample rate is rounded to an integer before time conversion.
//
// Envelope: 8 ms attack, 35 ms release AFTER key/pedal release. At the last
// note/event tick, still-held voices are released and counted. Output length
// is that tick's time plus tail_seconds; a short/zero tail truncates releases.
// Throws for invalid input. Report is replaced only after successful render.
AudioBuffer renderMidiFile(const MidiFile& midi, double sample_rate = 48000.0,
                           double tail_seconds = 0.1, MidiRenderReport* report = nullptr,
                           std::size_t max_output_frames = kDefaultRenderFrameLimit);

// Single-track convenience wrapper using the same performance renderer.
AudioBuffer renderNotes(const MidiTrack& track, const TempoMap& tempo,
                        double sample_rate = 48000.0, double tail_seconds = 0.1,
                        MidiRenderReport* report = nullptr,
                        std::size_t max_output_frames = kDefaultRenderFrameLimit);

// Convert the score through the strict score-to-MIDI bridge and render every
// part into one deterministic mono diagnostic mix. This uses the sine
// renderer behind renderNotes, not a production sampler or orchestral
// instrument. Returning by value gives callers a strong failure guarantee:
// invalid input throws before caller-owned audio state is changed.
AudioBuffer renderScore(const Score& score, double sample_rate = 48000.0,
                        double tail_seconds = 0.1, MidiRenderReport* report = nullptr,
                        std::size_t max_output_frames = kDefaultRenderFrameLimit);

}  // namespace daw
