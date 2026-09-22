#include "daw/project.hpp"

#include "daw/history.hpp"
#include "daw/meter_map.hpp"

#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace daw {
namespace {

constexpr std::uint64_t kMaxParts = 1024;
constexpr std::uint64_t kMaxMeasures = 1'000'000;
constexpr std::uint64_t kMaxNotes = 1'000'000;
constexpr std::uint64_t kMaxTotalMeasures = 1'000'000;
constexpr std::uint64_t kMaxTotalNotes = 1'000'000;
constexpr std::uint64_t kMaxMidiEvents = 1'000'000;
constexpr std::uint64_t kMaxTotalMidiEvents = 1'000'000;
constexpr std::size_t kMaxLineBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaxStringBytes = 1U * 1024U * 1024U;

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) *error = message;
  return false;
}

std::string hexEncode(const std::string& value) {
  static constexpr char digits[] = "0123456789abcdef";
  if (value.empty()) return "-";
  std::string result;
  result.reserve(value.size() * 2U);
  for (const char character : value) {
    const unsigned char byte = static_cast<unsigned char>(character);
    result.push_back(digits[(byte >> 4U) & 0x0fU]);
    result.push_back(digits[byte & 0x0fU]);
  }
  return result;
}

int hexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool hexDecode(const std::string& encoded, std::string* value, std::string* error) {
  if (encoded == "-") {
    value->clear();
    return true;
  }
  if (encoded.empty() || encoded.size() % 2U != 0U || encoded.size() / 2U > kMaxStringBytes) {
    return fail(error, "invalid project string encoding");
  }
  value->clear();
  value->reserve(encoded.size() / 2U);
  for (std::size_t index = 0; index < encoded.size(); index += 2U) {
    const int high = hexDigit(encoded[index]);
    const int low = hexDigit(encoded[index + 1U]);
    if (high < 0 || low < 0) return fail(error, "invalid project string encoding");
    value->push_back(static_cast<char>((high << 4) | low));
  }
  return true;
}

bool parseI64(const std::string& token, std::int64_t* value) {
  if (token.empty()) return false;
  const auto result = std::from_chars(token.data(), token.data() + token.size(), *value);
  return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}

bool parseU64(const std::string& token, std::uint64_t* value) {
  if (token.empty()) return false;
  const auto result = std::from_chars(token.data(), token.data() + token.size(), *value);
  return result.ec == std::errc{} && result.ptr == token.data() + token.size();
}

