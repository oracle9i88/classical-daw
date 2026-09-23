#include "daw/score.hpp"
#include "daw/meter_map.hpp"

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

// MusicXML's tempo is an XML Schema decimal, which has no exponent notation.
// Expand the round-trippable double spelling instead of fixed precision that
// could turn a small, positive tempo into zero.
std::string tempoDecimal(double bpm) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setprecision(std::numeric_limits<double>::max_digits10) << bpm;
  std::string value = output.str();
  const auto exponent_at = value.find_first_of("eE");
  if (exponent_at == std::string::npos) return value;
  const int exponent = std::stoi(value.substr(exponent_at + 1));
  std::string digits = value.substr(0, exponent_at);
  const auto dot = digits.find('.');
  int decimal_position = static_cast<int>(dot == std::string::npos ? digits.size() : dot);
  if (dot != std::string::npos) digits.erase(dot, 1);
  decimal_position += exponent;
  if (decimal_position <= 0) return "0." + std::string(static_cast<std::size_t>(-decimal_position), '0') + digits;
  const auto position = static_cast<std::size_t>(decimal_position);
  if (position >= digits.size()) return digits + std::string(position - digits.size(), '0');
  digits.insert(position, 1, '.');
  return digits;
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

// Only immediate children belong to this time cursor. In particular, a sound
// inside a direction is not another measure event, and its offset is not the
// direction's offset. Include self-closing sound/beat-unit-dot elements.
std::vector<XmlBlock> childElements(const std::string& xml, std::size_t begin, std::size_t end) {
  std::vector<XmlBlock> result;
  std::vector<std::string> stack;
  XmlBlock current;
  for (auto position = xml.find('<', begin); position < end; position = xml.find('<', position + 1)) {
    const auto close = xml.find('>', position);
    if (close == std::string::npos || close >= end) throw std::runtime_error("truncated MusicXML element");
    const bool closing = xml[position + 1] == '/';
    const auto name_begin = position + (closing ? 2 : 1);
    const auto name_end = xml.find_first_of(" \t\r\n/>", name_begin);
    const auto name = xml.substr(name_begin, name_end - name_begin);
    if (closing) {
      if (stack.empty() || stack.back() != name) throw std::runtime_error("mismatched MusicXML element");
      stack.pop_back();
      if (stack.empty()) {
        current.content_end = position;
        result.push_back(current);
      }
    } else {
      if (stack.empty()) current = {position, close, close + 1, close + 1, xml.substr(position, close - position + 1)};
      if (xml[close - 1] == '/') {
        if (stack.empty()) result.push_back(current);
      } else stack.push_back(name);
    }
    position = close;
  }
  if (!stack.empty()) throw std::runtime_error("unclosed MusicXML element");
  return result;
}

std::string elementName(const XmlBlock& block) {
  return block.opening.substr(1, block.opening.find_first_of(" \t\r\n/>", 1) - 1);
}

std::string decimalText(std::string value, const std::string& field) {
  value = trim(value);
  bool digit = false, point = false;
  for (std::size_t i = (!value.empty() && (value[0] == '+' || value[0] == '-')) ? 1 : 0; i < value.size(); ++i) {
    if (value[i] >= '0' && value[i] <= '9') digit = true;
    else if (value[i] == '.' && !point) point = true;
    else throw std::runtime_error("invalid MusicXML decimal in " + field);
  }
  if (!digit) throw std::runtime_error("missing MusicXML decimal in " + field);
  return value;
}

double tempoValue(const std::string& value) {
  std::istringstream input(decimalText(value, "tempo"));
  input.imbue(std::locale::classic());
  double bpm = 0;
  if (!(input >> bpm) || !std::isfinite(bpm) || bpm <= 0 || bpm > 1000000) {
    throw std::runtime_error("MusicXML tempo must be positive and at most 1000000 quarter notes per minute");
  }
  return bpm;
}

