#include "daw/session.hpp"
#include "daw/audio_limits.hpp"
#include "daw/midi_sequence.hpp"
#include "daw/score_midi.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace daw {
namespace {
void require(bool valid, const char* message) { if (!valid) throw std::invalid_argument(message); }
bool safeName(const std::string& name) {
  if (name.empty() || name.size() > 255 || name == "." || name == "..") return false;
  return std::none_of(name.begin(), name.end(), [](unsigned char c) {
    return c < 32 || c == 127 || c == '/' || c == '\\' || c == ':';
  });
}
void gainValid(double db) { require(std::isfinite(db) && db >= -60 && db <= 12, "gain must be finite and in -60..12 dB"); }
void stereoValid(const AudioBuffer& audio) {
  require(audio.channels == 2 && audio.sample_rate >= 8000 && audio.sample_rate <= 192000 &&
          audio.samples.size() % 2 == 0, "invalid stereo buffer");
  for (float sample : audio.samples) require(std::isfinite(sample), "non-finite audio sample");
}
void token(std::istream& input, const char* expected) {
  std::string word;
  require(static_cast<bool>(input >> word) && word == expected, "invalid session field or field order");
}
}

void validateSession(const Session& session) {
  require(safeName(session.score_file), "session score must be a sibling filename");
  gainValid(session.master_gain_db);
  require(!session.routes.empty() && session.routes.size() <= 64, "session requires 1..64 routes");
  std::set<std::string> ids;
  for (const auto& route : session.routes) {
    require(!route.part_id.empty() && route.part_id.size() <= 1024 && ids.insert(route.part_id).second,
            "duplicate, empty or oversized part ID");
    require(route.instrument == "pianoteq" || route.instrument == "swam-cello", "unsupported session instrument");
    gainValid(route.gain_db);
    require(std::isfinite(route.balance) && route.balance >= -1 && route.balance <= 1, "stereo balance must be in -1..1");
    require(route.preset.size() <= 4096 && (route.state_file.empty() || safeName(route.state_file)), "invalid preset/state reference");
    require(route.preset.empty() || route.state_file.empty(), "choose a preset OR saved state for each route");
    require(route.state_file.empty() || route.state_file != session.score_file, "score and state must be different files");
    require(route.frozen_file.empty() || (safeName(route.frozen_file) &&
        !route.state_file.empty() && route.frozen_file != session.score_file &&
        route.frozen_file != route.state_file), "frozen audio requires a separate sibling file and saved state");
  }
}

std::vector<bool> audibleSessionRoutes(const Session& session) {
  validateSession(session);
  const bool solo = std::any_of(session.routes.begin(), session.routes.end(), [](const auto& r) { return r.solo; });
  std::vector<bool> result;
  for (const auto& route : session.routes) result.push_back(!route.mute && (!solo || route.solo));
  return result;
}

Session parseSession(const std::string& text) {
  require(text.size() <= 1024U * 1024U, "session exceeds 1 MiB");
  std::istringstream input(text);
  input.imbue(std::locale::classic());
  token(input, "CLASSICAL_DAW_SESSION");
  int version = 0;
  require(static_cast<bool>(input >> version) && (version == 1 || version == 2), "unsupported session version");
  Session result;
  token(input, "score"); input >> std::quoted(result.score_file);
  token(input, "master_gain_db"); input >> result.master_gain_db;
  token(input, "routes");
  std::size_t count = 0;
  require(static_cast<bool>(input >> count) && count > 0 && count <= 64, "invalid route count");
  for (std::size_t i = 0; i < count; ++i) {
    token(input, "route");
    InstrumentRoute route;
    require(static_cast<bool>(input >> std::quoted(route.part_id) >> std::quoted(route.instrument) >>
        route.gain_db >> route.balance >> std::quoted(route.preset) >> std::quoted(route.state_file)), "truncated route");
    if (version == 2) {
      int mute = -1, solo = -1;
      require(static_cast<bool>(input >> mute >> solo >> std::quoted(route.frozen_file)) &&
          (mute == 0 || mute == 1) && (solo == 0 || solo == 1), "invalid mute/solo/frozen fields");
      route.mute = mute == 1; route.solo = solo == 1;
    }
    result.routes.push_back(std::move(route));
  }
  token(input, "end");
  input >> std::ws;
  require(input.eof(), "trailing session content");
  validateSession(result);
  return result;
}

std::string serializeSession(const Session& session) {
  validateSession(session);
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setprecision(17) << "CLASSICAL_DAW_SESSION 2\nscore " << std::quoted(session.score_file)
         << "\nmaster_gain_db " << session.master_gain_db << "\nroutes " << session.routes.size() << '\n';
  for (const auto& route : session.routes) {
    output << "route " << std::quoted(route.part_id) << ' ' << std::quoted(route.instrument) << ' '
           << route.gain_db << ' ' << route.balance << ' ' << std::quoted(route.preset) << ' '
           << std::quoted(route.state_file) << ' ' << static_cast<int>(route.mute) << ' '
           << static_cast<int>(route.solo) << ' ' << std::quoted(route.frozen_file) << '\n';
  }
  output << "end\n";
  return output.str();
}

