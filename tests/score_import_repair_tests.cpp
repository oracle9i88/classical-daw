// Import repairs: what the reader and the repair pass are allowed to change,
// and what they must still refuse. Strict callers pass no report and must see
// exactly the old behavior, because every other round-trip test relies on it.
#include "daw/score.hpp"
#include "daw/midi.hpp"
#include "daw/performance.hpp"
#include "daw/score_midi.hpp"
#include <chrono>
#include <cmath>
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

    // A grace chord: its tones sound together, and the second tone is not the
    // voice's first note just because the first one is still buffered.
    {
      daw::Score score; std::string error; daw::MusicXmlImportReport report;
      const auto tone = "<note><grace/><chord/><pitch><step>B</step><octave>3</octave></pitch>"
                        "<type>eighth</type></note>";
      require(read("<measure number='1'>" + grace("D", 4, "eighth") + tone + note("C", 4, 960) +
                   "</measure>", &score, &error, &report), error);
      require(report.grace_notes_timed == 2 && report.grace_notes_dropped == 0, "grace chord not timed");
      const auto& notes = score.parts.at(0).measures.at(0).notes;
      require(notes.size() == 3, "grace chord did not keep both tones");
      require(notes[0].start == notes[1].start && notes[0].duration == notes[1].duration,
              "grace chord tones did not sound together");
      // The chord tone borrows no time of its own, so the principal keeps half.
      require(notes[2].start == 480 && notes[2].duration == 480, "grace chord stole extra time");
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

    // The attack total and the per-block event density are two different
    // limits that were once one number. A movement of five thousand notes must
    // compile, and a burst that would overrun one realtime block must not.
    {
      static_assert(daw::kMaxAuditionAttacks > daw::kMaxEventsPerRealtimeSlice,
                    "the attack total collapsed back into the block limit");
      daw::Score score; daw::ScorePart part;
      const std::size_t count = 5000;
      for (std::size_t index = 0; index < count; ++index) {
        daw::ScoreMeasure measure;
        measure.number = static_cast<int>(index) + 1;
        measure.start = static_cast<daw::Tick>(index) * 240;
        measure.duration = 240;
        daw::ScoreNote note = at(measure.start, 240, "CDEFGAB"[index % 7], 4);
        measure.notes.push_back(note);
        part.measures.push_back(measure);
      }
      score.parts.push_back(part);
      daw::ScoreRepairReport report;
      daw::repairScoreForAudition(score, &report);
      daw::assignNoteIds(score);
      daw::PerformanceDocument document;
      document.score = score;
      document.piano_state.assign(64, 0);
      document.performances.push_back(daw::makePerformance(document.score, "long"));
      const auto sequence = daw::compilePerformance(document.score, document.performances.front());
      require(sequence.events.size() > count, "a five thousand note movement did not compile");

      // Now pack more than one block's worth of attacks into one instant.
      daw::Score dense; daw::ScorePart crowd; daw::ScoreMeasure bar;
      bar.number = 1; bar.start = 0; bar.duration = 3840;
      for (std::size_t index = 0; index <= daw::kMaxEventsPerRealtimeSlice; ++index) {
        daw::ScoreNote note = at(0, 960, 'C', 4);
        note.voice = static_cast<std::uint16_t>(index % 64U + 1U);
        note.staff = static_cast<std::uint16_t>(index / 64U + 1U);
        note.midi_channel = static_cast<std::int16_t>(index % 16U);
        bar.notes.push_back(note);
      }
      crowd.measures.push_back(bar);
      dense.parts.push_back(crowd);
      daw::assignNoteIds(dense);
      daw::Performance packed;
      bool refused = false;
      try {
        packed = daw::makePerformance(dense, "dense");
        (void)daw::compilePerformance(dense, packed);
      } catch (const std::exception&) { refused = true; }
      require(refused, "a burst larger than one realtime block was accepted");
    }

    // 960 has no factor of seven, so a septuplet has no exact tick. Rounding
    // POSITIONS rather than lengths is what keeps the group summing to the
    // quarter it occupies; rounding each length would lose a tick per note.
    {
      std::string body = "<measure number='1'><attributes><divisions>7</divisions></attributes>";
      for (int index = 0; index < 7; ++index) {
        body += "<note><pitch><step>" + std::string(1, "CDEFGAB"[index]) +
                "</step><octave>4</octave></pitch><duration>1</duration></note>";
      }
      body += "</measure>";
      daw::Score refused; std::string error;
      require(!read(body, &refused, &error, nullptr), "strict read accepted a septuplet");
      require(error.find("exactly") != std::string::npos, "strict septuplet error lost its reason");

      daw::Score score; daw::MusicXmlImportReport report;
      require(read(body, &score, &error, &report), error);
      require(report.rounded_positions > 0, "septuplet rounding was not reported");
      const auto& notes = score.parts.at(0).measures.at(0).notes;
      require(notes.size() == 7, "septuplet lost a note");
      require(notes.front().start == 0, "septuplet did not start on the beat");
      daw::Tick total = 0;
      for (const auto& note : notes) {
        require(note.duration > 0, "a septuplet note collapsed");
        require(note.start == total, "septuplet positions drifted apart");
        total += note.duration;
      }
      require(total == daw::kTicksPerQuarter, "septuplet did not sum to its quarter note");
    }

    // At high source resolutions an engraver can write a note shorter than a
    // tick. It was written to sound, so it is widened rather than dropped, and
    // the notes around it keep their own absolute positions.
    {
      const std::string body =
          "<measure number='1'><attributes><divisions>3840</divisions></attributes>"
          "<note><pitch><step>C</step><octave>4</octave></pitch><duration>1</duration></note>"
          "<note><pitch><step>D</step><octave>4</octave></pitch><duration>15359</duration></note>"
          "</measure>";
      daw::Score refused; std::string error;
      require(!read(body, &refused, &error, nullptr), "strict read accepted a sub-tick note");
      daw::Score score; daw::MusicXmlImportReport report;
      require(read(body, &score, &error, &report), error);
      require(report.notes_widened_to_one_tick == 1, "sub-tick note was not widened once");
      const auto& notes = score.parts.at(0).measures.at(0).notes;
      require(notes.size() == 2, "sub-tick note was dropped");
      require(notes[0].duration == 1, "sub-tick note did not get one tick");
      // A note that had no room to exist has to take room from somewhere, so
      // the bar ends one tick late per widened note. 15360 source units at
      // 3840 per quarter is four quarters; one widening makes it four plus a
      // tick. Half a millisecond is the honest price of not dropping a note.
      require(notes[1].start == 1, "the note after a widened one did not move");
      require(notes[1].start + notes[1].duration == 4 * daw::kTicksPerQuarter + 1,
              "widening shifted the bar by something other than one tick");
    }

    // Adopting a lane makes it editable, and a curve owns its lane, so what
    // the file specified has to survive the move. The lane is read every ten
    // milliseconds, which is the whole cost: every message must still be there
    // with its own value, within one read of where it was, and the reader must
    // never sample a value partway through a change that the file never had.
    {
      daw::Score score; daw::ScorePart part; daw::ScoreMeasure measure;
      measure.number = 1; measure.start = 0; measure.duration = 3840;
      measure.notes.push_back(at(0, 3840, 'C', 4));
      part.measures.push_back(measure);
      const daw::Tick marks[] = {120, 500, 960, 1437, 1920, 2400, 2880, 3600};
      bool down = true;
      for (daw::Tick tick : marks) {
        daw::MidiChannelEvent pedal;
        pedal.tick = tick; pedal.type = daw::MidiChannelEventType::ControlChange;
        pedal.channel = 0; pedal.data1 = 64; pedal.data2 = down ? 127 : 0;
        part.midi_events.push_back(pedal);
        down = !down;
      }
      score.parts.push_back(part);
      daw::assignNoteIds(score);

      daw::CurveAdoptionReport adopted;
      const auto curve = daw::curveFromScoreMessages(score, 0, 64, 1, &adopted);
      require(adopted.source_messages == 8, "not every message was seen");
      require(adopted.worst_shift_seconds <= 0.01, "a message moved more than one read");
      require(curve.points.front().seconds == 0, "a curve must start at zero");

      auto pedalEvents = [&](const daw::Performance& take) {
        std::vector<std::pair<std::size_t, int>> found;
        for (const auto& event : daw::compilePerformance(score, take).events) {
          if ((event.status & 0xf0) == 0xb0 && event.data1 == 64) found.push_back({event.frame, event.data2});
        }
        return found;
      };
      auto plain = daw::makePerformance(score, "plain");
      auto editable = plain;
      editable.curves.push_back(curve);
      const auto before = pedalEvents(plain);
      const auto after = pedalEvents(editable);
      // An adopted lane states its own starting value at zero, which the file
      // left implicit, so it may send one message more. It may not send fewer.
      require(after.size() >= before.size() && after.size() <= before.size() + 1,
              "adopting a lane changed how many messages it sends");
      for (const auto& event : after) {
        require(event.second == 0 || event.second == 127, "the lane was sampled partway through a change");
      }
      for (const auto& original : before) {
        double nearest = 1e9;
        for (const auto& moved : after) {
          if (moved.second != original.second) continue;
          nearest = std::min(nearest, std::abs(static_cast<double>(original.first) -
                                               static_cast<double>(moved.first)) / 48000);
        }
        require(nearest <= 0.011, "a message is missing or moved further than the read grid explains");
      }
      // A lane with nothing in it is refused rather than adopted as silence.
      auto refuses = [](auto call) {
        bool threw = false;
        try { call(); } catch (const std::exception&) { threw = true; }
        require(threw, "an impossible adoption was accepted");
      };
      refuses([&] { (void)daw::curveFromScoreMessages(score, 0, 11, 2, nullptr); });
      refuses([&] { (void)daw::curveFromScoreMessages(score, 0, 64, 0, nullptr); });
    }

    // Shaping a passage is one musical act. It has to be one command: the way
    // to know it is one is that a single undo puts the whole passage back.
    {
      daw::Score score; daw::ScorePart part;
      for (int bar = 0; bar < 4; ++bar) {
        daw::ScoreMeasure measure;
        measure.number = bar + 1; measure.start = bar * 3840; measure.duration = 3840;
        for (int beat = 0; beat < 4; ++beat) {
          measure.notes.push_back(at(measure.start + beat * 960, 720, "CDEG"[beat], 4));
        }
        part.measures.push_back(measure);
      }
      score.parts.push_back(part);
      daw::assignNoteIds(score);
      daw::PerformanceDocument document;
      document.score = score;
      document.piano_state.assign(64, 0);
      document.performances.push_back(daw::makePerformance(document.score, "take"));
      daw::WorkEditor editor(document);

      // Timing given to one note must survive a crescendo over it.
      require(editor.set({3, -0.025, 1.2, -1}), "single edit refused");
      const auto touched = editor.shapeRange(0, 4, daw::WorkEditor::Shape::Velocity, 60, 110);
      require(touched >= 8, "the crescendo reached too few notes");
      const auto& shaped = editor.document().performances.at(0).notes;
      const auto kept = std::find_if(shaped.begin(), shaped.end(),
                                     [](const auto& n) { return n.note_id == 3; });
      require(kept != shaped.end(), "the shaped passage lost a note that already had timing");
      require(kept->onset_seconds == -0.025 && kept->duration_scale == 1.2,
              "a crescendo discarded timing it was not asked to touch");
      require(kept->velocity > 60 && kept->velocity < 110, "the ramp did not reach this note");

      // One undo, not one per note.
      const auto revision = editor.revision();
      require(editor.undo(), "the passage could not be undone");
      require(editor.revision() == revision + 1, "undo did not advance one revision");
      const auto& restored = editor.document().performances.at(0).notes;
      require(restored.size() == 1 && restored.front().note_id == 3 &&
              restored.front().velocity == -1,
              "one undo did not put the whole passage back");
      require(editor.redo(), "the passage could not be redone");
      require(editor.document().performances.at(0).notes.size() == shaped.size(),
              "redo did not restore the whole passage");

      // A ramp landing on the written value leaves nothing behind.
      require(editor.shapeRange(100, 200, daw::WorkEditor::Shape::Velocity, 60, 110) == 0,
              "a range with no notes in it still wrote something");
      bool threw = false;
      try { (void)editor.shapeRange(4, 0, daw::WorkEditor::Shape::Velocity, 60, 110); }
      catch (const std::exception&) { threw = true; }
      require(threw, "a backwards range was accepted");
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
