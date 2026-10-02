#include "daw/performance_fixture.hpp"
#include "daw/performance_recovery.hpp"
#include "daw/live_performance.hpp"
#include "daw/performance_preroll.hpp"
#include <tuple>
#include "daw/session.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cmath>

namespace fs=std::filesystem;
void need(bool b,const char* why){if(!b)throw std::runtime_error(why);}
template<class F>void rejects(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}need(caught,"invalid operation accepted");}
std::string read(const fs::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
int main(){const auto root=fs::temp_directory_path()/("daw-readiness-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));try{
  fs::create_directory(root);auto d=daw::performanceFixture({1});d.active=0;
  d.score.parts[0].midi_events={{0,daw::MidiChannelEventType::ControlChange,0,64,127,1},
    {960,daw::MidiChannelEventType::ControlChange,0,64,0,2},
    {960,daw::MidiChannelEventType::ControlChange,0,64,127,3},
    {961,daw::MidiChannelEventType::ControlChange,0,64,0,4}};
  d.performances={daw::makePerformance(d.score,"source")};
  const auto events=[&](const daw::PerformanceDocument& doc){
    std::vector<std::pair<std::size_t,int>> result;
    for(auto e:daw::compilePerformance(doc.score,doc.performances[0]).events)
      if(e.status==0xb0&&e.data1==64)result.push_back({e.frame,e.data2});return result;
  };
  {
    auto edge=d;edge.score.bpm=93;
    auto take=daw::makePerformance(edge.score,"fractional endpoint");
    const auto end=daw::compilePerformance(edge.score,take).end_frame;
    const double seconds=static_cast<double>(end)/48000;
    need(seconds*48000>static_cast<double>(end),"endpoint fixture does not exercise FP rounding");
    take.curves={{1,0,64,{{1,0,127},{2,seconds,0}},true}};
    const auto compiled=daw::compilePerformance(edge.score,take);
    bool release=false;for(auto e:compiled.events)if(!e.terminal_reset&&e.frame==end&&e.status==0xb0&&e.data1==64&&e.data2==0)release=true;
    need(release,"quantized endpoint was rejected or moved");
  }
  const auto original=events(d);
  daw::WorkEditor edit(d);daw::CurveAdoptionReport adopted;
  edit.putCurve(daw::curveFromScoreMessages(d.score,0,64,1,&adopted));
  need(edit.document().performances[0].curves[0].stepped,"adoption is not stepped");
  need(adopted.worst_shift_seconds==0&&events(edit.document())==original,"repedal or dense messages changed");
  auto liveSequence=daw::compilePerformance(edit.document().score,edit.document().performances[0]);
  daw::LivePerformanceStream stream(liveSequence,1);
  std::vector<int> repedal;
  for(std::size_t f=0;f<24256;f+=256){auto b=stream.nextBlock(256);for(std::size_t i=0;i<b.count;++i){auto e=b.events[i];
    if(b.frame+e.frame==24000&&e.status==0xb0&&e.data1==64)repedal.push_back(e.data2);}}
  need(repedal==std::vector<int>({0,127}),"live stream lost repedal order");
  // Non-quantized seek replays exactly the same MIDI as uninterrupted output,
  // including pedal-held releases and same-time repedal, without a second attack.
  using Event=std::tuple<std::size_t,int,int,int,std::uint64_t>;
  std::vector<Event> continuous,positioned;
  const auto collect=[](auto& dest,const auto& block){for(std::size_t i=0;i<block.count;++i){const auto& e=block.events[i];
    dest.emplace_back(block.frame+e.frame,e.status,e.data1,e.data2,e.note_id);}};
  daw::LivePerformanceStream baseline(liveSequence,1),seek(liveSequence,1);
  for(int i=0;i<100;++i)collect(continuous,baseline.nextBlock(256));
  daw::prerollPerformance(seek,12801,[&](const auto& block){collect(positioned,block);});
  need(seek.frame()==12801,"seek rounded to a quantum");
  while(seek.frame()<25600)collect(positioned,seek.nextBlock(static_cast<std::uint32_t>(std::min<std::size_t>(256,25600-seek.frame()))));
  need(continuous==positioned,"preroll lost or duplicated ordered MIDI");
  rejects([&]{daw::prerollPerformance(seek,1,[](const auto&){});});
  rejects([&]{daw::prerollPerformance(seek,seek.endFrame(),[](const auto&){});});
  const auto source=root/"original";daw::savePerformanceDocument(d,source.string());
  const auto originalBytes=read(source/"performances.dawperformance");
  need(read(source/"performances.dawperformance").find("CLASSICAL_DAW_PERFORMANCE 1\n")==0,"legacy save format changed");
  daw::savePerformanceDocument(edit.document(),(root/"steps").string());
  need(read(root/"steps"/"performances.dawperformance").find("CLASSICAL_DAW_PERFORMANCE 2\n")==0,"step format not versioned");
  need(events(daw::loadPerformanceDocument((root/"steps").string()))==original,"stored repedal changed");
  need(edit.undo()&&events(edit.document())==original,"adoption undo changed messages");
  need(edit.redo()&&events(edit.document())==original,"adoption redo changed messages");

  daw::PerformanceRecovery journal(source.string()),other(source.string());
  need(journal.directory()!=other.directory(),"two writers share a recovery folder");
  journal.checkpoint(edit.document(),edit.revision());
  const auto first=edit.revision();edit.setGain(-18);journal.checkpoint(edit.document(),edit.revision());
  edit.set({2,.01,1,90});journal.checkpoint(edit.document(),edit.revision());
  need(!fs::exists(fs::path(journal.directory())/("revision-"+std::to_string(first))),"old checkpoint not bounded");
  need(daw::listPerformanceRecoveries(source.string()).size()==1,"empty checkpoint listed");
  const auto recovered=daw::readPerformanceRecovery(source.string(),journal.directory());
  need(recovered.gain_db==-18&&recovered.performances[0].notes.size()==1,"recovery lost accepted edits");
  need(events(recovered)==original,"recovery lost repedal");
  daw::restorePerformanceRecovery(source.string(),journal.directory(),(root/"restored").string());
  rejects([&]{daw::restorePerformanceRecovery(source.string(),journal.directory(),source.string());});
  const auto checksumBefore=read(fs::path(journal.directory())/"latest");
  // Simulate an interrupted write; last complete publication must survive.
  std::ofstream(fs::path(journal.directory())/".latest.tmp")<<"interrupted";
  edit.setGain(-19);rejects([&]{journal.checkpoint(edit.document(),edit.revision());});
  need(read(fs::path(journal.directory())/"latest")==checksumBefore,"failed save replaced the recovery pointer");
  need(daw::readPerformanceRecovery(source.string(),journal.directory()).gain_db==-18,"failed save lost previous checkpoint");
  journal.checkpoint(edit.document(),edit.revision());
  const auto corrupt=fs::path(journal.directory())/("revision-"+std::to_string(edit.revision()))/"performances.dawperformance";
  std::ofstream(corrupt,std::ios::app)<<"corrupt";
  rejects([&]{daw::readPerformanceRecovery(source.string(),journal.directory());});
  need(read(source/"performances.dawperformance")==originalBytes,"source changed");

  std::ofstream(source/"performances.dawperformance",std::ios::app)<<"stale";
  rejects([&]{journal.checkpoint(edit.document(),edit.revision()+1);});
  rejects([&]{daw::readPerformanceRecovery(source.string(),journal.directory());});

  // Output attenuation must happen BEFORE PCM clipping, not after.
  daw::AudioBuffer audio;audio.channels=2;audio.sample_rate=48000;audio.samples={2.F,-2.F,.5F,-.5F};
  const auto mix=daw::applyMasterMix(audio,-12);
  need(std::abs(audio.samples[0]-2*std::pow(10.,-.6))<1e-6&&mix.over_unity_samples==0,"output gain lost float headroom");
  fs::remove_all(root);std::cout<<"performance readiness tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<" fixtures="<<root<<'\n';return 1;}}
