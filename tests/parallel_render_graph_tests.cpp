#include "daw/parallel_render_graph.hpp"
#include "daw/output_blocks.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>

namespace {
thread_local bool realtime = false;
thread_local unsigned allocations = 0, frees = 0;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class F> void rejects(F f) {
  bool rejected = false;
  try { f(); } catch (const std::exception&) { rejected = true; }
  require(rejected, "invalid graph admitted");
}
// Independent absolute-time oracle: no ring/delay implementation shared with
// the host. Sparse asymmetric stereo samples expose swaps, wraps and boundaries.
float source(std::uint64_t frame, unsigned channel, unsigned lane) {
  if (frame >= 4099) return 0;
  const bool impulse = frame % 173 == 0 || frame % 557 == 556 || frame % 557 == 1 || frame == 4098;
  return impulse ? static_cast<float>((lane + 1) * (channel + 1)) * .0625F : 0;
}
struct Delayed final : daw::PreparedTrackRenderer {
  explicit Delayed(unsigned n, unsigned i = 0) : delay(n), index(i) {}
  unsigned delay, index;
  double fraction = 0;
  std::atomic<std::uint64_t> generation{0};
  std::uint64_t fail_at = UINT64_MAX, change_at = UINT64_MAX;
  bool nan = false;
  unsigned calls = 0;
  daw::RendererLatency latency() const override { return {(delay + fraction) / 48000., generation.load()}; }
  std::uint64_t latencyGeneration() const noexcept override { return generation.load(); }
  daw::TrackRenderError render(const daw::TimedMidiEvent*, std::size_t,
      std::uint64_t frame, float* out, std::uint32_t frames) noexcept override {
    ++calls;
    if (frame >= change_at) generation.fetch_add(1);
    if (frame >= fail_at) {
      if (nan) { out[0] = std::numeric_limits<float>::quiet_NaN(); return daw::TrackRenderError::None; }
      return daw::TrackRenderError::ReturnedError;
    }
    for (unsigned f = 0; f < frames; ++f)
      for (unsigned c = 0; c < 2; ++c)
        out[2*f+c] = frame+f < delay ? 0 : source(frame+f-delay,c,index);
    return daw::TrackRenderError::None;
  }
};
void matrixCase(const std::vector<unsigned>& latencies, const std::vector<unsigned>& requests, int only_lane) {
  std::vector<std::unique_ptr<Delayed>> renderers;
  std::vector<daw::RenderTrackBinding> bindings;
  std::vector<daw::TrackQuantum> inputs(latencies.size());
  const unsigned L = *std::max_element(latencies.begin(), latencies.end());
  for (unsigned i = 0; i < latencies.size(); ++i) {
    renderers.push_back(std::make_unique<Delayed>(latencies[i], i));
    bindings.push_back({"part-"+std::to_string(i), renderers.back().get(), 0});
    inputs[i].audible = only_lane < 0 || static_cast<unsigned>(only_lane) == i;
  }
  daw::ParallelRenderGraph graph(std::move(bindings));
  require(graph.latencyFrames() == L, "wrong L");
  for (unsigned i = 0; i < latencies.size(); ++i)
    require(graph.latencyInfo()[i].compensation_frames == L-latencies[i], "wrong max-minus-own");
  std::array<float, 558*2> audio{};
  std::uint64_t frame = 0; unsigned request = 0;
  const std::uint64_t end = 4099 + L + 101; // includes partial EOF and complete PDC drain
  while (frame < end) {
    const auto n = static_cast<unsigned>(std::min<std::uint64_t>(requests[request++ % requests.size()], end-frame));
    bool ok = true; realtime = true;
    daw::renderOutputBlocks(audio.data(), n, 2, 256, [&](float* p, unsigned f) noexcept {
      ok = graph.render(inputs.data(), inputs.size(), p, f) && ok;
    });
    realtime = false;
    require(ok, "matrix render failed");
    for (unsigned f = 0; f < n; ++f) for (unsigned c = 0; c < 2; ++c) {
      float expected = 0;
      if (frame+f >= L) for (unsigned i = 0; i < latencies.size(); ++i)
        if (inputs[i].audible) expected += source(frame+f-L,c,i);
      require(audio[f*2+c] == expected, "sample position/value differs from independent timeline");
    }
    frame += n;
  }
  require(graph.frame() == end, "partial end mismatch");
}
void faults() {
  Delayed a(173), b(700,1); b.fail_at = 370;
  daw::ParallelRenderGraph graph({{"healthy",&a,0},{"faulty",&b,0}});
  daw::TrackQuantum inputs[2]; float out[512]{};
  for (unsigned frame = 0; frame < 2220; frame += 185) {
    realtime = true; const bool ok = graph.render(inputs,2,out,185); realtime = false;
    require(ok, "one lane error stopped graph");
    if (frame >= 370) {
      const auto status = graph.trackStatus(1);
      require(status.quarantined() && status.failure_frame == 370, "missing pollable failure identity");
      for (unsigned f = 0; f < 185; ++f) for (unsigned c = 0; c < 2; ++c)
        require(out[2*f+c] == (frame+f < 700 ? 0 : source(frame+f-700,c,0)), "failed lane shifted/contaminated healthy lane");
    }
  }
  require(b.calls == 3 && a.calls == 12 && graph.latencyFrames() == 700, "quarantine called again or changed L");
  require(graph.statusText().find("track=\"faulty\" reported_seconds=") != std::string::npos &&
          graph.statusText().find("quarantined=1") != std::string::npos, "status lost fault");

  // A fast faulty track already has audio inside its host compensation line.
  // Quarantine must discard that buffered audio, not leak it on later quanta.
  Delayed fast(0), slow(700,1); fast.fail_at=185; fast.nan=true;
  daw::ParallelRenderGraph buffered({{"fast",&fast,0},{"slow",&slow,0}});
  for (unsigned frame=0; frame<1480; frame+=185) {
    require(buffered.render(inputs,2,out,185),"NaN lane stopped healthy renderer");
    for (unsigned f=0; f<185; ++f) for (unsigned c=0; c<2; ++c)
      require(out[2*f+c] == (frame+f < 700 ? 0 : source(frame+f-700,c,1)),"quarantined delay leaked old audio");
  }
  require(buffered.trackStatus(0).error==daw::TrackRenderError::NonfiniteOutput,"NaN not classified");

  Delayed x(0), y(173); y.change_at=0;
  daw::ParallelRenderGraph changing({{"x",&x,0},{"y",&y,0}});
  require(!changing.render(inputs,2,out,185) && changing.fault()==daw::RenderGraphFault::LatencyChanged,"mid-render latency notification ignored");
  require(std::all_of(out,out+370,[](float f){return f==0;}),"invalid alignment block escaped");
  const auto calls=x.calls;
  require(!changing.render(inputs,2,out,185) && x.calls==calls,"dirty graph continued rendering");

  Delayed p(0), q(0); daw::ParallelRenderGraph before({{"p",&p,0},{"q",&q,0}});
  q.generation.fetch_add(1);
  require(!before.render(inputs,2,out,185) && p.calls==0 && q.calls==0,"pre-render latency invalidation was late");

  Delayed m(0), n(0); daw::ParallelRenderGraph validation({{"m",&m,0},{"n",&n,0}});
  inputs[1].gain=std::numeric_limits<double>::quiet_NaN();
  require(!validation.render(inputs,2,out,185) && m.calls==0 && n.calls==0,"second lane validation partially rendered first");
}
void planChangeAndMute() {
  Delayed a(0), b(700,1);
  daw::ParallelRenderGraph graph({{"a",&a,0},{"b",&b,0}});
  std::array<float, 512> out{}; daw::TrackQuantum old[2], next[2];
  next[1].audible=false; // a new complete quantum; L still includes the silent lane
  require(graph.render(old,2,out.data(),100),"initial render");
  // Nonempty host delay retains initial impulses across this control change.
  for (unsigned frame=100;frame<2100;frame+=100) {
    require(graph.render(next,2,out.data(),100),"plan render");
    for(unsigned f=0;f<100;++f) for(unsigned c=0;c<2;++c)
      require(out[f*2+c]==(frame+f<700?0:source(frame+f-700,c,0)),"control change reset delay or changed L");
  }
  require(graph.latencyFrames()==700 && b.calls==21,"muted renderer clock did not advance");
}
void validation() {
  Delayed a(0), b(0);
  rejects([&]{daw::ParallelRenderGraph g({{"a",&a,0},{"a",&b,0}});});
  rejects([&]{daw::ParallelRenderGraph g({{"a",&a,0},{"b",&a,0}});});
  rejects([&]{daw::ParallelRenderGraph g({{"a",&a,-1}});});
  rejects([&]{daw::ParallelRenderGraph g({{"a",&a,1}});});
  b.fraction=.5; rejects([&]{daw::ParallelRenderGraph g({{"b",&b,0}});});
  b.fraction=std::numeric_limits<double>::infinity(); rejects([&]{daw::ParallelRenderGraph g({{"b",&b,0}});});
  b.fraction=0; b.delay=96001; rejects([&]{daw::ParallelRenderGraph g({{"b",&b,0}});});
}
void concurrentStatus() {
  Delayed a(0), b(960); b.fail_at=256*100;
  daw::ParallelRenderGraph graph({{"a",&a,0},{"b",&b,0}});
  std::atomic<bool> done{false}; std::atomic<bool> good{true};
  std::thread audio([&] {
    daw::TrackQuantum input[2]; float out[512]{}; realtime=true;
    for(unsigned i=0;i<2000;++i) if(!graph.render(input,2,out,256)) good=false;
    if(allocations || frees) good=false;
    realtime=false; done.store(true,std::memory_order_release);
  });
  do {
    const auto status=graph.trackStatus(1);
    if(status.quarantined()) require(status.failure_frame==256*100,"torn polled fault");
    (void)graph.frame();
  } while(!done.load(std::memory_order_acquire));
  audio.join(); require(good.load(),"concurrent render or allocation failure");
}
}
void* operator new(std::size_t n) { if(realtime) ++allocations; if(auto* p=std::malloc(n?n:1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { if(realtime && p) ++frees; std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p,std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,std::size_t) noexcept { ::operator delete(p); }
int main() {
  try {
    const std::vector<std::vector<unsigned>> cases{{0,0},{0,128},{0,173},{0,700},{173,512},{173,512,700},{0,960}};
    for(const auto& c:cases) for(int only=-1;only<static_cast<int>(c.size());++only) {
      for(unsigned n:{1U,185U,186U,256U,512U,557U,558U}) matrixCase(c,{n},only);
      matrixCase(c,{557,1,186,512,185,558,256},only);
    }
    faults(); planChangeAndMute(); validation(); concurrentStatus();
    require(!allocations && !frees,"host render allocated or freed");
    std::cout<<"PDC: 7 latency layouts, isolated lanes + mix, 8 callback patterns, exact stereo samples, "
               "partial EOF/drain, nonempty-delay control change, fixed L, fault quarantine, latency invalidation, "
               "pollable status and zero callback allocations/frees passed\n";
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
