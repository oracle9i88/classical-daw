#include "daw/score.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace daw {
namespace {

struct XmlBlock {
  std::size_t start = 0;
  std::size_t open_end = 0;
  std::size_t content_start = 0;
  std::size_t content_end = 0;
  std::string opening;
};

std::string trim(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c) != 0; }).base();
  if (first >= last) return {};
  return std::string(first, last);
}

bool tagBoundary(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0 || c == '>' || c == '/';
}

std::vector<XmlBlock> findBlocks(const std::string& xml, const std::string& tag, std::size_t begin, std::size_t end) {
  std::vector<XmlBlock> result;
  const std::string open_prefix = "<" + tag;
  const std::string close_tag = "</" + tag + ">";
  std::size_t search = begin;
  while (search < end) {
    const std::size_t start = xml.find(open_prefix, search);
    if (start == std::string::npos || start >= end) break;
    const std::size_t boundary = start + open_prefix.size();
    if (boundary >= end || !tagBoundary(xml[boundary])) {
      search = boundary;
      continue;
    }
    const std::size_t open_end = xml.find('>', boundary);
    if (open_end == std::string::npos || open_end >= end) break;
    if (open_end > start && xml[open_end - 1] == '/') {
      search = open_end + 1;
      continue;
    }
    const std::size_t close_start = xml.find(close_tag, open_end + 1);
    if (close_start == std::string::npos || close_start >= end) break;
    result.push_back({start, open_end, open_end + 1, close_start, xml.substr(start, open_end - start + 1)});
    search = close_start + close_tag.size();
  }
  return result;
}

bool hasSelfClosingTag(const std::string& xml, const std::string& tag, std::size_t begin, std::size_t end) {
  const std::string prefix = "<" + tag;
  std::size_t search = begin;
  while (search < end) {
    const std::size_t start = xml.find(prefix, search);
    if (start == std::string::npos || start >= end) return false;
    const std::size_t boundary = start + prefix.size();
    if (boundary >= end || !tagBoundary(xml[boundary])) {
      search = boundary;
      continue;
    }
    const std::size_t close = xml.find('>', boundary);
    if (close == std::string::npos || close >= end) return false;
    if (close > start && xml[close - 1] == '/') return true;
    search = close + 1;
  }
  return false;
}

bool hasElement(const std::string& xml, const std::string& tag, std::size_t begin, std::size_t end) {
  return hasSelfClosingTag(xml, tag, begin, end) || !findBlocks(xml, tag, begin, end).empty();
}

std::string attribute(const std::string& opening, const std::string& name, bool* present = nullptr);

bool hasSelfClosingAttribute(const std::string& xml, const std::string& tag, const std::string& name,
                             const std::string& value, std::size_t begin, std::size_t end) {
  const std::string prefix = "<" + tag;
  std::size_t search = begin;
  while (search < end) {
    const std::size_t start = xml.find(prefix, search);
    if (start == std::string::npos || start >= end) return false;
    const std::size_t boundary = start + prefix.size();
    if (boundary >= end || !tagBoundary(xml[boundary])) {
      search = boundary;
      continue;
    }
    const std::size_t close = xml.find('>', boundary);
    if (close == std::string::npos || close >= end) return false;
    if (attribute(xml.substr(start, close - start + 1), name) == value) return true;
    search = close + 1;
  }
  return false;
}