bool parseDouble(const std::string& token, double* value) {
  if (token.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const double parsed = std::strtod(token.c_str(), &end);
  if (errno == ERANGE || end == token.c_str() || end != token.c_str() + token.size() || !std::isfinite(parsed)) {
    return false;
  }
  *value = parsed;
  return true;
}

template <typename T>
bool parseSigned(const std::string& token, T* value) {
  std::int64_t parsed = 0;
  if (!parseI64(token, &parsed) || parsed < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
      parsed > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
    return false;
  }
  *value = static_cast<T>(parsed);
  return true;
}

template <typename T>
bool parseUnsigned(const std::string& token, T* value) {
  std::uint64_t parsed = 0;
  if (!parseU64(token, &parsed) || parsed > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) return false;
  *value = static_cast<T>(parsed);
  return true;
}

bool validStep(char step) {
  return step >= 'A' && step <= 'G';
}

bool validateScore(const Score& score, std::string* error) {
  if (score.divisions <= 0 || score.divisions > 1'000'000) return fail(error, "project divisions out of range");
  (void)scoreTempoMap(score);
  validateMeterMap(score.time_signature, score.meter_changes);
  if (score.parts.size() > kMaxParts) return fail(error, "project has too many parts");
  std::uint64_t total_measures = 0;
  std::uint64_t total_notes = 0;
  std::uint64_t total_midi_events = 0;
  for (const ScorePart& part : score.parts) {
    if (part.id.empty() || part.id.size() > kMaxStringBytes || part.name.size() > kMaxStringBytes) {
      return fail(error, "project part string out of range");
    }
    if (part.measures.size() > kMaxMeasures) return fail(error, "project has too many measures");
    if (part.measures.size() > kMaxTotalMeasures - total_measures) return fail(error, "project has too many measures");
    total_measures += static_cast<std::uint64_t>(part.measures.size());
    if (part.midi_events.size() > kMaxMidiEvents ||
        part.midi_events.size() > kMaxTotalMidiEvents - total_midi_events) {
      return fail(error, "project has too many MIDI events");
    }
    total_midi_events += static_cast<std::uint64_t>(part.midi_events.size());
    for (const MidiChannelEvent& event : part.midi_events) {
      if (!validMidiChannelEvent(event)) return fail(error, "project MIDI event out of range");
    }
    for (const ScoreMeasure& measure : part.measures) {
      if (measure.number <= 0 || measure.start < 0 || measure.notes.size() > kMaxNotes ||
          measure.notes.size() > kMaxTotalNotes - total_notes) {
        return fail(error, "project measure out of range");
      }
      total_notes += static_cast<std::uint64_t>(measure.notes.size());
      for (const ScoreNote& note : measure.notes) {
        if (note.start < 0 || note.duration <= 0 || note.duration > std::numeric_limits<Tick>::max() - note.start) {
          return fail(error, "project note timing out of range");
        }
        if (!validStep(note.pitch.step)) return fail(error, "project note pitch step is invalid");
        if (note.pitch.alter < -128 || note.pitch.alter > 127 || note.pitch.octave < -128 || note.pitch.octave > 127) {
          return fail(error, "project note pitch out of range");
        }
        if (note.rest && note.chord) return fail(error, "project rest cannot be a chord note");
        if (note.velocity > 127 || note.voice == 0 || note.staff == 0 ||
            ((note.tuplet_actual == 0) != (note.tuplet_normal == 0))) {
          return fail(error, "project note attribute out of range");
        }
        if (note.lyric.size() > kMaxStringBytes) return fail(error, "project note lyric out of range");
        if (note.midi_channel < -1 || note.midi_channel > 15 || note.midi_release_velocity > 127) {
          return fail(error, "project note MIDI metadata out of range");
        }
      }
    }
  }
  return true;
}

class LineReader {
 public:
  LineReader(const std::string& path, std::string* error) : input_(path, std::ios::binary), error_(error) {}

  bool opened() const { return input_.is_open(); }

  bool read(std::vector<std::string>* tokens) {
    std::string line;
    if (!std::getline(input_, line)) return fail(error_, "truncated project file");
    ++line_number_;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.size() > kMaxLineBytes || line.empty()) return fail(error_, "invalid project line");
    std::istringstream parser(line);
    tokens->clear();
    std::string token;
    while (parser >> token) tokens->push_back(std::move(token));
    if (tokens->empty()) return fail(error_, "invalid project line");
    return true;
  }

  bool hasTrailingData() {
    std::string line;
    if (!std::getline(input_, line)) return false;
    fail(error_, "trailing data after end_project");
    return true;
  }

  std::size_t lineNumber() const { return line_number_; }

 private:
  std::ifstream input_;
  std::string* error_ = nullptr;
  std::size_t line_number_ = 0;
};

bool expectLine(LineReader* reader, const std::string& key, std::size_t argument_count,
                std::vector<std::string>* arguments, std::string* error) {
  std::vector<std::string> tokens;
  if (!reader->read(&tokens)) return false;
  if (tokens.empty() || tokens.front() != key || tokens.size() != argument_count + 1U) {
    return fail(error, "invalid project line " + std::to_string(reader->lineNumber()) + ": expected " + key);
  }
  arguments->assign(tokens.begin() + 1, tokens.end());
  return true;
}