Tick tempoOffset(const std::string& xml, const XmlBlock& offset) {
  auto value = decimalText(xml.substr(offset.content_start, offset.content_end - offset.content_start), "tempo offset");
  const auto point = value.find('.');
  if (point != std::string::npos) {
    if (value.find_first_not_of('0', point + 1) != std::string::npos) {
      throw std::runtime_error("fractional MusicXML tempo offsets are unsupported at 960 divisions");
    }
    value.erase(point);
  }
  if (value.empty() || value == "+" || value == "-") value += '0';
  return integerText(value, "tempo offset");
}

Tick playbackOffset(const std::string& xml, const XmlBlock& parent, bool direction) {
  Tick result = 0;
  bool seen = false;
  for (const auto& child : childElements(xml, parent.content_start, parent.content_end)) {
    if (elementName(child) != "offset") continue;
    if (seen) throw std::runtime_error("multiple MusicXML tempo offsets");
    seen = true;
    bool present = false;
    const auto sound = attribute(child.opening, "sound", &present);
    if (present && sound != "yes" && sound != "no") throw std::runtime_error("invalid MusicXML offset sound flag");
    if (!direction || sound == "yes") result = tempoOffset(xml, child);
    else (void)decimalText(xml.substr(child.content_start, child.content_end - child.content_start), "visual offset");
  }
  return result;
}