std::string attribute(const std::string& opening, const std::string& name, bool* present) {
  if (present) *present = false;
  std::size_t position = opening.find_first_of(" \t\r\n/>");
  while (position != std::string::npos && position < opening.size()) {
    while (position < opening.size() && std::isspace(static_cast<unsigned char>(opening[position]))) ++position;
    if (position == opening.size() || opening[position] == '/' || opening[position] == '>') break;
    const std::size_t name_end = opening.find_first_of("= \t\r\n/>", position);
    if (name_end == std::string::npos) throw std::runtime_error("invalid MusicXML attribute");
    const std::string key = opening.substr(position, name_end - position);
    position = name_end;
    while (position < opening.size() && std::isspace(static_cast<unsigned char>(opening[position]))) ++position;
    if (position == opening.size() || opening[position++] != '=') throw std::runtime_error("invalid MusicXML attribute " + key);
    while (position < opening.size() && std::isspace(static_cast<unsigned char>(opening[position]))) ++position;
    if (position == opening.size() || (opening[position] != '\"' && opening[position] != '\'')) {
      throw std::runtime_error("invalid MusicXML attribute " + key);
    }
    const char delimiter = opening[position++];
    const std::size_t end = opening.find(delimiter, position);
    if (end == std::string::npos) throw std::runtime_error("invalid MusicXML attribute " + key);
    if (key == name) {
      if (present) *present = true;
      return opening.substr(position, end - position);
    }
    position = end + 1;
  }
  return {};
}

