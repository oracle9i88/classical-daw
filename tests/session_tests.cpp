#include "daw/session.hpp"
#include "daw/midi_sequence.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
template <typename F> void rejects(F function) {
  bool failed = false;
  try { function(); } catch (const std::exception&) { failed = true; }
  require(failed, "invalid session operation accepted");
}
daw::Score score() {
  daw::Score result;
  result.tempo_changes.push_back({960, 60});
  daw::ScoreNote piano, cello;
  piano.pitch = {'C', 0, 4}; piano.midi_channel = 0;
  cello.pitch = {'C', 0, 3}; cello.start = 1920; cello.midi_channel = 0;
  result.parts.push_back({"piano", "Piano", {{1, 0, {piano}, 3840}},
      {{0, daw::MidiChannelEventType::ControlChange, 0, 64, 127},
       {960, daw::MidiChannelEventType::ControlChange, 0, 64, 0}}});
  result.parts.push_back({"cello", "Cello", {{1, 0, {cello}, 3840}},
      {{0, daw::MidiChannelEventType::ControlChange, 0, 11, 90}}});
  return result;
}
daw::Session session() {
  return {"score.dawproj", -6, {{"cello", "swam-cello", -3, .2, "Cello", ""},
                                {"piano", "pianoteq", -2, -.2, "NY Steinway D Classical", ""}}};
}
}
int main() {
  try {
    const auto original = session();
    const auto text = daw::serializeSession(original);
    require(daw::serializeSession(daw::parseSession(text)) == text, "session persistence changed routing/mix/preset");
    auto settings = original;
    require(daw::audibleSessionRoutes(settings) == std::vector<bool>({true, true}), "default tracks not audible");
    settings.routes[0].solo = true;
    require(daw::audibleSessionRoutes(settings) == std::vector<bool>({true, false}), "solo selection wrong");
    settings.routes[0].mute = true;
    require(daw::audibleSessionRoutes(settings) == std::vector<bool>({false, false}), "mute must win over solo");
    settings.routes[1].solo = true;
    require(daw::audibleSessionRoutes(settings) == std::vector<bool>({false, true}), "multiple solos wrong");
    settings.routes[0].preset.clear(); settings.routes[0].state_file = "cello.aupreset";
    settings.routes[0].frozen_file = "cello.dawfreeze";
    const auto saved_settings = daw::parseSession(daw::serializeSession(settings));
    require(saved_settings.routes[0].mute && saved_settings.routes[0].solo &&
        saved_settings.routes[0].frozen_file == "cello.dawfreeze", "mute/solo/freeze reference not saved");
    const auto legacy = daw::parseSession("CLASSICAL_DAW_SESSION 1\nscore \"s.dawproj\"\nmaster_gain_db -6\nroutes 1\nroute \"p\" \"pianoteq\" 0 0 \"\" \"\"\nend\n");
    require(!legacy.routes[0].mute && !legacy.routes[0].solo && legacy.routes[0].frozen_file.empty(), "v1 defaults wrong");
    require(legacy.routes[0].track_delay_us == 0, "v1 delay default wrong");
    const std::string v2 = "CLASSICAL_DAW_SESSION 2\nscore \"s\"\nmaster_gain_db 0\nroutes 1\nroute \"p\" \"pianoteq\" 0 0 \"\" \"\" 0 0 \"\"\nend\n";
    require(daw::parseSession(v2).routes[0].track_delay_us == 0, "v2 delay default wrong");
    for (const auto delay : {std::int64_t{-25000},std::int64_t{17000},std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::max()}) {
      auto offset = original; offset.routes[0].track_delay_us = delay;
      require(daw::parseSession(daw::serializeSession(offset)).routes[0].track_delay_us == delay, "signed delay lost precision");
      rejects([&] { daw::planSession(offset,score()); });
      rejects([&] { daw::requireExecutableSession(offset); });
    }
    for (const std::string bad : {"9223372036854775808","-9223372036854775809","1.0","1e2","nan","--1"}) {
      auto malformed = v2; malformed[22] = '3';
      malformed.insert(malformed.find("\nend"), " " + bad);
      rejects([&] { daw::parseSession(malformed); });
    }
    rejects([&] { daw::parseSession("CLASSICAL_DAW_SESSION 2\nscore \"s\"\nmaster_gain_db 0\nroutes 1\nroute \"p\" \"pianoteq\" 0 0 \"\" \"\" 2 0 \"\"\nend"); });
    settings.routes[0].frozen_file = "../bad";
    rejects([&] { daw::serializeSession(settings); });
    auto plan = daw::planSession(original, score(), 48000, 1);
    require(plan.tracks.size() == 2 && plan.end_tick == 3840 && plan.frames == 216000, "shared tempo/end extent wrong");
    require(plan.tracks[0].midi.tracks[0].notes[0].pitch == 48 &&
            plan.tracks[1].midi.tracks[0].notes[0].pitch == 60, "route order overrode stable part ID");
    require(plan.tracks[0].midi.tracks[0].notes[0].channel == 0 && plan.tracks[1].midi.tracks[0].notes[0].channel == 0,
            "host remapped original MIDI channels");
    require(plan.tracks[0].midi.tracks[0].channel_events.size() == 1 &&
            plan.tracks[0].midi.tracks[0].channel_events[0].data1 == 11 &&
            plan.tracks[1].midi.tracks[0].channel_events.size() == 2 &&
            plan.tracks[1].midi.tracks[0].channel_events[0].data1 == 64, "controllers leaked between instances");
    for (const auto& track : plan.tracks) {
      const auto seq = daw::makeMidiSampleSequence(track.midi, 48000, 1, 1000000, plan.end_tick);
      require(seq.frames == plan.frames && seq.end_frame == 168000, "stem not aligned to common end");
    }
    auto invalid = original; invalid.routes.pop_back();
    rejects([&] { daw::planSession(invalid, score()); });
    invalid = original; invalid.routes[0].part_id = "unknown";
    rejects([&] { daw::planSession(invalid, score()); });
    invalid = original; invalid.routes[0].part_id = "piano";
    rejects([&] { daw::serializeSession(invalid); });
    invalid = original; invalid.score_file = "../score.dawproj";
    rejects([&] { daw::serializeSession(invalid); });
    invalid = original; invalid.routes[0].preset.clear(); invalid.routes[0].state_file = "/tmp/state.aupreset";
    rejects([&] { daw::serializeSession(invalid); });
    invalid.routes[0].state_file = "..\\state.aupreset";
    rejects([&] { daw::serializeSession(invalid); });
    invalid.routes[0].state_file = "cello.aupreset";
    require(daw::parseSession(daw::serializeSession(invalid)).routes[0].state_file == "cello.aupreset", "saved state reference lost");
    invalid.routes[0].preset = "Cello";
    rejects([&] { daw::serializeSession(invalid); });
    rejects([&] { daw::parseSession(text + "garbage"); });
    rejects([&] { daw::parseSession(text.substr(0, text.find("routes"))); });
    rejects([&] { daw::parseSession(std::string(1024U * 1024U + 1, 'x')); });
    invalid = original; invalid.master_gain_db = std::numeric_limits<double>::quiet_NaN();
    rejects([&] { daw::serializeSession(invalid); });
    invalid = original; invalid.routes[0].balance = 1.1;
    rejects([&] { daw::planSession(invalid, score()); });
    auto missing_expression = score(); missing_expression.parts[1].midi_events.clear();
    rejects([&] { daw::planSession(original, missing_expression); });
    auto long_score = score(); long_score.parts[1].measures[0].duration = 1000000000;
    rejects([&] { daw::planSession(original, long_score); });

    daw::AudioBuffer audio{48000, 2, {.4F, -.6F, .2F, .3F}};
    const auto before = audio.samples;
    daw::applyTrackMix(audio, 0, 0);
    require(audio.samples == before, "center balance altered stereo");
    daw::applyTrackMix(audio, 0, -1);
    require(audio.samples[0] == before[0] && audio.samples[1] == 0, "full left balance wrong");
    audio.samples = before;
    daw::applyTrackMix(audio, 0, 1);
    require(audio.samples[0] == 0 && audio.samples[1] == before[1], "full right balance wrong");
    audio.samples = before;
    daw::applyTrackMix(audio, 0, .5);
    require(std::abs(audio.samples[0] - before[0] / std::sqrt(2.0)) < 1e-7 && audio.samples[1] == before[1], "balance law wrong");
    const double half_gain_db = -20 * std::log10(2.0);
    audio.samples = {1.5F, -1.5F};
    daw::applyTrackMix(audio, half_gain_db, 0);
    daw::AudioBuffer mix{48000, 2, {0, 0}};
    daw::addStereoTrack(mix, audio); daw::addStereoTrack(mix, audio);
    require(mix.samples[0] == 1.5F, "intermediate mix clipped");
    auto meter = daw::applyMasterMix(mix, half_gain_db);
    require(mix.samples[0] == .75F && meter.peak == .75 && meter.over_unity_samples == 0, "master gain did not recover headroom");
    meter = daw::applyMasterMix(mix, 12);
    require(meter.over_unity_samples == 2 && mix.samples[0] > 1, "master overload silently clipped");
    const auto unchanged = mix.samples;
    auto bad = audio; bad.sample_rate = 44100;
    rejects([&] { daw::addStereoTrack(mix, bad); });
    bad = audio; bad.samples[1] = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { daw::addStereoTrack(mix, bad); });
    require(mix.samples == unchanged, "failed add changed mix");
    rejects([&] { daw::applyTrackMix(mix, 0, 2); });
    require(mix.samples == unchanged, "invalid balance changed mix");
    daw::AudioBuffer large{48000, 2, {std::numeric_limits<float>::max(), 0}};
    rejects([&] { daw::addStereoTrack(large, large); });
    require(large.samples[0] == std::numeric_limits<float>::max(), "overflow add modified output");
    std::cout << "Session routing, persistence, isolation, alignment, mute/solo and mix tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
