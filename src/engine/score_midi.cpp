#include "daw/score_midi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace daw {
namespace {

int pitchClass(char step) {
  switch (step) {
    case 'C': return 0;
    case 'D': return 2;
    case 'E': return 4;
    case 'F': return 5;
    case 'G': return 7;
    case 'A': return 9;
    case 'B': return 11;
    default: throw std::invalid_argument("score pitch step is out of range");
  }
}

bool validTimeSignature(const TimeSignature& signature) {
  if (signature.numerator == 0 || signature.denominator == 0) return false;
  return (signature.denominator & static_cast<std::uint8_t>(signature.denominator - 1U)) == 0U;
}

std::uint8_t toMidiPitch(const ScorePitch& pitch) {
  // Use a wide intermediate so even a hostile int octave/alter value cannot
  // wrap before the range check.
  const std::int64_t value = (static_cast<std::int64_t>(pitch.octave) + 1) * 12 +
                             static_cast<std::int64_t>(pitchClass(pitch.step)) +
                             static_cast<std::int64_t>(pitch.alter);
  if (value < 0 || value > 127) throw std::invalid_argument("score pitch is outside MIDI 0..127");
  return static_cast<std::uint8_t>(value);
}

void validateNoteTiming(const ScoreNote& note) {
  if (note.start < 0) throw std::invalid_argument("score note start cannot be negative");
  if (note.duration <= 0) throw std::invalid_argument("score note duration must be positive");
  if (note.start > std::numeric_limits<Tick>::max() - note.duration) {
    throw std::invalid_argument("score note timing overflows tick range");
  }
  if (note.velocity > 127) throw std::invalid_argument("score note velocity is outside MIDI 0..127");
  if (!note.rest && note.velocity == 0) {
    throw std::invalid_argument("score sounding note attack velocity must be in MIDI 1..127");
  }
  if (note.midi_channel < -1 || note.midi_channel > 15) {
    throw std::invalid_argument("score note MIDI channel must be -1 or in 0..15");
  }
  if (note.midi_release_velocity > 127) {
    throw std::invalid_argument("score note release velocity is outside MIDI 0..127");
  }
  if (note.voice == 0 || note.staff == 0) throw std::invalid_argument("score voice/staff numbers must be positive");
}

ScorePitch fromMidiPitch(std::uint8_t midi_pitch) {
  // Prefer a deterministic sharp spelling.  Enharmonic spelling can be
  // restored by a later notation/key-signature layer without changing the
  // MIDI-to-score timing contract.
  static constexpr std::array<char, 12> kSteps = {
      'C', 'C', 'D', 'D', 'E', 'F', 'F', 'G', 'G', 'A', 'A', 'B'};
  static constexpr std::array<int, 12> kAlter = {
      0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
  const int pitch = static_cast<int>(midi_pitch);
  const int pitch_class = pitch % 12;
  return ScorePitch{kSteps[static_cast<std::size_t>(pitch_class)],
                    kAlter[static_cast<std::size_t>(pitch_class)], pitch / 12 - 1};
}

constexpr std::size_t kMaximumImportedMeasures = 1000000;
constexpr std::size_t kMaximumImportedNoteSegments = 1000000;

std::filesystem::path temporaryPath(const std::string& destination) {
  // A sibling path keeps the final rename on one filesystem. This is not used
  // until conversion has succeeded; an existing destination is therefore
  // preserved if the input score is invalid.
  return std::filesystem::path(destination).concat(".tmp");
}

}  // namespace

bool scoreToMidiFile(const Score& score, MidiFile* midi, std::string* error) {
  if (midi == nullptr) {
    if (error) *error = "MIDI output pointer is null";
    return false;
  }
  try {
    if (score.parts.empty()) throw std::invalid_argument("MIDI export requires at least one score part");
    if (score.divisions != kTicksPerQuarter) {
      throw std::invalid_argument("score divisions must be 960 ticks per quarter");
    }
    if (!std::isfinite(score.bpm) || score.bpm <= 0.0) {
      throw std::invalid_argument("score tempo must be finite and positive");
    }
    if (!validTimeSignature(score.time_signature)) {
      throw std::invalid_argument("score time signature must have a positive numerator and power-of-two denominator");
    }

    MidiFile converted;
    converted.format = 1;
    converted.ticks_per_quarter = kTicksPerQuarter;
    converted.time_signature = score.time_signature;
    converted.tempo = scoreTempoMap(score);
    converted.tracks.reserve(score.parts.size());
    bool has_playback_content = false;
    for (std::size_t part_index = 0; part_index < score.parts.size(); ++part_index) {
      const ScorePart& part = score.parts[part_index];
      MidiTrack track;
      track.name = part.name.empty() ? part.id : part.name;
      for (const MidiChannelEvent& event : part.midi_events) {
        if (!validMidiChannelEvent(event)) throw std::invalid_argument("score MIDI channel event is invalid");
      }
      track.channel_events = part.midi_events;
      has_playback_content = has_playback_content || !track.channel_events.empty();
      std::vector<const ScoreNote*> ordered_notes;
      for (const ScoreMeasure& measure : part.measures) {
        if (measure.start < 0) throw std::invalid_argument("score measure start cannot be negative");
        for (const ScoreNote& note : measure.notes) {
          validateNoteTiming(note);
          if (note.rest) {
            if (note.tie_start || note.tie_stop) throw std::invalid_argument("score rest cannot carry a tie");
            continue;
          }
          if (note.midi_channel == -1 && (score.parts.size() > 16 || part_index >= 16)) {
            throw std::invalid_argument("MIDI export of more than 16 score parts requires explicit note channels");
          }
          has_playback_content = true;
          ordered_notes.push_back(&note);
        }
      }
      std::stable_sort(ordered_notes.begin(), ordered_notes.end(), [](const ScoreNote* left, const ScoreNote* right) {
        return left->start < right->start;
      });
      // A notated tie sustains one attack. Keep its identity within the
      // part/staff/voice, sounding pitch, and output channel, so another
      // instrument or route on the same pitch cannot accidentally complete it.
      using TieKey = std::tuple<std::uint16_t, std::uint16_t, std::uint8_t, std::uint8_t>;
      std::map<TieKey, std::size_t> active_ties;
      for (const ScoreNote* source : ordered_notes) {
        const ScoreNote& note = *source;
        const std::uint8_t pitch = toMidiPitch(note.pitch);
        const auto channel = note.midi_channel == -1 ? static_cast<std::uint8_t>(part_index)
                                                     : static_cast<std::uint8_t>(note.midi_channel);
        const TieKey key{note.staff, note.voice, pitch, channel};
        const auto tie = active_ties.find(key);
        const auto tieError = [&](const char* message) {
          return std::invalid_argument(std::string(message) + " in part " + part.id + " at tick " +
                                       std::to_string(note.start) + " (staff " + std::to_string(note.staff) +
                                       ", voice " + std::to_string(note.voice) + ", MIDI pitch " +
                                       std::to_string(pitch) + ")");
        };
        if (note.tie_stop) {
          if (tie == active_ties.end()) throw tieError("score tie stop has no matching start");
          MidiNote& sustained = track.notes[tie->second];
          if (sustained.end() != note.start) {
            throw tieError("score tied notes must be exactly contiguous");
          }
          sustained.duration = note.start + note.duration - sustained.start;
          sustained.off_order = note.midi_off_order;
          sustained.release_velocity = note.midi_release_velocity;
          // Continuation velocity is notation, not a second MIDI note-on.
          if (!note.tie_start) active_ties.erase(tie);
        } else {
          if (tie != active_ties.end()) throw tieError("score tie is missing its continuation stop");
          track.notes.push_back({note.start, note.duration, pitch, note.velocity, channel,
                                 note.midi_release_velocity, note.midi_on_order, note.midi_off_order});
          if (note.tie_start) active_ties.emplace(key, track.notes.size() - 1U);
        }
      }
      if (!active_ties.empty()) {
        const auto& missing = *active_ties.begin();
        throw std::invalid_argument("score tie start has no matching stop in part " + part.id + " at tick " +
                                    std::to_string(track.notes[missing.second].start) + " (staff " +
                                    std::to_string(std::get<0>(missing.first)) + ", voice " +
                                    std::to_string(std::get<1>(missing.first)) + ", MIDI pitch " +
                                    std::to_string(std::get<2>(missing.first)) + ")");
      }
      // Keep deterministic ordering for callers inspecting the in-memory
      // result. Equal-start notes are stable, so chords remain independent.
      std::stable_sort(track.notes.begin(), track.notes.end(), [](const MidiNote& left, const MidiNote& right) {
        return left.start < right.start;
      });
      converted.tracks.push_back(std::move(track));
    }
    // A larger imported arrangement has explicit routes even when several
    // tracks share one channel. Preserve the previous guard for a bare score
    // with no routed playback content at all.
    if (score.parts.size() > 16 && !has_playback_content) {
      throw std::invalid_argument("MIDI export supports at most 16 empty score parts");
    }
    *midi = std::move(converted);
    return true;
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}

bool writeScoreMidiFile(const Score& score, const std::string& path, std::string* error) {
  if (path.empty()) {
    if (error) *error = "MIDI output path is empty";
    return false;
  }
  MidiFile converted;
  std::string conversion_error;
  if (!scoreToMidiFile(score, &converted, &conversion_error)) {
    if (error) *error = conversion_error;
    return false;
  }

  const std::filesystem::path destination(path);
  const std::filesystem::path temporary = temporaryPath(path);
  std::error_code cleanup_error;
  std::filesystem::remove(temporary, cleanup_error);
  std::string write_error;
  if (!writeMidiFile(converted, temporary.string(), &write_error)) {
    std::filesystem::remove(temporary, cleanup_error);
    if (error) *error = write_error;
    return false;
  }
  std::error_code rename_error;
  std::filesystem::rename(temporary, destination, rename_error);
  if (rename_error) {
    std::filesystem::remove(temporary, cleanup_error);
    if (error) *error = "failed to finalize MIDI output: " + rename_error.message();
    return false;
  }
  return true;
}

bool midiToScore(const MidiFile& midi, Score* score, std::string* error) {
  if (score == nullptr) {
    if (error) *error = "score output pointer is null";
    return false;
  }
  try {
    if (midi.format != 0 && midi.format != 1) {
      throw std::invalid_argument("MIDI format must be 0 or 1");
    }
    if (midi.format == 0 && midi.tracks.size() > 1) {
      throw std::invalid_argument("MIDI format 0 can contain only one track");
    }
    if (midi.ticks_per_quarter != kTicksPerQuarter) {
      throw std::invalid_argument("MIDI import requires 960 ticks per quarter note");
    }
    if (!validTimeSignature(midi.time_signature)) {
      throw std::invalid_argument("MIDI time signature must have a positive numerator and power-of-two denominator");
    }
    if (midi.tracks.empty()) {
      throw std::invalid_argument("MIDI import requires at least one non-empty track");
    }
    const Tick measure_ticks = static_cast<Tick>(midi.time_signature.numerator) *
                               kTicksPerQuarter * 4 / midi.time_signature.denominator;
    if (measure_ticks <= 0) throw std::invalid_argument("MIDI time signature produces an empty measure");

    Score converted;
    converted.divisions = kTicksPerQuarter;
    converted.time_signature = midi.time_signature;
    const auto& tempos = midi.tempo.changes();
    if (tempos.empty() || tempos.front().tick != 0 || tempos.size() - 1 > kMaxScoreTempoChanges) {
      throw std::invalid_argument("MIDI import requires an initial tempo and a bounded tempo map");
    }
    converted.bpm = tempos.front().bpm;
    converted.tempo_changes.assign(tempos.begin() + 1, tempos.end());
    (void)scoreTempoMap(converted);
    converted.parts.reserve(midi.tracks.size());

    for (std::size_t track_index = 0; track_index < midi.tracks.size(); ++track_index) {
      const MidiTrack& track = midi.tracks[track_index];
      // Track 0 often carries only tempo/meter, but an event-only track can
      // still own program changes, pedal, or other essential playback data.
      if (track.notes.empty() && track.channel_events.empty()) continue;
      for (const MidiChannelEvent& event : track.channel_events) {
        if (!validMidiChannelEvent(event)) throw std::invalid_argument("MIDI channel event is invalid");
      }
      std::vector<ScoreNote> notes;
      notes.reserve(track.notes.size());
      Tick max_end = 0;
      for (const MidiNote& midi_note : track.notes) {
        if (midi_note.start < 0 || midi_note.duration <= 0 ||
            midi_note.start > std::numeric_limits<Tick>::max() - midi_note.duration) {
          throw std::invalid_argument("MIDI note timing is negative, empty, or overflowing");
        }
        if (midi_note.pitch > 127 || midi_note.velocity > 127 || midi_note.channel > 15 ||
            midi_note.release_velocity > 127) {
          throw std::invalid_argument("MIDI note has an out-of-range field");
        }
        if (midi_note.velocity == 0) {
          throw std::invalid_argument("MIDI sounding note attack velocity must be in 1..127");
        }
        const Tick end = midi_note.start + midi_note.duration;
        max_end = std::max(max_end, end);
        ScoreNote note;
        note.start = midi_note.start;
        note.duration = midi_note.duration;
        note.pitch = fromMidiPitch(midi_note.pitch);
        note.velocity = midi_note.velocity;
        note.midi_channel = midi_note.channel;
        note.midi_on_order = midi_note.on_order;
        note.midi_off_order = midi_note.off_order;
        note.midi_release_velocity = midi_note.release_velocity;
        note.voice = static_cast<std::uint16_t>(static_cast<std::uint16_t>(midi_note.channel) + 1U);
        note.staff = 1;
        notes.push_back(std::move(note));
      }
      std::stable_sort(notes.begin(), notes.end(), [](const ScoreNote& left, const ScoreNote& right) {
        if (left.start != right.start) return left.start < right.start;
        if (left.voice != right.voice) return left.voice < right.voice;
        return left.pitch.octave < right.pitch.octave;
      });
      // One MIDI channel becomes one score voice. Overlapping instances of
      // the same pitch cannot retain independent note/tie identities in that
      // voice, especially after splitting at barlines. Fail before returning
      // a score that could not be exported again without guessing.
      using VoicePitch = std::pair<std::uint16_t, std::uint8_t>;
      std::map<VoicePitch, Tick> previous_ends;
      for (const ScoreNote& note : notes) {
        const std::uint8_t pitch = toMidiPitch(note.pitch);
        const VoicePitch key{note.voice, pitch};
        const auto previous = previous_ends.find(key);
        if (previous != previous_ends.end() && note.start < previous->second) {
          throw std::invalid_argument("MIDI same-channel same-pitch overlap cannot be represented by one score voice in track " +
                                      std::to_string(track_index + 1U) + " at tick " + std::to_string(note.start) +
                                      " (channel " + std::to_string(note.voice - 1U) + ", MIDI pitch " +
                                      std::to_string(pitch) + ")");
        }
        previous_ends[key] = note.start + note.duration;
      }
      const Tick measure_count_tick = max_end / measure_ticks;
      const Tick measure_count_remainder = max_end % measure_ticks;
      // Channel events do not require notated bars. An event-only track gets
      // one editable measure even if its final controller is very late.
      const Tick measure_count_with_remainder =
          std::max<Tick>(1, measure_count_tick + (measure_count_remainder == 0 ? 0 : 1));
      if (static_cast<std::uint64_t>(measure_count_with_remainder) > kMaximumImportedMeasures) {
        throw std::invalid_argument("MIDI import requires a bounded measure count");
      }
      const std::size_t measure_count = static_cast<std::size_t>(measure_count_with_remainder);

      ScorePart part;
      const std::size_t part_index = converted.parts.size();
      part.id = "P" + std::to_string(part_index + 1);
      part.name = track.name.empty() ? "Part " + std::to_string(part_index + 1) : track.name;
      part.midi_events = track.channel_events;
      part.measures.resize(measure_count);
      for (std::size_t measure_index = 0; measure_index < measure_count; ++measure_index) {
        part.measures[measure_index].number = static_cast<int>(measure_index + 1);
        part.measures[measure_index].start =
            static_cast<Tick>(measure_index) * measure_ticks;
      }
      std::size_t segment_count = 0;
      for (const ScoreNote& note : notes) {
        const Tick note_end = note.start + note.duration;
        Tick segment_start = note.start;
        while (segment_start < note_end) {
          const Tick measure_index_tick = segment_start / measure_ticks;
          if (measure_index_tick < 0 ||
              static_cast<std::uint64_t>(measure_index_tick) >= measure_count) {
            throw std::invalid_argument("MIDI note cannot be assigned to a score measure");
          }
          if (segment_count >= kMaximumImportedNoteSegments) {
            throw std::invalid_argument("MIDI import requires a bounded note segment count");
          }
          const Tick until_barline = measure_ticks - segment_start % measure_ticks;
          const Tick segment_duration = std::min(note_end - segment_start, until_barline);
          ScoreNote segment = note;
          segment.start = segment_start;
          segment.duration = segment_duration;
          segment.tie_stop = segment_start != note.start;
          segment.tie_start = segment_duration < note_end - segment_start;
          if (segment.tie_stop) segment.midi_on_order = 0;
          if (segment.tie_start) {
            segment.midi_off_order = 0;
            segment.midi_release_velocity = 0;
          }
          part.measures[static_cast<std::size_t>(measure_index_tick)].notes.push_back(std::move(segment));
          segment_start += segment_duration;
          ++segment_count;
        }
      }
      // A held note can join a newly attacked chord at the next barline.
      // Recompute chord flags after splitting rather than copying the flag
      // from the source note, whose original onset may have been elsewhere.
      for (ScoreMeasure& measure : part.measures) {
        std::stable_sort(measure.notes.begin(), measure.notes.end(), [](const ScoreNote& left, const ScoreNote& right) {
          if (left.start != right.start) return left.start < right.start;
          return left.voice < right.voice;
        });
        for (std::size_t note_index = 1; note_index < measure.notes.size(); ++note_index) {
          if (measure.notes[note_index].start == measure.notes[note_index - 1].start &&
              measure.notes[note_index].voice == measure.notes[note_index - 1].voice) {
            measure.notes[note_index].chord = true;
          }
        }
      }
      converted.parts.push_back(std::move(part));
    }
    if (converted.parts.empty()) {
      throw std::invalid_argument("MIDI import requires at least one non-empty track");
    }
    *score = std::move(converted);
    return true;
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}

bool readMidiScoreFile(const std::string& path, Score* score, std::string* error) {
  if (path.empty()) {
    if (error) *error = "MIDI input path is empty";
    return false;
  }
  MidiFile midi;
  std::string read_error;
  if (!readMidiFile(path, &midi, &read_error)) {
    if (error) *error = read_error;
    return false;
  }
  return midiToScore(midi, score, error);
}

}  // namespace daw