std::string unescape(std::string value) {
  const std::pair<const char*, const char*> entities[] = {
      {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
  for (const auto& entity : entities) {
    std::size_t position = 0;
    while ((position = value.find(entity.first, position)) != std::string::npos) {
      value.replace(position, std::char_traits<char>::length(entity.first), entity.second);
      position += std::char_traits<char>::length(entity.second);
    }
  }
  return value;
}

void validateEntities(const std::string& xml) {
  const std::string allowed[] = {"&amp;", "&lt;", "&gt;", "&quot;", "&apos;"};
  std::size_t position = 0;
  while ((position = xml.find('&', position)) != std::string::npos) {
    bool known = false;
    for (const std::string& entity : allowed) {
      if (xml.compare(position, entity.size(), entity) == 0) {
        known = true;
        position += entity.size();
        break;
      }
    }
    if (!known) throw std::runtime_error("unsupported MusicXML entity");
  }
}

std::string textIn(const std::string& xml, const std::string& tag, std::size_t begin, std::size_t end,
                   bool required = false) {
  const auto blocks = findBlocks(xml, tag, begin, end);
  if (blocks.empty()) {
    if (required) throw std::runtime_error("MusicXML is missing <" + tag + ">");
    return {};
  }
  return unescape(trim(xml.substr(blocks.front().content_start, blocks.front().content_end - blocks.front().content_start)));
}

std::int64_t integerText(const std::string& value, const std::string& field) {
  if (value.empty()) throw std::runtime_error("MusicXML is missing " + field);
  std::size_t consumed = 0;
  const long long parsed = std::stoll(value, &consumed);
  if (consumed != value.size()) throw std::runtime_error("invalid MusicXML integer in " + field);
  return parsed;
}

double realText(const std::string& value, const std::string& field) {
  if (value.empty()) throw std::runtime_error("MusicXML is missing " + field);
  std::size_t consumed = 0;
  const double parsed = std::stod(value, &consumed);
  if (consumed != value.size() || !std::isfinite(parsed)) throw std::runtime_error("invalid MusicXML number in " + field);
  return parsed;
}

std::uint8_t noteVelocity(const std::string& opening) {
  bool present = false;
  const std::string value = trim(attribute(opening, "dynamics", &present));
  if (!present) return 100;  // Preserve the existing default for older files.

  // MusicXML dynamics is a non-negative decimal percentage: 100 means
  // MIDI velocity 90, not 100. Reject non-decimal and non-finite inputs.
  std::size_t position = 0;
  if (!value.empty() && (value[0] == '+' || value[0] == '-')) ++position;
  bool have_digit = false;
  bool have_point = false;
  for (; position < value.size(); ++position) {
    const char c = value[position];
    if (c >= '0' && c <= '9') have_digit = true;
    else if (c == '.' && !have_point) have_point = true;
    else throw std::runtime_error("invalid MusicXML dynamics decimal");
  }
  if (!have_digit) throw std::runtime_error("invalid MusicXML dynamics decimal");
  std::istringstream input(value);
  input.imbue(std::locale::classic());
  double percent = 0.0;
  if (!(input >> percent) || !std::isfinite(percent) || percent < 0.0 || percent > 127.0 * 100.0 / 90.0) {
    throw std::runtime_error("MusicXML dynamics is outside MIDI velocity 0..127");
  }
  return static_cast<std::uint8_t>(std::lround(percent * 90.0 / 100.0));
}

Tick measureLength(const TimeSignature& signature) {
  if (signature.numerator == 0 || signature.denominator == 0 || (signature.denominator & (signature.denominator - 1)) != 0) {
    throw std::runtime_error("unsupported MusicXML time signature");
  }
  const auto numerator = static_cast<Tick>(signature.numerator);
  const auto denominator = static_cast<Tick>(signature.denominator);
  return numerator * kTicksPerQuarter * 4 / denominator;
}

char validStep(const std::string& value) {
  if (value.size() != 1 || std::string("ABCDEFG").find(value[0]) == std::string::npos) {
    throw std::runtime_error("invalid MusicXML pitch step");
  }
  return value[0];
}

std::string escape(const std::string& value) {
  std::string result;
  for (const char c : value) {
    switch (c) {
      case '&': result += "&amp;"; break;
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '\"': result += "&quot;"; break;
      case '\'': result += "&apos;"; break;
      default: result += c; break;
    }
  }
  return result;
}

void writeNote(std::ostringstream& output, const ScoreNote& note, bool chord, std::uint16_t voice, std::uint16_t staff) {
  if (note.velocity > 127) throw std::invalid_argument("score note velocity is outside MIDI 0..127");
  const auto previous_precision = output.precision();
  output << "      <note dynamics=\"" << std::setprecision(std::numeric_limits<double>::max_digits10)
         << static_cast<double>(note.velocity) * 100.0 / 90.0 << "\">\n";
  output.precision(previous_precision);
  if (chord) output << "        <chord/>\n";
  if (note.rest) {
    output << "        <rest/>\n";
  } else {
    output << "        <pitch><step>" << note.pitch.step << "</step>\n";
    if (note.pitch.alter != 0) output << "          <alter>" << note.pitch.alter << "</alter>\n";
    output << "          <octave>" << note.pitch.octave << "</octave></pitch>\n";
  }
  output << "        <duration>" << note.duration << "</duration>\n";
  if (note.tie_stop) output << "        <tie type=\"stop\"/>\n";
  if (note.tie_start) output << "        <tie type=\"start\"/>\n";
  output << "        <voice>" << voice << "</voice>\n";
  if (note.tuplet_actual != 0 || note.tuplet_normal != 0) {
    if (note.tuplet_actual == 0 || note.tuplet_normal == 0) throw std::invalid_argument("tuplet actual/normal counts must both be set");
    output << "        <time-modification><actual-notes>" << note.tuplet_actual
           << "</actual-notes><normal-notes>" << note.tuplet_normal << "</normal-notes></time-modification>\n";
  }
  if (staff > 1) output << "        <staff>" << staff << "</staff>\n";
  if (note.tie_start || note.tie_stop) {
    output << "        <notations>\n";
    if (note.tie_start) output << "          <tied type=\"start\"/>\n";
    if (note.tie_stop) output << "          <tied type=\"stop\"/>\n";
    output << "        </notations>\n";
  }
  if (!note.lyric.empty()) {
    output << "        <lyric><text>" << escape(note.lyric) << "</text></lyric>\n";
  }
  output << "      </note>\n";
}

}  // namespace

bool writeMusicXmlFile(const Score& score, const std::string& path, std::string* error) {
  try {
    if (score.parts.empty()) throw std::invalid_argument("MusicXML score must contain at least one part");
    if (score.divisions != kTicksPerQuarter) throw std::invalid_argument("score divisions must be 960 ticks per quarter");
    if (!std::isfinite(score.bpm) || score.bpm <= 0.0) throw std::invalid_argument("score tempo must be finite and positive");
    (void)measureLength(score.time_signature);

    std::map<std::string, bool> part_ids;
    for (const ScorePart& part : score.parts) {
      if (part.id.empty()) throw std::invalid_argument("MusicXML part id cannot be empty");
      if (!part_ids.emplace(part.id, true).second) throw std::invalid_argument("MusicXML part ids must be unique");
      if (part.measures.empty()) throw std::invalid_argument("MusicXML part must contain a measure");
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           << "<score-partwise version=\"4.0\">\n"
           << "  <part-list>\n";
    for (const ScorePart& part : score.parts) {
      output << "    <score-part id=\"" << escape(part.id) << "\"><part-name>" << escape(part.name)
             << "</part-name></score-part>\n";
    }
    output << "  </part-list>\n";
    for (const ScorePart& part : score.parts) {
      output << "  <part id=\"" << escape(part.id) << "\">\n";
      for (std::size_t measure_index = 0; measure_index < part.measures.size(); ++measure_index) {
        const ScoreMeasure& measure = part.measures[measure_index];
        if (measure.start < 0) throw std::invalid_argument("score measure start cannot be negative");
        output << "    <measure number=\"" << measure.number << "\">\n";
        if (measure_index == 0) {
          output << "      <attributes><divisions>" << score.divisions << "</divisions><time><beats>"
                 << static_cast<int>(score.time_signature.numerator) << "</beats><beat-type>"
                 << static_cast<int>(score.time_signature.denominator) << "</beat-type></time></attributes>\n"
                 << "      <direction><sound tempo=\"" << score.bpm << "\"/></direction>\n";
        }
        using VoiceKey = std::pair<std::uint16_t, std::uint16_t>;  // staff, voice
        std::map<VoiceKey, std::vector<ScoreNote>> streams;
        for (const ScoreNote& note : measure.notes) streams[{note.staff, note.voice}].push_back(note);
        Tick stream_cursor = measure.start;
        bool first_stream = true;
        for (auto& stream : streams) {
          const VoiceKey key = stream.first;
          if (key.first == 0 || key.second == 0) throw std::invalid_argument("score voice/staff numbers must be positive");
          if (!first_stream && stream_cursor > measure.start) {
            output << "      <backup><duration>" << (stream_cursor - measure.start) << "</duration></backup>\n";
          }
          first_stream = false;
          std::vector<ScoreNote>& notes = stream.second;
          std::stable_sort(notes.begin(), notes.end(), [](const ScoreNote& left, const ScoreNote& right) {
            if (left.start != right.start) return left.start < right.start;
            // MusicXML chord tones cannot outlast the preceding tone. Put
            // the longest note first without changing any sounding duration.
            return left.duration > right.duration;
          });
          Tick cursor = measure.start;
          Tick last_start = std::numeric_limits<Tick>::min();
          for (const ScoreNote& note : notes) {
            if (note.duration <= 0 || note.start < measure.start) throw std::invalid_argument("invalid score note timing");
            if (note.start > std::numeric_limits<Tick>::max() - note.duration) {
              throw std::invalid_argument("score note timing overflows tick range");
            }
            if (!note.rest) {
              (void)validStep(std::string(1, note.pitch.step));
              if (note.pitch.octave < 0 || note.pitch.octave > 9) throw std::invalid_argument("score pitch octave is out of range");
            }
            const bool is_chord = note.start == last_start;
            if (!is_chord && note.start < cursor) {
              // Performance MIDI can contain a held tone under a later attack
              // in the same channel/voice. Move the XML time cursor back rather
              // than truncating that held tone or calling the later note a chord.
              output << "      <backup><duration>" << (cursor - note.start) << "</duration></backup>\n";
            }
            if (!is_chord && note.start > cursor) {
              output << "      <forward><duration>" << (note.start - cursor) << "</duration></forward>\n";
            }
            writeNote(output, note, is_chord, key.second, key.first);
            if (!is_chord) cursor = note.start + note.duration;
            last_start = note.start;
          }
          stream_cursor = cursor;
        }
        output << "    </measure>\n";
      }
      output << "  </part>\n";
    }
    output << "</score-partwise>\n";

    std::ofstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open MusicXML output file");
    const std::string content = output.str();
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file) throw std::runtime_error("failed while writing MusicXML file");
    return true;
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}

bool readMusicXmlFile(const std::string& path, Score* score, std::string* error) {
  if (score == nullptr) {
    if (error) *error = "score output pointer is null";
    return false;
  }
  try {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open MusicXML file");
    const std::string xml((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (xml.size() > 16 * 1024 * 1024) throw std::runtime_error("MusicXML file exceeds Alpha size limit");
    if (xml.find('\0') != std::string::npos || xml.find("<!--") != std::string::npos || xml.find("<![CDATA[") != std::string::npos ||
        xml.find("<!DOCTYPE") != std::string::npos) {
      throw std::runtime_error("MusicXML comments, CDATA, and external entities are unsupported in Alpha");
    }
    validateEntities(xml);
    std::size_t processing_instruction = xml.find("<?");
    while (processing_instruction != std::string::npos) {
      const std::size_t declaration_end = processing_instruction + 5;
      if (xml.compare(processing_instruction, 5, "<?xml") != 0 ||
          (declaration_end < xml.size() && !std::isspace(static_cast<unsigned char>(xml[declaration_end])) && xml[declaration_end] != '?')) {
        throw std::runtime_error("MusicXML processing instructions are unsupported in Alpha");
      }
      processing_instruction = xml.find("<?", processing_instruction + 2);
    }
    const std::size_t root_open = xml.find("<score-partwise");
    const std::size_t root_close = xml.find("</score-partwise>");
    const std::size_t root_boundary = root_open == std::string::npos ? std::string::npos : root_open + std::string("<score-partwise").size();
    if (root_open == std::string::npos || root_close == std::string::npos || root_boundary >= xml.size() ||
        !tagBoundary(xml[root_boundary]) ||
        root_close != xml.rfind("</score-partwise>")) {
      throw std::runtime_error("unsupported or truncated MusicXML root");
    }
    const std::string prefix = trim(xml.substr(0, root_open));
    if (!prefix.empty()) {
      if (prefix.rfind("<?xml", 0) != 0) throw std::runtime_error("unexpected data before MusicXML root");
      const std::size_t declaration_close = prefix.find("?>");
      if (declaration_close == std::string::npos || !trim(prefix.substr(declaration_close + 2)).empty()) {
        throw std::runtime_error("unexpected data before MusicXML root");
      }
    }
    if (!trim(xml.substr(root_close + std::string("</score-partwise>").size())).empty()) {
      throw std::runtime_error("unexpected data after MusicXML root");
    }

    Score parsed;
    const auto part_list = findBlocks(xml, "part-list", 0, xml.size());
    if (part_list.size() != 1) throw std::runtime_error("MusicXML requires exactly one part-list");
    const auto score_parts = findBlocks(xml, "score-part", part_list.front().content_start, part_list.front().content_end);
    if (score_parts.empty()) throw std::runtime_error("MusicXML part-list has no score-part entries");
    std::map<std::string, std::string> part_names;
    for (const XmlBlock& score_part : score_parts) {
      const std::string id = attribute(score_part.opening, "id");
      if (id.empty()) throw std::runtime_error("MusicXML score-part id cannot be empty");
      if (!part_names.emplace(id, textIn(xml, "part-name", score_part.content_start, score_part.content_end, true)).second) {
        throw std::runtime_error("MusicXML score-part ids must be unique");
      }
    }
    const auto parts = findBlocks(xml, "part", 0, xml.size());
    if (parts.empty()) throw std::runtime_error("MusicXML has no part elements");
    if (parts.size() != part_names.size()) throw std::runtime_error("MusicXML part and part-list counts differ");
    bool have_divisions = false;
    bool have_meter = false;
    bool have_tempo = false;

    auto parse_part = [&](const XmlBlock& part_block, const std::string& part_name) -> ScorePart {
      ScorePart parsed_part;
      parsed_part.id = attribute(part_block.opening, "id");
      parsed_part.name = part_name.empty() ? parsed_part.id : part_name;
      Tick measure_start = 0;
      const auto measures = findBlocks(xml, "measure", part_block.content_start, part_block.content_end);
      if (measures.empty()) throw std::runtime_error("MusicXML part has no measures");
      for (const XmlBlock& measure_block : measures) {
      ScoreMeasure measure;
      const std::string number = attribute(measure_block.opening, "number");
      measure.number = number.empty() ? static_cast<int>(parsed_part.measures.size() + 1) : static_cast<int>(integerText(number, "measure number"));
      measure.start = measure_start;
      const auto attributes = findBlocks(xml, "attributes", measure_block.content_start, measure_block.content_end);
      for (const XmlBlock& attribute_block : attributes) {
        const std::string divisions = textIn(xml, "divisions", attribute_block.content_start, attribute_block.content_end);
        if (!divisions.empty()) {
          if (integerText(divisions, "divisions") != kTicksPerQuarter) throw std::runtime_error("MusicXML divisions must be 960 in Alpha");
          if (have_divisions && parsed.divisions != kTicksPerQuarter) throw std::runtime_error("conflicting MusicXML divisions");
          have_divisions = true;
          parsed.divisions = kTicksPerQuarter;
        }
        const auto times = findBlocks(xml, "time", attribute_block.content_start, attribute_block.content_end);
        for (const XmlBlock& time : times) {
          const auto beats = integerText(textIn(xml, "beats", time.content_start, time.content_end, true), "beats");
          const auto beat_type = integerText(textIn(xml, "beat-type", time.content_start, time.content_end, true), "beat-type");
          if (beats < 1 || beats > 255 || beat_type < 1 || beat_type > 255) throw std::runtime_error("invalid MusicXML time signature");
          const TimeSignature meter{static_cast<std::uint8_t>(beats), static_cast<std::uint8_t>(beat_type)};
          if (have_meter && (parsed.time_signature.numerator != meter.numerator || parsed.time_signature.denominator != meter.denominator)) {
            throw std::runtime_error("conflicting MusicXML time signatures");
          }
          have_meter = true;
          parsed.time_signature = meter;
        }
      }
      const auto directions = findBlocks(xml, "direction", measure_block.content_start, measure_block.content_end);
      for (const XmlBlock& direction : directions) {
        std::size_t sound_search = direction.content_start;
        while (true) {
          const std::size_t sound_start = xml.find("<sound", sound_search);
          if (sound_start == std::string::npos || sound_start >= direction.content_end) break;
          if (sound_start + 6 >= direction.content_end || !tagBoundary(xml[sound_start + 6])) {
            sound_search = sound_start + 6;
            continue;
          }
          const std::size_t sound_end = xml.find('>', sound_start);
          if (sound_end == std::string::npos || sound_end >= direction.content_end) {
            throw std::runtime_error("truncated MusicXML sound element");
          }
          const std::string tempo = attribute(xml.substr(sound_start, sound_end - sound_start + 1), "tempo");
          if (!tempo.empty()) {
            const double bpm = realText(tempo, "tempo");
            if (bpm <= 0.0) throw std::runtime_error("MusicXML tempo must be positive");
            if (have_tempo && std::abs(parsed.bpm - bpm) > 1e-9) throw std::runtime_error("conflicting MusicXML tempos");
            have_tempo = true;
            parsed.bpm = bpm;
          }
          sound_search = sound_end + 1;
        }
      }

      struct TimedBlock {
        std::size_t position;
        std::string tag;
        XmlBlock block;
      };
      std::vector<TimedBlock> events;
      for (const auto& note : findBlocks(xml, "note", measure_block.content_start, measure_block.content_end)) events.push_back({note.start, "note", note});
      for (const auto& backup : findBlocks(xml, "backup", measure_block.content_start, measure_block.content_end)) events.push_back({backup.start, "backup", backup});
      for (const auto& forward : findBlocks(xml, "forward", measure_block.content_start, measure_block.content_end)) events.push_back({forward.start, "forward", forward});
      std::sort(events.begin(), events.end(), [](const TimedBlock& left, const TimedBlock& right) { return left.position < right.position; });
      Tick cursor = 0;
      Tick furthest_position = 0;
      using VoiceKey = std::pair<std::uint16_t, std::uint16_t>;  // staff, voice
      std::map<VoiceKey, Tick> last_note_starts;
      std::map<VoiceKey, bool> have_notes;
      for (const TimedBlock& event : events) {
        const Tick duration = integerText(textIn(xml, "duration", event.block.content_start, event.block.content_end, true), "duration");
        if (duration <= 0) throw std::runtime_error("MusicXML duration must be positive");
        if (event.tag == "backup") {
          if (duration > cursor) throw std::runtime_error("MusicXML backup crosses measure start");
          cursor -= duration;
          continue;
        }
        if (event.tag == "forward") {
          if (cursor > std::numeric_limits<Tick>::max() - duration) throw std::runtime_error("MusicXML tick overflow");
          cursor += duration;
          furthest_position = std::max(furthest_position, cursor);
          continue;
        }
        ScoreNote note;
        note.velocity = noteVelocity(event.block.opening);
        note.duration = duration;
        note.chord = hasElement(xml, "chord", event.block.content_start, event.block.content_end);
        note.rest = hasElement(xml, "rest", event.block.content_start, event.block.content_end);
        if (note.chord && note.rest) throw std::runtime_error("MusicXML rest cannot be a chord");
        const std::string voice = textIn(xml, "voice", event.block.content_start, event.block.content_end);
        const std::string staff = textIn(xml, "staff", event.block.content_start, event.block.content_end);
        const auto voice_number = voice.empty() ? 1 : integerText(voice, "voice");
        const auto staff_number = staff.empty() ? 1 : integerText(staff, "staff");
        if (voice_number <= 0 || voice_number > std::numeric_limits<std::uint16_t>::max() ||
            staff_number <= 0 || staff_number > std::numeric_limits<std::uint16_t>::max()) {
          throw std::runtime_error("MusicXML voice/staff number is out of range");
        }
        note.voice = static_cast<std::uint16_t>(voice_number);
        note.staff = static_cast<std::uint16_t>(staff_number);
        const VoiceKey key{note.staff, note.voice};
        if (note.chord && !have_notes[key]) throw std::runtime_error("MusicXML chord cannot be the first note in a voice");
        const Tick local_start = note.chord ? last_note_starts[key] : cursor;
        if (local_start < 0 || measure_start > std::numeric_limits<Tick>::max() - local_start) {
          throw std::runtime_error("MusicXML tick overflow");
        }
        note.start = measure_start + local_start;
        if (local_start > std::numeric_limits<Tick>::max() - duration) {
          throw std::runtime_error("MusicXML tick overflow");
        }
        furthest_position = std::max(furthest_position, local_start + duration);
        if (!note.rest) {
          const auto pitches = findBlocks(xml, "pitch", event.block.content_start, event.block.content_end);
          if (pitches.empty()) throw std::runtime_error("MusicXML note is missing pitch");
          note.pitch.step = validStep(textIn(xml, "step", pitches.front().content_start, pitches.front().content_end, true));
          const std::string alter = textIn(xml, "alter", pitches.front().content_start, pitches.front().content_end);
          if (alter.find('.') != std::string::npos) throw std::runtime_error("fractional MusicXML alter is unsupported in Alpha");
          note.pitch.alter = alter.empty() ? 0 : static_cast<int>(integerText(alter, "alter"));
          note.pitch.octave = static_cast<int>(integerText(textIn(xml, "octave", pitches.front().content_start, pitches.front().content_end, true), "octave"));
        }
        const auto time_modifications = findBlocks(xml, "time-modification", event.block.content_start, event.block.content_end);
        if (!time_modifications.empty()) {
          const auto actual = integerText(textIn(xml, "actual-notes", time_modifications.front().content_start,
                                                 time_modifications.front().content_end, true), "actual-notes");
          const auto normal = integerText(textIn(xml, "normal-notes", time_modifications.front().content_start,
                                                 time_modifications.front().content_end, true), "normal-notes");
          if (actual <= 0 || actual > std::numeric_limits<std::uint16_t>::max() || normal <= 0 ||
              normal > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("MusicXML tuplet count is out of range");
          }
          note.tuplet_actual = static_cast<std::uint16_t>(actual);
          note.tuplet_normal = static_cast<std::uint16_t>(normal);
        }
        note.tie_start = hasSelfClosingAttribute(xml, "tie", "type", "start", event.block.content_start, event.block.content_end) ||
                          hasSelfClosingAttribute(xml, "tied", "type", "start", event.block.content_start, event.block.content_end);
        note.tie_stop = hasSelfClosingAttribute(xml, "tie", "type", "stop", event.block.content_start, event.block.content_end) ||
                         hasSelfClosingAttribute(xml, "tied", "type", "stop", event.block.content_start, event.block.content_end);
        const auto lyrics = findBlocks(xml, "lyric", event.block.content_start, event.block.content_end);
        if (lyrics.size() > 1) throw std::runtime_error("MusicXML notes may contain only one lyric in Alpha");
        if (!lyrics.empty()) {
          const auto text_blocks = findBlocks(xml, "text", lyrics.front().content_start, lyrics.front().content_end);
          if (text_blocks.size() != 1) throw std::runtime_error("MusicXML lyric must contain one text element");
          note.lyric = textIn(xml, "text", lyrics.front().content_start, lyrics.front().content_end, true);
        }
        measure.notes.push_back(note);
        have_notes[key] = true;
        if (!note.chord) last_note_starts[key] = cursor;
        if (!note.chord) {
          if (cursor > std::numeric_limits<Tick>::max() - duration) throw std::runtime_error("MusicXML tick overflow");
          cursor += duration;
        }
      }
      parsed_part.measures.push_back(std::move(measure));
      const Tick nominal_length = measureLength(parsed.time_signature);
      const Tick measure_span = std::max(nominal_length, furthest_position);
      if (measure_span < 0 || measure_start > std::numeric_limits<Tick>::max() - measure_span) {
        throw std::runtime_error("MusicXML measure timing overflow");
      }
      measure_start += measure_span;
      }
      return parsed_part;
    };

    std::map<std::string, bool> seen_parts;
    for (const XmlBlock& part_block : parts) {
      const std::string id = attribute(part_block.opening, "id");
      if (id.empty()) throw std::runtime_error("MusicXML part id cannot be empty");
      const auto name = part_names.find(id);
      if (name == part_names.end()) throw std::runtime_error("MusicXML part id is missing from part-list");
      if (!seen_parts.emplace(id, true).second) throw std::runtime_error("MusicXML part ids must be unique");
      parsed.parts.push_back(parse_part(part_block, name->second));
    }
    if (seen_parts.size() != part_names.size()) throw std::runtime_error("MusicXML part-list contains an unreferenced score-part");
    *score = std::move(parsed);
    return true;
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}

}  // namespace daw
