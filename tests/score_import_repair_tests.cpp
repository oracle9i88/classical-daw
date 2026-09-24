// Import repairs: what the reader and the repair pass are allowed to change,
// and what they must still refuse. Strict callers pass no report and must see
// exactly the old behavior, because every other round-trip test relies on it.
#include "daw/score.hpp"
#include "daw/score_midi.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
namespace fs = std::filesystem;
void require(bool v, const std::string& why) { if (!v) throw std::runtime_error(why); }

int main() {
  const fs::path root = fs::temp_directory_path() /
      ("daw-repair-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    fs::create_directory(root);
    const std::string head = "<score-partwise><part-list><score-part id='P1'><part-name>Piano</part-name>"
                             "</score-part></part-list><part id='P1'>";
    const std::string tail = "</part></score-partwise>";
    auto note = [](const char* step, int octave, int duration) {
      return "<note><pitch><step>" + std::string(step) + "</step><octave>" + std::to_string(octave) +
             "</octave></pitch><duration>" + std::to_string(duration) + "</duration></note>";
    };
    auto grace = [](const char* step, int octave, const char* type) {
      return "<note><grace/><pitch><step>" + std::string(step) + "</step><octave>" +
             std::to_string(octave) + "</octave></pitch><type>" + type + "</type></note>";
    };
    auto read = [&](const std::string& body, daw::Score* score, std::string* error,
                    daw::MusicXmlImportReport* report) {
      std::ofstream out(root / "input.xml"); out << head << body << tail; out.close();
      return daw::readMusicXmlFile((root / "input.xml").string(), score, error, report);
    };

    // A grace note has no <duration>. Without a report the reader still refuses
    // the file rather than inventing a length for it.
    {
      const auto body = "<measure number='1'>" + grace("D", 4, "eighth") + note("C", 4, 960) + "</measure>";
      daw::Score untouched; untouched.bpm = 77; std::string error;
      require(!read(body, &untouched, &error, nullptr), "strict mode accepted a grace note");
      require(error.find("duration") != std::string::npos, "strict grace error lost its reason");
      require(untouched.bpm == 77 && untouched.parts.empty(), "failed strict read modified the caller");

      daw::Score score; daw::MusicXmlImportReport report;
      require(read(body, &score, &error, &report), error);
      require(report.grace_notes_timed == 1 && report.grace_notes_dropped == 0, "grace not timed once");
      const auto& notes = score.parts.at(0).measures.at(0).notes;
      require(notes.size() == 2, "grace and principal not both stored");
      // The grace borrows its written eighth, and the quarter keeps half.
      require(notes[0].pitch.step == 'D' && notes[0].start == 0 && notes[0].duration == 480, "grace placement");
      require(notes[1].pitch.step == 'C' && notes[1].start == 480 && notes[1].duration == 480, "principal shifted");
      // Borrowing must not move anything that follows the principal.
      require(notes[1].start + notes[1].duration == 960, "grace changed the written span");
    }

    // Graces are commonly written at the end of a bar, decorating the downbeat
    // of the next one, so the buffer has to outlive a measure.
    {
      daw::Score score; std::string error; daw::MusicXmlImportReport report;
      require(read("<measure number='1'>" + note("C", 4, 3840) + grace("B", 3, "eighth") +
                   "</measure><measure number='2'>" + note("C", 4, 960) + "</measure>",
                   &score, &error, &report), error);
      require(report.grace_notes_timed == 1, "cross-bar grace not timed");
      const auto& second = score.parts.at(0).measures.at(1).notes;
      require(second.size() == 2 && second[0].pitch.step == 'B', "grace not carried into the next bar");
      require(second[0].start == 3840 && second[1].start == 4320, "cross-bar grace placement");
    }

    // Nothing follows, so nothing can lend time. Dropping is counted, never silent.
    {
      daw::Score score; std::string error; daw::MusicXmlImportReport report;
      require(read("<measure number='1'>" + note("C", 4, 3840) + grace("B", 3, "eighth") + "</measure>",
                   &score, &error, &report), error);
      require(report.grace_notes_dropped == 1 && report.grace_notes_timed == 0, "orphan grace not dropped");
      require(score.parts.at(0).measures.at(0).notes.size() == 1, "orphan grace was stored anyway");
    }

    // Two marks at one tick: strict refuses, a repairing read takes the later.
    {
      const std::string body = "<measure number='1'><direction><direction-type/><sound tempo='90'/></direction>"
                               "<direction><direction-type/><sound tempo='144'/></direction>" +
                               note("C", 4, 960) + "</measure>";
      daw::Score score; std::string error;
      require(!read(body, &score, &error, nullptr), "strict mode accepted conflicting tempos");
      daw::MusicXmlImportReport report;
      require(read(body, &score, &error, &report), error);
      require(report.conflicting_tempos_resolved == 1 && score.bpm == 144, "later tempo did not win");
    }

    // Extra verses are dropped; the first one is still carried.
    {
      const std::string body = "<measure number='1'><note><pitch><step>C</step><octave>4</octave></pitch>"
                               "<duration>960</duration><lyric><text>one</text></lyric>"
                               "<lyric><text>two</text></lyric></note></measure>";
      daw::Score score; std::string error;
      require(!read(body, &score, &error, nullptr), "strict mode accepted two lyrics");
      daw::MusicXmlImportReport report;
      require(read(body, &score, &error, &report), error);
      require(report.extra_lyrics_dropped == 1, "extra verse not counted");
      require(score.parts.at(0).measures.at(0).notes.at(0).lyric == "one", "first verse lost");
    }

    // The repair pass. A null report leaves the score exactly as it was.
    auto build = [](std::initializer_list<daw::ScoreNote> notes) {
      daw::Score score; daw::ScorePart part; daw::ScoreMeasure measure;
      measure.number = 1; measure.start = 0; measure.duration = 3840;
      for (const auto& note : notes) measure.notes.push_back(note);
      part.measures.push_back(measure); score.parts.push_back(part); return score;
    };
    auto at = [](daw::Tick start, daw::Tick duration, char step, int octave) {
      daw::ScoreNote note; note.start = start; note.duration = duration;
      note.pitch.step = step; note.pitch.octave = octave; return note;
    };
    {
      daw::Score score = build({at(0, 960, 'C', 4), at(480, 960, 'C', 4)});
      const daw::Score before = score;
      daw::repairScoreForAudition(score, nullptr);
      require(score.parts[0].measures[0].notes[0].duration == before.parts[0].measures[0].notes[0].duration,
              "null report still changed the score");
    }
    // A release landing exactly on the next attack is rejected by the audition,
    // so one tick of silence is inserted and reported as a separation, not as
    // an audible shortening.
    {
      daw::Score score = build({at(0, 960, 'C', 4), at(960, 960, 'C', 4)});
      daw::ScoreRepairReport report;
      daw::repairScoreForAudition(score, &report);
      require(report.repeats_separated == 1 && report.overlaps_trimmed == 0, "repeat misclassified");
      require(score.parts[0].measures[0].notes[0].duration == 959, "repeat not separated by one tick");
    }
    // A genuine overlap is an audible change and is counted as one.
    {
      daw::Score score = build({at(0, 960, 'C', 4), at(480, 960, 'C', 4)});
      daw::ScoreRepairReport report;
      daw::repairScoreForAudition(score, &report);
      require(report.overlaps_trimmed == 1 && report.repeats_separated == 0, "overlap misclassified");
      require(score.parts[0].measures[0].notes[0].duration == 479, "overlap not trimmed");
    }
    // Two attacks at one instant cannot both be addressed on one channel.
    {
      daw::ScoreNote second = at(0, 960, 'C', 4); second.voice = 2;
      daw::Score score = build({at(0, 960, 'C', 4), second});
      daw::ScoreRepairReport report;
      daw::repairScoreForAudition(score, &report);
      require(report.overlaps_silenced == 1, "coincident unison not silenced");
      require(score.parts[0].measures[0].notes[1].rest, "silenced note still sounds");
    }
    // A tie whose segments do not join becomes two attacks, and the score that
    // comes out has to survive the bridge that refused the original.
    {
      daw::ScoreNote first = at(0, 480, 'C', 4); first.tie_start = true;
      daw::ScoreNote second = at(960, 960, 'C', 4); second.tie_stop = true;
      daw::Score score = build({first, second});
      std::string error;
      daw::MidiFile refused;
      require(!daw::scoreToMidiFile(score, &refused, &error), "bridge accepted a broken tie");
      daw::ScoreRepairReport report;
      daw::repairScoreForAudition(score, &report);
      require(report.broken_tie_chains_released == 1, "broken tie not released");
      require(!score.parts[0].measures[0].notes[0].tie_start &&
              !score.parts[0].measures[0].notes[1].tie_stop, "tie flags survived the release");
      daw::MidiFile accepted;
      require(daw::scoreToMidiFile(score, &accepted, &error), "repaired score still refused: " + error);
    }
    fs::remove_all(root);
    std::cout << "score import repair tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::error_code ignored; fs::remove_all(root, ignored);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
