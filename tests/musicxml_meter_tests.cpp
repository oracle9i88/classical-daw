#include "daw/score_midi.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using daw::Score;
using daw::ScoreMeasure;
using daw::ScoreNote;
using daw::Tick;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

struct TempDirectory {
  std::filesystem::path path;
  TempDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("classical_daw_xml_meter_" + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    require(std::filesystem::create_directory(path), "cannot create isolated MusicXML meter test directory");
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

void writeText(const std::filesystem::path& path, const std::string& value) {
  std::ofstream file(path, std::ios::binary);
  file << value;
  require(static_cast<bool>(file), "cannot write MusicXML meter fixture");
}

std::string readText(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  require(static_cast<bool>(file), "cannot read MusicXML meter fixture");
  return {(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()};
}

std::string time(int numerator, int denominator) {
  return "<attributes><time><beats>" + std::to_string(numerator) + "</beats><beat-type>" +
         std::to_string(denominator) + "</beat-type></time></attributes>";
}

std::string xmlNote(Tick duration, int voice = 1, const std::string& step = "C") {
  return "<note><pitch><step>" + step + "</step><octave>4</octave></pitch><duration>" +
         std::to_string(duration) + "</duration><voice>" + std::to_string(voice) + "</voice></note>";
}

std::string forward(Tick duration) {
  return "<forward><duration>" + std::to_string(duration) + "</duration></forward>";
}

std::string backup(Tick duration) {
  return "<backup><duration>" + std::to_string(duration) + "</duration></backup>";
}

std::string measure(const std::string& number, const std::string& body, const std::string& attributes = "") {
  return "<measure number='" + number + "'" + attributes + ">" + body + "</measure>";
}

std::string document(const std::vector<std::string>& part_bodies) {
  std::string result = "<score-partwise><part-list>";
  for (std::size_t index = 0; index < part_bodies.size(); ++index) {
    const auto id = "P" + std::to_string(index + 1);
    result += "<score-part id='" + id + "'><part-name>" + id + "</part-name></score-part>";
  }
  result += "</part-list>";
  for (std::size_t index = 0; index < part_bodies.size(); ++index) {
    result += "<part id='P" + std::to_string(index + 1) + "'>" + part_bodies[index] + "</part>";
  }
  return result + "</score-partwise>";
}

ScoreNote note(Tick start, Tick duration, char step = 'C', int voice = 1) {
  ScoreNote result;
  result.start = start;
  result.duration = duration;
  result.pitch = {step, 0, 4};
  result.velocity = 90;
  result.voice = static_cast<std::uint16_t>(voice);
  return result;
}

Score scoreWithMeasures(std::vector<ScoreMeasure> measures) {
  Score score;
  score.parts = {daw::ScorePart{"P1", "Piano", std::move(measures)}};
  return score;
}

bool signature(const daw::TimeSignature& value, int numerator, int denominator) {
  return value.numerator == numerator && value.denominator == denominator &&
         value.clocks_per_click == 24 && value.notated_32nds_per_quarter == 8;
}

using Performance = std::vector<std::tuple<std::size_t, Tick, Tick, int, int>>;
Performance performance(const daw::MidiFile& midi) {
  Performance result;
  for (std::size_t track = 0; track < midi.tracks.size(); ++track) {
    for (const auto& value : midi.tracks[track].notes) {
      result.emplace_back(track, value.start, value.end(), value.pitch, value.velocity);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

Score roundTrip(const Score& source, const std::filesystem::path& path,
                daw::MusicXmlExportReport* report = nullptr) {
  std::string error;
  require(daw::writeMusicXmlFile(source, path.string(), &error, report), "write meter-aware XML: " + error);
  Score restored;
  require(daw::readMusicXmlFile(path.string(), &restored, &error), "read meter-aware XML: " + error);
  return restored;
}

void canonicalMeterAndMidiPerformance(const std::filesystem::path& path) {
  daw::MidiFile source;
  source.tempo = daw::TempoMap(91.25);
  source.tempo.addChange(960, 123.45);
  source.time_signature = {4, 4, 36, 8};
  source.meter_changes = {{960, {4, 4, 12, 8}}, {3840, {3, 4, 24, 8}},
                          {5000, {3, 4, 24, 8}}, {6720, {6, 8, 36, 8}}};
  source.tracks = {{"Piano", {{0, 8400, 60, 92, 0}, {480, 480, 67, 60, 0},
                               {7000, 200, 64, 78, 0}}, {}}};
  Score score;
  std::string error;
  require(daw::midiToScore(source, &score, &error), "create variable-meter score: " + error);
  daw::MusicXmlExportReport report;
  const auto restored = roundTrip(score, path, &report);
  require(signature(restored.time_signature, 4, 4) && restored.meter_changes.size() == 2 &&
              restored.meter_changes[0].tick == 3840 && signature(restored.meter_changes[0].signature, 3, 4) &&
              restored.meter_changes[1].tick == 6720 && signature(restored.meter_changes[1].signature, 6, 8),
          "MusicXML did not preserve canonical n/d changes at their absolute ticks");
  require(report.omitted_meter_playback_metadata == 4,
          "initial click, click-only, redundant, and changed-meter click omissions must each count once");
  require(report.omitted_tempo_changes == 0 && restored.tempo_changes.size() == 1 &&
              restored.tempo_changes[0].tick == 960 && restored.tempo_changes[0].bpm == 123.45 &&
              std::abs(restored.bpm - 91.25) < 1.0e-12,
          "mixed meter and tempo interchange lost a speed change");
  daw::MidiFile result;
  require(daw::scoreToMidiFile(restored, &result, &error), "restore MIDI after variable-meter XML: " + error);
  require(performance(result) == performance(source),
          "MIDI -> Score -> XML -> Score -> MIDI changed notes across meter changes");
  require(score.meter_changes.size() == 4 && score.time_signature.clocks_per_click == 36,
          "XML canonicalization mutated caller-owned raw meter metadata");
}

void partialMeasuresAndTailSilence(const std::filesystem::path& path) {
  // Last serialized voice ends at 200, while another voice reaches 600.
  // The partial bar still ends at 1000. Padding must use the actual XML
  // cursor, so an incorrect 400-tick forward cannot shorten it to 600.
  auto score = scoreWithMeasures({{1, 0, {note(0, 600), note(0, 200, 'E', 2)}},
                                  {2, 1000, {note(1000, 400, 'D')}},
                                  {3, 2440, {note(2440, 200, 'G')}}});
  score.meter_changes = {{1000, {3, 4, 24, 8}}, {2440, {6, 8, 24, 8}}};
  auto restored = roundTrip(score, path);
  require(restored.parts[0].measures.size() == 3 && restored.parts[0].measures[1].start == 1000 &&
              restored.parts[0].measures[2].start == 2440 && restored.meter_changes.size() == 2 &&
              restored.meter_changes[0].tick == 1000 && restored.meter_changes[1].tick == 2440,
          "partial measures lost tail silence or shifted meter changes");
  const auto xml = readText(path);
  const auto first_measure = xml.find("<measure");
  const auto first_end = xml.find('>', first_measure);
  const auto opening = xml.substr(first_measure, first_end - first_measure + 1);
  require(opening.find("implicit=\"yes\"") != std::string::npos || opening.find("implicit='yes'") != std::string::npos,
          "a short nonfinal measure must explicitly use implicit=yes");
  require(xml.find("<forward>") != std::string::npos, "partial trailing silence lacks explicit timing");

  score.parts[0].measures[0].notes.clear();
  restored = roundTrip(score, path);
  require(restored.parts[0].measures[0].notes.empty() && restored.parts[0].measures[1].start == 1000,
          "an empty partial measure must retain its duration through a forward element");

  // Oversized nonfinal spans can include silence as well as notes. Both
  // full/overfull and partial spans must preserve the supplied next start.
  auto overfull_silence = scoreWithMeasures({{1, 0, {note(0, 480)}}, {2, 5000, {note(5000, 480)}}});
  restored = roundTrip(overfull_silence, path);
  require(restored.parts[0].measures[1].start == 5000,
          "an overfull span containing trailing silence was shortened to a nominal bar");

  // XML is also a notation codec: isolated tie flags need not first pass
  // the stricter playable-tie bridge used by scoreToMidiFile.
  auto isolated_tie = scoreWithMeasures({{1, 0, {note(0, 480)}}, {2, 1000, {note(1000, 480)}}});
  isolated_tie.parts[0].measures[0].notes[0].tie_start = true;
  restored = roundTrip(isolated_tie, path);
  require(restored.parts[0].measures[0].notes[0].tie_start,
          "meter-aware XML writer rejected or erased an isolated notation tie flag");
}

void readerPartialAndCanonicalRules(const std::filesystem::path& path) {
  std::string error;
  Score score;
  const auto pickup = measure("1", time(4, 4) + xmlNote(600) + backup(600) + xmlNote(200, 2), " implicit='yes'") +
                      measure("2", time(3, 4) + xmlNote(960));
  writeText(path, document({pickup}));
  require(daw::readMusicXmlFile(path.string(), &score, &error), "read polyphonic partial measure: " + error);
  require(score.parts[0].measures[1].start == 600 && score.meter_changes.size() == 1 &&
              score.meter_changes[0].tick == 600 && signature(score.meter_changes[0].signature, 3, 4),
          "implicit measure did not advance by its furthest sounding endpoint");

  const auto empty_partial = measure("1", time(4, 4) + forward(1000), " implicit='yes'") +
                             measure("2", time(3, 4) + xmlNote(960));
  writeText(path, document({empty_partial}));
  require(daw::readMusicXmlFile(path.string(), &score, &error) && score.parts[0].measures[1].start == 1000,
          "forward-only partial measure did not determine its duration");
  const auto empty_regular = measure("1", time(4, 4), " implicit='no'") + measure("2", xmlNote(960));
  writeText(path, document({empty_regular}));
  require(daw::readMusicXmlFile(path.string(), &score, &error) && score.parts[0].measures[1].start == 3840,
          "implicit=no empty measure must preserve nominal-length legacy behavior");

  const auto duplicates = measure("1", time(4, 4) + time(4, 4) + xmlNote(960)) +
                          measure("2", time(4, 4) + xmlNote(960)) +
                          measure("3", time(3, 4) + time(3, 4) + xmlNote(960));
  writeText(path, document({duplicates}));
  require(daw::readMusicXmlFile(path.string(), &score, &error) && score.meter_changes.size() == 1 &&
              score.meter_changes[0].tick == 7680 && signature(score.meter_changes[0].signature, 3, 4),
          "identical leading time declarations must canonicalize to actual n/d changes only");
}

void terminalPartialExtent(const std::filesystem::path& path) {
  std::string error;
  Score score;
  writeText(path, document({measure("1", time(4, 4) + xmlNote(480) + forward(480), " implicit='yes'")}));
  require(daw::readMusicXmlFile(path.string(), &score, &error), "read terminal partial with trailing silence: " + error);
  require(score.parts[0].measures.size() == 1 && score.parts[0].measures[0].start == 0 &&
              score.parts[0].measures[0].duration == 960 && score.parts[0].measures[0].notes.size() == 1 &&
              score.parts[0].measures[0].notes[0].duration == 480,
          "terminal partial must store its entire 960-tick span separately from its 480-tick note");
  const auto restored = roundTrip(score, path);
  require(restored.parts[0].measures.size() == 1 && restored.parts[0].measures[0].duration == 960 &&
              restored.parts[0].measures[0].notes.size() == 1 &&
              restored.parts[0].measures[0].notes[0].start == 0 &&
              restored.parts[0].measures[0].notes[0].duration == 480,
          "terminal partial trailing silence was lost on XML -> Score -> XML -> Score");

  writeText(path, document({measure("1", time(4, 4) + forward(960), " implicit='yes'")}));
  require(daw::readMusicXmlFile(path.string(), &score, &error) && score.parts[0].measures.size() == 1 &&
              score.parts[0].measures[0].duration == 960 && score.parts[0].measures[0].notes.empty(),
          "an empty terminal partial must store the forward-only duration");
  const auto empty_restored = roundTrip(score, path);
  require(empty_restored.parts[0].measures.size() == 1 && empty_restored.parts[0].measures[0].start == 0 &&
              empty_restored.parts[0].measures[0].duration == 960 && empty_restored.parts[0].measures[0].notes.empty(),
          "empty terminal partial duration was replaced by a nominal full measure on re-export");
}

void independentParts(const std::filesystem::path& path) {
  const auto full = measure("1", time(4, 4) + xmlNote(960)) +
                    measure("2", time(3, 4) + xmlNote(960)) +
                    measure("3", time(6, 8) + xmlNote(960));
  std::string error;
  Score score;
  writeText(path, document({full, full}));
  require(daw::readMusicXmlFile(path.string(), &score, &error), "read aligned two-part meter maps: " + error);
  require(score.parts.size() == 2 && score.meter_changes.size() == 2 &&
              score.parts[1].measures[1].start == 3840 && score.parts[1].measures[2].start == 6720,
          "two aligned parts did not merge into one global meter map");
  (void)roundTrip(score, path);

  // A shorter part is allowed to be a prefix. Its nominal last measure
  // ends at 3840, so changes beginning there belong only to the long part.
  writeText(path, document({full, measure("1", time(4, 4) + xmlNote(960))}));
  require(daw::readMusicXmlFile(path.string(), &score, &error) && score.parts[1].measures.size() == 1 &&
              score.meter_changes.size() == 2,
          "shorter prefix part was rejected or erased later global meter changes");
  const auto restored = roundTrip(score, path);
  require(restored.parts[1].measures.size() == 1 && restored.meter_changes.size() == 2,
          "prefix-part XML export failed to retain the longer part's meter map");
}

void shorterPartBorrowsPartialBoundary(const std::filesystem::path& path) {
  auto prefix_note = note(0, 960, 'E', 2);
  prefix_note.velocity = 73;
  prefix_note.staff = 2;
  prefix_note.lyric = "prefix";
  auto score = scoreWithMeasures({{1, 0, {prefix_note}}});
  score.meter_changes = {{1920, {3, 4, 24, 8}}};
  score.parts.push_back({"P2", "Longer part", {{1, 0, {note(0, 480)}},
                                                {2, 1920, {note(1920, 480, 'G')}}}});
  auto restored = roundTrip(score, path);
  require(restored.parts.size() == 2 && restored.parts[0].measures.size() == 1 &&
              restored.parts[0].measures[0].notes.size() == 1 && restored.parts[1].measures[1].start == 1920 &&
              restored.meter_changes.size() == 1 && restored.meter_changes[0].tick == 1920 &&
              signature(restored.meter_changes[0].signature, 3, 4),
          "shorter part did not borrow the existing partial-bar boundary from the longer part");
  const auto& restored_note = restored.parts[0].measures[0].notes[0];
  require(restored_note.start == 0 && restored_note.duration == 960 && restored_note.pitch.step == 'E' &&
              restored_note.pitch.octave == 4 && restored_note.velocity == 73 && restored_note.voice == 2 &&
              restored_note.staff == 2 && restored_note.lyric == "prefix",
          "borrowing a shorter final boundary changed the prefix note or its notation metadata");

  score.parts[0].measures[0].notes.clear();
  score.parts[0].midi_events = {{1000, daw::MidiChannelEventType::ControlChange, 0, 7, 100, 0}};
  daw::MusicXmlExportReport report;
  restored = roundTrip(score, path, &report);
  require(restored.parts[0].measures.size() == 1 && restored.parts[0].measures[0].notes.empty() &&
              restored.parts[1].measures[1].start == 1920 && restored.meter_changes.size() == 1 &&
              restored.meter_changes[0].tick == 1920 && report.omitted_midi_events == 1,
          "event-only prefix part lacked a timed empty partial measure or damaged the global meter map");
}

void rejectedReaderFixtures(const std::filesystem::path& path) {
  auto reject = [&](const std::string& xml, const std::string& reason) {
    writeText(path, xml);
    auto output = scoreWithMeasures({{1, 0, {note(17, 43, 'G')}}});
    output.parts[0].name = "untouched";
    output.time_signature = {5, 8, 12, 8};
    output.meter_changes = {{111, {7, 8, 42, 8}}};
    std::string error;
    require(!daw::readMusicXmlFile(path.string(), &output, &error) && !error.empty(), "reader accepted " + reason);
    require(output.parts.size() == 1 && output.parts[0].name == "untouched" &&
                output.parts[0].measures[0].notes[0].start == 17 &&
                output.time_signature.numerator == 5 && output.time_signature.clocks_per_click == 12 &&
                output.meter_changes.size() == 1 && output.meter_changes[0].tick == 111,
            "failed XML read changed the destination: " + reason);
  };
  for (const auto& prefix : {xmlNote(960), forward(960), xmlNote(960) + backup(960)}) {
    reject(document({measure("1", time(4, 4) + prefix + time(3, 4))}),
           "a time declaration after note/forward/backup events");
  }
  reject(document({measure("1", time(4, 4) + time(3, 4) + xmlNote(960))}),
         "conflicting leading time declarations at one tick");
  reject(document({measure("1", "<attributes><time number='1'><beats>4</beats><beat-type>4</beat-type>"
                                "</time></attributes>" + xmlNote(960))}), "staff-specific time@number");
  reject(document({measure("1", "<attributes><time><beats>3+2</beats><beat-type>8</beat-type>"
                                "</time></attributes>" + xmlNote(960))}), "additive beats");
  reject(document({measure("1", "<attributes><time><beats>3</beats><beat-type>4</beat-type>"
                                "<beats>2</beats><beat-type>4</beat-type></time></attributes>" + xmlNote(960))}),
         "composite time components");
  reject(document({measure("1", "<attributes><time><senza-misura/></time></attributes>" + xmlNote(960))}),
         "senza-misura");
  reject(document({measure("1", time(4, 4) + xmlNote(960), " non-controlling='yes'")}),
         "non-controlling measure");
  reject(document({measure("1", time(4, 4), " implicit='yes'")}), "an empty implicit measure with no duration");
  // Non-numeric/zero labels are covered by musicxml_entry_tests.


  const auto changes = measure("1", time(4, 4) + xmlNote(960)) + measure("2", time(3, 4) + xmlNote(960));
  const auto stays = measure("1", time(4, 4) + xmlNote(960)) + measure("2", xmlNote(960));
  reject(document({changes, stays}), "one part changing meter while another sustains the previous meter");
  reject(document({measure("1", time(3, 4) + xmlNote(960)), measure("1", xmlNote(960))}),
         "a part without time incorrectly inheriting another part's non-default initial meter");
  const auto one_partial = measure("1", time(4, 4) + forward(1000), " implicit='yes'") +
                           measure("2", time(3, 4) + xmlNote(960));
  const auto two_partials = measure("1", time(4, 4) + forward(500), " implicit='yes'") +
                            measure("2", forward(500), " implicit='yes'") +
                            measure("3", time(3, 4) + xmlNote(960));
  reject(document({one_partial, two_partials}), "inconsistent barlines during otherwise equal shared meter states");
  reject(document({measure("1", forward(std::numeric_limits<Tick>::max()), " implicit='yes'") +
                   measure("2", xmlNote(1))}), "overflow after a maximal-duration measure");
}

void rejectedWriterStates(const std::filesystem::path& path) {
  auto reject = [&](const Score& invalid, const std::string& reason) {
    writeText(path, "protected-meter-xml");
    daw::MusicXmlExportReport report{11, 22, 33, 44};
    std::string error;
    require(!daw::writeMusicXmlFile(invalid, path.string(), &error, &report) && !error.empty(),
            "writer accepted " + reason);
    require(readText(path) == "protected-meter-xml" && report.omitted_midi_events == 11 &&
                report.omitted_note_midi_metadata == 22 && report.omitted_tempo_changes == 33 &&
                report.omitted_meter_playback_metadata == 44,
            "failed XML write changed its destination or report: " + reason);
  };
  reject(scoreWithMeasures({{1, 1000, {note(1000, 960)}}}), "a nonzero first measure start");
  reject(scoreWithMeasures({{1, 0, {note(0, 480)}}, {2, 0, {note(0, 480)}}}), "duplicate measure starts");
  reject(scoreWithMeasures({{1, 0, {note(0, 480)}}, {2, 3840, {note(3840, 480)}},
                            {3, 2880, {note(2880, 480)}}}), "descending measure starts");
  reject(scoreWithMeasures({{1, 0, {note(0, 4000)}}, {2, 3840, {note(3840, 480)}}}),
         "a note crossing the next supplied measure boundary");
  reject(scoreWithMeasures({{0, 0, {note(0, 480)}}}), "zero measure number");
  auto invalid = scoreWithMeasures({{1, 0, {note(0, 960)}}});
  invalid.meter_changes = {{1920, {3, 4, 24, 8}}};
  reject(invalid, "a structural meter change inside an unsplit measure");
  invalid.meter_changes = {{100000, {3, 4, 24, 8}}};
  reject(invalid, "a late structural change having no writable part boundary");
  invalid.meter_changes.clear();
  invalid.time_signature.notated_32nds_per_quarter = 4;
  reject(invalid, "an initial nonstandard bb ratio");
  invalid = scoreWithMeasures({{1, 0, {note(0, 960)}}, {2, 3840, {note(3840, 960)}}});
  invalid.meter_changes = {{3840, {3, 4, 24, 4}}};
  reject(invalid, "a later nonstandard bb ratio");

  auto mismatched_parts = scoreWithMeasures({{1, 0, {}}, {2, 1000, {note(1000, 480)}}});
  mismatched_parts.parts.push_back({"P2", "Other", {{1, 0, {}}, {2, 500, {}}, {3, 1000, {note(1000, 480)}}}});
  reject(mismatched_parts, "inconsistent shared part barlines");

  auto crossing_borrowed_boundary = scoreWithMeasures({{1, 0, {note(0, 2400)}}});
  crossing_borrowed_boundary.meter_changes = {{1920, {3, 4, 24, 8}}};
  crossing_borrowed_boundary.parts.push_back({"P2", "Longer part", {{1, 0, {note(0, 480)}},
                                                                  {2, 1920, {note(1920, 480)}}}});
  reject(crossing_borrowed_boundary, "an untied note crossing a borrowed final partial-bar boundary");

  auto bad_extent = scoreWithMeasures({{1, 0, {note(0, 480)}}});
  bad_extent.parts[0].measures[0].duration = -1;
  reject(bad_extent, "a negative explicit measure duration");
  bad_extent.parts[0].measures[0].duration = 240;
  reject(bad_extent, "a note ending beyond the explicit final measure duration");

  bad_extent = scoreWithMeasures({{1, 0, {note(0, 480)}}, {2, 1920, {note(1920, 480)}}});
  bad_extent.parts[0].measures[0].duration = 960;
  reject(bad_extent, "an explicit nonfinal duration leaving a gap before the next measure start");
  bad_extent.parts[0].measures[0].duration = 2880;
  reject(bad_extent, "an explicit nonfinal duration overlapping the next measure start");

  const Tick last_start = std::numeric_limits<Tick>::max() - 10;
  bad_extent = scoreWithMeasures({{1, 0, {}}, {2, last_start, {}}});
  bad_extent.parts[0].measures[1].duration = 20;
  reject(bad_extent, "an explicit final measure endpoint overflowing Tick");
}

}  // namespace

int main() {
  try {
    const TempDirectory directory;
    const auto path = directory.path / "meter.musicxml";
    canonicalMeterAndMidiPerformance(path);
    partialMeasuresAndTailSilence(path);
    readerPartialAndCanonicalRules(path);
    terminalPartialExtent(path);
    independentParts(path);
    shorterPartBorrowsPartialBoundary(path);
    rejectedReaderFixtures(path);
    rejectedWriterStates(path);
    std::cout << "classical-daw MusicXML meter tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
