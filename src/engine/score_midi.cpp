#include "daw/score_midi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
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
    // One channel per part keeps the exported file deterministic and avoids
    // channel collisions.  This bridge does not yet emit program changes or
    // a channel-allocation map, so reject rather than silently aliasing parts.
    if (score.parts.size() > 16) throw std::invalid_argument("MIDI export supports at most 16 score parts");
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
    converted.tempo = TempoMap(score.bpm);
    converted.tracks.reserve(score.parts.size());
    for (std::size_t part_index = 0; part_index < score.parts.size(); ++part_index) {
      const ScorePart& part = score.parts[part_index];
      MidiTrack track;
      track.name = part.name.empty() ? part.id : part.name;
      const auto channel = static_cast<std::uint8_t>(part_index);
      for (const ScoreMeasure& measure : part.measures) {
        if (measure.start < 0) throw std::invalid_argument("score measure start cannot be negative");
        for (const ScoreNote& note : measure.notes) {
          validateNoteTiming(note);
          if (note.rest) continue;
          track.notes.push_back({note.start, note.duration, toMidiPitch(note.pitch), note.velocity, channel});
        }
      }
      // Keep deterministic ordering for callers inspecting the in-memory
      // result. Equal-start notes are stable, so chords remain independent.
      std::stable_sort(track.notes.begin(), track.notes.end(), [](const MidiNote& left, const MidiNote& right) {
        return left.start < right.start;
      });
      converted.tracks.push_back(std::move(track));
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

    double first_bpm = 0.0;
    for (const TempoChange& change : midi.tempo.changes()) {
      if (std::isfinite(change.bpm) && change.bpm > 0.0) {
        first_bpm = change.bpm;
        break;
      }
    }
    if (!(first_bpm > 0.0)) {
      throw std::invalid_argument("MIDI import requires a valid tempo");
    }

    Score converted;
    converted.divisions = kTicksPerQuarter;
    converted.time_signature = midi.time_signature;
    converted.bpm = first_bpm;
    converted.parts.reserve(midi.tracks.size());

    for (std::size_t track_index = 0; track_index < midi.tracks.size(); ++track_index) {
      const MidiTrack& track = midi.tracks[track_index];
      if (track.notes.empty()) {
        throw std::invalid_argument("MIDI import rejects empty tracks");
      }
      std::vector<ScoreNote> notes;
      notes.reserve(track.notes.size());
      Tick max_end = 0;
      for (const MidiNote& midi_note : track.notes) {
        if (midi_note.start < 0 || midi_note.duration <= 0 ||
            midi_note.start > std::numeric_limits<Tick>::max() - midi_note.duration) {
          throw std::invalid_argument("MIDI note timing is negative, empty, or overflowing");
        }
        if (midi_note.pitch > 127 || midi_note.velocity > 127 || midi_note.channel > 15) {
          throw std::invalid_argument("MIDI note has an out-of-range field");
        }
        const Tick end = midi_note.start + midi_note.duration;
        max_end = std::max(max_end, end);
        ScoreNote note;
        note.start = midi_note.start;
        note.duration = midi_note.duration;
        note.pitch = fromMidiPitch(midi_note.pitch);
        note.velocity = midi_note.velocity;
        note.voice = static_cast<std::uint16_t>(static_cast<std::uint16_t>(midi_note.channel) + 1U);
        note.staff = 1;
        notes.push_back(std::move(note));
      }
      std::stable_sort(notes.begin(), notes.end(), [](const ScoreNote& left, const ScoreNote& right) {
        if (left.start != right.start) return left.start < right.start;
        if (left.voice != right.voice) return left.voice < right.voice;
        return left.pitch.octave < right.pitch.octave;
      });
      for (std::size_t note_index = 1; note_index < notes.size(); ++note_index) {
        if (notes[note_index].start == notes[note_index - 1].start &&
            notes[note_index].voice == notes[note_index - 1].voice) {
          notes[note_index].chord = true;
        }
      }

      const Tick measure_count_tick = max_end / measure_ticks;
      const Tick measure_count_remainder = max_end % measure_ticks;
      const Tick measure_count_with_remainder =
          measure_count_tick + (measure_count_remainder == 0 ? 0 : 1);
      if (measure_count_with_remainder <= 0 ||
          static_cast<std::uint64_t>(measure_count_with_remainder) > kMaximumImportedMeasures) {
        throw std::invalid_argument("MIDI import requires a bounded measure count");
      }
      const std::size_t measure_count = static_cast<std::size_t>(measure_count_with_remainder);

      ScorePart part;
      part.id = "P" + std::to_string(track_index + 1);
      part.name = track.name.empty() ? "Part " + std::to_string(track_index + 1) : track.name;
      part.measures.resize(measure_count);
      for (std::size_t measure_index = 0; measure_index < measure_count; ++measure_index) {
        part.measures[measure_index].number = static_cast<int>(measure_index + 1);
        part.measures[measure_index].start =
            static_cast<Tick>(measure_index) * measure_ticks;
      }
      for (ScoreNote& note : notes) {
        const Tick measure_index_tick = note.start / measure_ticks;
        if (measure_index_tick < 0 ||
            static_cast<std::uint64_t>(measure_index_tick) >= measure_count) {
          throw std::invalid_argument("MIDI note cannot be assigned to a score measure");
        }
        part.measures[static_cast<std::size_t>(measure_index_tick)].notes.push_back(std::move(note));
      }
      converted.parts.push_back(std::move(part));
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
