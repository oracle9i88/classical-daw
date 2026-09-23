#include "daw/live_session.hpp"
#include "daw/parallel_render_graph.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
thread_local bool realtime=false;
thread_local std::size_t allocations=0,frees=0;
thread_local long fail_after=-1;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
template<class F> void rejects(F f) {
  bool caught=false; try{f();}catch(const std::exception&){caught=true;}
  require(caught,"invalid session update admitted");
}
daw::MidiSampleSequence sequence(unsigned expression=100,std::size_t end=4096) {
  daw::MidiSampleSequence s; s.end_frame=end; s.frames=end+128;
  s.events={{0,0xb0,11,static_cast<std::uint8_t>(expression)}, {0,0xb0,64,127},
      {0,0x90,60,96,1},{end-20,0x80,60,20,1}};
  for(auto cc:{64,66,69,123}) s.events.push_back({end,0xb0,static_cast<std::uint8_t>(cc),0,0,true});
  return s;
}
std::vector<daw::LiveTrackPlan> plans(std::uint64_t revision=0,std::size_t end=4096) {
  return {{"piano",sequence(80,end),revision%2?.75:.25,{}},
          {"cello",sequence(110,end),revision%2?.25:.75,{}}};
}
unsigned noteCount(const daw::LiveSessionStream::TrackBlock& b,unsigned type) {
  unsigned count=0; for(std::size_t i=0;i<b.count;++i) if((b.events[i].status&0xf0)==type) ++count;
  return count;
}
void atomicAdmission() {
  daw::LiveSessionStream s(plans());
  auto first=s.nextBlock(64);
  require(first.count==2 && noteCount(first.tracks[0],0x90)==1 && noteCount(first.tracks[1],0x90)==1,
      "equal performed IDs/MIDI channels collided across tracks");
  auto invalid=plans(1); invalid[1].sequence.events[3].data1=62;
  rejects([&]{s.submit(invalid,1);});
  require(!s.hasPendingUpdate() && s.appliedRevision()==0,"late preparation failure published a subset");
  auto guarded=plans(1);
  guarded[0].sequence.events[0].data2=20; // would audibly alter the earlier lane
  guarded[0].sequence.events[2].data1=guarded[0].sequence.events[3].data1=65;
  guarded[1].reject_started_onsets={1};
  std::reverse(guarded.begin(),guarded.end());
  const auto rejected=s.submit(guarded,1);
  realtime=true; auto b=s.nextBlock(64); realtime=false;
  const auto receipt=s.receipt(rejected);
  require(receipt.decision==daw::LiveSessionStream::Decision::RejectedStartedOnset &&
      receipt.track_slot==1 && s.trackId(receipt.track_slot)=="cello" && receipt.note_id==1,
      "all-track guard lost stable track/note identity");
  require(b.revision==0 && b.tracks[0].revision==0 && b.tracks[1].revision==0 &&
      b.tracks[0].gain==.25 && b.tracks[1].gain==.75 && b.tracks[0].count==0 && b.tracks[1].count==0,
      "earlier track emitted repitch/CC or accepted gain before later track rejection");
  auto accepted=plans(1); std::reverse(accepted.begin(),accepted.end());
  const auto ticket=s.submit(accepted,1); // same editor revision, distinct request ticket
  realtime=true; b=s.nextBlock(185); realtime=false;
  require(ticket!=rejected && s.receipt(ticket).decision==daw::LiveSessionStream::Decision::Applied &&
      b.frame==128 && b.tracks[0].gain==.75 && b.tracks[1].gain==.25 &&
      b.tracks[0].revision==1 && b.tracks[1].revision==1 && b.tracks[0].count==0 && b.tracks[1].count==0,
      "whole-plan acceptance restarted a held note, changed controllers or mixed revisions");
  unsigned off[2]{};
  while(s.frame()<s.endFrame()) {
    realtime=true; b=s.nextBlock(256); realtime=false;
    for(unsigned i=0;i<2;++i) { require(b.tracks[i].frame==b.frame && b.tracks[i].frames==b.frames,"independent lane clock"); off[i]+=noteCount(b.tracks[i],0x80); }
  }
  require(off[0]==1 && off[1]==1,"held note across swap did not release exactly once per lane");
  const auto cancelled=s.submit(plans(2),2);
  require(s.cancelPending(cancelled) && s.receipt(cancelled).decision==daw::LiveSessionStream::Decision::Cancelled &&
      s.appliedRevision()==1,"whole-plan cancellation mutated accepted revision");
}
void failureAllocation() {
  bool reached=false; unsigned failures=0;
  for(long budget=0;budget<300;++budget) {
    daw::LiveSessionStream s(plans()); auto next=plans(1);
    fail_after=budget;
    try { s.submit(std::move(next),1); fail_after=-1; reached=true; }
    catch(const std::bad_alloc&) { fail_after=-1; ++failures;
      require(!s.hasPendingUpdate() && s.appliedRevision()==0,"allocation failure partially published");
      const auto b=s.nextBlock(1); require(b.tracks[0].gain==.25 && b.tracks[1].gain==.75,"allocation changed active plan");
    }
    if(reached) break;
  }
  require(reached && failures>0,"allocation injection never completed the sweep");
  std::cout<<"whole_plan_preparation_allocation_failures="<<failures<<'\n';
  daw::LiveSessionStream s(plans()); auto missing=plans(); missing.pop_back();
  rejects([&]{s.submit(missing,1);});
  auto duplicate=plans(); duplicate[1].track_id="piano";
  rejects([&]{s.submit(duplicate,1);});
  auto offset=plans(); offset[1].track_delay_us=-20000;
  rejects([&]{s.submit(offset,1);});
  auto pending=s.submit(plans(1),1);
  rejects([&]{s.submit(plans(2),2);});
  require(s.cancelPending(pending),"mailbox backpressure damaged pending candidate");
}
void crossTrackSchedule() {
  auto input=plans();
  for(auto& track:input) {
    track.sequence.events.push_back({400,0x90,65,80,2});
    track.sequence.events.push_back({500,0x80,65,0,2});
    std::stable_sort(track.sequence.events.begin(),track.sequence.events.end(),[](const auto& a,const auto& b){return a.frame<b.frame;});
  }
  daw::LiveSessionStream s(input); s.nextBlock(64);
  for(auto& track:input) {
    for(auto& e:track.sequence.events) if(e.note_id==2) e.frame+=400;
    track.reject_started_onsets={2};
  }
  std::reverse(input.begin(),input.end()); const auto ticket=s.submit(input,1);
  unsigned attacks[2]{},releases[2]{};
  while(s.frame()<1024) {
    realtime=true; const auto b=s.nextBlock(185); realtime=false;
    require(b.revision==1,"future cross-track edit not adopted together");
    for(unsigned i=0;i<2;++i) for(std::size_t j=0;j<b.tracks[i].count;++j) {
      const auto& e=b.tracks[i].events[j]; if(e.note_id!=2) continue;
      if((e.status&0xf0)==0x90) {require(b.frame+e.frame==800,"cross-track onset misaligned");++attacks[i];}
      if((e.status&0xf0)==0x80) {require(b.frame+e.frame==900,"cross-track release misaligned");++releases[i];}
    }
  }
  require(attacks[0]==1 && attacks[1]==1 && releases[0]==1 && releases[1]==1 &&
      s.receipt(ticket).frame==64,"future notes lost/duplicated at whole-plan boundary");
}
struct ImpulseRenderer final: daw::PreparedTrackRenderer {
  explicit ImpulseRenderer(unsigned latency): delay(latency) {}
  unsigned delay;
  std::uint64_t attack=UINT64_MAX;
  daw::RendererLatency latency() const override { return {delay/48000.,0}; }
  std::uint64_t latencyGeneration() const noexcept override { return 0; }
  daw::TrackRenderError render(const daw::TimedMidiEvent* events,std::size_t count,std::uint64_t frame,
      float* out,std::uint32_t frames) noexcept override {
    for(std::size_t i=0;i<count;++i) if((events[i].status&0xf0)==0x90) attack=frame+events[i].frame;
    for(unsigned i=0;i<frames;++i) out[2*i]=out[2*i+1]=
        attack!=UINT64_MAX && frame+i>=attack+delay && frame+i<attack+delay+10 ? .5F:0;
    return daw::TrackRenderError::None;
  }
};
void pdcIntegration() {
  daw::LiveSessionStream s(plans()); ImpulseRenderer piano(0),cello(960);
  daw::ParallelRenderGraph graph({{"piano",&piano,0},{"cello",&cello,0}});
  daw::TrackQuantum quantum[2]; float audio[512]{};
  for(unsigned frame=0;frame<2048;frame+=128) {
    if(frame==128) { auto next=plans(1); s.submit(std::move(next),1); }
    realtime=true;
    auto block=s.nextBlock(128);
    for(unsigned i=0;i<2;++i) quantum[i]={block.tracks[i].events,block.tracks[i].count,block.tracks[i].gain,block.tracks[i].audible};
    const auto ok=graph.render(quantum,2,audio,128); realtime=false;
    require(ok,"session/PDC integration render failed");
    for(unsigned i=0;i<128;++i) require(audio[2*i]==(frame+i>=960 && frame+i<970?.5F:0),
        "whole-plan swap reset buffered audio or retriggered held keys");
  }
}
void concurrentExchange() {
  daw::LiveSessionStream s(plans(0,500000));
  std::atomic<bool> stop{false},ok{true},started{false};
  std::thread audio([&] {
    realtime=true;
    while(!stop.load()) {
      const auto b=s.nextBlock(1);
      for(unsigned i=0;i<2;++i) {
        const auto& track=b.tracks[i];
        if(track.revision!=b.revision || track.frame!=b.frame || track.frames!=b.frames ||
            track.gain!=(i==0 ? (b.revision%2?.75:.25):(b.revision%2?.25:.75))) ok=false;
      }
      started=true;
    }
    if(allocations || frees) ok=false;
    realtime=false;
  });
  while(!started.load()) std::this_thread::yield();
  for(std::uint64_t revision=1;revision<=1000;++revision) {
    if(revision%10==0) {
      auto bad=plans(revision,500000); bad[1].reject_started_onsets={1};
      const auto ticket=s.submit(std::move(bad),revision);
      const auto r=s.waitForDecision(ticket);
      if(r.decision!=daw::LiveSessionStream::Decision::RejectedStartedOnset || r.track_slot!=1) ok=false;
    }
    auto next=plans(revision,500000); if(revision%2) std::reverse(next.begin(),next.end());
    const auto ticket=s.submit(std::move(next),revision);
    if(s.waitForDecision(ticket).decision!=daw::LiveSessionStream::Decision::Applied) ok=false;
  }
  stop=true; audio.join(); require(ok.load(),"mixed revision/gain/frame or invalid ACK under concurrent exchange");
  require(s.liveUpdateCountAfterStop()==1000,"rejected requests incremented applied count");
}
}
void* operator new(std::size_t n) {
  if(realtime) ++allocations;
  if(fail_after==0) throw std::bad_alloc();
  if(fail_after>0) --fail_after;
  if(auto* p=std::malloc(n?n:1)) return p; throw std::bad_alloc();
}
void* operator new[](std::size_t n) {return ::operator new(n);}
void operator delete(void* p) noexcept {if(realtime && p) ++frees; std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
int main() {
  try {
    atomicAdmission(); failureAllocation(); crossTrackSchedule(); pdcIntegration(); concurrentExchange();
    require(!allocations && !frees,"session callback allocated/freed");
    std::cout<<"Whole-session mailbox: stable track IDs, duplicate note/channel namespaces, all-track guards, "
      "preparation allocation failures, cancellation/backpressure, held release, nonempty PDC swap, "
      "1000 accepted + 100 rejected concurrent revisions, zero callback allocations/frees passed\n";
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
