#include "daw/performance.hpp"
#include "daw/live_session.hpp"
#include "daw/project.hpp"
#include "daw/performance_recovery.hpp"
#include "daw/session_render_source.hpp"
#include "daw/score_midi.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>

namespace fs=std::filesystem;
namespace {
bool count_mix_allocations = false;
unsigned mix_allocations = 0;
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}need(caught,"invalid input accepted");}
std::string read(const fs::path& p){std::ifstream in(p,std::ios::binary);return{std::istreambuf_iterator<char>(in),{}};}
void write(const fs::path& p,const std::string& text){std::ofstream out(p,std::ios::binary);out<<text;}
struct Temp{fs::path root=fs::temp_directory_path()/("daw-ensemble-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 Temp(){need(fs::create_directory(root),"tmp failed");}~Temp(){std::error_code e;fs::remove_all(root,e);}};
daw::ScoreNote note(daw::Tick at,daw::Tick length,char step,int octave){daw::ScoreNote n;n.start=at;n.duration=length;n.pitch={step,0,octave};n.midi_channel=0;return n;}
daw::PerformanceDocument fixture(){
  daw::PerformanceDocument d;d.score.bpm=120;
  daw::ScorePart p{"piano","Piano",{},{}},c{"cello","Cello",{}, {}};
  auto tie1=note(2880,960,'G',4);tie1.tie_start=true;
  auto tie2=note(3840,960,'G',4);tie2.tie_stop=true;
  p.measures={{1,0,{note(0,960,'C',4),note(960,480,'D',4),tie1},3840},{2,3840,{tie2},3840},{3,7680,{},3840}};
  c.measures={{1,0,{note(0,960,'C',3),note(1920,480,'E',3)},3840},{2,3840,{note(3840,960,'D',3)},3840},{3,7680,{},3840}};
  p.midi_events={{0,daw::MidiChannelEventType::ControlChange,0,64,0},{1000,daw::MidiChannelEventType::ControlChange,0,64,127},{2000,daw::MidiChannelEventType::ControlChange,0,64,0}};
  c.midi_events={{0,daw::MidiChannelEventType::ControlChange,0,11,50},{3840,daw::MidiChannelEventType::ControlChange,0,11,80}};
  d.score.parts={p,c};daw::assignNoteIds(d.score);
  d.performances.push_back(daw::makePerformance(d.score,"Both"));
  d.routes={{"cello","swam-cello",{'c','e','l','l','o'},-3,.2,false,false,0},{"piano","pianoteq",{'p','i','a','n','o'},-6,-.2,false,false,0}};
  return d;
}
const daw::CompiledPerformanceTrack& track(const std::vector<daw::CompiledPerformanceTrack>& tracks,const char* id){for(const auto& t:tracks)if(t.part_id==id)return t;throw std::runtime_error("missing part");}
std::size_t onset(const daw::MidiSampleSequence& s,std::uint64_t id){for(const auto& e:s.events)if(e.note_id==id && (e.status&0xf0)==0x90)return e.frame;throw std::runtime_error("missing attack");}
void compare(const fs::path& a,const fs::path& b){for(const auto* name:{"score.dawproj","performances.dawperformance","route-1.aupreset","route-2.aupreset"})need(read(a/name)==read(b/name),"save/reopen/undo bytes differ");}
struct Renderer:daw::PreparedTrackRenderer{
 unsigned ons=0,offs=0;double value;
 explicit Renderer(double v):value(v){}
 daw::RendererLatency latency()const override{return{0,0};}
 std::uint64_t latencyGeneration()const noexcept override{return 0;}
 daw::TrackRenderError render(const daw::TimedMidiEvent* events,std::size_t count,std::uint64_t,float* out,std::uint32_t n)noexcept override{
  for(std::size_t i=0;i<count;++i){ons+=(events[i].status&0xf0)==0x90 && events[i].data2;offs+=(events[i].status&0xf0)==0x80;}
  std::fill_n(out,n*2,static_cast<float>(value));return daw::TrackRenderError::None;
 }
};
void compilation(){
 auto d=fixture();daw::validatePerformanceDocument(d);
 const auto original=daw::compilePerformanceTracks(d.score,d.performances[0]);
 need(original.size()==2 && original[0].part_id=="piano" && original[1].part_id=="cello","score part identity order");
 need(d.performances[0].mapping.size()==6 && d.performances[0].mapping[2].notation_ids==std::vector<std::uint64_t>({3,4}) && d.performances[0].mapping[3].id==4 && d.performances[0].mapping[3].notation_ids[0]==5,"multi-part tie/independent namespaces");
 for(const auto& t:original)need(t.sequence.end_frame==288000 && t.sequence.frames==528000,"terminal silent bar lost");
 auto take=d.performances[0];take.notes.push_back({3,.08,.9,85});
 auto changed=daw::compilePerformanceTracks(d.score,take);
 need(onset(track(changed,"piano").sequence,3)==onset(track(original,"piano").sequence,3)+3840,"tie onset edit wrong part");
 need(onset(track(changed,"cello").sequence,4)==onset(track(original,"cello").sequence,4),"piano edit touched cello");
 auto bad=take;bad.mapping[2].notation_ids.push_back(5);rejects([&]{daw::compilePerformanceTracks(d.score,bad);});
 bad=take;bad.mapping[3].id=1;rejects([&]{daw::compilePerformanceTracks(d.score,bad);});
 daw::ControlCurve curve{11,0,11,{{1,0,110}},true,"piano"};take.curves.push_back(curve);
 changed=daw::compilePerformanceTracks(d.score,take);
 need(track(changed,"piano").sequence.events.front().data2==110 && track(changed,"cello").sequence.events.front().data2==50,"CC11 leaked across parts sharing channel 0");
 auto adopted=daw::curveFromScoreMessages(d.score,0,11,12,nullptr,"cello");need(adopted.part_id=="cello" && adopted.points.front().value==50 && adopted.points.size()==2,"part adoption stole piano events");
 rejects([&]{daw::curveFromScoreMessages(d.score,0,11,12);});
 bad=take;bad.curves[0].part_id.clear();rejects([&]{daw::compilePerformanceTracks(d.score,bad);});
 bad=take;bad.curves[0].part_id="missing";rejects([&]{daw::compilePerformanceTracks(d.score,bad);});
 take.notes.clear();take.notes.push_back({6,6,1,-1});changed=daw::compilePerformanceTracks(d.score,take);
 need(changed[0].sequence.frames==changed[1].sequence.frames && changed[0].sequence.end_frame==408000,"late cello edit did not extend shared end");
 for(const auto& t:changed)for(const auto& e:t.sequence.events)if(e.terminal_reset)need(e.frame==408000,"reset not moved with ensemble end");
 auto plans=daw::planPerformanceDocument(d);need(plans[0].track_id=="cello" && plans[1].track_id=="piano" && plans[0].balance==.2,"route identity not retained");
 d.routes[1].solo=true;plans=daw::planPerformanceDocument(d);need(!plans[0].audible && plans[1].audible,"solo ignored");
 d.routes[1].mute=true;plans=daw::planPerformanceDocument(d);need(!plans[0].audible && !plans[1].audible,"mute must win over solo");
}
void commonEndCurves() {
  auto d = fixture();
  auto& take = d.performances[0];
  // Notated end is 6 s. Piano's tied attack now releases at 8 s; cello must
  // be able to shape the shared extension, even when it is compiled first.
  take.notes.push_back({3, 5.5, 1, -1});
  for (bool reverse : {false, true}) {
    if (reverse) std::reverse(d.score.parts.begin(), d.score.parts.end());
    for (bool stepped : {false, true}) {
      take.curves = {{20, 0, 11, {{1, 0, 50}, {2, 7, 90}, {3, 8, 70}}, stepped, "cello"},
                     {21, 0, 64, {{1, 0, 0}, {2, 8, 127}}, stepped, "cello"}};
      const auto tracks = daw::compilePerformanceTracks(d.score, take);
      for (const auto& t : tracks) {
        need(t.sequence.end_frame == 384000 && t.sequence.frames == 624000,
             "curves changed the shared end or tail");
        for (const auto& e : t.sequence.events)
          if (e.terminal_reset) need(e.frame == 384000, "reset before common end");
      }
      const auto& events = track(tracks, "cello").sequence.events;
      // Linear lanes coalesce unchanged integer CC values, so inspect the
      // held controller state at 7 s, rather than requiring a redundant event.
      int expression = -1;
      for (const auto& e : events)
        if (e.frame <= 336000 && e.status == 0xb0 && e.data1 == 11) expression = e.data2;
      need(expression == 90, "cello expression in another part's extension was lost");
      int final_pedal = -1;
      for (const auto& e : events)
        if (e.frame == 384000 && e.status == 0xb0 && e.data1 == 64) final_pedal = e.data2;
      need(final_pedal == 0, "curve endpoint defeated terminal pedal release");
      take.curves[0].points.back().seconds = 8 + 1.0 / 48000;
      rejects([&] { daw::compilePerformanceTracks(d.score, take); });
    }
  }
  daw::WorkEditor editor(fixture());
  editor.set({3, 5.5, 1, -1});
  editor.putCurve({20, 0, 11, {{1, 0, 50}, {2, 7, 90}}, true, "cello"});
  const auto revision = editor.revision();
  rejects([&] { editor.set({3, 0, 1, -1}); });
  need(editor.revision() == revision && editor.document().performances[0].notes[0].onset_seconds == 5.5,
       "shrinking past another part's curve did not roll back");
  need(editor.undo() && editor.undo(), "shared-end curve/note chronological undo failed");
  need(editor.redo() && editor.redo(), "shared-end curve/note chronological redo failed");
}
void legacySourceEnd() {
  daw::Score s;
  // Legacy duration=0 does not constrain the note to the nominal barline.
  s.parts = {{"piano", "Piano", {{1, 0, {note(0, 4800, 'C', 4)}, 0}}, {}}};
  daw::assignNoteIds(s);
  auto take = daw::makePerformance(s, "Legacy");
  auto compiled = daw::compilePerformance(s, take);
  need(compiled.end_frame == 120000 && compiled.frames == 360000,
       "note beyond nominal barline was truncated");
  s.parts[0].midi_events = {{6000, daw::MidiChannelEventType::ControlChange, 0, 64, 0}};
  compiled = daw::compilePerformance(s, take);
  need(compiled.end_frame == 150000 && compiled.frames == 390000,
       "late source controller was truncated");
  daw::MidiFile midi;
  std::string error;
  need(daw::scoreToMidiFile(s, &midi, &error), "legacy MIDI reference failed");
  // The pre-70f1d67 compiler passed only measure extents as minimum_end_tick.
  // The MIDI scheduler ALREADY extended that minimum for notes/controllers.
  const auto reference = daw::makeMidiSampleSequence(midi, 48000, 5, 48000U * 60 * 30, 0);
  need(reference.end_frame == compiled.end_frame && reference.frames == compiled.frames &&
       reference.events.size() == compiled.events.size(), "legacy compiler end behavior changed");
  for (std::size_t i = 0; i < reference.events.size(); ++i) {
    const auto& a = reference.events[i];
    const auto& b = compiled.events[i];
    need(a.frame == b.frame && a.status == b.status && a.data1 == b.data1 &&
         a.data2 == b.data2 && a.note_id == b.note_id && a.terminal_reset == b.terminal_reset,
         "legacy note/controller/reset schedule changed");
  }
}
void scalarMixValidation() {
  auto d = fixture();
  for (unsigned i = 1; i < 16; ++i) d.performances.push_back(d.performances.front());
  daw::WorkEditor editor(std::move(d));
  const std::string part = "piano";
  count_mix_allocations = true;
  const bool accepted = editor.setRouteMix(part, {12, 1, true, true, -12345});
  const bool undone = editor.undo();
  const bool redone = editor.redo();
  count_mix_allocations = false;
  need(accepted && undone && redone && mix_allocations == 0,
       "scalar mix edits/undo/redo allocated or recompiled unchanged takes");
  const auto revision = editor.revision();
  rejects([&] { editor.setRouteMix(part, {12.01, 0, false, false, 0}); });
  rejects([&] { editor.setRouteMix(part, {0, 1.01, false, false, 0}); });
  rejects([&] { editor.setRouteMix(part, {std::numeric_limits<double>::infinity(), 0, false, false, 0}); });
  rejects([&] { editor.setRouteMix(part, {0, std::numeric_limits<double>::quiet_NaN(), false, false, 0}); });
  need(editor.revision() == revision && editor.document().routes[1].gain_db == 12 &&
       editor.document().routes[1].balance == 1 && editor.document().routes[1].track_delay_us == -12345,
       "invalid scalar mix command changed state/history");
}
void storageAndHistory(){
 Temp temp;auto d=fixture();d.import_receipt="{\"source\":\"工程\"}\n";
 daw::savePerformanceDocument(d,(temp.root/"base").string());need(read(temp.root/"base"/"performances.dawperformance").rfind("CLASSICAL_DAW_PERFORMANCE 4\n",0)==0,"explicit routes not versioned");
 daw::WorkEditor e(d);
 need(e.set({4,.02,.9,88}) && e.setPitch(2,{'F',0,4}) && e.putCurve({10,0,11,{{1,0,70}},true,"cello"}) && e.setRouteMix("piano",{-9,-1,true,false,0}),"cross-part edits failed");
 need(e.undo() && !e.document().routes[1].mute,"mix order wrong");
 need(e.undo() && e.document().performances[0].curves.empty(),"curve order wrong");
 need(e.undo() && e.document().score.parts[0].measures[0].notes[1].pitch.step=='D',"notation order wrong");
 need(e.undo() && e.document().performances[0].notes.empty() && !e.undo(),"performance order wrong");
 daw::savePerformanceDocument(e.document(),(temp.root/"undone").string());compare(temp.root/"base",temp.root/"undone");
 for(unsigned i=0;i<4;++i)need(e.redo(),"redo failed");
 daw::savePerformanceDocument(e.document(),(temp.root/"edited").string());
 auto reopened=daw::loadPerformanceDocument((temp.root/"edited").string());daw::savePerformanceDocument(reopened,(temp.root/"reopen").string());compare(temp.root/"edited",temp.root/"reopen");
 need(reopened.routes[0].state==d.routes[0].state && reopened.routes[1].state==d.routes[1].state && reopened.import_receipt==d.import_receipt,"route state/receipt lost");
 const auto revision=e.revision();e.setCommitAdmission([](const auto&,auto){throw std::runtime_error("late admission rejected");});
 rejects([&]{e.setRouteMix("cello",{-12,0,true,false,0});});
 need(e.revision()==revision && e.document().routes[0].gain_db==-3 && !e.document().routes[0].mute,"rejected route edit modified history/data");
 rejects([&]{e.set({5,.03,.8,80});});need(e.document().performances[0].notes.size()==1,"rejected part override remained");
 e.setCommitAdmission({});need(e.shapeRange(0,10,daw::WorkEditor::Shape::Velocity,70,80,"cello")==3,"part shape count");
 for(const auto& n:e.document().performances[0].notes)need(n.note_id>=4,"cello shaping wrote piano overrides");
 rejects([&]{e.shapeRange(0,10,daw::WorkEditor::Shape::Velocity,70,80,"missing");});
 daw::PerformanceRecovery recovery((temp.root/"base").string());
 for(std::uint64_t i=1;i<=3;++i){e.setGain(-12-static_cast<double>(i));recovery.checkpoint(e.document(),i);}
 need(!fs::exists(fs::path(recovery.directory())/"revision-1"),"multi-state old snapshot not removed");
 auto recovered=daw::readPerformanceRecovery((temp.root/"base").string(),recovery.directory());
 need(recovered.routes.size()==2 && recovered.gain_db==-15,"multi-state recovery failed");
 auto data=read(temp.root/"base"/"route-2.aupreset");data[0]='X';write(temp.root/"base"/"route-2.aupreset",data);
 rejects([&]{daw::readPerformanceRecovery((temp.root/"base").string(),recovery.directory());});
 rejects([&]{daw::loadPerformanceDocument((temp.root/"base").string());});
}
void liveAdmission(){
 auto d=fixture();Renderer cello(.25),piano(.5);daw::SessionRenderSource source(daw::planPerformanceDocument(d),{{"cello",&cello,0},{"piano",&piano,0}});float out[512]{};
 source.render(out,256);daw::WorkEditor e(d);
 e.setCommitAdmission([&](const auto& candidate,auto revision){
  auto plans=daw::planPerformanceDocument(candidate);
  // Force rejection on the already-started cello while the entire candidate
  // document is temporarily installed by WorkEditor.
  for(auto& p:plans)if(p.track_id=="cello")p.reject_started_onsets={4};
  const auto ticket=source.submit(std::move(plans),revision);source.render(out,256);
  if(source.waitForDecision(ticket).decision!=daw::LiveSessionStream::Decision::Applied)throw std::runtime_error("whole plan rejected");
 });
 rejects([&]{e.set({4,.02,.9,90});});need(e.revision()==0 && e.document().performances[0].notes.empty(),"late-track rejection committed editor");
 e.setCommitAdmission([&](const auto& candidate,auto revision){const auto ticket=source.submit(daw::planPerformanceDocument(candidate),revision);source.render(out,256);need(source.waitForDecision(ticket).decision==daw::LiveSessionStream::Decision::Applied,"live mix acceptance");});
 e.setRouteMix("piano",{-12,1,false,true,0});
 const auto expected=static_cast<float>(piano.value*std::pow(10.,(d.gain_db-12)/20));
 need(out[0]==0 && out[1]==expected && out[510]==0 && out[511]==expected && source.appliedRevision()==1 && cello.ons==1 && piano.ons==1,"quantum-boundary gain/balance step, solo atomicity or held-note restart");
}
}
void* operator new(std::size_t n) {
  if (count_mix_allocations) ++mix_allocations;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
int main(){try{compilation();commonEndCurves();legacySourceEnd();scalarMixValidation();storageAndHistory();liveAdmission();std::cout<<"multi-part performance: shared-end step/linear curves, legacy source-end parity, allocation-free scalar mix edits, global IDs/ties, one history, v4 reopen/recovery and atomic admission passed\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
