#include "daw/render.hpp"
#include "daw/score_midi.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error(reason); }
struct Directory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("daw_xml_fractional_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Directory() { require(std::filesystem::create_directory(path), "create test directory"); }
  ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
std::string attributes(const std::string& divisions) { return "<attributes><divisions>"+divisions+"</divisions></attributes>"; }
std::string note(const std::string& duration, const std::string& extra={}, int voice=1, char pitch='A') {
  const bool chord=extra=="<chord/>";
  return "<note>"+(chord?extra:"")+"<pitch><step>"+pitch+"</step><octave>4</octave></pitch><duration>"+duration+
      "</duration>"+(chord?"":extra)+"<voice>"+std::to_string(voice)+"</voice></note>";
}
std::string move(const std::string& kind,const std::string& duration) { return "<"+kind+"><duration>"+duration+"</duration></"+kind+">"; }
std::string measure(const std::string& body,int number=1) { return "<measure number='"+std::to_string(number)+"' implicit='yes'>"+body+"</measure>"; }
std::string document(const std::string& body) {
  return "<score-partwise><part-list><score-part id='P1'><part-name>Fractional</part-name></score-part></part-list><part id='P1'>"+body+"</part></score-partwise>";
}
void write(const std::filesystem::path& path,const std::string& xml) { std::ofstream out(path);out<<xml;require(bool(out),"write test file"); }
daw::Score load(const std::filesystem::path& path,const std::string& body) {
  write(path,document(body)); daw::Score score; std::string error;
  const bool ok=daw::readMusicXmlFile(path.string(),&score,&error); require(ok,error); return score;
}
std::string music(bool fractional) {
  const std::string d=fractional?"+.500000":"960",half=fractional?".25":"480",quarter=fractional?"0.125":"240";
  const std::string next=fractional?"1.25":"960",next_half=fractional?".625":"480";
  return measure(attributes(d)+"<sound tempo='60'/>"+note(d,"<tie type='start'/>")+note(half,"<chord/>",1,'E')+
      move("backup",d)+move("forward",quarter)+
      "<direction><direction-type><words>Tempo</words></direction-type><offset sound='yes'>"+quarter+
      "</offset><sound tempo='120'/></direction>"+note(quarter,{},2,'G')+move("forward",half))+
      measure(attributes(next)+"<sound tempo='90'><offset>"+next_half+"</offset></sound>"+note(next,"<tie type='stop'/>"),2);
}
void equivalence(const std::filesystem::path& path) {
  const auto baseline=load(path,music(false)), fractional=load(path,music(true));
  require(fractional.divisions==960 && fractional.parts[0].measures[0].duration==960 &&
              fractional.parts[0].measures[1].start==960 && fractional.parts[0].measures[1].duration==960,"fractional bar drift");
  require(fractional.bpm==60 && fractional.tempo_changes.size()==2 && fractional.tempo_changes[0].tick==480 &&
              fractional.tempo_changes[0].bpm==120 && fractional.tempo_changes[1].tick==1440 && fractional.tempo_changes[1].bpm==90,
          "fractional tempo offset drift");
  const auto expected=daw::renderScore(baseline,12000,0.1);
  require(expected.samples==daw::renderScore(fractional,12000,0.1).samples,"fractional import changed samples");
  std::string error; require(daw::writeMusicXmlFile(fractional,path.string(),&error),error);
  daw::Score restored; require(daw::readMusicXmlFile(path.string(),&restored,&error),error);
  require(expected.samples==daw::renderScore(restored,12000,0.1).samples,"fractional export changed samples");

  for (const std::string& unit : {"0.000000000000000001","0.123456789012345678","1.234567890123456789","9223372036854775807"}) {
    // Huge decimal scales and mantissas cancel exactly. Offset returns from
    // the end of the note to zero without rounding a binary floating value.
    const auto score=load(path,measure(attributes(unit)+note(unit)+"<sound tempo='60'><offset>-"+unit+"</offset></sound>"));
    require(score.bpm==60 && score.tempo_changes.empty() && score.parts[0].measures[0].notes[0].duration==960,
            "exact fractional cancellation failed");
  }
  const auto aliases=load(path,measure(attributes("+.50")+attributes("0.5000")+note(".25")+
      "<sound tempo='80'><offset>-0.000000000000000000000</offset></sound>"));
  require(aliases.parts[0].measures[0].duration==480 && aliases.tempo_changes[0].tick==480,"equivalent decimal declarations conflicted");
  const auto ordinary=load(path,measure(attributes("24")+note(".5")));
  require(ordinary.parts[0].measures[0].notes[0].duration==20,"fractional duration with integral divisions failed");
  // One source unit is 400 ticks: .0025 -> one tick. Check every duration
  // from 1 to 100 ticks independently, rather than comparing two parsers.
  for (int tick=1;tick<=100;++tick) {
    const int numerator=tick*25;
    std::string digits=std::to_string(numerator);
    const std::string raw="0."+std::string(4-digits.size(),'0')+digits;
    const auto score=load(path,measure(attributes("2.4")+note(raw)));
    require(score.parts[0].measures[0].duration==tick,"exact decimal grid test changed a tick");
  }
}
void failures(const std::filesystem::path& path) {
  std::vector<std::string> bodies;
  for(const std::string& unit : {"0","-0.5",".0000000000000000001","9.223372036854775808","1/2","1e-3","NaN"})
    bodies.push_back(attributes(unit)+note("1"));
  bodies.push_back(attributes("1")+note("0.0001")); // .096 tick; never silently round.
  bodies.push_back(attributes("960")+note("0.5"));
  bodies.push_back(attributes("0.1")+note("0.100000000000000001")); // binary double would round this difference away.
  bodies.push_back(attributes("0.5")+attributes("0.500000000000000001")+note(".5"));
  bodies.push_back(attributes("0.5")+note(".0001"));
  bodies.push_back(attributes("0.000000000000000001")+note("1")); // 960e18 ticks overflows.
  bodies.push_back(attributes("1")+note("1.0000000000000000001"));
  bodies.push_back(attributes("1")+note("9.223372036854775808"));
  bodies.push_back(attributes("0.5")+note("-.25"));
  bodies.push_back(attributes("0.5")+note("-0.000"));
  bodies.push_back(attributes("0.5")+attributes("0.5001")+note(".5"));
  bodies.push_back(attributes("0.5")+note(".5")+attributes("1.25"));
  bodies.push_back(attributes("1")+note("1")+"<sound tempo='80'><offset>0.0001</offset></sound>");
  bodies.push_back(attributes("1")+"<sound tempo='80'><offset>-0.5</offset></sound>"+note("1"));
  bodies.push_back(attributes("1")+note("1")+move("backup","0.0001"));
  bodies.push_back(attributes("1")+move("forward","0.0001"));
  daw::Score destination; destination.bpm=73; destination.tempo_changes={{12,61}};
  destination.parts={{"sentinel","Keep",{{1,0,{},960}}}};
  for(std::size_t i=0;i<bodies.size();++i) {
    write(path,document(measure(bodies[i]))); std::string error;
    require(!daw::readMusicXmlFile(path.string(),&destination,&error) && !error.empty(),"accepted invalid fraction "+std::to_string(i));
    require(destination.bpm==73 && destination.tempo_changes.size()==1 && destination.tempo_changes[0].tick==12 &&
                destination.parts.size()==1 && destination.parts[0].id=="sentinel","failed fraction import changed destination");
  }
}
}
int main(){
  try { Directory directory; const auto path=directory.path/"fractional.musicxml";equivalence(path);failures(path);
    std::cout<<"MusicXML fractional arithmetic, playback and failure tests passed\n";return 0;
  } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
