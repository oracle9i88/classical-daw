#pragma once
#include "daw/performance.hpp"
namespace daw {
// Internal acceptance fixture: eight 4/4 bars, one cross-bar tie, two versions,
// stored CC64 pedal and linear CC11 expression curves. Not a musical demo claim.
inline PerformanceDocument performanceFixture(std::vector<std::uint8_t> state) {
  PerformanceDocument d;d.piano_state=std::move(state);
  ScorePart piano{"piano","Eight-bar performance acceptance",{},{}};
  for(int bar=0;bar<8;++bar) {
    ScoreMeasure measure;measure.number=bar+1;measure.start=bar*3840;measure.duration=3840;
    for(int beat=0;beat<4;++beat) {
      ScoreNote n;n.start=measure.start+beat*960;n.duration=720;n.velocity=76;n.midi_channel=0;
      n.pitch.step="CDEG"[beat];n.pitch.octave=4;
      if(bar==1&&beat==3){n.duration=960;n.tie_start=true;}
      if(bar==2&&beat==0){n.pitch.step='G';n.tie_stop=true;}
      measure.notes.push_back(n);
    }
    piano.measures.push_back(measure);
  }
  d.score.parts.push_back(std::move(piano));assignNoteIds(d.score);
  auto base=makePerformance(d.score,"Written timing");
  base.curves={{1,0,64,{{1,0,0},{2,.1,127},{3,.8,127},{4,1,0},{5,2,127},{6,3,0},{7,16,0}}},
               {2,0,11,{{1,0,80},{2,4,120},{3,8,60},{4,12,100},{5,16,90}}}};
  auto expressive=base;expressive.name="Editable performance";
  d.performances={std::move(base),std::move(expressive)};d.active=1;return d;
}
}
