#pragma once
#include "audio_unit_instrument.hpp"
#include "daw/audio_output_source.hpp"
#include "daw/live_performance.hpp"
#include "daw/performance.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace daw {
// One AU and monotonic sample clock for the whole run. The control thread
// compiles immutable plans; the callback reconciles voices at block boundaries.
class PerformanceAudition final : public AudioOutputSource {
 public:
  explicit PerformanceAudition(const PerformanceDocument& d, bool silent=false, bool capture=false,
                               std::uint64_t revision=0)
      : au_(InstrumentKind::Pianoteq9), stream_(compilePerformance(d.score,d.performances.at(d.active)),
          std::pow(10.,d.gain_db/20),revision),
        state_(d.piano_state), take_(d.active), accepted_notes_(d.performances.at(d.active).notes),
        gain_(std::pow(10.,d.gain_db/20)), silent_(silent) {
    au_.restoreState(state_); au_.prepareRealtime();
    if(capture) { captured_.resize(stream_.endFrame()*2); audit_.resize(262144); }
  }
  bool acceptsFormat(double rate,std::uint32_t channels) const noexcept override {return rate==48000 && channels==2;}
  void start() noexcept {armed_.store(true,std::memory_order_release);}
  // Control only. Wait for the block-boundary decision before WorkEditor commits.
  // Rejection leaves the playing plan intact, including unrelated fields in the
  // candidate. A processed onset stays locked after release or conflict suppression.
  LivePerformanceStream::Receipt submit(const PerformanceDocument& d, std::uint64_t revision) {
    if(d.active!=take_ || d.piano_state!=state_) throw std::invalid_argument("stop playback before changing performance or instrument state");
    if(failed()) throw std::runtime_error("audition has failed");
    auto sequence=compilePerformance(d.score,d.performances.at(d.active));
    if(!captured_.empty() && sequence.frames>captured_.size()/2)
      throw std::length_error("live plan exceeds this probe's fixed capture capacity");
    auto next_notes=d.performances.at(d.active).notes;
    const auto guarded=daw::changedPerformanceOnsets(accepted_notes_,next_notes);
    const auto ticket=stream_.submit(std::move(sequence),std::pow(10.,d.gain_db/20),revision,guarded);
    const auto result=stream_.waitForDecision(ticket);
    if(result.decision==LivePerformanceStream::Decision::RejectedStartedOnset)
      throw std::invalid_argument("performed note "+std::to_string(result.note_id)+
        " onset has already been processed in this playback pass; onset edit was not saved. Stop playback to change its onset.");
    if(result.decision!=LivePerformanceStream::Decision::Applied)
      throw std::runtime_error("live edit cancelled before audio acceptance; playback did not acknowledge it");
    accepted_notes_.swap(next_notes); // no fallible work after acceptance
    return result;
  }
  void render(float* out,std::uint32_t frames) noexcept override {
    std::fill(out,out+frames*2,0.F);
    if(!armed_.load(std::memory_order_acquire)||failed_.load())return;
    const auto started=std::chrono::steady_clock::now();
    const auto block=stream_.nextBlock(frames);
    if(stream_.failed()){failed_.store(true);return;}
    if(!block.frames)return;
    if(!captured_.empty() && (block.frame>captured_.size()/2 || block.frames>captured_.size()/2-block.frame)){
      failed_.store(true);return;
    }
    if(!audit_.empty() && block.count>audit_.size()-audit_size_){failed_.store(true);return;}
    if(!au_.renderRealtime(block.events,block.count,out,block.frames)){failed_.store(true);return;}
    if(!audit_.empty())for(std::size_t i=0;i<block.count;++i){
      auto event=block.events[i];event.frame+=block.frame;audit_[audit_size_++]=event;
    }
    for(std::size_t frame=0;frame<block.frames;++frame) {
      const auto gain=gain_+(block.gain-gain_)*static_cast<double>(frame+1)/block.frames;
      for(std::size_t channel=0;channel<2;++channel) {
        const auto i=frame*2+channel;
        out[i]=static_cast<float>(out[i]*gain);peak_=std::max(peak_,std::abs(static_cast<double>(out[i])));
        if(std::abs(out[i])>1){++clipped_;out[i]=std::clamp(out[i],-1.F,1.F);}
        if(!captured_.empty())captured_[block.frame*2+i]=out[i];
        if(silent_)out[i]=0;
      }
    }
    gain_=block.gain;
    published_.store(block.frame+block.frames,std::memory_order_release);
    const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    const auto ratio=seconds/(static_cast<double>(frames)/48000);
    worst_budget_ratio_=std::max(worst_budget_ratio_,ratio);if(ratio>=1)++deadline_misses_;
  }
  bool done() const noexcept {return !stream_.hasPendingUpdate() && published_.load(std::memory_order_acquire)>=stream_.endFrame();}
  bool failed() const noexcept {return failed_.load()||stream_.failed();}
  std::size_t frame() const noexcept {return published_.load(std::memory_order_acquire);}
  double latency() const noexcept {return au_.realtimeLatencySeconds();}
  std::uint64_t appliedRevision() const noexcept {return stream_.appliedRevision();}
  std::size_t appliedFrame() const noexcept {return stream_.appliedFrame();}
  // Statistics/capture accessed only AFTER CoreAudio has stopped/joined.
  double peakAfterStop() const noexcept {return peak_;}
  std::uint64_t clippedAfterStop() const noexcept {return clipped_;}
  std::uint64_t suppressedConflictsAfterStop() const noexcept {return stream_.suppressedConflictsAfterStop();}
  std::uint64_t liveUpdateCountAfterStop() const noexcept {return stream_.liveUpdateCountAfterStop();}
  double worstBudgetRatioAfterStop() const noexcept {return worst_budget_ratio_;}
  std::uint64_t deadlineMissesAfterStop() const noexcept {return deadline_misses_;}
  const std::vector<float>& capturedAfterStop() const noexcept {return captured_;}
  std::vector<TimedMidiEvent> eventsAfterStop() const {return {audit_.begin(),audit_.begin()+static_cast<std::ptrdiff_t>(audit_size_)};}
 private:
  AudioUnitInstrument au_;LivePerformanceStream stream_;
  std::vector<std::uint8_t> state_;std::size_t take_;
  std::vector<NotePerformance> accepted_notes_; // control thread only
  double gain_,peak_=0,worst_budget_ratio_=0;bool silent_;
  std::vector<float> captured_;std::vector<TimedMidiEvent> audit_;std::size_t audit_size_=0;
  std::uint64_t clipped_=0,deadline_misses_=0;
  std::atomic<std::size_t> published_{0};std::atomic<bool> armed_{false},failed_{false};
};
}
