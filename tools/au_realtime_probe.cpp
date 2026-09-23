// Opt-in real hardware / Pianoteq spike, deliberately independent of Score.
#include "audio_unit_instrument.hpp"
#include "coreaudio_output.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

namespace {
class Probe final : public daw::AudioOutputSource {
 public:
  explicit Probe(daw::AudioUnitInstrument& au) : au_(au) {
    for (std::size_t second=0;second<30;++second) {
      events.push_back({second*48000,0xb0,64,static_cast<std::uint8_t>(second%2 ? 0 : 127)});
      for (std::size_t i=0;i<20;++i) events.push_back({second*48000+i*2400,0xb0,11,static_cast<std::uint8_t>(50+(second*20+i)%70)});
      events.push_back({second*48000+137,0x90,static_cast<std::uint8_t>(60+second%12),80});
      events.push_back({second*48000+24013,0x80,static_cast<std::uint8_t>(60+second%12),32});
    }
    std::stable_sort(events.begin(),events.end(),[](auto a,auto b){return a.frame<b.frame;});
  }
  bool acceptsFormat(double rate,std::uint32_t channels) const noexcept override {return rate==48000 && channels==2;}
  void render(float* out,std::uint32_t frames) noexcept override {
    if (!armed.load()) {std::fill(out,out+frames*2,0.F);return;}
    const auto start=std::chrono::steady_clock::now();
    std::array<daw::TimedMidiEvent,256> block{};std::size_t n=0;
    while(next<events.size() && events[next].frame<position+frames) {
      if(n==block.size()){failed.store(true);break;}
      block[n]=events[next++];block[n++].frame-=position;
    }
    if(!au_.renderRealtime(block.data(),n,out,frames)) failed.store(true);
    for(std::size_t i=0;i<frames*2;++i){peak=std::max(peak,std::abs(static_cast<double>(out[i])));out[i]=0;}
    position+=frames; sent+=n;
    const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    max_seconds=std::max(max_seconds,seconds);if(seconds>static_cast<double>(frames)/48000)++deadline_misses;
    frame.store(position);
  }
  daw::AudioUnitInstrument& au_;std::vector<daw::TimedMidiEvent> events;
  std::atomic<bool> armed{false},failed{false};std::atomic<std::size_t> frame{0};
  std::size_t position=0,next=0,sent=0,deadline_misses=0;double peak=0,max_seconds=0;
};
}
int main(int argc,char** argv) {
  try {
    if(argc!=2)throw std::runtime_error("usage: daw_au_realtime_probe PIANO.aupreset");
    const auto size=std::filesystem::file_size(argv[1]);if(!size || size>16U*1024*1024)throw std::runtime_error("invalid state size");
    std::ifstream in(argv[1],std::ios::binary);const std::vector<std::uint8_t> state{std::istreambuf_iterator<char>(in),{}};
    daw::AudioUnitInstrument au(daw::InstrumentKind::Pianoteq9);au.restoreState(state);au.prepareRealtime();
    Probe probe(au);daw::CoreAudioOutput output;std::string error;
    if(!output.setAudioSource(&probe,&error)||!output.start(&error))throw std::runtime_error(error);
    probe.armed.store(true);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(35);
    while(probe.frame.load()<30U*48000 && !probe.failed.load()) {
      if(std::chrono::steady_clock::now()>deadline || !output.checkHealth(&error))throw std::runtime_error("output failure/timeout: "+error);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    output.stop();
    std::cout<<"frames="<<probe.position<<" sent="<<probe.sent<<" peak="<<probe.peak<<" callback_errors="<<output.xrunCount()
             <<" deadline_misses="<<probe.deadline_misses<<" max_callback_seconds="<<probe.max_seconds
             <<" reported_plugin_latency_seconds="<<au.realtimeLatencySeconds()<<'\n';
    if(probe.failed.load() || probe.peak<=0 || probe.sent!=probe.events.size() || output.xrunCount() || probe.deadline_misses)
      throw std::runtime_error("real AU callback acceptance failed");
    std::cout<<"PASS 30 seconds real Pianoteq in CoreAudio callbacks, note/release offsets, CC64 and CC11; speaker output silenced\n";
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
