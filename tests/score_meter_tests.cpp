#include "daw/meter_map.hpp"
#include "daw/render.hpp"
#include "daw/score_midi.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using daw::Tick;
using daw::TimeSignature;
using daw::TimeSignatureChange;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void rejected(Function&& function, const std::string& reason) {
  bool threw = false;
  try {
    function();
  } catch (const std::invalid_argument&) {
    threw = true;
  } catch (const std::length_error&) {
    threw = true;
  }
  require(threw, reason);
}

bool sameSignature(const TimeSignature& a, const TimeSignature& b) {
  return a.numerator == b.numerator && a.denominator == b.denominator &&
         a.clocks_per_click == b.clocks_per_click &&
         a.notated_32nds_per_quarter == b.notated_32nds_per_quarter;
}

bool sameChanges(const std::vector<TimeSignatureChange>& a,
                 const std::vector<TimeSignatureChange>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t index = 0; index < a.size(); ++index) {
    if (a[index].tick != b[index].tick || !sameSignature(a[index].signature, b[index].signature)) return false;
  }
  return true;
}

struct TempDirectory {
  std::filesystem::path path;
  TempDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("classical_daw_score_meter_" + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    require(std::filesystem::create_directory(path), "could not create isolated meter test directory");
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::string readText(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  require(static_cast<bool>(file), "could not read protected meter test destination");
  return {(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()};
}

void writeText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
  require(static_cast<bool>(file), "could not write protected meter test destination");
}

daw::Score basicScore() {
  daw::Score score;
  score.parts = {daw::ScorePart{"P1", "Meter fixture", {daw::ScoreMeasure{1, 0, {daw::ScoreNote{}}}}}};
  return score;
}

void gridEquals(const TimeSignature& initial, const std::vector<TimeSignatureChange>& changes,
                Tick end, const std::vector<std::pair<Tick, Tick>>& expected,
                const std::string& reason) {
  const auto grid = daw::makeMeasureGrid(initial, changes, end);
  require(grid.size() == expected.size(), reason + ": wrong measure count");
  for (std::size_t index = 0; index < grid.size(); ++index) {
    require(grid[index].start == expected[index].first && grid[index].end == expected[index].second,
            reason + ": wrong boundaries at measure " + std::to_string(index + 1));
  }
}

void mathematicalMeasureGrid() {
  const TimeSignature common{4, 4, 24, 8};
  require(daw::meterMeasureTicks(common) == 3840, "4/4 must span four MIDI quarter notes");
  require(daw::meterMeasureTicks({3, 4, 24, 8}) == 2880, "3/4 bar length");
  require(daw::meterMeasureTicks({6, 8, 36, 8}) == 2880, "6/8 bar length is independent of metronome clicks");
  gridEquals(common, {{3840, {3, 4, 24, 8}}, {6720, {6, 8, 36, 8}}}, 9600,
             {{0, 3840}, {3840, 6720}, {6720, 9600}}, "4/4 to 3/4 to 6/8");
  gridEquals(common, {{3840, {3, 4, 24, 8}}, {6720, {6, 8, 36, 8}}}, 10000,
             {{0, 3840}, {3840, 6720}, {6720, 9600}, {9600, 12480}}, "complete final measure");
  gridEquals(common, {{1000, {3, 4, 24, 8}}, {2440, {6, 8, 36, 8}}}, 5000,
             {{0, 1000}, {1000, 2440}, {2440, 5320}}, "mid-bar structural changes must close partial measures");
  gridEquals(common, {{960, {4, 4, 36, 8}}, {1920, {4, 4, 36, 8}}, {4000, {4, 4, 12, 8}}}, 7000,
             {{0, 3840}, {3840, 7680}}, "click-only and redundant meter events must not restart bars");
  gridEquals(common, {}, 0, {{0, 3840}}, "empty content still needs an editable bar");
  gridEquals(common, {}, 3840, {{0, 3840}}, "exact bar end must not create an extra measure");

  // The bb byte changes notation-to-MIDI-quarter scale, while cc describes
  // metronome clicks. Neither byte is inferred from a conventional n/d pair.
  require(daw::meterMeasureTicks({4, 4, 24, 4}) == 7680, "bb=4 must double the notated bar's tick length");
  gridEquals(common, {{1920, {4, 4, 24, 4}}}, 10000,
             {{0, 1920}, {1920, 9600}, {9600, 17280}}, "a bb change is structural");
  require(daw::meterMeasureTicks({5, 8, 24, 3}) == 6400, "integer odd-bb bar length must remain exact");
  require(daw::meterMeasureTicks({7, 4, 24, 7}) == 7680, "odd bb may be representable without rounding");
  rejected([] { (void)daw::meterMeasureTicks({1, 128, 24, 7}); },
           "fractional-tick meter must be rejected rather than truncated");
  rejected([] { (void)daw::makeMeasureGrid({1, 128, 24, 7}, {}, 960); },
           "grid silently rounded a fractional-tick bar length");
}

bool sameNote(const daw::MidiNote& a, const daw::MidiNote& b) {
  return a.start == b.start && a.duration == b.duration && a.pitch == b.pitch &&
         a.velocity == b.velocity && a.channel == b.channel &&
         a.release_velocity == b.release_velocity && a.on_order == b.on_order && a.off_order == b.off_order;
}

void tiedNotesAndSound(const std::filesystem::path& directory) {
  daw::MidiFile source;
  source.time_signature = {4, 4, 24, 8};
  source.meter_changes = {{3840, {3, 4, 24, 8}}, {6720, {6, 8, 36, 8}}};
  source.tracks = {{"Conductor", {}, {}},
                   {"Strings", {{3000, 7200, 69, 90, 2, 61, 1, 9},
                                {6720, 1200, 76, 82, 2, 32, 5, 6}}, {}}};
  const auto source_map = source.meter_changes;
  daw::Score imported;
  std::string error;
  require(daw::midiToScore(source, &imported, &error), "variable-meter score import: " + error);
  require(imported.parts.size() == 1 && imported.parts[0].measures.size() == 4,
          "variable meter did not construct the four required measures");
  require(sameSignature(imported.time_signature, source.time_signature) &&
              sameChanges(imported.meter_changes, source.meter_changes), "score import lost meter metadata");
  const Tick expected_starts[] = {3000, 3840, 6720, 9600};
  const Tick expected_durations[] = {840, 2880, 2880, 600};
  const Tick expected_bars[] = {0, 3840, 6720, 9600};
  for (std::size_t index = 0; index < 4; ++index) {
    const auto& measure = imported.parts[0].measures[index];
    require(measure.start == expected_bars[index], "score measure start ignored a meter change");
    const auto held = std::find_if(measure.notes.begin(), measure.notes.end(), [](const daw::ScoreNote& value) {
      return value.pitch.step == 'A' && value.pitch.octave == 4;
    });
    require(held != measure.notes.end() && held->start == expected_starts[index] &&
                held->duration == expected_durations[index] && held->tie_stop == (index > 0) &&
                held->tie_start == (index < 3) && held->midi_channel == 2,
            "held note was not tied at actual variable-meter boundaries");
    require(held->midi_on_order == (index == 0 ? 1U : 0U) &&
                held->midi_off_order == (index == 3 ? 9U : 0U),
            "meter-based tie split damaged source attack/release ordinals");
  }
  daw::MidiFile restored;
  require(daw::scoreToMidiFile(imported, &restored, &error), "variable-meter score export: " + error);
  require(restored.tracks.size() == 1 && restored.tracks[0].notes.size() == 2 &&
              sameNote(restored.tracks[0].notes[0], source.tracks[1].notes[0]) &&
              sameNote(restored.tracks[0].notes[1], source.tracks[1].notes[1]),
          "meter changes altered note onset, endpoint, velocity, route, or source order");
  require(sameSignature(restored.time_signature, source.time_signature) &&
              sameChanges(restored.meter_changes, source_map), "MIDI export omitted meter metadata");
  const auto original_audio = daw::renderMidiFile(source, 8000.0, 0.1);
  require(daw::renderScore(imported, 8000.0, 0.1).samples == original_audio.samples,
          "variable-meter score conversion changed the sounding performance");

  // Include the wire boundary, then compare the score rendering to the
  // parsed SMF. This isolates notation changes from SMF source-order renumbering.
  const auto path = directory / "meter-roundtrip.mid";
  require(daw::writeMidiFile(source, path.string(), &error), "write variable-meter SMF: " + error);
  daw::MidiFile wire;
  require(daw::readMidiFile(path.string(), &wire, &error), "read variable-meter SMF: " + error);
  require(sameSignature(wire.time_signature, source.time_signature) && sameChanges(wire.meter_changes, source_map),
          "SMF lost one of the four meter payload fields or an event tick");
  daw::Score wire_score;
  require(daw::midiToScore(wire, &wire_score, &error), "SMF-to-score meter bridge: " + error);
  require(daw::renderScore(wire_score, 8000.0, 0.1).samples == daw::renderMidiFile(wire, 8000.0, 0.1).samples,
          "SMF -> Score changed audio while retaining the same tempo and notes");

  auto bb_scaled = source;
  bb_scaled.time_signature = {4, 4, 24, 4};
  bb_scaled.meter_changes.clear();
  daw::Score scaled_score;
  require(daw::midiToScore(bb_scaled, &scaled_score, &error), "bb-scaled notation import: " + error);
  require(scaled_score.parts[0].measures.size() == 2 && scaled_score.parts[0].measures[1].start == 7680,
          "bb=4 did not change the notation grid");
  require(daw::renderScore(scaled_score, 8000.0, 0.1).samples == original_audio.samples,
          "meter or bb metadata changed real-time playback speed independently of tempo");
  require(sameChanges(source.meter_changes, source_map) && source.tracks.size() == 2,
          "meter conversion changed caller-owned MIDI source data");
}

void partialBarsAndLateEvents() {
  daw::MidiFile source;
  source.meter_changes = {{1000, {3, 4, 24, 8}}, {2440, {6, 8, 36, 8}}, {20000, {5, 4, 24, 8}}};
  source.tracks = {{"Conductor", {}, {}}, {"Piano", {{500, 4500, 60, 88, 0}}, {}},
                   {"Controller only", {}, {{30000, daw::MidiChannelEventType::ControlChange, 1, 7, 100, 0}}}};
  daw::Score score;
  std::string error;
  require(daw::midiToScore(source, &score, &error), "partial-bar and late-meter import: " + error);
  require(score.parts.size() == 2 && score.parts[0].measures.size() == 3,
          "late meter events must not inflate a note part's required measures");
  const Tick starts[] = {0, 1000, 2440};
  const Tick note_starts[] = {500, 1000, 2440};
  const Tick durations[] = {500, 1440, 2560};
  for (std::size_t index = 0; index < 3; ++index) {
    const auto& measure = score.parts[0].measures[index];
    require(measure.start == starts[index] && measure.notes.size() == 1 &&
                measure.notes[0].start == note_starts[index] && measure.notes[0].duration == durations[index],
            "mid-bar meter change moved or rounded a sustained note");
  }
  require(score.parts[1].measures.size() == 1 && score.parts[1].measures[0].start == 0 &&
              score.parts[1].measures[0].notes.empty() && score.parts[1].midi_events.size() == 1,
          "a late event-only track must retain one empty editable measure");
  require(sameChanges(score.meter_changes, source.meter_changes), "global meter event after all notes was discarded");
  daw::MidiFile restored;
  require(daw::scoreToMidiFile(score, &restored, &error) && sameChanges(restored.meter_changes, source.meter_changes),
          "late global meter event did not survive score export");
  require(restored.tracks[0].notes.size() == 1 &&
              sameNote(restored.tracks[0].notes[0], source.tracks[1].notes[0]),
          "partial-bar tie reconstruction changed the original note endpoints");
}

void xmlSafety(const std::filesystem::path& directory) {
  const auto path = directory / "protected.musicxml";
  std::string error;
  auto score = basicScore();
  auto protected_export = [&](const daw::Score& unsupported) {
    writeText(path, "protected-meter-score");
    daw::MusicXmlExportReport report{11, 22, 33, 44};
    require(!daw::writeMusicXmlFile(unsupported, path.string(), &error, &report) && !error.empty(),
            "MusicXML accepted an unsupported meter grid");
    require(readText(path) == "protected-meter-score" && report.omitted_midi_events == 11 &&
                report.omitted_note_midi_metadata == 22 && report.omitted_tempo_changes == 33 &&
                report.omitted_meter_playback_metadata == 44,
            "unsupported meter XML export damaged the destination or diagnostics");
  };
  score.meter_changes = {{1920, {3, 4, 24, 8}}};
  protected_export(score);
  score.meter_changes = {{960, {4, 4, 36, 8}}};
  protected_export(score);  // Even a click-only later event is explicitly unsupported by this XML slice.
  score.meter_changes.clear();
  score.time_signature = {4, 4, 24, 4};
  protected_export(score);
  score.time_signature = {4, 4, 36, 8};
  daw::MusicXmlExportReport report{11, 22, 33, 44};
  require(daw::writeMusicXmlFile(score, path.string(), &error, &report), "constant meter XML export: " + error);
  require(report.omitted_meter_playback_metadata == 1 && report.omitted_midi_events == 0 &&
              report.omitted_note_midi_metadata == 0 && report.omitted_tempo_changes == 0,
          "initial nonstandard click metadata omission was not explicitly counted");
  score.time_signature.clocks_per_click = 24;
  require(daw::writeMusicXmlFile(score, path.string(), &error, &report) &&
              report.omitted_meter_playback_metadata == 0,
          "successful ordinary meter export retained stale click-metadata diagnostics");
}

void invalidMapsAndBounds() {
  const TimeSignature common{4, 4, 24, 8};
  for (const auto signature : std::vector<TimeSignature>{{0, 4, 24, 8}, {4, 0, 24, 8},
                                                       {4, 3, 24, 8}, {4, 4, 24, 0}}) {
    rejected([&] { daw::validateMeterMap(signature, {}); }, "invalid initial meter was accepted");
    rejected([&] { daw::validateMeterMap(common, {{960, signature}}); }, "invalid later meter was accepted");
  }
  const std::vector<std::vector<TimeSignatureChange>> invalid_maps = {
      {{0, common}}, {{-1, common}}, {{960, common}, {480, common}}, {{480, common}, {480, common}},
      {{960, {0, 4, 24, 8}}}, {{960, {4, 3, 24, 8}}}, {{960, {4, 4, 24, 0}}}};
  for (const auto& changes : invalid_maps) {
    rejected([&] { daw::validateMeterMap(common, changes); }, "invalid ordered meter map was accepted");
    auto invalid_score = basicScore();
    invalid_score.meter_changes = changes;
    daw::MidiFile destination;
    destination.time_signature = {7, 8, 42, 8};
    destination.meter_changes = {{111, {5, 4, 12, 8}}};
    destination.tracks = {{"untouched", {{17, 43, 42, 61, 4}}, {}}};
    const auto old_changes = destination.meter_changes;
    std::string error;
    require(!daw::scoreToMidiFile(invalid_score, &destination, &error) && !error.empty(),
            "score-to-MIDI accepted invalid meter changes");
    require(sameSignature(destination.time_signature, {7, 8, 42, 8}) &&
                sameChanges(destination.meter_changes, old_changes) && destination.tracks.size() == 1 &&
                destination.tracks[0].name == "untouched" && destination.tracks[0].notes[0].pitch == 42,
            "failed meter export changed the caller's destination MIDI");
    require(sameChanges(invalid_score.meter_changes, changes), "failed export changed source meter state");

    daw::MidiFile invalid_midi;
    invalid_midi.meter_changes = changes;
    invalid_midi.tracks = {{"source", {{0, 960, 60, 90, 0}}, {}}};
    auto output = basicScore();
    output.time_signature = {5, 8, 12, 8};
    output.meter_changes = {{123, {7, 8, 42, 8}}};
    const auto old_score_changes = output.meter_changes;
    require(!daw::midiToScore(invalid_midi, &output, &error) && !error.empty(),
            "MIDI-to-score accepted invalid meter changes");
    require(sameSignature(output.time_signature, {5, 8, 12, 8}) &&
                sameChanges(output.meter_changes, old_score_changes) && output.parts[0].name == "Meter fixture",
            "failed meter import changed the caller's destination score");
    require(sameChanges(invalid_midi.meter_changes, changes), "failed import changed source meter state");
  }
  rejected([&] { (void)daw::makeMeasureGrid(common, {}, -1); }, "negative grid end was accepted");
  rejected([&] { (void)daw::makeMeasureGrid(common, {}, 0, 0); }, "zero measure-count bound was accepted");
  rejected([&] { (void)daw::makeMeasureGrid(common, {}, 3841, 1); }, "grid exceeded its measure-count bound");
  require(daw::makeMeasureGrid(common, {}, 3840, 1).size() == 1, "inclusive measure-count bound failed");
  rejected([&] { (void)daw::makeMeasureGrid(common, {}, std::numeric_limits<Tick>::max(), 2); },
           "huge grid end must fail within the supplied resource bound");

  // Ordered and otherwise valid redundant entries isolate the million-event
  // limit from signature validity and bar-reset behavior.
  std::vector<TimeSignatureChange> oversized;
  oversized.reserve(1000001);
  for (std::size_t index = 0; index < 1000001; ++index) {
    oversized.push_back({static_cast<Tick>(index + 1), common});
  }
  rejected([&] { daw::validateMeterMap(common, oversized); }, "meter map accepted more than one million changes");

  daw::MidiFile fractional;
  fractional.time_signature = {1, 128, 24, 7};
  fractional.tracks = {{"fractional grid", {{0, 960, 60, 90, 0}}, {}}};
  auto output = basicScore();
  output.parts[0].name = "protected fractional import";
  std::string error;
  require(!daw::midiToScore(fractional, &output, &error) && !error.empty() &&
              output.parts[0].name == "protected fractional import",
          "fractional-tick notation was silently rounded or damaged the output score");
}

}  // namespace

int main() {
  try {
    const TempDirectory directory;
    mathematicalMeasureGrid();
    tiedNotesAndSound(directory.path);
    partialBarsAndLateEvents();
    xmlSafety(directory.path);
    invalidMapsAndBounds();
    std::cout << "classical-daw score meter tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
