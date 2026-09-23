#pragma once

#include "daw/score.hpp"
#include "daw/midi.hpp"
#include "daw/wav.hpp"
#include <string>
#include <vector>

namespace daw {

struct InstrumentRoute {
  std::string part_id;
  std::string instrument;  // pianoteq or swam-cello in the current host
  double gain_db = 0;
  double balance = 0;  // -1 left, 0 unchanged stereo, +1 right
  std::string preset;
  std::string state_file;  // sibling filename, exclusive with preset
  bool mute = false;
  bool solo = false;
  std::string frozen_file;  // optional pre-fader audio, explicitly reused by CLI
};
struct Session {
  std::string score_file;
  double master_gain_db = -6;
  std::vector<InstrumentRoute> routes;
};
struct RoutedTrack {
  InstrumentRoute route;
  MidiFile midi;
};
struct SessionPlan {
  std::vector<RoutedTrack> tracks;
  Tick end_tick = 0;
  std::size_t frames = 0;
};

// Session v2 adds mute/solo/frozen audio; v1 loads with all tracks audible.
// Text is bounded to 1 MiB / 64 routes; paths cannot escape that directory.
// Parsing returns a new value; malformed input cannot partially modify a session.
Session parseSession(const std::string& text);
std::string serializeSession(const Session& session);
void validateSession(const Session& session);
// Any solo selects only solo tracks; mute always wins, including muted solos.
std::vector<bool> audibleSessionRoutes(const Session& session);

// Exactly one independent instrument per score part, matched by stable part ID,
// never by UI route order or MIDI channel. No cross-part controller inheritance.
// Authored notes without explicit MIDI channels use score part index % 16;
// reusing a channel is safe only because each route has an independent instance.
// Existing first-16 defaults and all explicit imported channels stay unchanged.
// Shares the complete tempo/meter map and preserves explicit terminal silence.
enum class SessionPlanMode { Buffered, Streaming };
// Buffered (default): 256 MiB per stereo audio buffer. Streaming: fixed 48 kHz,
// at most two hours including tail; only schedules events, no audio allocation.
SessionPlan planSession(const Session& session, const Score& score,
                        std::uint32_t rate = 48000, double tail = 5.0,
                        SessionPlanMode mode = SessionPlanMode::Buffered);

struct MixReport {
  double peak = 0;
  double rms = 0;
  std::uint64_t over_unity_samples = 0;
};
// Offline float processing. No intermediate clipping: a later master fader may
// recover headroom. Stereo balance attenuates the opposite channel with cosine;
// center leaves both channels untouched. Throws on bad shape, gain or NaN/Inf.
void applyTrackMix(AudioBuffer& audio, double gain_db, double balance);
void addStereoTrack(AudioBuffer& mix, const AudioBuffer& track);
MixReport applyMasterMix(AudioBuffer& audio, double gain_db);

}  // namespace daw
