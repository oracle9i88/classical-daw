#include "daw/render.hpp"
#include "daw/score_midi.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

int fail(const std::string& message) {
  std::cerr << "FAIL: " << message << '\n';
  return 1;
}

daw::ScoreNote note(daw::Tick start, daw::Tick duration, bool tie_start = false,
                    bool tie_stop = false, std::uint8_t velocity = 100,
                    std::uint16_t voice = 1, std::uint16_t staff = 1) {
  daw::ScoreNote result;
  result.start = start;
  result.duration = duration;
  result.tie_start = tie_start;
  result.tie_stop = tie_stop;
  result.velocity = velocity;
  result.voice = voice;
  result.staff = staff;
  return result;
}

daw::Score scoreWithNotes(std::vector<daw::ScoreNote> notes) {
  daw::Score result;
  result.parts = {daw::ScorePart{"P1", "Strings", {daw::ScoreMeasure{1, 0, std::move(notes)}}}};
  return result;
}

const daw::ScoreNote* findPitch(const daw::ScoreMeasure& measure, char step) {
  const auto found = std::find_if(measure.notes.begin(), measure.notes.end(),
                                [step](const daw::ScoreNote& value) { return value.pitch.step == step; });
  return found == measure.notes.end() ? nullptr : &*found;
}

bool equalPerformance(const daw::MidiTrack& left, const daw::MidiTrack& right) {
  if (left.notes.size() != right.notes.size()) return false;
  auto ordered = [](const daw::MidiTrack& track) {
    std::vector<daw::MidiNote> notes = track.notes;
    std::sort(notes.begin(), notes.end(), [](const daw::MidiNote& a, const daw::MidiNote& b) {
      if (a.start != b.start) return a.start < b.start;
      return a.pitch < b.pitch;
    });
    return notes;
  };
  const auto a = ordered(left);
  const auto b = ordered(right);
  for (std::size_t index = 0; index < a.size(); ++index) {
    // Score export assigns a stable channel per part; channel preservation
    // is outside the tie contract, while every audible note field must match.
    if (a[index].start != b[index].start || a[index].duration != b[index].duration ||
        a[index].pitch != b[index].pitch || a[index].velocity != b[index].velocity) return false;
  }
  return true;
}

}  // namespace