bool parseCount(const std::string& token, std::uint64_t limit, std::size_t* result, std::string* error,
                const std::string& field) {
  std::uint64_t parsed = 0;
  if (!parseU64(token, &parsed) || parsed > limit || parsed > std::numeric_limits<std::size_t>::max()) {
    return fail(error, "project " + field + " count out of range");
  }
  *result = static_cast<std::size_t>(parsed);
  return true;
}

}  // namespace

bool writeProjectFile(const Score& score, const std::string& path, std::string* error) {
  try {
    if (path.empty()) return fail(error, "project path is empty");
    if (!validateScore(score, error)) return false;

    std::ostringstream output;
    output.precision(17);
    // Version 5 retains all four MIDI meter fields and later meter changes.
    // Earlier versions keep their default meter metadata and empty meter map.
    output << "CLASSICAL_DAW_PROJECT 5\n";
    output << "divisions " << score.divisions << "\n";
    output << "bpm " << score.bpm << "\n";
    output << "tempo_changes " << score.tempo_changes.size() << "\n";
    for (const TempoChange& change : score.tempo_changes) {
      output << "tempo " << change.tick << ' ' << change.bpm << "\n";
    }
    output << "meter " << static_cast<unsigned int>(score.time_signature.numerator) << ' '
           << static_cast<unsigned int>(score.time_signature.denominator) << ' '
           << static_cast<unsigned int>(score.time_signature.clocks_per_click) << ' '
           << static_cast<unsigned int>(score.time_signature.notated_32nds_per_quarter) << "\n";
    output << "meter_changes " << score.meter_changes.size() << "\n";
    for (const TimeSignatureChange& change : score.meter_changes) {
      output << "meter_change " << change.tick << ' '
             << static_cast<unsigned int>(change.signature.numerator) << ' '
             << static_cast<unsigned int>(change.signature.denominator) << ' '
             << static_cast<unsigned int>(change.signature.clocks_per_click) << ' '
             << static_cast<unsigned int>(change.signature.notated_32nds_per_quarter) << "\n";
    }
    output << "parts " << score.parts.size() << "\n";
    for (std::size_t part_index = 0; part_index < score.parts.size(); ++part_index) {
      const ScorePart& part = score.parts[part_index];
      output << "part " << part_index << "\n";
      output << "id " << hexEncode(part.id) << "\n";
      output << "name " << hexEncode(part.name) << "\n";
      output << "measures " << part.measures.size() << "\n";
      for (std::size_t measure_index = 0; measure_index < part.measures.size(); ++measure_index) {
        const ScoreMeasure& measure = part.measures[measure_index];
        output << "measure " << measure_index << "\n";
        output << "number " << measure.number << "\n";
        output << "start " << measure.start << "\n";
        output << "notes " << measure.notes.size() << "\n";
        for (std::size_t note_index = 0; note_index < measure.notes.size(); ++note_index) {
          const ScoreNote& note = measure.notes[note_index];
          output << "note " << note_index << "\n";
          output << "start " << note.start << "\n";
          output << "duration " << note.duration << "\n";
          output << "pitch_step " << note.pitch.step << "\n";
          output << "pitch_alter " << note.pitch.alter << "\n";
          output << "pitch_octave " << note.pitch.octave << "\n";
          output << "rest " << (note.rest ? 1 : 0) << "\n";
          output << "chord " << (note.chord ? 1 : 0) << "\n";
          output << "tie_start " << (note.tie_start ? 1 : 0) << "\n";
          output << "tie_stop " << (note.tie_stop ? 1 : 0) << "\n";
          output << "velocity " << static_cast<unsigned int>(note.velocity) << "\n";
          output << "voice " << note.voice << "\n";
          output << "staff " << note.staff << "\n";
          output << "tuplet_actual " << note.tuplet_actual << "\n";
          output << "tuplet_normal " << note.tuplet_normal << "\n";
          output << "lyric " << hexEncode(note.lyric) << "\n";
          output << "midi_channel " << note.midi_channel << "\n";
          output << "midi_on_order " << note.midi_on_order << "\n";
          output << "midi_off_order " << note.midi_off_order << "\n";
          output << "midi_release_velocity " << static_cast<unsigned int>(note.midi_release_velocity) << "\n";
          output << "end_note\n";
        }
        output << "end_measure\n";
      }
      output << "midi_events " << part.midi_events.size() << "\n";
      for (const MidiChannelEvent& event : part.midi_events) {
        output << "midi_event " << event.tick << ' ' << static_cast<unsigned int>(event.type) << ' '
               << static_cast<unsigned int>(event.channel) << ' ' << static_cast<unsigned int>(event.data1) << ' '
               << static_cast<unsigned int>(event.data2) << ' ' << event.order << "\n";
      }
      output << "end_part\n";
    }
    output << "end_project\n";
    const std::string serialized = output.str();

    const std::filesystem::path destination(path);
    std::filesystem::path temporary = destination;
    temporary += ".tmp";
    {
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      if (!file) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return fail(error, "cannot open temporary project file");
      }
      file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
      file.flush();
      if (!file) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return fail(error, "cannot write temporary project file");
      }
    }
    if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
      const int code = errno;
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return fail(error, "cannot atomically replace project file: " + std::generic_category().message(code));
    }
    return true;
  } catch (const std::exception& exception) {
    return fail(error, std::string("project write failed: ") + exception.what());
  }
}