SessionPlan planSession(const Session& session, const Score& score, std::uint32_t rate, double tail, SessionPlanMode mode) {
  require(mode == SessionPlanMode::Buffered || mode == SessionPlanMode::Streaming, "unknown session plan mode");
  require(mode != SessionPlanMode::Streaming || rate == 48000, "streaming sessions require 48000 Hz");
  validateSession(session);
  require(score.parts.size() == session.routes.size(), "every score part must have exactly one instrument route");
  MidiFile full;
  std::string error;
  if (!scoreToMidiFile(score, &full, &error, ScoreMidiChannelPolicy::IndependentParts))
    throw std::invalid_argument("session score: " + error);
  require(score.parts.size() == session.routes.size() && full.tracks.size() == score.parts.size(),
          "every score part must have exactly one instrument route");
  std::map<std::string, std::size_t> parts;
  SessionPlan plan;
  for (std::size_t i = 0; i < score.parts.size(); ++i) {
    require(parts.emplace(score.parts[i].id, i).second, "score part IDs must be unique");
    for (const auto& measure : score.parts[i].measures) {
      require(measure.start >= 0 && measure.duration >= 0 &&
              measure.start <= std::numeric_limits<Tick>::max() - measure.duration, "invalid measure extent");
      plan.end_tick = std::max(plan.end_tick, measure.start + measure.duration);
    }
  }
  for (const auto& track : full.tracks) {
    for (const auto& note : track.notes) plan.end_tick = std::max(plan.end_tick, note.end());
    for (const auto& event : track.channel_events) plan.end_tick = std::max(plan.end_tick, event.tick);
  }
  const auto frame_limit = mode == SessionPlanMode::Streaming ? kMaxStreamAudioFrames : kMaxBufferedAudioFrames;
  plan.frames = makeMidiSampleSequence(full, rate, tail, frame_limit, plan.end_tick).frames;
  for (const auto& route : session.routes) {
    auto found = parts.find(route.part_id);
    require(found != parts.end(), "instrument route references an unknown score part");
    MidiFile isolated;
    isolated.tempo = full.tempo;
    isolated.time_signature = full.time_signature;
    isolated.meter_changes = full.meter_changes;
    isolated.tracks.push_back(full.tracks[found->second]);
    const auto sequence = makeMidiSampleSequence(isolated, rate, tail, frame_limit, plan.end_tick);
    if (route.instrument == "swam-cello") requireInitialExpression(sequence);
    plan.tracks.push_back({route, std::move(isolated)});
  }
  return plan;
}

bool sameSessionPerformance(const SessionPlan& previous, std::size_t previous_track,
                            const SessionPlan& current, std::size_t current_track) {
  require(previous_track < previous.tracks.size() && current_track < current.tracks.size(),
          "performance track index out of range");
  if (previous.frames != current.frames) return false;
  const auto before = makeMidiSampleSequence(previous.tracks[previous_track].midi, 48000, 5,
                                             kMaxStreamAudioFrames, previous.end_tick);
  const auto after = makeMidiSampleSequence(current.tracks[current_track].midi, 48000, 5,
                                            kMaxStreamAudioFrames, current.end_tick);
  if (before.frames != previous.frames || after.frames != current.frames)
    throw std::invalid_argument("performance comparison requires 48 kHz / five-second-tail plans");
  if (before.end_frame != after.end_frame || before.events.size() != after.events.size()) return false;
  for (std::size_t i = 0; i < before.events.size(); ++i) {
    const auto& a = before.events[i]; const auto& b = after.events[i];
    if (a.frame != b.frame || a.status != b.status || a.data1 != b.data1 || a.data2 != b.data2) return false;
  }
  return true;
}

void applyTrackMix(AudioBuffer& audio, double db, double balance) {
  stereoValid(audio);
  gainValid(db);
  require(std::isfinite(balance) && balance >= -1 && balance <= 1, "invalid stereo balance");
  const double gain = std::pow(10.0, db / 20.0);
  constexpr double half_pi = 1.57079632679489661923;
  const double left = gain * (balance >= 1 ? 0 : balance > 0 ? std::cos(balance * half_pi) : 1);
  const double right = gain * (balance <= -1 ? 0 : balance < 0 ? std::cos(balance * half_pi) : 1);
  for (std::size_t i = 0; i < audio.samples.size(); ++i) {
    const double value = audio.samples[i] * (i % 2 == 0 ? left : right);
    require(std::abs(value) <= std::numeric_limits<float>::max(), "mix gain overflows float");
  }
  for (std::size_t i = 0; i < audio.samples.size(); ++i) audio.samples[i] =
      static_cast<float>(audio.samples[i] * (i % 2 == 0 ? left : right));
}

void addStereoTrack(AudioBuffer& mix, const AudioBuffer& track) {
  stereoValid(mix); stereoValid(track);
  require(mix.sample_rate == track.sample_rate && mix.samples.size() == track.samples.size(), "stem timing/format mismatch");
  for (std::size_t i = 0; i < mix.samples.size(); ++i) require(
      std::abs(static_cast<double>(mix.samples[i]) + track.samples[i]) <= std::numeric_limits<float>::max(), "mix sum overflows float");
  for (std::size_t i = 0; i < mix.samples.size(); ++i) mix.samples[i] += track.samples[i];
}

MixReport applyMasterMix(AudioBuffer& audio, double db) {
  applyTrackMix(audio, db, 0);
  MixReport report;
  double energy = 0;
  for (float sample : audio.samples) {
    report.peak = std::max(report.peak, std::abs(static_cast<double>(sample)));
    energy += static_cast<double>(sample) * sample;
    if (sample < -1 || sample > 1) ++report.over_unity_samples;
  }
  if (!audio.samples.empty()) report.rms = std::sqrt(energy / static_cast<double>(audio.samples.size()));
  return report;
}

}  // namespace daw
