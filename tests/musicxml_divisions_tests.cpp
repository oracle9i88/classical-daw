#include "daw/render.hpp"
#include "daw/score_midi.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace {
using daw::Tick;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
struct Directory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("daw_xml_divisions_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Directory() { require(std::filesystem::create_directory(path), "create fixture directory"); }
  ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
std::string value(Tick number) { return std::to_string(number); }
std::string attribute(const std::string& divisions) {
  return "<attributes><divisions>" + divisions + "</divisions></attributes>";
}
std::string note(const std::string& duration, const std::string& extra = {}, int voice = 1, char pitch = 'A') {
  const bool chord = extra == "<chord/>";
  return "<note>" + (chord ? extra : "") + "<pitch><step>" + pitch + "</step><octave>4</octave></pitch><duration>" + duration +
      "</duration>" + (chord ? "" : extra) + "<voice>" + std::to_string(voice) + "</voice></note>";
}
std::string move(const std::string& kind, Tick duration) { return "<"+kind+"><duration>"+value(duration)+"</duration></"+kind+">"; }
std::string measure(const std::string& body, int number = 1, bool partial = false) {
  return "<measure number='" + std::to_string(number) + "'" + (partial ? " implicit='yes'" : "") + ">" + body + "</measure>";
}
std::string doc(const std::string& first, const std::string& second = {}) {
  return "<score-partwise><part-list><score-part id='P1'><part-name>First</part-name></score-part>" +
      (second.empty() ? std::string{} : "<score-part id='P2'><part-name>Second</part-name></score-part>") +
      "</part-list><part id='P1'>" + first + "</part>" +
      (second.empty() ? std::string{} : "<part id='P2'>" + second + "</part>") + "</score-partwise>";
}
void write(const std::filesystem::path& path, const std::string& xml) {
  std::ofstream out(path); out << xml; require(bool(out), "write fixture");
}
daw::Score load(const std::filesystem::path& path, const std::string& xml) {
  write(path, xml); daw::Score result; std::string error;
  const bool ok = daw::readMusicXmlFile(path.string(), &result, &error);
  require(ok, "import: " + error); return result;
}
std::string music(Tick divisions, bool change) {
  const Tick next = change ? divisions * 2 : divisions;
  return measure(attribute(value(divisions)) + "<sound tempo='60'/>" + note(value(2*divisions), "<tie type='start'/>") +
      move("backup",2*divisions) + move("forward",divisions/2) +
      "<direction><direction-type><words>Tempo</words></direction-type><offset sound='yes'>" + value(divisions/2) +
      "</offset><sound tempo='120'/></direction>" + note(value(divisions/2),{},2,'G') + move("forward",divisions), 1, true) +
      measure((change ? attribute(value(next)) : "") + "<attributes><time><beats>3</beats><beat-type>4</beat-type></time></attributes>" +
      note(value(next),"<tie type='stop'/>") + note(value(next/2),"<chord/>",1,'E') +
      "<sound tempo='90'><offset>" + value(-next/2) + "</offset></sound>" + move("forward",next/4), 2, true);
}
using NoteKey = std::tuple<Tick,Tick,int,int>;
std::vector<NoteKey> performance(const daw::Score& score) {
  daw::MidiFile midi; std::string error;
  const bool ok = daw::scoreToMidiFile(score,&midi,&error); require(ok,error);
  std::vector<NoteKey> notes;
  for (const auto& track : midi.tracks) for (const auto& n : track.notes) notes.emplace_back(n.start,n.duration,n.pitch,n.velocity);
  return notes;
}
void equivalence(const std::filesystem::path& path) {
  const auto reference = load(path,doc(music(960,false)));
  const auto expected = performance(reference);
  const auto audio = daw::renderScore(reference,12000,0.1);
  require(expected.size()==3, "tie must be merged with no extra attack");
  for (Tick divisions : {4,12,24,48,96,384,480,960,1920,10080}) for (bool changed : {false,true}) {
    const auto score = load(path,doc(music(divisions,changed)));
    require(score.divisions == 960 && performance(score) == expected, "source divisions changed note performance");
    require(score.parts[0].measures[0].duration == 1920 && score.parts[0].measures[1].start == 1920 &&
                score.parts[0].measures[1].duration == 1200, "partial measures drifted after scaling");
    require(score.bpm == 60 && score.tempo_changes.size() == 2 && score.tempo_changes[0].tick == 960 &&
                score.tempo_changes[0].bpm == 120 && score.tempo_changes[1].tick == 2400 && score.tempo_changes[1].bpm == 90,
            "tempo offset was not scaled with its active divisions");
    require(score.meter_changes.size()==1 && score.meter_changes[0].tick==1920, "meter changed position");
    require(daw::renderScore(score,12000,0.1).samples == audio.samples, "scaled score changed rendered samples");
    std::string error; require(daw::writeMusicXmlFile(score,path.string(),&error),error);
    daw::Score restored; require(daw::readMusicXmlFile(path.string(),&restored,&error),error);
    require(performance(restored)==expected && daw::renderScore(restored,12000,0.1).samples==audio.samples,
            "normalized 960-division export changed playback");
  }
  const auto mixed = load(path,doc(music(24,true),music(480,false)));
  require(mixed.parts.size()==2 && mixed.tempo_changes.size()==2 && performance(mixed).size()==6,
          "part-specific divisions did not merge into one global timeline");
  const auto defaults = load(path,doc(measure(attribute("24")+note("24")),measure(note("960"))));
  require(defaults.parts[0].measures[0].notes[0].duration==960 && defaults.parts[1].measures[0].notes[0].duration==960,
          "undeclared part inherited another part's units instead of legacy 960");
  const auto thirds = load(path,doc(measure(attribute("3.000") + note("1.0") + note("1") + note("1"))));
  require(thirds.parts[0].measures[0].notes[2].start==640 && thirds.parts[0].measures[0].notes[2].duration==320,
          "triplet arithmetic rounded or double-scaled note values");
  const auto rest = load(path,doc(measure(attribute("24") +
      "<note><rest/><duration>12</duration></note><note><pitch><step>C</step><octave>4</octave></pitch>"
      "<duration>8</duration><time-modification><actual-notes>3</actual-notes><normal-notes>2</normal-notes></time-modification></note>")));
  const auto& notes = rest.parts[0].measures[0].notes;
  require(notes[0].rest && notes[0].duration==480 && notes[1].start==480 && notes[1].duration==320 &&
              notes[1].tuplet_actual==3 && notes[1].tuplet_normal==2, "rest or tuplet normalization lost timing/notation");
  const auto unusual = load(path,doc(measure(attribute("7")+note("7"))));
  require(unusual.parts[0].measures[0].notes[0].duration==960, "exact timing rejected solely because divisions does not divide 960");
  // Reduce before multiplying: INT64_MAX * 960 would overflow, but ratio = 1 quarter.
  const auto huge = value(std::numeric_limits<Tick>::max());
  const auto reduced = load(path,doc(measure(attribute(huge)+note(huge))));
  require(reduced.parts[0].measures[0].notes[0].duration==960, "reducible ratio overflowed");
}
void failures(const std::filesystem::path& path) {
  std::vector<std::string> bodies;
  for (const std::string& invalid : {"0","-1","0.0000000000000000001","1e3","9223372036854775808",""}) bodies.push_back(attribute(invalid)+note("960"));
  bodies.push_back("<attributes><divisions/></attributes>"+note("1"));
  bodies.push_back("<attributes><divisions>24</divisions><divisions>48</divisions></attributes>"+note("24"));
  bodies.push_back(attribute("24")+attribute("48")+note("24"));
  bodies.push_back(attribute("24")+note("24")+attribute("48"));
  bodies.push_back(attribute("24")+note("24")+move("backup",24)+attribute("48"));
  bodies.push_back(attribute("24")+"<sound tempo='90'/>"+attribute("48")+note("24"));
  bodies.push_back(attribute("7")+note("1"));
  bodies.push_back(attribute("7")+move("forward",1));
  bodies.push_back(attribute("7")+note("7")+move("backup",1));
  bodies.push_back(attribute("7")+"<sound tempo='90'><offset>1</offset></sound>"+note("7"));
  bodies.push_back(attribute("1920")+note("1"));
  bodies.push_back(attribute("1")+note(value(std::numeric_limits<Tick>::max())));
  bodies.push_back(attribute("1")+"<sound tempo='90'><offset>"+value(std::numeric_limits<Tick>::min())+"</offset></sound>");
  bodies.push_back(attribute("24")+note("0.01"));
  bodies.push_back(attribute("24")+note("0"));
  daw::Score destination; destination.bpm = 71; destination.tempo_changes = {{123,47}};
  destination.parts={{"sentinel","Original",{{1,0,{},960}}}};
  for (std::size_t i=0;i<bodies.size();++i) {
    write(path,doc(measure(bodies[i]))); std::string error;
    require(!daw::readMusicXmlFile(path.string(),&destination,&error) && !error.empty(), "accepted invalid divisions fixture " + std::to_string(i));
    require(destination.bpm==71 && destination.tempo_changes.size()==1 && destination.tempo_changes[0].tick==123 &&
                destination.parts.size()==1 && destination.parts[0].id=="sentinel", "failed conversion mutated destination");
  }
}
}
int main() {
  try {
    Directory directory; const auto path=directory.path/"divisions.musicxml";
    equivalence(path); failures(path);
    std::cout<<"MusicXML divisions normalization and rejection tests passed\n"; return 0;
  } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
