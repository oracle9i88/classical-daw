#include "daw/session.hpp"
#include "daw/project.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

// Original engineering fixture, not a transcription of a published work.
daw::ScorePitch pitch(int midi) {
  constexpr std::array<char, 12> steps{'C','C','D','D','E','F','F','G','G','A','A','B'};
  constexpr std::array<int, 12> alters{0,1,0,1,0,0,1,0,1,0,1,0};
  return {steps[static_cast<std::size_t>(midi % 12)], alters[static_cast<std::size_t>(midi % 12)], midi / 12 - 1};
}
daw::ScoreNote note(daw::Tick offset, daw::Tick length, int key, std::uint8_t velocity, std::uint16_t voice = 1) {
  daw::ScoreNote result;
  result.start = offset; result.duration = length; result.pitch = pitch(key);
  result.velocity = velocity; result.voice = voice;
  result.midi_channel = 0;  // Deliberately same channel in independent instances.
  return result;
}
int main(int argc, char** argv) {
  if (argc != 2) { std::cerr << "Usage: daw_session_fixture NEW_DIRECTORY\n"; return 2; }
  try {
    const std::filesystem::path root(argv[1]);
    if (std::filesystem::exists(std::filesystem::symlink_status(root))) throw std::runtime_error("fixture directory must be new");
    daw::Score score;
    score.bpm = 84;
    score.tempo_changes.push_back({6 * 3840, 76});
    daw::ScorePart piano{"piano", "Piano", {}, {}}, cello{"cello", "Cello", {}, {}};
    cello.midi_events.push_back({0, daw::MidiChannelEventType::ControlChange, 0, 11, 45});
    const std::array<std::array<int, 3>, 8> chords{{{48,51,55}, {44,48,51}, {41,44,48}, {43,47,50},
                                                 {48,51,55}, {46,50,53}, {43,47,50}, {48,51,55}}};
    const std::array<int, 8> pattern{0,1,2,1,0,1,2,1};
    const std::array<std::uint8_t, 8> curve{42,55,68,82,93,86,71,48};
    for (std::size_t bar = 0; bar < chords.size(); ++bar) {
      const auto start = static_cast<daw::Tick>(bar) * 3840;
      daw::ScoreMeasure pm{static_cast<int>(bar + 1), start, {}, 3840}, cm = pm;
      for (std::size_t i = 0; i < pattern.size(); ++i) {
        pm.notes.push_back(note(start + static_cast<daw::Tick>(i) * 480, 390,
            chords[bar][static_cast<std::size_t>(pattern[i])] + 12,
            static_cast<std::uint8_t>(i % 2 == 0 ? 63 : 53)));
      }
      pm.notes.push_back(note(start, 2800, chords[bar][0] - 12, 42, 2));
      cm.notes.push_back(note(start + 480, 2880, chords[bar][0], 84));
      piano.midi_events.push_back({start, daw::MidiChannelEventType::ControlChange, 0, 64, 95});
      piano.midi_events.push_back({start + 3600, daw::MidiChannelEventType::ControlChange, 0, 64, 0});
      for (std::size_t i = 0; i < curve.size(); ++i) cello.midi_events.push_back(
          {start + 480 + static_cast<daw::Tick>(i) * 360, daw::MidiChannelEventType::ControlChange, 0, 11, curve[i]});
      piano.measures.push_back(std::move(pm)); cello.measures.push_back(std::move(cm));
    }
    // A final explicitly notated silent bar tests common-end preservation.
    piano.measures.push_back({9, 8 * 3840, {}, 3840});
    cello.measures.push_back({9, 8 * 3840, {}, 3840});
    score.parts = {piano, cello};
    daw::Session session{"score.dawproj", -3, {{"piano", "pianoteq", -4.5, -.2, "NY Steinway D Classical", ""},
                                             {"cello", "swam-cello", -6, .2, "Cello", ""}}};
    const auto plan = daw::planSession(session, score);
    if (!std::filesystem::create_directory(root)) throw std::runtime_error("cannot create fixture directory");
    std::string error;
    if (!daw::writeProjectFile(score, (root / session.score_file).string(), &error)) throw std::runtime_error(error);
    for (const auto& track : plan.tracks) {
      if (!daw::writeMidiFile(track.midi, (root / (track.route.part_id + ".mid")).string(), &error)) throw std::runtime_error(error);
    }
    std::ofstream output(root / "duet.dawsession");
    output << daw::serializeSession(session); output.close();
    if (!output) throw std::runtime_error("fixture session write failed");
    std::cout << "Wrote original piano/cello fixture, " << plan.frames << " common frames\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
