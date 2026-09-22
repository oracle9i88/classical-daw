// Read-only source-file probe for MIDI -> Score -> MusicXML -> Score -> MIDI.
// The output directory must not exist; generated MusicXML stays there for review.
// Build from the repository root:
// c++ -std=c++17 -Isrc/engine/include scripts/check_score_interchange.cpp \
//   src/engine/{midi,score,score_midi,tempo_map}.cpp -o /tmp/check_score_interchange
// Usage: check_score_interchange NEW_OUTPUT_DIRECTORY INPUT_MIDI [INPUT_MIDI ...]

#include "daw/score_midi.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

namespace {

using NoteKey = std::tuple<daw::Tick, daw::Tick, int, int>;

std::vector<NoteKey> noteKeys(const daw::MidiTrack& track) {
  std::vector<NoteKey> result;
  result.reserve(track.notes.size());
  for (const auto& note : track.notes) {
    result.emplace_back(note.start, note.end(), note.pitch, note.velocity);
  }
  std::sort(result.begin(), result.end());
  return result;
}

void printNote(const char* label, const std::vector<NoteKey>& notes, std::size_t index) {
  std::cerr << label;
  if (index == notes.size()) {
    std::cerr << "<missing>";
    return;
  }
  const auto& note = notes[index];
  std::cerr << "start=" << std::get<0>(note) << " end=" << std::get<1>(note)
            << " pitch=" << std::get<2>(note) << " velocity=" << std::get<3>(note);
}

bool checkFile(const std::string& input_path, const std::filesystem::path& xml_path,
               std::size_t* imported_notes, std::size_t* verified_notes) {
  daw::MidiFile original, exported;
  daw::MidiImportReport report;
  daw::Score score, restored;
  std::string error;
  const auto failure = [&](const char* stage) {
    std::cerr << "FAIL " << input_path << " at " << stage << ": " << error << '\n';
    return false;
  };
  if (!daw::readMidiFile(input_path, &original, &error, &report)) return failure("readMidiFile");
  std::vector<std::size_t> nonempty_tracks;
  std::size_t file_notes = 0;
  for (std::size_t index = 0; index < original.tracks.size(); ++index) {
    file_notes += original.tracks[index].notes.size();
    if (!original.tracks[index].notes.empty() || !original.tracks[index].channel_events.empty()) {
      nonempty_tracks.push_back(index);
    }
  }
  *imported_notes += file_notes;
  std::cout << "INPUT " << input_path << " notes=" << file_notes
            << " source_ppq=" << report.source_ticks_per_quarter
            << " normalized_ppq=" << original.ticks_per_quarter
            << " rounded_note_boundaries=" << report.rounded_note_boundaries << '\n';
  if (!daw::midiToScore(original, &score, &error)) return failure("midiToScore");
  daw::MusicXmlExportReport omissions;
  if (!daw::writeMusicXmlFile(score, xml_path.string(), &error, &omissions)) return failure("writeMusicXmlFile");
  std::cout << "  MusicXML omitted_channel_events=" << omissions.omitted_midi_events
            << " omitted_note_playback_metadata=" << omissions.omitted_note_midi_metadata << '\n';
  if (!daw::readMusicXmlFile(xml_path.string(), &restored, &error)) return failure("readMusicXmlFile");
  if (!daw::scoreToMidiFile(restored, &exported, &error)) return failure("scoreToMidiFile");

  // The score importer omits metadata-only conductor tracks, but retains
  // event-only tracks (whose events MusicXML reports as omitted). All remaining
  // tracks must correspond in source order, including duplicate note counts.
  if (exported.tracks.size() != nonempty_tracks.size()) {
    std::cerr << "FAIL " << input_path << " nonempty track count " << nonempty_tracks.size()
              << " -> " << exported.tracks.size() << '\n';
    return false;
  }
  for (std::size_t index = 0; index < nonempty_tracks.size(); ++index) {
    const std::size_t source_index = nonempty_tracks[index];
    const auto before = noteKeys(original.tracks[source_index]);
    const auto after = noteKeys(exported.tracks[index]);
    if (before != after) {
      std::size_t mismatch = 0;
      while (mismatch < std::min(before.size(), after.size()) && before[mismatch] == after[mismatch]) ++mismatch;
      std::cerr << "FAIL " << input_path << " source_track=" << source_index << " output_track=" << index
                << " note_count=" << before.size() << " -> " << after.size()
                << " first_sorted_mismatch=" << mismatch << '\n';
      printNote("  before: ", before, mismatch);
      printNote("\n  after:  ", after, mismatch);
      std::cerr << '\n';
      return false;
    }
    std::cout << "  PASS source_track=" << source_index << " output_track=" << index
              << " notes=" << before.size() << '\n';
  }
  *verified_notes += file_notes;
  std::cout << "PASS " << input_path << " notes=" << file_notes << " xml=" << xml_path << '\n';
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " NEW_OUTPUT_DIRECTORY INPUT_MIDI [INPUT_MIDI ...]\n";
    return 2;
  }
  const std::filesystem::path output_directory(argv[1]);
  std::error_code directory_error;
  if (!std::filesystem::create_directory(output_directory, directory_error)) {
    std::cerr << "Output directory must not exist and must be creatable: " << output_directory;
    if (directory_error) std::cerr << " (" << directory_error.message() << ')';
    std::cerr << '\n';
    return 2;
  }
  std::cout << "Compare note start/end/pitch/velocity per nonempty track in normalized 960 PPQ.\n"
            << "Channel remapping and tempo/meter-map semantics are not verified; empty tracks are omitted.\n";
  std::size_t imported_notes = 0;
  std::size_t verified_notes = 0;
  bool passed = true;
  for (int index = 2; index < argc; ++index) {
    const auto xml_path = output_directory / ("roundtrip-" + std::to_string(index - 1) + ".musicxml");
    if (!checkFile(argv[index], xml_path, &imported_notes, &verified_notes)) passed = false;
  }
  std::cout << (passed ? "PASS" : "FAIL") << " imported_notes=" << imported_notes
            << " verified_notes=" << verified_notes << '\n';
  return passed ? 0 : 1;
}
