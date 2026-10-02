#include "daw/session_render_source.hpp"
#include "daw/output_blocks.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
thread_local bool realtime=false;
thread_local unsigned allocations=0,frees=0;
void require(bool b,const char* s) { if(!b)throw std::runtime_error(s); }
template<class F> void rejects(F f) { bool caught=false;try{f();}catch(const std::exception&){caught=true;}require(caught,"invalid admission"); }
constexpr unsigned end=4099;
daw::MidiSampleSequence sequence() {
  daw::MidiSampleSequence s;s.frames=end;s.end_frame=end-2;
  s.events={{0,0xb0,11,100},{0,0x90,60,90,1},{end-3,0x80,60,0,1},{end-2,0xb0,123,0,0,true}};
  return s;
}
std::vector<daw::LiveTrackPlan> plans() { return {{"piano",sequence(),.25,{}},{"cello",sequence(),.75,{}}}; }
float signal(std::uint64_t f,unsigned channel,unsigned lane) {
  return f<end && (f%173==0 || f==end-1) ? static_cast<float>((channel+1)*(lane+1))*.125F : 0;
}
struct Renderer final:daw::PreparedTrackRenderer {
  unsigned delay,index,calls=0,on=0,off=0,resets=0;
  std::uint64_t generation=0,fail_at=UINT64_MAX;
  std::atomic<bool>* entered=nullptr;
  std::atomic<bool>* resume=nullptr;
  Renderer(unsigned d,unsigned i):delay(d),index(i){}
  daw::RendererLatency latency() const override {return {delay/48000.,generation};}
  std::uint64_t latencyGeneration() const noexcept override {return generation;}
  daw::TrackRenderError render(const daw::TimedMidiEvent* e,std::size_t n,std::uint64_t f,float* out,std::uint32_t frames) noexcept override {
    ++calls;
    if(entered && f+frames==end){entered->store(true,std::memory_order_release);while(!resume->load(std::memory_order_acquire))std::this_thread::yield();}
    for(std::size_t i=0;i<n;++i){on+=(e[i].status&0xf0)==0x90;off+=(e[i].status&0xf0)==0x80;resets+=e[i].terminal_reset && e[i].data1==123;}
    if(f>=fail_at)return daw::TrackRenderError::ReturnedError;
    for(unsigned i=0;i<frames;++i)for(unsigned c=0;c<2;++c)out[i*2+c]=f+i<delay?0:signal(f+i-delay,c,index);
    return daw::TrackRenderError::None;
  }
};
void matrix(unsigned a,unsigned b,unsigned request) {
  Renderer x(a,0),y(b,1);daw::SessionRenderSource s(plans(),{{"piano",&x,0},{"cello",&y,0}});
  const auto L=std::max(a,b);float out[1116]{};unsigned f=0;
  while(!s.done() && f<end+L+558) {
    realtime=true;
    daw::renderOutputBlocks(out,request,2,256,[&](float* p,unsigned n)noexcept{s.render(p,n);});
    realtime=false;require(!s.failed(),"combined render failed");
    for(unsigned i=0;i<request;++i)for(unsigned c=0;c<2;++c) {
      const auto expected=f+i<L ? 0: .25F*signal(f+i-L,c,0)+.75F*signal(f+i-L,c,1);
      require(out[i*2+c]==expected,"combined exact PDC/drain mismatch");
    }
    f+=request;
  }
  require(s.done() && s.frame()==end+L,"lost partial EOF or PDC drain");
  require(x.on==1 && y.on==1 && x.off==1 && y.off==1 && x.resets==1 && y.resets==1,"duplicate/missing MIDI release/reset");
  s.render(out,256);require(std::all_of(out,out+512,[](float v){return v==0;}),"finished output not silent");
  rejects([&]{s.submit(plans(),1);});
}
void edits() {
  Renderer x(173,0),y(960,1);daw::SessionRenderSource s(plans(),{{"piano",&x,0},{"cello",&y,0}});float out[512]{};
  s.render(out,185);
  auto next=plans();next[0].gain=.75;next[1].gain=.25;next[1].reject_started_onsets={1};
  const auto reject=s.submit(next,1);s.render(out,185);
  require(s.waitForDecision(reject).decision==daw::LiveSessionStream::Decision::RejectedStartedOnset,"late-track guard missed");
  next[1].reject_started_onsets.clear();std::reverse(next.begin(),next.end());
  const auto accept=s.submit(next,1);realtime=true;s.render(out,185);realtime=false;
  const auto receipt=s.waitForDecision(accept);
  require(receipt.decision==daw::LiveSessionStream::Decision::Applied && receipt.frame==370,"atomic revision frame wrong");
  for(unsigned f=555;f<2035;f+=185) {
    s.render(out,185);
    for(unsigned i=0;i<185;++i)for(unsigned c=0;c<2;++c) {
      const auto expected=f+i<960?0:.75F*signal(f+i-960,c,0)+.25F*signal(f+i-960,c,1);
      require(out[i*2+c]==expected,"route reorder/whole gain edit/reset of buffered audio");
    }
  }
  require(x.on==1 && y.on==1,"swap retriggered held notes");
}
void endRace() {
  Renderer x(0,0),y(960,1);std::atomic<bool> entered{false},resume{false};
  x.entered=&entered;x.resume=&resume;
  daw::SessionRenderSource s(plans(),{{"piano",&x,0},{"cello",&y,0}});
  bool callback_clean=false;
  std::thread audio([&]{float out[512]{};realtime=true;while(!s.done() && !s.failed())s.render(out,256);
    callback_clean=!allocations && !frees;realtime=false;});
  while(!entered.load(std::memory_order_acquire))std::this_thread::yield();
  std::uint64_t ticket=0;
  try { ticket=s.submit(plans(),1); }
  catch(...) { resume.store(true,std::memory_order_release);audio.join();throw; }
  resume.store(true,std::memory_order_release);audio.join();
  require(s.done() && callback_clean,"EOF race runtime/callback failure");
  require(s.waitForDecision(ticket).decision==daw::LiveSessionStream::Decision::Cancelled,
      "publication racing last render silently accepted without playback");
}
void faults() {
  Renderer x(173,0),y(960,1);y.fail_at=185;
  daw::SessionRenderSource s(plans(),{{"piano",&x,0},{"cello",&y,0}});float out[512]{};
  for(unsigned f=0;f<2220;f+=185) {
    s.render(out,185);require(!s.failed(),"one plugin failure stopped whole session");
    if(f>=185)for(unsigned i=0;i<185;++i)for(unsigned c=0;c<2;++c)
      require(out[i*2+c]==(f+i<960?0:.25F*signal(f+i-960,c,0)),"fault shifted healthy lane");
  }
  require(s.graph().trackStatus(1).quarantined() && y.calls==2 && s.latencyFrames()==960,"quarantine/status/fixed L failed");
  ++x.generation;s.render(out,185);
  require(s.failed() && s.graph().fault()==daw::RenderGraphFault::LatencyChanged,"latency change did not stop runtime");
  require(std::all_of(out,out+370,[](float f){return f==0;}),"latency failure leaked audio");
  rejects([&]{s.submit(plans(),1);});
  Renderer p(0,0),q(0,1);
  auto wrong=plans();std::reverse(wrong.begin(),wrong.end());
  rejects([&]{daw::SessionRenderSource bad(wrong,{{"piano",&p,0},{"cello",&q,0}});});
  auto many=plans();many.push_back({"third",sequence(),1,{}});
  rejects([&]{daw::SessionRenderSource bad(many,{{"piano",&p,0},{"cello",&q,0},{"third",&p,0}});});
}
}
void* operator new(std::size_t n){if(realtime)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{if(realtime && p)++frees;std::free(p);}
void operator delete[](void* p)noexcept{::operator delete(p);}
void operator delete(void* p,std::size_t)noexcept{::operator delete(p);}
void operator delete[](void* p,std::size_t)noexcept{::operator delete(p);}
int main(){try{
  for(auto pair:{std::pair<unsigned,unsigned>{0,0},{0,173},{173,512},{173,700},{0,960},{700,960}})
    for(unsigned n:{1U,185U,256U,512U,557U,558U})matrix(pair.first,pair.second,n);
  edits();faults();endRace();require(!allocations && !frees,"callback allocated/freed");
  std::cout<<"session runtime: 36 exact-sample PDC/end-drain cases, whole revision/reordered routes, held notes, quarantine, latency stop, zero callback allocations/frees passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
