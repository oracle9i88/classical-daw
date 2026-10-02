#pragma once
#include "audio_unit_track_renderer.hpp"
#include "audio_unit_runtime.hpp"
#include "daw/session_render_source.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace daw {
// Saved multi-part document -> prepared two-AU graph. One control producer,
// one audio consumer, and one editor admission. Output must stop/join first.
class EnsembleAudition final:public AudioOutputSource {
 public:
  explicit EnsembleAudition(const PerformanceDocument& d,bool silent=false,std::uint64_t revision=0)
    : routes_(d.routes),take_(d.active),accepted_notes_(d.performances.at(d.active).notes),silent_(silent){
    validatePerformanceDocument(d);
    if(routes_.empty() || routes_.size()>2)throw std::invalid_argument("realtime ensemble supports one or two explicit routes; larger arrangements require the separate offline/frozen player, not this editor");
    auto plans=planPerformanceDocument(d);
    // All saved-state/range checks precede any plugin creation.
    for(std::size_t i=0;i<routes_.size();++i)validateSequence(routes_[i],plans[i].sequence);
    std::vector<RenderTrackBinding> bindings;
    for(std::size_t i=0;i<routes_.size();++i){
      auto renderer=std::make_unique<AudioUnitTrackRenderer>(kind(routes_[i]),routes_[i].state,plans[i].sequence);
      bindings.push_back({routes_[i].part_id,renderer.get(),routes_[i].track_delay_us});renderers_.push_back(std::move(renderer));
      for(const auto& e:plans[i].sequence.events)if((e.status&0xf0)==0x90 && e.data2)note_routes_.emplace(e.note_id,routes_[i].part_id);
    }
    source_=std::make_unique<SessionRenderSource>(std::move(plans),std::move(bindings),revision);
  }
  bool acceptsFormat(double r,std::uint32_t c)const noexcept override{return source_->acceptsFormat(r,c);}
  void start()noexcept{armed_.store(true,std::memory_order_release);}
  void render(float* out,std::uint32_t n)noexcept override{
    if(!armed_.load(std::memory_order_acquire)){std::fill_n(out,n*2,0.F);return;}
    source_->render(out,n);
    for(std::size_t i=0;i<n*2;++i){
      peak_=std::max(peak_,std::abs(static_cast<double>(out[i])));
      if(std::abs(out[i])>1){++clipped_;out[i]=std::clamp(out[i],-1.F,1.F);}
      if(silent_)out[i]=0;
    }
  }
  LiveSessionStream::Receipt submit(const PerformanceDocument& d,std::uint64_t revision){
    if(d.active!=take_ || d.routes.size()!=routes_.size())throw std::invalid_argument("stop playback before changing take or route topology");
    if(failed())throw std::runtime_error("ensemble audition failed; stop and rebuild");
    auto plans=planPerformanceDocument(d);
    auto next_notes=d.performances.at(d.active).notes;
    const auto guards=changedPerformanceOnsets(accepted_notes_,next_notes);
    for(auto& plan:plans){
      const auto route=std::find_if(routes_.begin(),routes_.end(),[&](const auto& r){return r.part_id==plan.track_id;});
      const auto candidate=std::find_if(d.routes.begin(),d.routes.end(),[&](const auto& r){return r.part_id==plan.track_id;});
      if(route==routes_.end() || candidate==d.routes.end() || candidate->instrument!=route->instrument || candidate->state!=route->state)
        throw std::invalid_argument("stop playback before changing an instrument or saved state");
      validateSequence(*candidate,plan.sequence);
      for(auto id:guards){const auto at=note_routes_.find(id);if(at==note_routes_.end())throw std::invalid_argument("live note identity changed; stop first");if(at->second==plan.track_id)plan.reject_started_onsets.push_back(id);}
    }
    const auto ticket=source_->submit(std::move(plans),revision);
    const auto result=source_->waitForDecision(ticket);
    if(result.decision==LiveSessionStream::Decision::RejectedStartedOnset)
      throw std::invalid_argument("performed note "+std::to_string(result.note_id)+" onset has already been processed; stop playback to change it. Edit was not saved.");
    if(result.decision!=LiveSessionStream::Decision::Applied)throw std::runtime_error("ensemble edit was cancelled before audio acceptance");
    accepted_notes_.swap(next_notes);return result;
  }
  bool done()const noexcept{return source_->done();}
  bool failed()const noexcept{return source_->failed();}
  std::uint64_t frame()const noexcept{return source_->frame();}
  double latency()const noexcept{return source_->latencyFrames()/48000.;}
  std::uint64_t appliedRevision()const noexcept{return source_->appliedRevision();}
  std::size_t appliedFrame()const noexcept{return source_->appliedFrame();}
  std::uint64_t suppressedConflictsAfterStop()const noexcept{return source_->suppressedConflictsAfterStop();}
  std::string statusText()const{return source_->graph().statusText();}
  std::vector<TrackRenderStatistics> trackStatisticsAfterStop()const {std::vector<TrackRenderStatistics> result;for(const auto& r:renderers_)result.push_back(r->statisticsAfterStop());return result;}
  double peakAfterStop()const noexcept{return peak_;}
  std::uint64_t clippedAfterStop()const noexcept{return clipped_;}
 private:
  static InstrumentKind kind(const PerformanceRoute& r){
    if(r.instrument=="pianoteq")return InstrumentKind::Pianoteq9;
    if(r.instrument=="swam-cello")return InstrumentKind::SwamCello3;
    throw std::invalid_argument("unsupported realtime instrument");
  }
  static void validateSequence(const PerformanceRoute& r,const MidiSampleSequence& s){
    if(kind(r)!=InstrumentKind::SwamCello3)return;
    requireInitialExpression(s);const auto transpose=swamCelloStateTranspose(r.state);
    for(const auto& e:s.events)if((e.status&0xf0)==0x90 && e.data2){const auto pitch=static_cast<int>(e.data1)+transpose;
      if(pitch<36 || pitch>89)throw std::invalid_argument("SWAM note outside saved-state range; edit was not saved");}
  }
  std::vector<PerformanceRoute> routes_;std::size_t take_;
  std::vector<NotePerformance> accepted_notes_;
  std::map<std::uint64_t,std::string> note_routes_;
  std::vector<std::unique_ptr<AudioUnitTrackRenderer>> renderers_;
  std::unique_ptr<SessionRenderSource> source_; // destroyed before renderers
  std::atomic<bool> armed_{false};bool silent_;
  double peak_=0;std::uint64_t clipped_=0;
};
}