// Simple metronome marks are a fallback when there is no authoritative sound
// tempo. Metric modulation, ranges and textual per-minute values need a richer
// model; refusing them avoids inventing a constant playback speed.
double metronomeTempo(const std::string& xml, const XmlBlock& metronome) {
  double unit = 0, per_minute = 0;
  int dots = 0;
  const std::map<std::string, double> units{{"maxima",32}, {"long",16}, {"breve",8}, {"whole",4},
      {"half",2}, {"quarter",1}, {"eighth",0.5}, {"16th",0.25}, {"32nd",0.125},
      {"64th",0.0625}, {"128th",0.03125}, {"256th",0.015625}, {"512th",0.0078125}, {"1024th",0.00390625}};
  for (const auto& child : childElements(xml, metronome.content_start, metronome.content_end)) {
    const auto name = elementName(child);
    const auto value = trim(xml.substr(child.content_start, child.content_end - child.content_start));
    if (name == "beat-unit" && unit == 0 && per_minute == 0) {
      const auto found = units.find(value);
      if (found == units.end()) throw std::runtime_error("unsupported MusicXML metronome beat unit");
      unit = found->second;
    } else if (name == "beat-unit-dot" && unit != 0 && per_minute == 0 && dots < 3) ++dots;
    else if (name == "per-minute" && unit != 0 && per_minute == 0) per_minute = tempoValue(value);
    else throw std::runtime_error("unsupported MusicXML metronome structure or metric modulation");
  }
  const double bpm = per_minute * unit * (2.0 - std::ldexp(1.0, -dots));
  if (!std::isfinite(bpm) || bpm <= 0 || bpm > 1000000) throw std::runtime_error("unsupported MusicXML metronome tempo");
  return bpm;
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

bool sameNotationMeter(const TimeSignature& a, const TimeSignature& b) {
  return a.numerator == b.numerator && a.denominator == b.denominator;
}

using NotationMeters = std::vector<TimeSignatureChange>;  // Includes tick zero.

TimeSignature notationMeterAt(const NotationMeters& meters, Tick tick) {
  const auto after = std::upper_bound(meters.begin(), meters.end(), tick,
      [](Tick position, const TimeSignatureChange& change) { return position < change.tick; });
  return (after - 1)->signature;
}

NotationMeters notationMeters(const Score& score, MusicXmlExportReport& report) {
  validateMeterMap(score.time_signature, score.meter_changes);
  if (score.time_signature.notated_32nds_per_quarter != 8) {
    throw std::invalid_argument("MusicXML export does not support nonstandard notation ratios");
  }
  NotationMeters result{{0, {score.time_signature.numerator, score.time_signature.denominator}}};
  report.omitted_meter_playback_metadata = score.time_signature.clocks_per_click != 24 ? 1 : 0;
  auto previous = score.time_signature;
  for (const auto& change : score.meter_changes) {
    if (change.signature.notated_32nds_per_quarter != 8) {
      throw std::invalid_argument("MusicXML export does not support nonstandard notation ratios");
    }
    const bool repeated = sameNotationMeter(previous, change.signature);
    if (repeated || change.signature.clocks_per_click != 24) ++report.omitted_meter_playback_metadata;
    if (!repeated) result.push_back({change.tick, {change.signature.numerator, change.signature.denominator}});
    previous = change.signature;
  }
  return result;
}

// Score has one global meter map. Independent or conflicting part timelines
// cannot be folded into it without guessing. A shorter part may be a prefix.
std::size_t synchronousReference(const std::vector<ScorePart>& parts, const std::vector<Tick>& ends) {
  const auto longest = static_cast<std::size_t>(std::max_element(ends.begin(), ends.end()) - ends.begin());
  const auto& reference = parts[longest].measures;
  for (std::size_t p = 0; p < parts.size(); ++p) {
    const auto& measures = parts[p].measures;
    for (std::size_t m = 0; m < measures.size(); ++m) {
      if (m >= reference.size() || measures[m].start != reference[m].start) {
        throw std::invalid_argument("MusicXML parts require synchronized measure starts in this score model");
      }
    }
    if (measures.size() < reference.size() && reference[measures.size()].start < ends[p]) {
      throw std::invalid_argument("MusicXML parts have conflicting measure spans");
    }
  }
  return longest;
}

std::vector<std::vector<Tick>> exportMeasureSpans(const Score& score, const NotationMeters& meters) {
  std::vector<std::vector<Tick>> all_spans;
  std::vector<Tick> ends;
  const auto& reference_bars = std::max_element(score.parts.begin(), score.parts.end(),
      [](const ScorePart& a, const ScorePart& b) { return a.measures.size() < b.measures.size(); })->measures;
  for (const auto& part : score.parts) {
    if (part.measures.empty() || part.measures.front().start != 0) {
      throw std::invalid_argument("MusicXML parts require a first measure at tick zero");
    }
    std::vector<Tick> spans;
    for (std::size_t m = 0; m < part.measures.size(); ++m) {
      const auto& measure = part.measures[m];
      if (measure.number <= 0 || measure.start < 0 || measure.duration < 0 ||
          measure.start > std::numeric_limits<Tick>::max() - measure.duration) {
        throw std::invalid_argument("MusicXML measure number, start or duration is invalid");
      }
      Tick furthest = 0;
      for (const auto& note : measure.notes) {
        if (note.start < measure.start || note.duration <= 0 ||
            note.start > std::numeric_limits<Tick>::max() - note.duration) {
          throw std::invalid_argument("invalid MusicXML score note timing");
        }
        furthest = std::max(furthest, note.start + note.duration - measure.start);
      }
      Tick span = measure.duration > 0 ? measure.duration
                                      : std::max(measureLength(notationMeterAt(meters, measure.start)), furthest);
      if (furthest > span) throw std::invalid_argument("MusicXML note exceeds the explicit measure duration");
      if (m + 1 < part.measures.size()) {
        const Tick next = part.measures[m + 1].start;
        if (next <= measure.start) throw std::invalid_argument("MusicXML measure starts must strictly increase");
        if (measure.duration > 0 && measure.duration != next - measure.start) {
          throw std::invalid_argument("MusicXML explicit measure duration conflicts with its next boundary");
        }
        span = next - measure.start;
        if (furthest > span) throw std::invalid_argument("MusicXML note crosses its stored measure boundary; split it into ties first");
      } else if (measure.duration == 0 && m + 1 < reference_bars.size()) {
        // A short or event-only part still shares the other parts' barline.
        // Its final partial bar has no stored end; recover that boundary
        // from the existing longer bar sequence rather than padding to an
        // unrelated nominal length through the following meter change.
        const Tick next = reference_bars[m + 1].start;
        if (next <= measure.start || furthest > next - measure.start) {
          throw std::invalid_argument("MusicXML shorter part crosses a shared measure boundary");
        }
        span = next - measure.start;
      }
      if (measure.start > std::numeric_limits<Tick>::max() - span) {
        throw std::invalid_argument("MusicXML measure timing overflow");
      }
      const auto next_meter = std::upper_bound(meters.begin(), meters.end(), measure.start,
          [](Tick tick, const TimeSignatureChange& change) { return tick < change.tick; });
      if (next_meter != meters.end() && next_meter->tick < measure.start + span) {
        throw std::invalid_argument("MusicXML meter change must fall on a stored measure boundary; rebar the score first");
      }
      spans.push_back(span);
    }
    ends.push_back(part.measures.back().start + spans.back());
    all_spans.push_back(std::move(spans));
  }
  const auto reference = synchronousReference(score.parts, ends);
  const auto& measures = score.parts[reference].measures;
  for (const auto& change : meters) {
    const auto found = std::lower_bound(measures.begin(), measures.end(), change.tick,
        [](const ScoreMeasure& measure, Tick tick) { return measure.start < tick; });
    if (found == measures.end() || found->start != change.tick) {
      throw std::invalid_argument("MusicXML meter change is outside the stored measure boundaries");
    }
  }
  return all_spans;
}

void requireSharedMeters(const std::vector<NotationMeters>& maps, const std::vector<Tick>& ends,
                         std::size_t reference) {
  const auto& expected = maps[reference];
  for (std::size_t p = 0; p < maps.size(); ++p) {
    std::size_t a = 0, b = 0;
    for (;;) {
      if (!sameNotationMeter(maps[p][a].signature, expected[b].signature)) {
        throw std::runtime_error("conflicting MusicXML part time signatures; polymeter is unsupported");
      }
      const Tick next_a = a + 1 < maps[p].size() ? maps[p][a + 1].tick : std::numeric_limits<Tick>::max();
      const Tick next_b = b + 1 < expected.size() ? expected[b + 1].tick : std::numeric_limits<Tick>::max();
      const Tick next = std::min(next_a, next_b);
      if (next >= ends[p]) break;
      if (next == next_a) ++a;
      if (next == next_b) ++b;
    }
  }
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

bool writeMusicXmlFile(const Score& score, const std::string& path, std::string* error, MusicXmlExportReport* report) {
  try {
    if (score.parts.empty()) throw std::invalid_argument("MusicXML score must contain at least one part");
    if (score.divisions != kTicksPerQuarter) throw std::invalid_argument("score divisions must be 960 ticks per quarter");
    const auto tempos = scoreTempoMap(score);
    MusicXmlExportReport omissions;
    const auto meters = notationMeters(score, omissions);
    const auto measure_spans = exportMeasureSpans(score, meters);
    std::map<std::string, bool> part_ids;
    std::vector<Tick> part_ends;
    for (std::size_t p = 0; p < score.parts.size(); ++p) {
      part_ends.push_back(score.parts[p].measures.back().start + measure_spans[p].back());
    }
    const auto tempo_part = synchronousReference(score.parts, part_ends);
    if (tempos.changes().back().tick > part_ends[tempo_part]) {
      throw std::invalid_argument("MusicXML tempo change is outside the stored measure extent");
    }
    for (const ScorePart& part : score.parts) {
      if (part.id.empty()) throw std::invalid_argument("MusicXML part id cannot be empty");
      if (!part_ids.emplace(part.id, true).second) throw std::invalid_argument("MusicXML part ids must be unique");
      if (part.measures.empty()) throw std::invalid_argument("MusicXML part must contain a measure");
      omissions.omitted_midi_events += part.midi_events.size();
      for (const auto& measure : part.measures) for (const auto& note : measure.notes) {
        if (note.midi_channel != -1 || note.midi_on_order != 0 || note.midi_off_order != 0 ||
            note.midi_release_velocity != 0) ++omissions.omitted_note_midi_metadata;
      }
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output.precision(std::numeric_limits<double>::max_digits10);
    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           << "<score-partwise version=\"4.0\">\n"
           << "  <part-list>\n";
    for (const ScorePart& part : score.parts) {
      output << "    <score-part id=\"" << escape(part.id) << "\"><part-name>" << escape(part.name)
             << "</part-name></score-part>\n";
    }
    output << "  </part-list>\n";
    for (std::size_t part_index = 0; part_index < score.parts.size(); ++part_index) {
      const ScorePart& part = score.parts[part_index];
      output << "  <part id=\"" << escape(part.id) << "\">\n";
      for (std::size_t measure_index = 0; measure_index < part.measures.size(); ++measure_index) {
        const ScoreMeasure& measure = part.measures[measure_index];
        const auto meter = notationMeterAt(meters, measure.start);
        const Tick span = measure_spans[part_index][measure_index];
        output << "    <measure number=\"" << measure.number << '\"';
        if (span < measureLength(meter)) output << " implicit=\"yes\"";
        output << ">\n";
        const auto at_start = std::lower_bound(meters.begin(), meters.end(), measure.start,
            [](const TimeSignatureChange& change, Tick tick) { return change.tick < tick; });
        if (measure_index == 0 || (at_start != meters.end() && at_start->tick == measure.start)) {
          output << "      <attributes>";
          if (measure_index == 0) output << "<divisions>" << score.divisions << "</divisions>";
          output << "<time><beats>" << static_cast<int>(meter.numerator) << "</beats><beat-type>"
                 << static_cast<int>(meter.denominator) << "</beat-type></time></attributes>\n";
        }
        // Write the global map once, in the longest part. Playback offsets
        // place changes inside held notes without splitting/retriggering them.
        if (part_index == tempo_part) {
          auto tempo = std::lower_bound(tempos.changes().begin(), tempos.changes().end(), measure.start,
              [](const TempoChange& change, Tick tick) { return change.tick < tick; });
          const Tick end = measure.start + span;
          const bool last_measure = measure_index + 1 == part.measures.size();
          for (; tempo != tempos.changes().end() && (tempo->tick < end || (last_measure && tempo->tick == end)); ++tempo) {
            output << "      <direction><direction-type><metronome><beat-unit>quarter</beat-unit><per-minute>"
                   << tempoDecimal(tempo->bpm) << "</per-minute></metronome></direction-type><offset sound=\"yes\">"
                   << tempo->tick - measure.start << "</offset><sound tempo=\""
                   << tempoDecimal(tempo->bpm) << "\"/></direction>\n";
          }
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
        // A partial/empty measure has no duration attribute. Advance to the
        // stored boundary explicitly, including silence after its last voice.
        const Tick measure_end = measure.start + span;
        if (stream_cursor < measure_end) {
          output << "      <forward><duration>" << (measure_end - stream_cursor) << "</duration></forward>\n";
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
    if (report) *report = omissions;
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
    std::map<Tick, double> global_tempos;
    std::vector<NotationMeters> part_meters;
    std::vector<Tick> part_ends;

    auto parse_part = [&](const XmlBlock& part_block, const std::string& part_name) -> ScorePart {
      ScorePart parsed_part;
      parsed_part.id = attribute(part_block.opening, "id");
      parsed_part.name = part_name.empty() ? parsed_part.id : part_name;
      Tick measure_start = 0;
      TimeSignature active_meter{};
      NotationMeters local_meters{{0, active_meter}};
      const auto measures = findBlocks(xml, "measure", part_block.content_start, part_block.content_end);
      if (measures.empty()) throw std::runtime_error("MusicXML part has no measures");
      for (const XmlBlock& measure_block : measures) {
      ScoreMeasure measure;
      const std::string number = attribute(measure_block.opening, "number");
      const auto numeric_number = number.empty() ? static_cast<std::int64_t>(parsed_part.measures.size() + 1)
                                                  : integerText(number, "measure number");
      if (numeric_number <= 0 || numeric_number > std::numeric_limits<int>::max()) {
        throw std::runtime_error("MusicXML measure numbers must be positive integers in this score model");
      }
      measure.number = static_cast<int>(numeric_number);
      measure.start = measure_start;
      bool implicit_present = false, noncontrolling_present = false;
      const auto implicit = attribute(measure_block.opening, "implicit", &implicit_present);
      const auto noncontrolling = attribute(measure_block.opening, "non-controlling", &noncontrolling_present);
      if (implicit_present && implicit != "yes" && implicit != "no") {
        throw std::runtime_error("invalid MusicXML implicit measure flag");
      }
      if (noncontrolling_present && noncontrolling != "no") {
        throw std::runtime_error("MusicXML non-controlling measures are unsupported by the global score timeline");
      }
      struct TimedBlock {
        std::size_t position;
        std::string tag;
        XmlBlock block;
      };
      std::vector<TimedBlock> events;
      for (const auto& child : childElements(xml, measure_block.content_start, measure_block.content_end)) {
        const auto name = elementName(child);
        if (name == "attributes" || name == "note" || name == "backup" || name == "forward" ||
            name == "direction" || name == "sound") events.push_back({child.start, name, child});
      }
      Tick cursor = 0;
      Tick furthest_position = 0;
      Tick furthest_tempo = 0;
      using VoiceKey = std::pair<std::uint16_t, std::uint16_t>;  // staff, voice
      std::map<VoiceKey, Tick> last_note_starts;
      std::map<VoiceKey, bool> have_notes;
      bool have_timed_event = false;
      bool have_measure_meter = false;
      for (const TimedBlock& event : events) {
        if (event.tag == "attributes") {
          const auto& block = event.block;
          const auto divisions = textIn(xml, "divisions", block.content_start, block.content_end);
          if (!divisions.empty()) {
            if (integerText(divisions, "divisions") != kTicksPerQuarter) {
              throw std::runtime_error("MusicXML divisions must be 960 in Alpha");
            }
            if (have_divisions && parsed.divisions != kTicksPerQuarter) {
              throw std::runtime_error("conflicting MusicXML divisions");
            }
            have_divisions = true;
            parsed.divisions = kTicksPerQuarter;
          }
          const auto times = findBlocks(xml, "time", block.content_start, block.content_end);
          if (hasSelfClosingTag(xml, "time", block.content_start, block.content_end) || times.size() > 1) {
            throw std::runtime_error("unsupported MusicXML time signature structure");
          }
          if (!times.empty()) {
            // MusicXML allows later attributes, including after a backup.
            // This slice supports leading time declarations only, and must
            // not retroactively apply a later declaration to earlier notes.
            if (have_timed_event || cursor != 0) {
              throw std::runtime_error("MusicXML mid-measure time declarations are unsupported; start a new measure");
            }
            const auto& time = times.front();
            bool numbered = false;
            (void)attribute(time.opening, "number", &numbered);
            if (numbered || findBlocks(xml, "beats", time.content_start, time.content_end).size() != 1 ||
                findBlocks(xml, "beat-type", time.content_start, time.content_end).size() != 1 ||
                hasElement(xml, "interchangeable", time.content_start, time.content_end)) {
              throw std::runtime_error("MusicXML staff-specific or composite time signatures are unsupported");
            }
            const auto beats = integerText(textIn(xml, "beats", time.content_start, time.content_end, true), "beats");
            const auto beat_type = integerText(textIn(xml, "beat-type", time.content_start, time.content_end, true), "beat-type");
            if (beats < 1 || beats > 255 || beat_type < 1 || beat_type > 128) {
              throw std::runtime_error("invalid MusicXML time signature");
            }
            const TimeSignature meter{static_cast<std::uint8_t>(beats), static_cast<std::uint8_t>(beat_type)};
            (void)measureLength(meter);
            if (have_measure_meter && !sameNotationMeter(active_meter, meter)) {
              throw std::runtime_error("conflicting MusicXML time signatures at one measure start");
            }
            have_measure_meter = true;
            if (measure_start == 0) local_meters.front().signature = meter;
            else if (!sameNotationMeter(active_meter, meter)) local_meters.push_back({measure_start, meter});
            active_meter = meter;
          }
          continue;
        }
        if (event.tag == "direction" || event.tag == "sound") {
          const bool is_direction = event.tag == "direction";
          const auto direction_offset = [&] { return is_direction ? playbackOffset(xml, event.block, true) : 0; };
          const auto record = [&](double bpm, Tick offset) {
            if (offset < -cursor || (offset > 0 && cursor > std::numeric_limits<Tick>::max() - offset)) {
              throw std::runtime_error("MusicXML tempo offset crosses measure start or overflows");
            }
            const Tick local_tick = cursor + offset;
            if (measure_start > std::numeric_limits<Tick>::max() - local_tick) throw std::runtime_error("MusicXML tempo tick overflow");
            furthest_tempo = std::max(furthest_tempo, local_tick);
            const Tick tick = measure_start + local_tick;
            const auto found = global_tempos.find(tick);
            if (found != global_tempos.end()) {
              if (found->second != bpm) throw std::runtime_error("conflicting MusicXML tempos at the same tick");
            } else {
              if (global_tempos.size() >= kMaxScoreTempoChanges + 1) throw std::runtime_error("MusicXML tempo count exceeds limit");
              global_tempos.emplace(tick, bpm);
            }
          };
          bool have_sound_tempo = false;
          const auto sound_blocks = is_direction ? childElements(xml, event.block.content_start, event.block.content_end)
                                                : std::vector<XmlBlock>{event.block};
          for (const auto& sound : sound_blocks) {
            if (elementName(sound) != "sound") continue;
            bool present = false;
            const auto value = attribute(sound.opening, "tempo", &present);
            if (!present) continue;
            bool conditional = false;
            (void)attribute(sound.opening, "time-only", &conditional);
            if (conditional) throw std::runtime_error("repeat-specific MusicXML tempos are unsupported");
            bool own_offset = false;
            for (const auto& child : childElements(xml, sound.content_start, sound.content_end)) {
              if (elementName(child) == "offset") own_offset = true;
            }
            record(tempoValue(value), own_offset ? playbackOffset(xml, sound, false) : direction_offset());
            have_sound_tempo = true;
          }
          if (is_direction && !have_sound_tempo) {
            for (const auto& metronome : findBlocks(xml, "metronome", event.block.content_start, event.block.content_end)) {
              record(metronomeTempo(xml, metronome), direction_offset());
            }
          }
          continue;  // Directions never advance the note cursor or bar extent.
        }
        have_timed_event = true;
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
      const Tick nominal_length = measureLength(active_meter);
      // implicit means an unnumbered measure, commonly a pickup. It carries
      // no duration itself: infer actual extent from notes and forwards.
      const Tick measure_span = implicit == "yes" ? furthest_position : std::max(nominal_length, furthest_position);
      if (measure_span == 0) throw std::runtime_error("MusicXML empty implicit measure has no explicit duration");
      if (measure_span < 0 || measure_start > std::numeric_limits<Tick>::max() - measure_span) {
        throw std::runtime_error("MusicXML measure timing overflow");
      }
      if (furthest_tempo > measure_span) throw std::runtime_error("MusicXML tempo offset crosses measure end");
      measure.duration = measure_span;
      parsed_part.measures.push_back(std::move(measure));
      measure_start += measure_span;
      }
      part_meters.push_back(std::move(local_meters));
      part_ends.push_back(measure_start);
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
    const auto reference = synchronousReference(parsed.parts, part_ends);
    requireSharedMeters(part_meters, part_ends, reference);
    parsed.time_signature = part_meters[reference].front().signature;
    parsed.meter_changes.assign(part_meters[reference].begin() + 1, part_meters[reference].end());
    validateMeterMap(parsed.time_signature, parsed.meter_changes);
    for (const auto& tempo : global_tempos) {
      if (tempo.first == 0) parsed.bpm = tempo.second;
      else parsed.tempo_changes.push_back({tempo.first, tempo.second});
    }
    (void)scoreTempoMap(parsed);
    *score = std::move(parsed);
    return true;
  } catch (const std::exception& exception) {
    if (error) *error = exception.what();
    return false;
  }
}

}  // namespace daw
