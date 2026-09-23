#include "daw/session.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
daw::SessionPlan plan(const daw::Session& session, const daw::Score& score) {
  return daw::planSession(session, score, 48000, 5, daw::SessionPlanMode::Streaming);
}
}
int main() {
  try {
    daw::Score score;
    daw::ScoreNote note; note.midi_channel = 0; note.start = 480; note.duration = 480;
    score.parts = {{"piano", "Piano", {{1, 0, {note}, 3840}}},
                   {"other", "Other", {{1, 0, {note}, 3840}}}};
    score.parts[0].midi_events = {{0, daw::MidiChannelEventType::ControlChange, 0, 64, 127},
                                 {0, daw::MidiChannelEventType::ControlChange, 0, 64, 0}};
    daw::Session session{"score.dawproj", -6,
      {{"piano", "pianoteq", -6, 0, "", ""}, {"other", "pianoteq", -6, 0, "", ""}}};
    const auto before = plan(session, score);
    auto compare = [&](const daw::Score& changed, bool first, bool second) {
      const auto after = plan(session, changed);
      require(daw::sameSessionPerformance(before, 0, after, 0) == first, "first route reuse decision wrong");
      require(daw::sameSessionPerformance(before, 1, after, 1) == second, "second route reuse decision wrong");
    };
    compare(score, true, true);
    auto changed = score;
    changed.parts[0].name = "Renamed";
    changed.parts[0].measures[0].notes[0].lyric = "new lyric";
    compare(changed, true, true);
    // Tie spelling changes but the instrument receives the same single attack.
    changed = score;
    auto& notes = changed.parts[0].measures[0].notes;
    auto continuation = notes[0]; continuation.start += 240; continuation.duration = 240;
    continuation.tie_stop = true; notes[0].duration = 240; notes[0].tie_start = true;
    notes.push_back(continuation); compare(changed, true, true);
    // Mix targets and UI route order cannot determine audio cache identity.
    auto mixed = session; mixed.master_gain_db = -20;
    mixed.routes[0].gain_db = -15; mixed.routes[0].mute = true; mixed.routes[1].solo = true;
    std::reverse(mixed.routes.begin(), mixed.routes.end());
    const auto reordered = plan(mixed, score);
    require(daw::sameSessionPerformance(before, 0, reordered, 1) &&
            daw::sameSessionPerformance(before, 1, reordered, 0), "mix/order invalidated performance");
    for (int mutation = 0; mutation < 6; ++mutation) {
      changed = score; auto& n = changed.parts[0].measures[0].notes[0];
      if (mutation == 0) ++n.pitch.alter;
      if (mutation == 1) --n.velocity;
      if (mutation == 2) ++n.start;
      if (mutation == 3) ++n.duration;
      if (mutation == 4) ++n.midi_release_velocity;
      if (mutation == 5) n.midi_channel = 2;
      compare(changed, false, true);
    }
    for (auto type : {daw::MidiChannelEventType::ControlChange, daw::MidiChannelEventType::PitchBend,
                      daw::MidiChannelEventType::PolyPressure, daw::MidiChannelEventType::ChannelPressure,
                      daw::MidiChannelEventType::ProgramChange}) {
      changed = score;
      changed.parts[0].midi_events.push_back({240, type, 0, 11, 0});
      compare(changed, false, true);
    }
    changed = score;
    std::reverse(changed.parts[0].midi_events.begin(), changed.parts[0].midi_events.end());
    compare(changed, false, true); // Same-frame ordering matters.
    changed = score; changed.bpm = 90; compare(changed, false, false);
    changed = score; changed.tempo_changes = {{960, 60}}; compare(changed, false, false);
    changed = score; changed.parts[0].measures[0].duration += 960;
    compare(changed, false, false); // Shared release/tail changes even on other part.
    // The current host has no beat/meter callback. Equivalent audible schedules
    // remain reusable when only notation changes.
    changed = score; changed.time_signature = {2, 2, 24, 8}; compare(changed, true, true);
    bool rejected = false;
    try { daw::sameSessionPerformance(before, 2, before, 0); } catch (const std::exception&) { rejected = true; }
    require(rejected, "invalid track index accepted");
    const auto short_tail = daw::planSession(session, score, 48000, 1);
    rejected = false;
    try { daw::sameSessionPerformance(short_tail, 0, short_tail, 0); } catch (const std::exception&) { rejected = true; }
    require(rejected, "wrong render contract accepted");
    std::cout << "PASS per-part changes, ordered controllers, note/release channels, ties/notation, shared timing and routing\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