bool readProjectFile(const std::string& path, Score* score, std::string* error) {
  try {
    if (score == nullptr) return fail(error, "project score output is null");
    if (path.empty()) return fail(error, "project path is empty");
    LineReader reader(path, error);
    if (!reader.opened()) return fail(error, "cannot open project file");

    std::vector<std::string> arguments;
    if (!expectLine(&reader, "CLASSICAL_DAW_PROJECT", 1, &arguments, error)) return false;
    std::uint64_t project_version = 0;
    if (!parseU64(arguments[0], &project_version) || project_version < 1 || project_version > 5) {
      return fail(error, "unsupported project version: " + arguments[0]);
    }

    Score parsed;
    if (!expectLine(&reader, "divisions", 1, &arguments, error) || !parseSigned(arguments[0], &parsed.divisions)) {
      return fail(error, "invalid project divisions");
    }
    if (!expectLine(&reader, "bpm", 1, &arguments, error) || !parseDouble(arguments[0], &parsed.bpm)) {
      return fail(error, "invalid project bpm");
    }
    if (project_version >= 4) {
      if (!expectLine(&reader, "tempo_changes", 1, &arguments, error)) return false;
      std::size_t tempo_count = 0;
      if (!parseCount(arguments[0], kMaxScoreTempoChanges, &tempo_count, error, "tempo change")) return false;
      parsed.tempo_changes.reserve(tempo_count);
      for (std::size_t index = 0; index < tempo_count; ++index) {
        TempoChange change;
        if (!expectLine(&reader, "tempo", 2, &arguments, error) ||
            !parseSigned(arguments[0], &change.tick) || !parseDouble(arguments[1], &change.bpm)) {
          return fail(error, "invalid project tempo change");
        }
        parsed.tempo_changes.push_back(change);
      }
    }
    if (!expectLine(&reader, "meter", project_version >= 5 ? 4U : 2U, &arguments, error) ||
        !parseUnsigned(arguments[0], &parsed.time_signature.numerator) ||
        !parseUnsigned(arguments[1], &parsed.time_signature.denominator) ||
        (project_version >= 5 &&
         (!parseUnsigned(arguments[2], &parsed.time_signature.clocks_per_click) ||
          !parseUnsigned(arguments[3], &parsed.time_signature.notated_32nds_per_quarter)))) {
      return fail(error, "invalid project meter");
    }
    if (project_version >= 5) {
      if (!expectLine(&reader, "meter_changes", 1, &arguments, error)) return false;
      std::size_t meter_count = 0;
      if (!parseCount(arguments[0], kMaxMeterChanges, &meter_count, error, "meter change")) return false;
      parsed.meter_changes.reserve(meter_count);
      for (std::size_t index = 0; index < meter_count; ++index) {
        TimeSignatureChange change;
        if (!expectLine(&reader, "meter_change", 5, &arguments, error) ||
            !parseSigned(arguments[0], &change.tick) ||
            !parseUnsigned(arguments[1], &change.signature.numerator) ||
            !parseUnsigned(arguments[2], &change.signature.denominator) ||
            !parseUnsigned(arguments[3], &change.signature.clocks_per_click) ||
            !parseUnsigned(arguments[4], &change.signature.notated_32nds_per_quarter)) {
          return fail(error, "invalid project meter change");
        }
        parsed.meter_changes.push_back(change);
      }
    }
    if (!expectLine(&reader, "parts", 1, &arguments, error)) return false;
    std::size_t part_count = 0;
    if (!parseCount(arguments[0], kMaxParts, &part_count, error, "part")) return false;
    parsed.parts.resize(part_count);
    std::uint64_t total_measures = 0;
    std::uint64_t total_notes = 0;
    std::uint64_t total_midi_events = 0;

    for (std::size_t part_index = 0; part_index < part_count; ++part_index) {
      if (!expectLine(&reader, "part", 1, &arguments, error)) return false;
      std::size_t encoded_index = 0;
      if (!parseCount(arguments[0], part_count == 0 ? 0 : part_count - 1U, &encoded_index, error, "part index") ||
          encoded_index != part_index) {
        return fail(error, "project part index is out of order");
      }
      ScorePart& part = parsed.parts[part_index];
      if (!expectLine(&reader, "id", 1, &arguments, error) || !hexDecode(arguments[0], &part.id, error) || part.id.empty()) {
        return fail(error, "invalid project part id");
      }
      if (!expectLine(&reader, "name", 1, &arguments, error) || !hexDecode(arguments[0], &part.name, error)) {
        return fail(error, "invalid project part name");
      }
      if (!expectLine(&reader, "measures", 1, &arguments, error)) return false;
      std::size_t measure_count = 0;
      if (!parseCount(arguments[0], kMaxMeasures, &measure_count, error, "measure")) return false;
      if (measure_count > kMaxTotalMeasures - total_measures) return fail(error, "project has too many measures");
      total_measures += static_cast<std::uint64_t>(measure_count);
      part.measures.resize(measure_count);

      for (std::size_t measure_index = 0; measure_index < measure_count; ++measure_index) {
        if (!expectLine(&reader, "measure", 1, &arguments, error)) return false;
        std::size_t encoded_measure_index = 0;
        if (!parseCount(arguments[0], measure_count == 0 ? 0 : measure_count - 1U, &encoded_measure_index, error,
                        "measure index") || encoded_measure_index != measure_index) {
          return fail(error, "project measure index is out of order");
        }
        ScoreMeasure& measure = part.measures[measure_index];
        if (!expectLine(&reader, "number", 1, &arguments, error) || !parseSigned(arguments[0], &measure.number)) {
          return fail(error, "invalid project measure number");
        }
        if (!expectLine(&reader, "start", 1, &arguments, error) || !parseSigned(arguments[0], &measure.start)) {
          return fail(error, "invalid project measure start");
        }
        if (!expectLine(&reader, "notes", 1, &arguments, error)) return false;
        std::size_t note_count = 0;
        if (!parseCount(arguments[0], kMaxNotes, &note_count, error, "note")) return false;
        if (note_count > kMaxTotalNotes - total_notes) return fail(error, "project has too many notes");
        total_notes += static_cast<std::uint64_t>(note_count);
        measure.notes.resize(note_count);

        for (std::size_t note_index = 0; note_index < note_count; ++note_index) {
          if (!expectLine(&reader, "note", 1, &arguments, error)) return false;
          std::size_t encoded_note_index = 0;
          if (!parseCount(arguments[0], note_count == 0 ? 0 : note_count - 1U, &encoded_note_index, error, "note index") ||
              encoded_note_index != note_index) {
            return fail(error, "project note index is out of order");
          }
          ScoreNote& note = measure.notes[note_index];
          if (!expectLine(&reader, "start", 1, &arguments, error) || !parseSigned(arguments[0], &note.start) ||
              !expectLine(&reader, "duration", 1, &arguments, error) || !parseSigned(arguments[0], &note.duration)) {
            return fail(error, "invalid project note timing");
          }
          if (!expectLine(&reader, "pitch_step", 1, &arguments, error) || arguments[0].size() != 1U) {
            return fail(error, "invalid project note pitch step");
          }
          note.pitch.step = arguments[0][0];
          if (!expectLine(&reader, "pitch_alter", 1, &arguments, error) || !parseSigned(arguments[0], &note.pitch.alter) ||
              !expectLine(&reader, "pitch_octave", 1, &arguments, error) || !parseSigned(arguments[0], &note.pitch.octave)) {
            return fail(error, "invalid project note pitch");
          }
          auto parseBoolField = [&](const std::string& key, bool* field) -> bool {
            std::vector<std::string> values;
            if (!expectLine(&reader, key, 1, &values, error)) return false;
            if (values[0] == "0") {
              *field = false;
              return true;
            }
            if (values[0] == "1") {
              *field = true;
              return true;
            }
            return fail(error, "invalid project boolean field " + key);
          };
          if (!parseBoolField("rest", &note.rest) || !parseBoolField("chord", &note.chord) ||
              !parseBoolField("tie_start", &note.tie_start) || !parseBoolField("tie_stop", &note.tie_stop)) {
            return false;
          }
          std::uint16_t velocity = 0;
          if (!expectLine(&reader, "velocity", 1, &arguments, error) || !parseUnsigned(arguments[0], &velocity) ||
              velocity > 127) {
            return fail(error, "invalid project note velocity");
          }
          note.velocity = static_cast<std::uint8_t>(velocity);
          if (!expectLine(&reader, "voice", 1, &arguments, error) || !parseUnsigned(arguments[0], &note.voice) ||
              !expectLine(&reader, "staff", 1, &arguments, error) || !parseUnsigned(arguments[0], &note.staff) ||
              !expectLine(&reader, "tuplet_actual", 1, &arguments, error) ||
              !parseUnsigned(arguments[0], &note.tuplet_actual) || !expectLine(&reader, "tuplet_normal", 1, &arguments, error) ||
              !parseUnsigned(arguments[0], &note.tuplet_normal)) {
            return fail(error, "invalid project note attributes");
          }
          if (project_version >= 2) {
            if (!expectLine(&reader, "lyric", 1, &arguments, error) || !hexDecode(arguments[0], &note.lyric, error)) {
              return fail(error, "invalid project note lyric");
            }
          }
          if (project_version >= 3) {
            if (!expectLine(&reader, "midi_channel", 1, &arguments, error) ||
                !parseSigned(arguments[0], &note.midi_channel) ||
                !expectLine(&reader, "midi_on_order", 1, &arguments, error) ||
                !parseU64(arguments[0], &note.midi_on_order) ||
                !expectLine(&reader, "midi_off_order", 1, &arguments, error) ||
                !parseU64(arguments[0], &note.midi_off_order) ||
                !expectLine(&reader, "midi_release_velocity", 1, &arguments, error) ||
                !parseUnsigned(arguments[0], &note.midi_release_velocity)) {
              return fail(error, "invalid project note MIDI metadata");
            }
          }
          if (!expectLine(&reader, "end_note", 0, &arguments, error)) return false;
        }
        if (!expectLine(&reader, "end_measure", 0, &arguments, error)) return false;
      }
      if (project_version >= 3) {
        if (!expectLine(&reader, "midi_events", 1, &arguments, error)) return false;
        std::size_t event_count = 0;
        if (!parseCount(arguments[0], kMaxMidiEvents, &event_count, error, "MIDI event")) return false;
        if (event_count > kMaxTotalMidiEvents - total_midi_events) {
          return fail(error, "project has too many MIDI events");
        }
        total_midi_events += static_cast<std::uint64_t>(event_count);
        part.midi_events.resize(event_count);
        for (MidiChannelEvent& event : part.midi_events) {
          std::uint8_t type = 0;
          if (!expectLine(&reader, "midi_event", 6, &arguments, error) ||
              !parseSigned(arguments[0], &event.tick) || !parseUnsigned(arguments[1], &type) ||
              !parseUnsigned(arguments[2], &event.channel) || !parseUnsigned(arguments[3], &event.data1) ||
              !parseUnsigned(arguments[4], &event.data2) || !parseU64(arguments[5], &event.order)) {
            return fail(error, "invalid project MIDI event");
          }
          event.type = static_cast<MidiChannelEventType>(type);
          if (!validMidiChannelEvent(event)) return fail(error, "project MIDI event out of range");
        }
      }
      if (!expectLine(&reader, "end_part", 0, &arguments, error)) return false;
    }
    if (!expectLine(&reader, "end_project", 0, &arguments, error)) return false;
    if (reader.hasTrailingData()) return false;
    if (!validateScore(parsed, error)) return false;
    *score = std::move(parsed);
    return true;
  } catch (const std::exception& exception) {
    return fail(error, std::string("project read failed: ") + exception.what());
  }
}

