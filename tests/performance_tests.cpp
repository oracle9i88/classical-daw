#include "daw/performance_fixture.hpp"
#include "daw/project.hpp"
#include "daw/history.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>

namespace fs=std::filesystem;
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}require(failed,"invalid operation accepted");}
std::string read(const fs::path& p){std::ifstream in(p,std::ios::binary);return{std::istreambuf_iterator<char>(in),{}};}
void write(const fs::path& p,const std::string& s){std::ofstream out(p,std::ios::binary);out<<s;}
struct Temp{fs::path root=fs::temp_directory_path()/("daw-performance-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
Temp(){require(fs::create_directory(root),"temp failed");}~Temp(){std::error_code e;fs::remove_all(root,e);}};
}
int main(){try{
  Temp temp;auto d=daw::performanceFixture({'f','a','k','e'});
  require(d.score.next_note_id==33 && d.performances[0].mapping.size()==31,"fixture identity counts");
  require(d.performances[0].mapping[7].notation_ids==std::vector<std::uint64_t>({8,9}),"tie lacks explicit two-to-one correspondence");
  require(d.performances[0].mapping[8].id==9 && d.performances[0].mapping[8].notation_ids[0]==10,"identity namespaces collapsed");
  const auto base=daw::compilePerformance(d.score,d.performances[0]);
  std::size_t cc11=0,cc64=0,tie_on=0,tie_off=0;
  for(const auto& e:base.events){if(e.status==0xb0&&e.data1==11)++cc11;if(e.status==0xb0&&e.data1==64)++cc64;
    if(e.note_id==8){if(e.status==0x90)++tie_on;else if(e.status==0x80)++tie_off;}}
  require(cc11>100&&cc64>10&&tie_on==1&&tie_off==1,"continuous controls/tie missing in performed sequence");
  auto tied_take=d.performances[0];tied_take.notes.push_back({8,.05,.9,-1});
  const auto tied_sequence=daw::compilePerformance(d.score,tied_take);
  std::size_t shifted_tie_attacks=0;
  for(const auto& e:tied_sequence.events)if(e.note_id==8&&e.status==0x90){
    require(e.frame==170400,"tied performed note addressed wrong onset");++shifted_tie_attacks;
  }
  require(shifted_tie_attacks==1,"tie edit created another attack");
  auto stale_mapping=d.performances[0];stale_mapping.mapping[7].notation_ids={8};
  rejects([&]{daw::compilePerformance(d.score,stale_mapping);});
  stale_mapping=d.performances[0];stale_mapping.mapping[7].notation_ids={9,8};
  rejects([&]{daw::compilePerformance(d.score,stale_mapping);});
  stale_mapping=d.performances[0];stale_mapping.mapping[8].notation_ids={8,9};
  rejects([&]{daw::compilePerformance(d.score,stale_mapping);});
  daw::savePerformanceDocument(d,(temp.root/"base").string());
  daw::WorkEditor editor(d);
  require(editor.setPitch(2,{'F',0,4}),"pitch edit failed");
  require(editor.set({2,.04,.94,85}),"performed edit failed");
  require(editor.setCurvePoint(2,2,100),"curve edit failed");
  require(editor.setGain(-15),"gain edit failed");
  require(editor.undo()&&editor.document().gain_db==-12,"gain undo order");
  require(editor.undo()&&editor.document().performances[1].curves[1].points[1].value==120,"curve undo order");
  require(editor.undo()&&editor.document().performances[1].notes.empty(),"performance undo order");
  require(editor.document().score.parts[0].measures[0].notes[1].pitch.step=='F',"performance undo changed notation");
  require(editor.undo()&&editor.document().score.parts[0].measures[0].notes[1].pitch.step=='D',"notation undo order");
  require(!editor.undo(),"history has extra state");for(int i=0;i<4;++i)require(editor.redo(),"redo failed");
  require(!editor.redo(),"extra redo state");
  daw::savePerformanceDocument(editor.document(),(temp.root/"edited").string());
  fs::rename(temp.root/"edited",temp.root/"moved");
  auto reopened=daw::loadPerformanceDocument((temp.root/"moved").string());
  daw::savePerformanceDocument(reopened,(temp.root/"reopened").string());
  for(const auto* name:{"score.dawproj","performances.dawperformance","piano.aupreset"})
    require(read(temp.root/"moved"/name)==read(temp.root/"reopened"/name),"document bytes changed on reopen");
  const auto revision=editor.revision();
  rejects([&]{editor.set({999,0,1,80});});rejects([&]{editor.set({1,-1,1,80});});
  rejects([&]{editor.setCurvePoint(2,2,128);});rejects([&]{editor.setGain(std::numeric_limits<double>::quiet_NaN());});
  require(editor.revision()==revision,"invalid edit committed a revision");
  require(editor.undo(),"branch undo");editor.setGain(-20);require(!editor.redo(),"redo branch not discarded");
  rejects([&]{daw::savePerformanceDocument(d,(temp.root/"base").string());});
  daw::WorkEditor timing_only(d);timing_only.set({2,.06,1,-1});timing_only.setCurvePoint(2,2,110);
  daw::savePerformanceDocument(timing_only.document(),(temp.root/"timing").string());
  require(read(temp.root/"base"/"score.dawproj")==read(temp.root/"timing"/"score.dawproj"),"performance edit changed written score");
  // A live mailbox admission is part of command acceptance, not a later best
  // effort. Rejection must not commit data, history, take selection or revision.
  daw::WorkEditor admitted(d);
  admitted.setCommitAdmission([](const auto&,auto){throw std::runtime_error("mailbox full");});
  rejects([&]{admitted.setPitch(2,{'F',0,4});});
  rejects([&]{admitted.set({2,.04,.94,85});});
  rejects([&]{admitted.setCurvePoint(2,2,100);});
  rejects([&]{admitted.setGain(-15);});rejects([&]{admitted.select(0);});
  require(admitted.revision()==0 && !admitted.undo(),"rejected live admission changed history");
  daw::savePerformanceDocument(admitted.document(),(temp.root/"admission-rejected").string());
  for(const auto* name:{"score.dawproj","performances.dawperformance","piano.aupreset"})
    require(read(temp.root/"base"/name)==read(temp.root/"admission-rejected"/name),"live rejection changed document bytes");
  admitted.setCommitAdmission({});admitted.setPitch(2,{'F',0,4});
  admitted.setCommitAdmission([](const auto&,auto){throw std::runtime_error("mailbox full");});
  rejects([&]{admitted.undo();});
  require(admitted.revision()==1 && admitted.document().score.parts[0].measures[0].notes[1].pitch.step=='F',"rejected undo changed revision/data");
  admitted.setCommitAdmission({});require(admitted.undo(),"admitted undo lost cursor");
  admitted.setCommitAdmission([](const auto&,auto){throw std::runtime_error("mailbox full");});
  rejects([&]{admitted.setGain(-15);});rejects([&]{admitted.redo();});
  admitted.setCommitAdmission({});require(admitted.redo(),"rejected new branch destroyed redo");
  // Stored onset offsets are elapsed seconds, invariant under tempo-map edits.
  auto seconds_take=d.performances[1];seconds_take.notes.push_back({2,.1,1,-1});
  const auto original_tempo=daw::compilePerformance(d.score,seconds_take);
  auto slower=d.score;slower.bpm=60;const auto slower_tempo=daw::compilePerformance(slower,seconds_take);
  auto onset=[](const auto& sequence){for(const auto& e:sequence.events)if(e.note_id==2&&e.status==0x90)return e.frame;throw std::runtime_error("missing timing note");};
  require(onset(original_tempo)==28800 && onset(slower_tempo)==52800 && seconds_take.notes[0].onset_seconds==.1,"seconds policy silently rescaled with tempo");
  auto invalid=d.score;invalid.parts[0].measures[0].notes[1].id=1;
  rejects([&]{daw::assignNoteIds(invalid);});std::string error;
  require(!daw::writeProjectFile(invalid,(temp.root/"bad").string(),&error),"duplicate ID saved");
  auto source=read(temp.root/"base"/"score.dawproj");require(source.find("CLASSICAL_DAW_PROJECT 7\n")==0,"not v7");
  auto corrupt=source;const auto at=corrupt.find("note_id 2\n");require(at!=std::string::npos,"missing note id fixture");corrupt.replace(at,10,"note_id 1\n");write(temp.root/"bad-score",corrupt);
  daw::Score untouched=d.score;require(!daw::readProjectFile((temp.root/"bad-score").string(),&untouched,&error),"duplicate ID read");require(untouched.parts[0].measures[0].notes[1].id==2,"failed read modified caller");
  auto legacy=d.score;legacy.next_note_id=0;for(auto& p:legacy.parts)for(auto& m:p.measures)for(auto& n:m.notes)n.id=0;
  require(daw::writeProjectFile(legacy,(temp.root/"legacy").string(),&error),"legacy save");require(read(temp.root/"legacy").find("CLASSICAL_DAW_PROJECT 6\n")==0,"legacy forced migration");
  require(daw::readProjectFile((temp.root/"legacy").string(),&legacy,&error),"legacy load");daw::assignNoteIds(legacy);require(legacy.next_note_id==33,"explicit migration failed");
  daw::ScoreHistory history(d.score);auto extended=d.score;auto note=extended.parts[0].measures[0].notes[0];note.id=0;note.pitch.octave=5;extended.parts[0].measures[0].notes.push_back(note);daw::assignNoteIds(extended);
  require(history.commit(extended,&error)&&history.undo(&extended,&error),"identity history setup");require(extended.next_note_id==34,"undo recycled ID allocator");
  extended.parts[0].measures[0].notes.push_back(note);daw::assignNoteIds(extended);require(extended.parts[0].measures[0].notes.back().id==34,"deleted ID reused");
  write(temp.root/"base"/"score.dawproj",source+"\n");rejects([&]{daw::loadPerformanceDocument((temp.root/"base").string());});
  std::cout<<"PASS dual IDs/ties, curves, interleaved score/performance/curve/mix undo, immutable notation, v7, allocator, saved/reopened exact bytes\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
