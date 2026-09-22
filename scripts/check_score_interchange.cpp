// Read-only source-file probe for MIDI -> Score -> MusicXML -> Score -> MIDI.
// The output directory must not exist; generated MusicXML stays there for review.
// Build from the repository root:
// c++ -std=c++17 -Isrc/engine/include scripts/check_score_interchange.cpp \
//   src/engine/{midi,meter_map,score,score_midi,score_tempo,tempo_map}.cpp -o /tmp/check_score_interchange
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
using MeterKey = std::tuple<daw::Tick, int, int>;

bool meterKeys(const daw::TimeSignature& initial, const std::vector<daw::TimeSignatureChange>& changes,
               std::vector<MeterKey>* result, std::string* error) {
  if (initial.notated_32nds_per_quarter != 8 ||
      std::any_of(changes.begin(), changes.end(), [](const daw::TimeSignatureChange& change) {
        return change.signature.notated_32nds_per_quarter != 8;
      })) {
    *error = "this notation comparison requires bb = 8 for every meter";
    return false;
  }
  result->clear();
  result->emplace_back(0, initial.numerator, initial.denominator);
  // Click-only and repeated signatures do not change notation or restart bars.
  // Keep the tick of each actual n/d transition, including a return to an
  // earlier signature after an intervening different meter.
  for (const auto& change : changes) {
    if (std::get<1>(result->back()) != change.signature.numerator ||
        std::get<2>(result->back()) != change.signature.denominator) {
      result->emplace_back(change.tick, change.signature.numerator, change.signature.denominator);
    }
  }
  return true;
}

bool compareMeters(const std::string& input_path, const char* stage,
                   const std::vector<MeterKey>& before, const std::vector<MeterKey>& after) {
  if (before == after) return true;
  std::size_t mismatch = 0;
  while (mismatch < std::min(before.size(), after.size()) && before[mismatch] == after[mismatch]) ++mismatch;
  std::cerr << "FAIL " << input_path << " at " << stage << " meter_count=" << before.size()
            << " -> " << after.size() << " first_meter_mismatch=" << mismatch << '\n';
  const auto print = [&](const char* label, const std::vector<MeterKey>& entries) {
    std::cerr << label;
    if (mismatch == entries.size()) std::cerr << "<missing>";
    else std::cerr << "tick=" << std::get<0>(entries[mismatch])
                   << " n=" << std::get<1>(entries[mismatch]) << " d=" << std::get<2>(entries[mismatch]);
    std::cerr << '\n';
  };
  print("  before: ", before);
  print("  after:  ", after);
  return false;
}