bool writeProjectRecoveryFile(const Score& score, const std::string& path, std::string* error) {
  if (path.empty()) return fail(error, "project path is empty");
  std::string sidecar = path;
  sidecar += ".recovery";
  if (!writeProjectFile(score, sidecar, error)) {
    if (error != nullptr && !error->empty()) *error = "recovery write failed: " + *error;
    return false;
  }
  return true;
}

bool writeProjectRecoveryFile(const ScoreHistory& history, const std::string& path, std::string* error) {
  Score current;
  if (!history.snapshot(&current, error)) {
    if (error != nullptr && !error->empty()) *error = "recovery snapshot failed: " + *error;
    return false;
  }
  return writeProjectRecoveryFile(current, path, error);
}

bool readProjectFileWithRecovery(const std::string& path, Score* score, ProjectLoadSource* source,
                                 std::string* error) {
  if (score == nullptr) return fail(error, "project score output is null");
  if (path.empty()) return fail(error, "project path is empty");
  if (source != nullptr) *source = ProjectLoadSource::None;
  if (error != nullptr) error->clear();

  Score parsed;
  std::string primary_error;
  if (readProjectFile(path, &parsed, &primary_error)) {
    *score = std::move(parsed);
    if (source != nullptr) *source = ProjectLoadSource::Primary;
    return true;
  }

  std::string recovery_path = path + ".recovery";
  std::string recovery_error;
  if (readProjectFile(recovery_path, &parsed, &recovery_error)) {
    *score = std::move(parsed);
    if (source != nullptr) *source = ProjectLoadSource::Recovery;
    return true;
  }

  std::string temporary_path = path + ".tmp";
  std::string temporary_error;
  if (readProjectFile(temporary_path, &parsed, &temporary_error)) {
    *score = std::move(parsed);
    if (source != nullptr) *source = ProjectLoadSource::Temporary;
    return true;
  }

  return fail(error, "no valid project candidate; primary: " + primary_error +
                       "; recovery: " + recovery_error + "; temporary: " + temporary_error);
}

}  // namespace daw
