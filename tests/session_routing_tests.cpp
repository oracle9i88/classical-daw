#include "daw/session.hpp"
#include "daw/score_midi.hpp"
#include "daw/midi_sequence.hpp"
#include "daw/project.hpp"
#include "daw/render.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <cmath>
#include <tuple>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
  bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; }
  require(rejected, "invalid routing accepted");
}
struct Temp {
  fs::path root = fs::temp_directory_path() / ("daw-route-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Temp() { require(fs::create_directory(root), "cannot create test directory"); }
  ~Temp() { std::error_code ec; fs::remove_all(root, ec); }
};
daw::Score ensemble(std::size_t count) {
  daw::Score score;
  score.tempo_changes = {{960, 90}, {3840, 72}};
  score.meter_changes = {{3840, {3, 4, 24, 8}}};
  for (std::size_t i = 0; i < count; ++i) {
    daw::ScoreNote attack, continuation;
    attack.duration = 480; attack.tie_start = true;
    attack.velocity = static_cast<std::uint8_t>(50+i%50);
    if (i == 17) attack.midi_channel = 3; // Explicit imported routing wins.
    continuation = attack; continuation.start = 480; continuation.tie_start = false; continuation.tie_stop = true;
    continuation.midi_release_velocity = 37;
    const auto channel = static_cast<std::uint8_t>(i == 17 ? 3 : i%16);
    score.parts.push_back({"part-"+std::to_string(i), "Part "+std::to_string(i),
      {{1, 0, {attack, continuation}, 3840}, {2, 3840, {}, 2880}},
      {{0, daw::MidiChannelEventType::ControlChange, channel, 7, static_cast<std::uint8_t>(i == 0 ? 0 : 127)},
       {0, daw::MidiChannelEventType::ControlChange, channel, 11, static_cast<std::uint8_t>(50+i)},
       {0, daw::MidiChannelEventType::ControlChange, channel, 64, 127},
       {1440, daw::MidiChannelEventType::ControlChange, channel, 64, 0},
       {1920, daw::MidiChannelEventType::PitchBend, channel, static_cast<std::uint8_t>(i), 64}}});
  }
  return score;
}
daw::Session session(const daw::Score& score) {
  daw::Session session{"score.dawproj", -6, {}};
  for (const auto& part : score.parts) session.routes.push_back({part.id, "pianoteq", -6, 0, "", ""});
  std::reverse(session.routes.begin(), session.routes.end()); // UI order is not routing identity.
  return session;
}
void same(const daw::MidiFile& a, const daw::MidiFile& b, daw::Tick end) {
  auto meter = [](const daw::TimeSignature& s) { return std::make_tuple(s.numerator, s.denominator,
      s.clocks_per_click, s.notated_32nds_per_quarter); };
  require(meter(a.time_signature) == meter(b.time_signature) && a.meter_changes.size() == b.meter_changes.size(), "initial meter/map size changed");
  for (std::size_t i = 0; i < a.meter_changes.size(); ++i)
    require(a.meter_changes[i].tick == b.meter_changes[i].tick &&
            meter(a.meter_changes[i].signature) == meter(b.meter_changes[i].signature), "meter change lost");
  require(a.tempo.changes().size() == b.tempo.changes().size(), "tempo map size changed");
  for (std::size_t i = 0; i < a.tempo.changes().size(); ++i) {
    const auto& x = a.tempo.changes()[i]; const auto& y = b.tempo.changes()[i];
    require(x.tick == y.tick && std::llround(60000000./x.bpm) == std::llround(60000000./y.bpm), "MIDI tempo map changed");
  }
  const auto left = daw::makeMidiSampleSequence(a, 48000, 1, 1000000, end);
  const auto right = daw::makeMidiSampleSequence(b, 48000, 1, 1000000, end);
  require(left.frames == right.frames && left.end_frame == right.end_frame && left.events.size() == right.events.size(),
          "reopened route timing changed");
  for (std::size_t i = 0; i < left.events.size(); ++i) {
    const auto& x = left.events[i]; const auto& y = right.events[i];
    require(x.frame == y.frame && x.status == y.status && x.data1 == y.data1 && x.data2 == y.data2,
            "reopened note/controller sequence changed");
  }
}
}
int main(int argc, char** argv) {
  try {
    require(argc == 1 || (argc == 3 && std::string(argv[1]) == "--fixture"), "usage: daw_session_routing_tests [--fixture NEW_DIRECTORY]");
    Temp temp;
    for (std::size_t count : {16U, 17U, 32U, 64U}) {
      const auto score = ensemble(count); const auto routing = session(score);
      const auto plan = daw::planSession(routing, score, 48000, 1);
      require(plan.tracks.size() == count && plan.end_tick == 6720 && plan.frames == 288000, "ensemble duration or route count incorrect");
      std::string error;
      const auto path = (temp.root/"score.dawproj").string();
      require(daw::writeProjectFile(score, path, &error), "project save failed: "+error);
      daw::Score reopened;
      require(daw::readProjectFile(path, &reopened, &error), "project reopen failed: "+error);
      const auto restored = daw::planSession(daw::parseSession(daw::serializeSession(routing)), reopened, 48000, 1);
      for (std::size_t i = 0; i < count; ++i) {
        const auto original_index = count-1-i;
        const auto& route = plan.tracks[i];
        const auto& track = route.midi.tracks.at(0);
        require(route.route.part_id == score.parts[original_index].id && route.midi.tracks.size() == 1, "part ID route isolation failed");
        const auto channel = original_index == 17 ? 3 : original_index%16;
        require(track.notes.size() == 1 && track.notes[0].channel == channel && track.notes[0].duration == 960 &&
                track.notes[0].release_velocity == 37 && track.notes[0].velocity == 50+original_index%50,
                "authored/explicit channel, tie or velocity lost");
        require(track.channel_events.size() == 5 && track.channel_events[1].data2 == 50+original_index &&
                track.channel_events[4].data1 == original_index, "controller leaked from another part");
        same(route.midi, restored.tracks[i].midi, plan.end_tick);
        const auto midi_path = (temp.root/("track-"+std::to_string(i)+".mid")).string();
        require(daw::writeMidiFile(route.midi, midi_path, &error), "stem MIDI write failed: "+error);
        daw::MidiFile midi;
        require(daw::readMidiFile(midi_path, &midi, &error), "stem MIDI read failed: "+error);
        same(route.midi, midi, plan.end_tick);
        require(score.parts[original_index].measures[0].notes[0].midi_channel == (original_index == 17 ? 3 : -1),
                "planning mutated score channel metadata");
      }
      // Diagnostic only: part 0's CC7=0 must not silence part 16 on the same channel.
      if (count > 16) {
        const auto quiet = daw::renderMidiFile(plan.tracks[count-1].midi);
        const auto audible = daw::renderMidiFile(plan.tracks[count-17].midi);
        require(std::all_of(quiet.samples.begin(), quiet.samples.end(), [](float x){ return x == 0; }) &&
                std::any_of(audible.samples.begin(), audible.samples.end(), [](float x){ return x != 0; }), "same-channel volume leaked between routes");
        daw::MidiFile sentinel; sentinel.tracks.push_back({"untouched", {}, {}});
        require(!daw::scoreToMidiFile(score, &sentinel, &error) && sentinel.tracks[0].name == "untouched",
                "shared-output MIDI guard was weakened");
      }
      if (count == 64) {
        // XML does not preserve performance-only MIDI fields. Test the authored
        // notation slice separately, without claiming those fields round-trip.
        auto notation = score;
        for (auto& part : notation.parts) {
          part.midi_events.clear();
          for (auto& measure : part.measures) for (auto& note : measure.notes) {
            note.midi_channel = -1; note.midi_release_velocity = 0;
          }
        }
        const auto xml = (temp.root/"ensemble.musicxml").string();
        require(daw::writeMusicXmlFile(notation, xml, &error), "XML write failed: "+error);
        daw::Score xml_score;
        require(daw::readMusicXmlFile(xml, &xml_score, &error), "XML read failed: "+error);
        const auto expected = daw::planSession(routing, notation, 48000, 1);
        const auto from_xml = daw::planSession(routing, xml_score, 48000, 1);
        for (std::size_t i = 0; i < count; ++i) same(expected.tracks[i].midi, from_xml.tracks[i].midi, expected.end_tick);
        require(from_xml.frames == expected.frames && from_xml.end_tick == expected.end_tick, "XML silent ending lost");
      }
    }
    auto score = ensemble(64); auto routing = session(score);
    routing.routes.back().instrument = "swam-cello";
    score.parts[0].midi_events.erase(score.parts[0].midi_events.begin()+1);
    rejects([&] { daw::planSession(routing, score); }); // CC11 from part 16 cannot initialize this cello.
    score = ensemble(65); routing = session(score);
    rejects([&] { daw::planSession(routing, score); });
    std::string error;
    daw::MidiFile sentinel; sentinel.tracks.push_back({"untouched", {}, {}});
    require(!daw::scoreToMidiFile(score, &sentinel, &error, daw::ScoreMidiChannelPolicy::IndependentParts) &&
            sentinel.tracks[0].name == "untouched", "independent conversion exceeded part budget");
    require(!daw::scoreToMidiFile(ensemble(1), &sentinel, &error, static_cast<daw::ScoreMidiChannelPolicy>(99)) &&
            sentinel.tracks[0].name == "untouched", "invalid policy changed output");
    score = ensemble(64); routing = session(score);
    for (auto& part : score.parts) { part.midi_events.clear(); for (auto& m : part.measures) m.notes.clear(); }
    const auto silent = daw::planSession(routing, score, 48000, 1);
    require(silent.tracks.size() == 64 && silent.frames == 288000 && silent.tracks.back().midi.tracks[0].notes.empty(),
            "large silent parts lost shared extent");
    if (argc == 3) {
      const fs::path destination(argv[2]);
      require(fs::create_directory(destination), "fixture output must be new");
      score = ensemble(64); routing = session(score);
      require(daw::writeProjectFile(score, (destination/"score.dawproj").string(), &error), error);
      std::ofstream output(destination/"session.dawsession"); output << daw::serializeSession(routing); output.close();
      require(bool(output), "fixture session write failed");
      const auto plan = daw::planSession(routing, score);
      for (std::size_t i = 0; i < plan.tracks.size(); ++i)
        require(daw::writeMidiFile(plan.tracks[i].midi, (destination/("track-"+std::to_string(i+1)+".mid")).string(), &error), error);
    }
    std::cout << "PASS 16/17/32/64 independent parts, MIDI/project/XML reopen, ties, maps and controller isolation; 65 rejected\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
