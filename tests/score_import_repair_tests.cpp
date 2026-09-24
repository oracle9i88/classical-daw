// Import repairs: what the reader and the repair pass are allowed to change,
// and what they must still refuse. Strict callers pass no report and must see
// exactly the old behavior, because every other round-trip test relies on it.
#include "daw/score.hpp"
#include "daw/midi.hpp"
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
    // A silenced chain must not hide a longer one still sounding underneath it.
    // Following only the previous chain in start order left the collision
    // between the long chain and a later attack completely unexamined.
    {
      daw::ScoreNote shorter = at(0, 240, 'C', 4); shorter.voice = 2;
      daw::Score score = build({at(0, 960, 'C', 4), shorter, at(960, 480, 'C', 4)});
      daw::ScoreRepairReport report;
      daw::repairScoreForAudition(score, &report);
      require(report.overlaps_silenced == 1, "coincident attack not silenced");
      require(report.repeats_separated == 1, "long chain hidden behind the silenced one");
      std::string error; daw::MidiFile midi;
      require(daw::scoreToMidiFile(score, &midi, &error), error);
    }
    // Releasing a tie leaves its segments sounding separately. A collision pass
    // still holding the old grouping trims the wrong note and leaves the two
    // halves abutting, which is exactly what the audition refuses.
    {
      daw::ScoreNote head = at(0, 240, 'C', 4); head.tie_start = true;
      daw::ScoreNote body = at(240, 2880, 'C', 4); body.tie_stop = true; body.tie_start = true;
      daw::ScoreNote later = at(1920, 480, 'C', 4); later.voice = 2;
      daw::Score score = build({head, body, later});
      daw::ScoreRepairReport report;
      daw::repairScoreForAudition(score, &report);
      const auto& notes = score.parts[0].measures[0].notes;
      require(!notes[0].tie_start && !notes[1].tie_stop, "unterminated chain not released");
      // The released head must not still be abutting its old continuation.
      require(notes[0].start + notes[0].duration < notes[1].start ||
              notes[1].rest || notes[0].rest, "released segments left abutting");
      std::string error; daw::MidiFile midi;
      require(daw::scoreToMidiFile(score, &midi, &error), error);
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
    // The MIDI entry refuses an overlap during conversion, before the repair
    // pass could ever see the score. Asking for repairs there shortens the
    // sounding note instead; a second attack at the same instant is dropped.
    {
      daw::MidiFile midi;
      midi.format = 0;
      midi.ticks_per_quarter = daw::kTicksPerQuarter;
      daw::MidiTrack track;
      track.notes.push_back({0, 960, 60, 90, 0, 0, 0, 0, 0, {}});
      track.notes.push_back({480, 960, 60, 90, 0, 0, 0, 0, 0, {}});
      midi.tracks.push_back(track);

      daw::Score refused; std::string error;
      require(!daw::midiToScore(midi, &refused, &error, nullptr), "strict MIDI accepted an overlap");
      require(error.find("overlap") != std::string::npos, "strict MIDI error lost its reason");

      daw::Score score; daw::ScoreRepairReport report;
      require(daw::midiToScore(midi, &score, &error, &report), error);
      require(report.overlaps_trimmed == 1 && report.overlaps_silenced == 0, "MIDI overlap not trimmed once");
      daw::MidiFile round_trip;
      require(daw::scoreToMidiFile(score, &round_trip, &error), "repaired MIDI score still refused: " + error);
    }
    {
      daw::MidiFile midi;
      midi.format = 0;
      midi.ticks_per_quarter = daw::kTicksPerQuarter;
      daw::MidiTrack track;
      track.notes.push_back({0, 960, 60, 90, 0, 0, 0, 0, 0, {}});
      track.notes.push_back({0, 480, 60, 70, 0, 0, 0, 0, 0, {}});
      midi.tracks.push_back(track);
      daw::Score score; std::string error; daw::ScoreRepairReport report;
      require(daw::midiToScore(midi, &score, &error, &report), error);
      require(report.overlaps_silenced == 1, "coincident MIDI attack not dropped");
    }

    // A note released at or before its own attack, and a release with nothing
    // to close. Exporters emit both; neither can sound, so neither is music
    // that dropping could lose. Strict reads still refuse the file.
    {
      const unsigned char bytes[] = {
        'M','T','h','d',0,0,0,6,0,0,0,1,0x03,0xC0,
        'M','T','r','k',0,0,0,16,
        0x00,0x90,60,90,   0x00,0x80,60,0,      // zero-length note
        0x00,0x80,62,0,                         // orphan release
        0x00,0xFF,0x2F,0x00
      };
      const auto path = (root / "degenerate.mid").string();
      { std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes)); }
      daw::MidiFile refused; std::string error;
      require(!daw::readMidiFile(path, &refused, &error), "strict read accepted a silent note");
      daw::MidiFile file; daw::ScoreRepairReport repairs;
      require(daw::readMidiFile(path, &file, &error, nullptr, &repairs), error);
      require(repairs.dropped_silent_notes == 1, "silent note not dropped once");
      require(repairs.dropped_orphan_releases == 1, "orphan release not dropped once");
      require(file.tracks.at(0).notes.empty(), "a dropped note was stored anyway");
    }

    // An exporter writing one instrument as a track per staff. The tracks
    // share a channel, which is what makes them one instrument, so the score
    // keeps one part. Two tracks on separate channels stay separate.
    {
      auto twoTrack = [](std::uint8_t first, std::uint8_t second) {
        daw::MidiFile midi;
        midi.format = 1;
        midi.ticks_per_quarter = daw::kTicksPerQuarter;
        daw::MidiTrack upper, lower;
        upper.notes.push_back({0, 480, 72, 90, first, 0, 0, 0, 0, {}});
        lower.notes.push_back({0, 480, 48, 90, second, 0, 0, 0, 0, {}});
        midi.tracks.push_back(upper);
        midi.tracks.push_back(lower);
        return midi;
      };
      daw::Score strict; std::string error;
      require(daw::midiToScore(twoTrack(0, 0), &strict, &error, nullptr), error);
      require(strict.parts.size() == 2, "strict conversion stopped making one part per track");

      daw::Score merged; daw::ScoreRepairReport report;
      require(daw::midiToScore(twoTrack(0, 0), &merged, &error, &report), error);
      require(report.merged_instrument_tracks == 1, "shared-channel tracks not merged");
      require(merged.parts.size() == 1, "one instrument did not become one part");

      daw::Score separate; daw::ScoreRepairReport untouched;
      require(daw::midiToScore(twoTrack(0, 1), &separate, &error, &untouched), error);
      require(untouched.merged_instrument_tracks == 0 && separate.parts.size() == 2,
              "tracks on different channels were merged anyway");
    }

    // Pedal marks are written playback data, not an interpretation, so they
    // are read whether or not repairs were asked for. A retake releases and
    // presses again at one tick; sostenuto and una corda are not read.
    {
      auto pedal = [](const char* type) {
        return "<direction><direction-type><pedal type='" + std::string(type) +
               "'/></direction-type></direction>";
      };
      daw::Score score; std::string error;
      require(read("<measure number='1'>" + pedal("start") + note("C", 4, 960) +
                   pedal("change") + note("D", 4, 960) + pedal("stop") +
                   "<direction><direction-type><pedal type='sostenuto'/></direction-type></direction>" +
                   note("E", 4, 960) + "</measure>", &score, &error, nullptr), error);
      const auto& events = score.parts.at(0).midi_events;
      require(events.size() == 4, "pedal marks did not become four messages");
      for (const auto& event : events) {
        require(event.data1 == 64, "a pedal mark used the wrong controller");
        require(event.type == daw::MidiChannelEventType::ControlChange, "pedal is not a control change");
      }
      require(events[0].tick == 0 && events[0].data2 == 127, "pedal press");
      require(events[1].tick == 960 && events[1].data2 == 0, "retake did not release first");
      require(events[2].tick == 960 && events[2].data2 == 127, "retake did not press again");
      require(events[3].tick == 1920 && events[3].data2 == 0, "pedal release");
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
