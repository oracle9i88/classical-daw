// Opt-in licensed AU integration. Always silent; not part of CTest/CI.
#include "audio_unit_track_renderer.hpp"
#include "coreaudio_output.hpp"
#include "daw/session_render_source.hpp"
#include "daw/output_blocks.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>

namespace {
void require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
std::vector<std::uint8_t> readState(const char* path){
  std::ifstream in(path,std::ios::binary|std::ios::ate);
  require(in.good(),"cannot open AU state");const auto n=in.tellg();
  require(n>0 && n<=16*1024*1024,"AU state outside 1..16 MiB");
  std::vector<std::uint8_t> data(static_cast<std::size_t>(n));in.seekg(0);
  require(static_cast<bool>(in.read(reinterpret_cast<char*>(data.data()),n)),"AU state short read");return data;
}
daw::MidiSampleSequence sequence(bool cello){
  daw::MidiSampleSequence s;s.frames=288001;s.end_frame=192000;
  const auto pitch=static_cast<std::uint8_t>(cello?48:60);
  s.events={{0,0xb0,11,100},{0,0xb0,64,0},{137,0x90,pitch,85,1},
    {72000,0x80,pitch,20,1},{96017,0x90,static_cast<std::uint8_t>(pitch+7),90,2},
    {144123,0x80,static_cast<std::uint8_t>(pitch+7),20,2}};
  for(auto cc:{64,66,69,123})s.events.push_back({s.end_frame,0xb0,static_cast<std::uint8_t>(cc),0,0,true});
  return s;
}
struct Meter final:daw::PreparedTrackRenderer {
  daw::AudioUnitTrackRenderer au;
  double peak=0,energy=0;std::uint64_t samples=0,on=0,off=0;
  Meter(daw::InstrumentKind k,const std::vector<std::uint8_t>& state,const daw::MidiSampleSequence& s):au(k,state,s){}
  daw::RendererLatency latency()const override{return au.latency();}
  std::uint64_t latencyGeneration()const noexcept override{return au.latencyGeneration();}
  daw::TrackRenderError render(const daw::TimedMidiEvent* events,std::size_t count,std::uint64_t f,float* out,std::uint32_t n)noexcept override{
    const auto error=au.render(events,count,f,out,n);
    for(std::size_t i=0;i<count;++i){on+=(events[i].status&0xf0)==0x90 && events[i].data2;off+=(events[i].status&0xf0)==0x80;}
    for(unsigned i=0;i<n*2;++i){peak=std::max(peak,std::abs(static_cast<double>(out[i])));energy+=out[i]*static_cast<double>(out[i]);}
    samples+=n*2;return error;
  }
};
struct Silent final:daw::AudioOutputSource {
  daw::SessionRenderSource& source;std::atomic<bool> armed{false};double peak=0;
  explicit Silent(daw::SessionRenderSource& s):source(s){}
  bool acceptsFormat(double r,std::uint32_t c)const noexcept override{return source.acceptsFormat(r,c);}
  void render(float* out,std::uint32_t n)noexcept override{
    if(armed.load(std::memory_order_acquire)){
      source.render(out,n);for(unsigned i=0;i<n*2;++i)peak=std::max(peak,std::abs(static_cast<double>(out[i])));
    }
    std::fill_n(out,n*2,0.F);
  }
};
}
int main(int argc,char** argv){try{
  require(argc==3 || (argc==4 && std::string(argv[3])=="--hardware-silent"),
    "usage: daw_session_au_probe PIANO.aupreset CELLO.aupreset [--hardware-silent]");
  const bool hardware=argc==4;
  std::vector<daw::LiveTrackPlan> plans{{"piano",sequence(false),.35,{}},{"cello",sequence(true),.35,{}}};
  Meter piano(daw::InstrumentKind::Pianoteq9,readState(argv[1]),plans[0].sequence);
  Meter cello(daw::InstrumentKind::SwamCello3,readState(argv[2]),plans[1].sequence);
  daw::SessionRenderSource session(plans,{{"piano",&piano,0},{"cello",&cello,0}});
  Silent silent(session);daw::CoreAudioOutput output;
  std::string error;
  std::cout<<std::setprecision(17)<<"scope="<<(hardware?"silent_CoreAudio_two_AU":"manually_driven_two_AU_no_device")
    <<" au_instances=2 output_devices_opened="<<hardware<<" speaker_output=silenced"
    <<" workload=two_monophonic_lanes duration_seconds=6.00002 saved_states_written=0\n"
    <<"piano_version="<<piano.au.componentVersion()<<" cello_version="<<cello.au.componentVersion()<<'\n'
    <<session.graph().statusText();
  if(hardware){require(output.setAudioSource(&silent,&error),error.c_str());require(output.start(&error),error.c_str());std::cout<<output.diagnostics()<<'\n';}
  silent.armed.store(true,std::memory_order_release);
  std::uint64_t ticket=0;float audio[1114]{};
  const auto timeout=std::chrono::steady_clock::now()+std::chrono::seconds(15);
  while(!session.done() && !session.failed()){
    if(!ticket && session.frame()>=24000){
      // One revision changes BOTH future attacks while the initial notes sound.
      for(auto& p:plans){p.sequence.events[4].frame+=137;p.sequence.events[5].frame+=137;p.reject_started_onsets={2};}
      std::reverse(plans.begin(),plans.end());ticket=session.submit(plans,1);
    }
    if(hardware){require(output.checkHealth(&error),error.c_str());std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    else daw::renderOutputBlocks(audio,557,2,256,[&](float* p,unsigned n)noexcept{silent.render(p,n);});
    require(std::chrono::steady_clock::now()<timeout,"probe timeout");
  }
  output.stop();
  const auto receipt=ticket?session.waitForDecision(ticket):daw::LiveSessionStream::Receipt{};
  std::cout<<session.graph().statusText()<<"revision="<<receipt.revision<<" application_frame="<<receipt.frame
    <<" graph_output_end_frame="<<session.frame()<<" graph_peak="<<silent.peak<<'\n';
  for(auto* p:{&piano,&cello})std::cout<<"lane_peak="<<p->peak<<" lane_rms="<<(p->samples?std::sqrt(p->energy/p->samples):0)
    <<" note_on="<<p->on<<" note_off="<<p->off<<" latency_generation="<<p->latencyGeneration()<<'\n';
  if(hardware)std::cout<<daw::formatCallbackTiming(output.callbackTimingAfterStop())
    <<" callback_errors="<<output.xrunCount()<<'\n';
  require(session.done() && !session.failed(),"session runtime failed");
  require(receipt.decision==daw::LiveSessionStream::Decision::Applied && receipt.revision==1,"whole-plan edit was not applied");
  require(session.frame()==288001+session.latencyFrames(),"PDC drain ended early");
  for(unsigned i=0;i<2;++i)require(!session.graph().trackStatus(i).quarantined(),"real AU quarantined");
  for(auto* p:{&piano,&cello})require(p->peak>1e-5 && p->on==2 && p->off==2,"silent lane or duplicated/lost held voice");
  require(silent.peak>1e-5,"empty mix");
  if(hardware){const auto t=output.callbackTimingAfterStop();require(t.valid_budget_callbacks && !t.invalid_budget_callbacks && !t.deadline_misses && !t.headroom_misses && !output.xrunCount(),"complete callback budget/health gate failed");}
  std::cout<<"PASS; no listening sign-off; no general orchestra workload claim\n";
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
