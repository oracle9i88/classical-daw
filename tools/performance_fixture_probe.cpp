// Synthetic eight-bar fixture ONLY: specialized isolated-onset assertion.
// Explicit local acceptance. Captures actual CoreAudio callback output, never
// feeds a rendered WAV back to the player. Default speaker output is silenced.
#include "performance_audition.hpp"
#include "coreaudio_output.hpp"
#include "daw/wav.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

namespace fs=std::filesystem;
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return{std::istreambuf_iterator<char>(in),{}};}
daw::AudioBuffer run(const daw::PerformanceDocument& doc,bool audible) {
  daw::PerformanceAudition source(doc,!audible,true);daw::CoreAudioOutput output;std::string error;
  if(!output.setAudioSource(&source,&error)||!output.start(&error))throw std::runtime_error(error);
  source.start();const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(40);
  while(!source.done()&&!source.failed()) {
    if(std::chrono::steady_clock::now()>deadline||!output.checkHealth(&error))throw std::runtime_error("output timeout/failure: "+error);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  output.stop();require(!source.failed()&&source.peakAfterStop()>0&&!source.clippedAfterStop()&&!output.xrunCount(),"callback playback failed, clipped or silent");
  std::cout<<"actual_callback_frames="<<source.frame()<<" peak="<<source.peakAfterStop()<<" latency_seconds="<<source.latency()<<" callback_errors="<<output.xrunCount()<<'\n'<<std::flush;
  return {48000,2,source.capturedAfterStop()};
}
double rms(const daw::AudioBuffer& audio,std::size_t start,std::size_t end){double e=0;for(auto i=start*2;i<end*2;++i)e+=audio.samples[i]*audio.samples[i];return std::sqrt(e/static_cast<double>((end-start)*2));}
}
int main(int argc,char** argv){try{
  const bool audible=argc==4&&std::string(argv[3])=="--audible-edited";
  require(argc==3||audible,"usage: daw_performance_fixture_probe DOCUMENT NEW_DIRECTORY [--audible-edited]");
  const fs::path root(argv[2]);require(!fs::exists(fs::symlink_status(root))&&fs::create_directory(root),"output must be new");
  auto original=daw::loadPerformanceDocument(argv[1]);
  daw::WorkEditor edit(original);edit.setPitch(2,{'F',0,4});edit.set({1,.12,.94,80});edit.setCurvePoint(2,2,105);
  require(edit.undo()&&edit.undo()&&edit.undo(),"unified undo failed");
  require(edit.document().performances[1].notes.empty()&&edit.document().score.parts[0].measures[0].notes[1].pitch.step=='D',"unified undo incorrect");
  require(edit.redo()&&edit.redo()&&edit.redo(),"unified redo failed");
  daw::savePerformanceDocument(edit.document(),(root/"edited").string());
  auto reopened=daw::loadPerformanceDocument((root/"edited").string());
  daw::savePerformanceDocument(reopened,(root/"reopened").string());
  for(const auto* name:{"score.dawproj","performances.dawperformance","piano.aupreset"})
    require(read(root/"edited"/name)==read(root/"reopened"/name),"saved/reopened hard data gate failed");
  const auto before=run(original,false);const auto after=run(edit.document(),audible);const auto restored=run(reopened,false);
  require(before.samples.size()==after.samples.size()&&after.samples.size()==restored.samples.size(),"audio length changed");
  const double early_before=rms(before,960,4800),early_after=rms(after,960,4800);
  require(early_before>1e-5&&early_after<early_before*.01+1e-8&&rms(after,7200,14400)>1e-5,"performed onset edit did not reach callback audio");
  double error=0,energy=0,max_delta=0;
  for(std::size_t i=0;i<after.samples.size();++i){const double delta=after.samples[i]-restored.samples[i];error+=delta*delta;energy+=after.samples[i]*after.samples[i];max_delta=std::max(max_delta,std::abs(delta));}
  const double relative=std::sqrt(error/std::max(energy,1e-20));
  std::string message;
  require(daw::writeWavPcm16(before,(root/"before-callback.wav").string(),&message),"before capture write failed");
  require(daw::writeWavPcm16(after,(root/"edited-callback.wav").string(),&message),"edited capture write failed");
  require(daw::writeWavPcm16(restored,(root/"reopened-callback.wav").string(),&message),"reopened capture write failed");
  std::cout<<"early_rms_before="<<early_before<<" early_rms_after="<<early_after<<" reopened_relative_rms_error="<<relative<<" reopened_max_sample_delta="<<max_delta<<'\n';
  // Declared before measuring: 2% normalized RMS, 0.01 full-scale max error.
  require(relative<=.02&&max_delta<=.01,"reopened audio exceeded declared soft tolerance");
  std::cout<<"PASS actual AU/CoreAudio edit audition, interleaved unified undo/redo, saved score/performance bytes, reopened audio tolerance\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