std::vector<daw::Tick> measureStarts(const daw::ScorePart& part) {
  std::vector<daw::Tick> result;
  result.reserve(part.measures.size());
  for (const auto& measure : part.measures) result.push_back(measure.start);
  return result;
}

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
               std::size_t* imported_notes, std::size_t* verified_notes,
               std::size_t* imported_meters, std::size_t* verified_meters) {
  daw::MidiFile original, exported;
  daw::MidiImportReport report;
  daw::Score score, restored;
  std::string error;
  const auto failure = [&](const char* stage) {
    std::cerr << "FAIL " << input_path << " at " << stage << ": " << error << '\n';
    return false;
  };
  if (!daw::readMidiFile(input_path, &original, &error, &report)) return failure("readMidiFile");
  std::vector<MeterKey> original_meters, score_meters, restored_meters, exported_meters;
  if (!meterKeys(original.time_signature, original.meter_changes, &original_meters, &error)) {
    return failure("source meter scope");
  }
  *imported_meters += original_meters.size();
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
            << " rounded_note_boundaries=" << report.rounded_note_boundaries
            << " canonical_meter_entries=" << original_meters.size() << '\n';
  if (!daw::midiToScore(original, &score, &error)) return failure("midiToScore");
  if (!meterKeys(score.time_signature, score.meter_changes, &score_meters, &error)) return failure("Score meter scope");
  if (!compareMeters(input_path, "MIDI -> Score", original_meters, score_meters)) return false;
  daw::MusicXmlExportReport omissions;
  if (!daw::writeMusicXmlFile(score, xml_path.string(), &error, &omissions)) return failure("writeMusicXmlFile");
  std::cout << "  MusicXML omitted_channel_events=" << omissions.omitted_midi_events
            << " omitted_note_playback_metadata=" << omissions.omitted_note_midi_metadata
            << " omitted_tempo_changes=" << omissions.omitted_tempo_changes
            << " omitted_meter_playback_metadata=" << omissions.omitted_meter_playback_metadata << '\n';
  if (!daw::readMusicXmlFile(xml_path.string(), &restored, &error)) return failure("readMusicXmlFile");
  if (!meterKeys(restored.time_signature, restored.meter_changes, &restored_meters, &error)) {
    return failure("restored Score meter scope");
  }
  if (!compareMeters(input_path, "Score -> MusicXML -> Score", score_meters, restored_meters)) return false;
  if (score.parts.size() != restored.parts.size()) {
    std::cerr << "FAIL " << input_path << " restored part count " << score.parts.size()
              << " -> " << restored.parts.size() << '\n';
    return false;
  }
  std::size_t checked_measure_starts = 0;
  std::size_t checked_measure_durations = 0;
  for (std::size_t index = 0; index < score.parts.size(); ++index) {
    const auto before = measureStarts(score.parts[index]);
    const auto after = measureStarts(restored.parts[index]);
    if (before != after) {
      std::size_t mismatch = 0;
      while (mismatch < std::min(before.size(), after.size()) && before[mismatch] == after[mismatch]) ++mismatch;
      std::cerr << "FAIL " << input_path << " part=" << index << " measure_count=" << before.size()
                << " -> " << after.size() << " first_measure_start_mismatch=" << mismatch
                << " before=" << (mismatch == before.size() ? "<missing>" : std::to_string(before[mismatch]))
                << " after=" << (mismatch == after.size() ? "<missing>" : std::to_string(after[mismatch])) << '\n';
      return false;
    }
    for (std::size_t measure_index = 0; measure_index < score.parts[index].measures.size(); ++measure_index) {
      const auto duration = score.parts[index].measures[measure_index].duration;
      if (duration == 0) continue;  // Legacy unspecified extent has no parity claim.
      const auto restored_duration = restored.parts[index].measures[measure_index].duration;
      if (duration != restored_duration) {
        std::cerr << "FAIL " << input_path << " part=" << index << " measure=" << measure_index
                  << " explicit_measure_duration=" << duration << " -> " << restored_duration << '\n';
        return false;
      }
      ++checked_measure_durations;
    }
    checked_measure_starts += before.size();
  }
  if (!daw::scoreToMidiFile(restored, &exported, &error)) return failure("scoreToMidiFile");
  if (!meterKeys(exported.time_signature, exported.meter_changes, &exported_meters, &error)) {
    return failure("exported MIDI meter scope");
  }
  if (!compareMeters(input_path, "MIDI -> MusicXML -> MIDI", original_meters, exported_meters)) return false;

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
  *verified_meters += original_meters.size();
  std::cout << "PASS " << input_path << " notes=" << file_notes
            << " verified_meter_entries=" << original_meters.size()
            << " verified_measure_starts=" << checked_measure_starts
            << " verified_measure_durations=" << checked_measure_durations << " xml=" << xml_path << '\n';
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
            << "Compare canonical n/d meter maps (bb = 8, cc ignored), measure starts and explicit durations.\n"
            << "Adjacent equal n/d entries are folded; tempo and channel semantics are not verified.\n"
            << "Metadata-only tracks are omitted; MusicXML omission counts are reported per file.\n";
  std::size_t imported_notes = 0;
  std::size_t verified_notes = 0;
  std::size_t imported_meters = 0;
  std::size_t verified_meters = 0;
  bool passed = true;
  for (int index = 2; index < argc; ++index) {
    const auto xml_path = output_directory / ("roundtrip-" + std::to_string(index - 1) + ".musicxml");
    if (!checkFile(argv[index], xml_path, &imported_notes, &verified_notes, &imported_meters, &verified_meters)) passed = false;
  }
  std::cout << (passed ? "PASS" : "FAIL") << " imported_notes=" << imported_notes
            << " verified_notes=" << verified_notes << " imported_meter_entries=" << imported_meters
            << " verified_meter_entries=" << verified_meters << '\n';
  return passed ? 0 : 1;
}
