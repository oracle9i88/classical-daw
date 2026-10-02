// Opt-in AU integration check; no CoreAudio output device, no audible playback.
#include "performance_audition.hpp"
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <vector>
#include <cmath>

int main(int argc,char** argv){try {
  if(argc!=2)throw std::invalid_argument("usage: daw_performance_range_probe DOCUMENT");
  const auto d=daw::loadPerformanceDocument(argv[1]);
  constexpr std::size_t from=60001,frames=24000,until=from+frames;
  if(daw::compilePerformance(d.score,d.performances.at(d.active)).frames<until)
    throw std::invalid_argument("probe document must span the 1.25..1.75s check window");
  const auto run=[&](bool seek) {
    daw::PerformanceAudition audition(d);
    if(seek)audition.prepareRange(from,until);
    audition.start();float block[512]{};
    if(!seek)while(audition.frame()<from) {
      audition.render(block,static_cast<std::uint32_t>(std::min<std::size_t>(256,from-audition.frame())));
      if(audition.failed())throw std::runtime_error("reference preroll failed");
    }
    std::vector<float> result;result.reserve(frames*2);
    while(audition.frame()<until) {
      const auto count=static_cast<std::uint32_t>(std::min<std::size_t>(256,until-audition.frame()));
      audition.render(block,count);
      if(audition.failed())throw std::runtime_error("range render failed");
      result.insert(result.end(),block,block+2*count);
    }
    if(seek) {
      if(!audition.done())throw std::runtime_error("range boundary not reached");
      audition.render(block,256);
      for(float f:block)if(f!=0)throw std::runtime_error("range leaked after end");
      if(audition.frame()!=until)throw std::runtime_error("range advanced past end");
    }
    return result;
  };
  auto reference=run(false);const auto positioned=run(true);
  double error=0,energy=0,peak=0;
  for(std::size_t i=0;i<reference.size();++i) {
    const auto frame=i/2;
    const auto fade=std::min(std::min(1.,static_cast<double>(frame+1)/128),std::min(1.,static_cast<double>(frames-frame-1)/128));
    const auto expected=reference[i]*fade;
    error+=std::pow(positioned[i]-expected,2);energy+=expected*expected;
    peak=std::max(peak,std::abs(static_cast<double>(positioned[i])));
  }
  const double ratio=energy>0?std::sqrt(error/energy):1;
  std::cout<<std::setprecision(17)<<"{\"output_devices_opened\":0,\"range_start_frame\":"<<from
    <<",\"range_end_frame\":"<<until<<",\"peak\":"<<peak<<",\"relative_rms_error\":"<<ratio
    <<",\"scope\":\"manually_driven_AU_not_hardware_callback\"}\n";
  if(peak<=1e-5 || ratio>.02)throw std::runtime_error("range AU comparison outside 2% tolerance");
  return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
