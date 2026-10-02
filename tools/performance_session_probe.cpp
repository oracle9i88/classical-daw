// Opt-in real saved-document/CoreAudio integration. Speakers ALWAYS silenced.
#include "ensemble_audition.hpp"
#include "coreaudio_output.hpp"
#include "daw/project.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>

namespace fs=std::filesystem;
namespace {
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::string read(const fs::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
std::uint64_t target(const daw::LiveTrackPlan& plan,bool future){
 for(const auto& e:plan.sequence.events)if((e.status&0xf0)==0x90 && e.data2 && (future?e.frame>=144000:e.frame<48000))return e.note_id;
 throw std::runtime_error("probe needs an early attack and a future attack after 3 seconds on each route");
}
}
int main(int argc,char** argv){try{
 need(argc==4 && std::string(argv[3])=="--hardware-silent","usage: daw_performance_session_probe DOCUMENT NEW_EVIDENCE_DIRECTORY --hardware-silent");
 const fs::path root(argv[2]);need(!fs::exists(fs::symlink_status(root)),"evidence directory must be new");need(fs::create_directory(root),"evidence parent must exist");
 const auto d=daw::loadPerformanceDocument(argv[1]);need(d.routes.size()==2,"probe requires two explicit saved routes");
 const auto plans=daw::planPerformanceDocument(d);
 std::vector<std::uint64_t> ids;for(const auto& p:plans)ids.push_back(target(p,true));
 const auto started=target(plans[0],false);
 std::uint64_t cello_notation=0;std::uint8_t cello_channel=0;
 for(std::size_t i=0;i<plans.size();++i)if(d.routes[i].instrument=="swam-cello")
   {for(const auto& m:d.performances[d.active].mapping)if(m.id==ids[i])cello_notation=m.notation_ids.front();
    for(const auto& e:plans[i].sequence.events)if(e.note_id==ids[i] && (e.status&0xf0)==0x90)cello_channel=static_cast<std::uint8_t>(e.status&15);}
 need(cello_notation!=0,"probe needs a saved SWAM cello route");
 daw::savePerformanceDocument(d,(root/"baseline").string());
 daw::WorkEditor editor(d);daw::EnsembleAudition audition(d,true,editor.revision());
 daw::CoreAudioOutput output;std::string error;
 editor.setCommitAdmission([&](const auto& candidate,auto revision){audition.submit(candidate,revision);});
 std::cout<<std::setprecision(17)<<"scope=saved_document_two_AU_silent_CoreAudio score_parts="<<d.score.parts.size()
   <<" performed_notes="<<d.performances[d.active].mapping.size()<<" output_devices_opened=1 speaker_output=silenced\n";
 need(output.setAudioSource(&audition,&error),error.c_str());need(output.start(&error),error.c_str());
 std::cout<<output.diagnostics()<<'\n'<<audition.statusText();audition.start();
 bool edited=false;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
 while(!audition.done() && !audition.failed()){
   need(output.checkHealth(&error),error.c_str());need(std::chrono::steady_clock::now()<deadline,"probe timeout");
   if(!edited && audition.frame()>=48000){
     bool rejected=false;
     try{editor.set({started,.008,.85,80});}catch(const std::invalid_argument& e){rejected=std::string(e.what()).find("already been processed")!=std::string::npos;}
     need(rejected && editor.revision()==0 && editor.document().performances[d.active].notes.empty(),"already-started edit was not rolled back");
     rejected=false;
     try{editor.setPitch(cello_notation,{'C',0,8});}catch(const std::invalid_argument& e){rejected=std::string(e.what()).find("outside saved-state range")!=std::string::npos;}
     need(rejected && editor.revision()==0,"out-of-range SWAM edit accepted into history");
     for(auto id:ids)editor.set({id,.008,.85,80});
     const auto cello=std::find_if(d.routes.begin(),d.routes.end(),[](const auto& r){return r.instrument=="swam-cello";});
     std::uint64_t curve_id=1;
     while(std::any_of(d.performances[d.active].curves.begin(),d.performances[d.active].curves.end(),[&](const auto& c){return c.id==curve_id;}))++curve_id;
     auto curve=daw::curveFromScoreMessages(d.score,cello_channel,11,curve_id,nullptr,cello->part_id);editor.putCurve(std::move(curve));
     editor.setRouteMix(d.routes[0].part_id,{d.routes[0].gain_db-1,d.routes[0].balance,d.routes[0].mute,d.routes[0].solo,d.routes[0].track_delay_us});
     for(unsigned i=0;i<4;++i)need(editor.undo(),"live chronological undo failed");
     for(unsigned i=0;i<4;++i)need(editor.redo(),"live chronological redo failed");
     need(audition.appliedRevision()==editor.revision() && editor.revision()==12,"document and whole-session revisions diverged");
     std::cout<<"already_started_edit_rejected=1 out_of_range_edit_rejected=1 live_undo=4 live_redo=4 revision="<<editor.revision()
       <<" applied_frame="<<audition.appliedFrame()<<" edited_performed_ids="<<ids[0]<<','<<ids[1]<<'\n';edited=true;
   }
   std::this_thread::sleep_for(std::chrono::milliseconds(1));
 }
 output.stop();need(edited && audition.done() && !audition.failed(),"saved-document playback failed");
 std::cout<<audition.statusText()<<daw::formatCallbackTiming(output.callbackTimingAfterStop())<<" callback_errors="<<output.xrunCount()
   <<" suppressed_conflicts="<<audition.suppressedConflictsAfterStop()<<" mix_peak="<<audition.peakAfterStop()<<'\n';
 const auto stats=audition.trackStatisticsAfterStop();
 for(std::size_t i=0;i<stats.size();++i){
   std::size_t expected=0;for(const auto& e:plans[i].sequence.events)expected+=(e.status&0xf0)==0x90 && e.data2;
   std::cout<<"track="<<std::quoted(d.routes[i].part_id)<<" peak="<<stats[i].peak<<" note_on="<<stats[i].note_ons<<" note_off="<<stats[i].note_offs<<" expected_attacks="<<expected<<'\n';
   need(stats[i].peak>1e-5 && stats[i].note_ons==expected && stats[i].note_offs==expected,"track silent or held note retriggered/lost");
 }
 daw::savePerformanceDocument(editor.document(),(root/"edited").string());
 need(read(root/"baseline"/"score.dawproj")==read(root/"edited"/"score.dawproj"),"performance/curve/mix edits or rejected pitch changed notation bytes");
 auto reopened=daw::loadPerformanceDocument((root/"edited").string());daw::savePerformanceDocument(reopened,(root/"reopened").string());
 for(const auto* name:{"score.dawproj","performances.dawperformance","route-1.aupreset","route-2.aupreset"})
   need(read(root/"edited"/name)==read(root/"reopened"/name),"reopened saved document bytes changed");
 const auto t=output.callbackTimingAfterStop();need(t.valid_budget_callbacks && !t.invalid_budget_callbacks && !t.deadline_misses && !t.headroom_misses && !output.xrunCount(),"complete callback budget/health gate failed");
 std::cout<<"PASS saved_score_bytes_unchanged=1 saved_document_reopen_bytes_equal=1 no_listening_signoff=1 device_presentation_time=unknown\n";
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