int main() {
  using namespace daw;
  std::string error;

  // The first C starts halfway through the last beat and crosses three
  // barlines. The E attacks at the first barline beside the held C.
  MidiFile original;
  original.tracks = {MidiTrack{"Violin", {{3360, 8640, 60, 83, 2},
                                         {3840, 960, 64, 95, 2},
                                         {960, 480, 67, 70, 2},
                                         {0, 960, 55, 64, 7}}}};
  Score imported;
  if (!midiToScore(original, &imported, &error)) return fail("barline split: " + error);
  if (imported.parts.size() != 1 || imported.parts[0].measures.size() != 4) {
    return fail("long MIDI note did not allocate four score measures");
  }
  const Tick expected_start[] = {3360, 3840, 7680, 11520};
  const Tick expected_duration[] = {480, 3840, 3840, 480};
  for (std::size_t index = 0; index < 4; ++index) {
    const ScoreNote* segment = findPitch(imported.parts[0].measures[index], 'C');
    if (!segment || segment->start != expected_start[index] || segment->duration != expected_duration[index] ||
        segment->tie_start != (index < 3) || segment->tie_stop != (index > 0) ||
        segment->velocity != 83 || segment->voice != 3) {
      return fail("barline split lost duration, voice, velocity, or tie direction");
    }
  }
  const ScoreNote* chord_e = findPitch(imported.parts[0].measures[1], 'E');
  if (!chord_e || !chord_e->chord || chord_e->tie_start || chord_e->tie_stop) {
    return fail("barline split did not rebuild the chord beside a held note");
  }
  MidiFile exported;
  if (!scoreToMidiFile(imported, &exported, &error) || !equalPerformance(original.tracks[0], exported.tracks[0])) {
    return fail("MIDI -> tied score -> MIDI changed the sounding performance: " + error);
  }

  // Independent voices and staves can sustain the same sounding pitch.
  // Source order is deliberately scrambled; continuation velocities must
  // never replace the original attack velocity.
  Score polyphonic = scoreWithNotes({note(1920, 960, false, true, 20),
                                    note(0, 960, true, false, 110),
                                    note(960, 960, true, true, 30),
                                    note(960, 480, false, true, 25, 2),
                                    note(0, 960, true, false, 60, 2),
                                    note(480, 480, false, true, 25, 1, 2),
                                    note(0, 480, true, false, 50, 1, 2)});
  if (!scoreToMidiFile(polyphonic, &exported, &error) || exported.tracks[0].notes.size() != 3) {
    return fail("independent tied voices: " + error);
  }
  const Tick voice_durations[] = {2880, 1440, 960};
  const std::uint8_t attack_velocities[] = {110, 60, 50};
  for (std::size_t index = 0; index < 3; ++index) {
    if (exported.tracks[0].notes[index].start != 0 || exported.tracks[0].notes[index].duration != voice_durations[index] ||
        exported.tracks[0].notes[index].velocity != attack_velocities[index]) {
      return fail("ties crossed voice/staff boundaries or retriggered velocity");
    }
  }

  // Two identical, untied adjacent notes are intentional repeated attacks.
  const Score repeated = scoreWithNotes({note(0, 960), note(960, 960)});
  if (!scoreToMidiFile(repeated, &exported, &error) || exported.tracks[0].notes.size() != 2) {
    return fail("untied repeated notes were incorrectly merged");
  }

  // Direct score rendering uses the bridge too: tying two segments must be
  // sample-for-sample identical to rendering one sustained diagnostic voice.
  const Score tied = scoreWithNotes({note(0, 960, true, false, 90), note(960, 960, false, true, 45)});
  const Score sustained = scoreWithNotes({note(0, 1920, false, false, 90)});
  if (renderScore(tied, 48000.0, 0.0).samples != renderScore(sustained, 48000.0, 0.0).samples) {
    return fail("tied score rendering reattacked at its notated segment boundary");
  }

  // 6/8 uses three quarter-note ticks per bar. Exact barline endings do not
  // create a zero-length extra segment or a dangling tie.
  MidiFile compound;
  compound.time_signature = {6, 8};
  compound.tracks = {MidiTrack{"Cello", {{2400, 3360, 48, 72, 0}}}};
  if (!midiToScore(compound, &imported, &error) || imported.parts[0].measures.size() != 2 ||
      imported.parts[0].measures[0].notes[0].duration != 480 ||
      imported.parts[0].measures[1].notes[0].duration != 2880 ||
      imported.parts[0].measures[1].notes[0].tie_start ||
      !scoreToMidiFile(imported, &exported, &error) || !equalPerformance(compound.tracks[0], exported.tracks[0])) {
    return fail("compound-meter tie segmentation: " + error);
  }

  std::vector<Score> malformed = {
      scoreWithNotes({note(0, 960, true)}),
      scoreWithNotes({note(0, 960, false, true)}),
      scoreWithNotes({note(0, 960, true), note(1920, 960, false, true)}),
      scoreWithNotes({note(0, 960, true), note(480, 960, false, true)}),
      scoreWithNotes({note(0, 960, true), note(960, 960)}),
      scoreWithNotes({note(0, 960, true), note(960, 960, false, true, 100, 2)}),
      scoreWithNotes({note(0, 960, true), note(960, 960, false, true, 100, 1, 2)}),
      scoreWithNotes({note(0, 960, true), note(960, 960, false, true)}),
      scoreWithNotes({note(0, 960, true), note(0, 960, true), note(960, 960, false, true)}),
      scoreWithNotes({note(0, 960, true)}),
  };
  malformed[7].parts[0].measures[0].notes[1].pitch.step = 'D';
  malformed.back().parts[0].measures[0].notes[0].rest = true;
  Score different_parts = scoreWithNotes({note(0, 960, true)});
  different_parts.parts.push_back(ScorePart{"P2", "Cello", {ScoreMeasure{1, 0, {note(960, 960, false, true)}}}});
  malformed.push_back(std::move(different_parts));
  MidiFile sentinel;
  sentinel.tracks = {MidiTrack{"untouched", {{123, 456, 78, 90, 1}}}};
  for (const Score& invalid : malformed) {
    exported = sentinel;
    error.clear();
    if (scoreToMidiFile(invalid, &exported, &error) || error.find("tie") == std::string::npos ||
        exported.tracks.size() != 1 || exported.tracks[0].name != "untouched" ||
        !equalPerformance(exported.tracks[0], sentinel.tracks[0])) {
      return fail("malformed tie was accepted or mutated output: " + error);
    }
  }

  const Score overflowing = scoreWithNotes({note(0, 960, true),
                                          note(std::numeric_limits<Tick>::max(), 1, false, true)});
  exported = sentinel;
  if (scoreToMidiFile(overflowing, &exported, &error) ||
      error.find("overflows") == std::string::npos || !equalPerformance(exported.tracks[0], sentinel.tracks[0])) {
    return fail("overflowing tied segment changed the output or was accepted");
  }

  // A sustained note and a same-channel, same-pitch reattack would create
  // ambiguous tie identities. Refuse the import rather than returning a
  // document which fails to export. Also cover reversed input order and
  // overlap confined to one bar, which follows the same conservative policy.
  const auto scoreUnchanged = [](const Score& value) {
    return value.parts.size() == 1 && value.parts[0].name == "Strings" &&
           value.parts[0].measures.size() == 1 && value.parts[0].measures[0].notes.size() == 1 &&
           value.parts[0].measures[0].notes[0].start == 123 &&
           value.parts[0].measures[0].notes[0].duration == 456 &&
           value.parts[0].measures[0].notes[0].velocity == 100;
  };
  MidiFile overlap;
  overlap.tracks = {MidiTrack{"Overlapping strings", {{960, 960, 60, 80, 0}, {0, 4800, 60, 90, 0}}}};
  for (const Tick duration : {Tick{4800}, Tick{1920}}) {
    overlap.tracks[0].notes[1].duration = duration;
    imported = scoreWithNotes({note(123, 456)});
    error.clear();
    if (midiToScore(overlap, &imported, &error) || error.find("same-channel same-pitch overlap") == std::string::npos ||
        !scoreUnchanged(imported)) {
      return fail("ambiguous overlapping MIDI import was accepted or damaged output: " + error);
    }
  }
  overlap.tracks[0].notes[1].duration = 4800;
  overlap.tracks[0].notes[0].channel = 1;
  if (!midiToScore(overlap, &imported, &error) || !scoreToMidiFile(imported, &exported, &error) ||
      !equalPerformance(overlap.tracks[0], exported.tracks[0])) {
    return fail("different-channel same-pitch notes were incorrectly rejected: " + error);
  }
  overlap.tracks[0].notes[0].channel = 0;
  overlap.tracks[0].notes[0].start = 4800;
  if (!midiToScore(overlap, &imported, &error) || !scoreToMidiFile(imported, &exported, &error) ||
      !equalPerformance(overlap.tracks[0], exported.tracks[0])) {
    return fail("adjacent same-channel same-pitch reattacks were incorrectly rejected: " + error);
  }

  // Velocity zero is encoded by SMF as note-off. Reject it at both score
  // boundaries instead of successfully exporting a note which disappears.
  const Score zero_velocity = scoreWithNotes({note(0, 960, false, false, 0)});
  exported = sentinel;
  error.clear();
  if (scoreToMidiFile(zero_velocity, &exported, &error) || error.find("velocity") == std::string::npos ||
      exported.tracks.size() != 1 || exported.tracks[0].name != "untouched" ||
      !equalPerformance(exported.tracks[0], sentinel.tracks[0])) {
    return fail("zero attack velocity score was accepted or damaged output: " + error);
  }
  MidiFile zero_velocity_midi;
  zero_velocity_midi.tracks = {MidiTrack{"silent note", {{0, 960, 60, 0, 0}}}};
  imported = scoreWithNotes({note(123, 456)});
  error.clear();
  if (midiToScore(zero_velocity_midi, &imported, &error) || error.find("velocity") == std::string::npos ||
      !scoreUnchanged(imported)) {
    return fail("zero attack velocity MIDI was accepted or damaged output: " + error);
  }
  Score silent_rest = zero_velocity;
  silent_rest.parts[0].measures[0].notes[0].rest = true;
  if (!scoreToMidiFile(silent_rest, &exported, &error) || !exported.tracks[0].notes.empty()) {
    return fail("zero velocity rest should remain valid and emit no note: " + error);
  }

  // Real file and MusicXML round-trips must not reintroduce attacks at any
  // of the three barlines. Each serialized MIDI note must still be unique.
  const auto temporary = std::filesystem::temp_directory_path();
  const auto xml_path = temporary / "classical_daw_ties.musicxml";
  const auto midi_path = temporary / "classical_daw_ties.mid";
  if (!midiToScore(original, &imported, &error) || !writeMusicXmlFile(imported, xml_path.string(), &error)) {
    return fail("tied MusicXML write: " + error);
  }
  Score xml_score;
  if (!readMusicXmlFile(xml_path.string(), &xml_score, &error) ||
      !writeScoreMidiFile(xml_score, midi_path.string(), &error)) return fail("tied MusicXML -> MIDI: " + error);
  MidiFile file_roundtrip;
  if (!readMidiFile(midi_path.string(), &file_roundtrip, &error) ||
      !equalPerformance(original.tracks[0], file_roundtrip.tracks[0])) return fail("tied file performance: " + error);
  std::filesystem::remove(xml_path);
  std::filesystem::remove(midi_path);

  // Invalid ties must also preserve an existing destination file.
  {
    std::ofstream protected_file(midi_path);
    protected_file << "existing-midi";
  }
  if (writeScoreMidiFile(malformed.front(), midi_path.string(), &error)) return fail("dangling tie was written");
  if (writeScoreMidiFile(zero_velocity, midi_path.string(), &error)) return fail("zero velocity attack was written");
  std::ifstream protected_file(midi_path);
  const std::string contents((std::istreambuf_iterator<char>(protected_file)), std::istreambuf_iterator<char>());
  if (contents != "existing-midi") return fail("invalid tie damaged the destination file");
  protected_file.close();
  std::filesystem::remove(midi_path);

  std::cout << "classical-daw score tie tests passed\n";
  return 0;
}
