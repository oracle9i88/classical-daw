#pragma once
#include "audio_unit_instrument.hpp"
#include "daw/audio_output_source.hpp"
#include "daw/performance.hpp"
#include <array>
#include <atomic>
#include <cmath>

namespace daw {
// Prepared/captured on control thread; callback owns the AU and immutable plan.
// Editing destroys this run only AFTER output stops, then compiles a fresh run.
class PerformanceAudition final : public AudioOutputSource {
 public:
  explicit PerformanceAudition(const PerformanceDocument& d, bool silent=false, bool capture=false)
      : au_(InstrumentKind::Pianoteq9), sequence_(compilePerformance(d.score,d.performances.at(d.active))),
        gain_(std::pow(10.,d.gain_db/20)), silent_(silent) {
    au_.restoreState(d.piano_state);au_.prepareRealtime();
    if(capture)captured_.resize(sequence_.frames*2);
  }
  bool acceptsFormat(double rate,std::uint32_t channels) const noexcept override {return rate==48000 && channels==2;}
  void start() noexcept {armed_.store(true,std::memory_order_release);}
  void render(float* out,std::uint32_t frames) noexcept override {
    std::fill(out,out+frames*2,0.F);
    if(!armed_.load(std::memory_order_acquire)||failed_.load()||position_>=sequence_.frames)return;
    const auto count=static_cast<std::uint32_t>(std::min<std::size_t>(frames,sequence_.frames-position_));
    std::size_t messages=0;
    while(next_<sequence_.events.size() && sequence_.events[next_].frame<position_+count) {
      if(messages==block_.size()){failed_.store(true);return;}
      block_[messages]=sequence_.events[next_++];block_[messages++].frame-=position_;
    }
    if(!au_.renderRealtime(block_.data(),messages,out,count)){failed_.store(true);return;}
    for(std::size_t i=0;i<count*2;++i) {
      out[i]=static_cast<float>(out[i]*gain_);peak_=std::max(peak_,std::abs(static_cast<double>(out[i])));
      if(std::abs(out[i])>1){++clipped_;out[i]=std::clamp(out[i],-1.F,1.F);}
      if(!captured_.empty())captured_[position_*2+i]=out[i];
      if(silent_)out[i]=0;
    }
    position_+=count; published_.store(position_,std::memory_order_release);
  }
  bool done() const noexcept {return published_.load(std::memory_order_acquire)>=sequence_.frames;}
  bool failed() const noexcept {return failed_.load();}
  std::size_t frame() const noexcept {return published_.load();}
  double latency() const noexcept {return au_.realtimeLatencySeconds();}
  // Statistics/capture accessed only AFTER CoreAudio has stopped/joined.
  double peakAfterStop() const noexcept {return peak_;}
  std::uint64_t clippedAfterStop() const noexcept {return clipped_;}
  const std::vector<float>& capturedAfterStop() const noexcept {return captured_;}
 private:
  AudioUnitInstrument au_;MidiSampleSequence sequence_;
  std::array<TimedMidiEvent,4096> block_{};
  double gain_,peak_=0;bool silent_;
  std::vector<float> captured_;
  std::size_t position_=0,next_=0;std::uint64_t clipped_=0;
  std::atomic<std::size_t> published_{0};std::atomic<bool> armed_{false},failed_{false};
};
}
