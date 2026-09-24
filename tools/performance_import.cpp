// Data-only bridge. Never instantiate a plugin or open an output device.
#include "daw/performance.hpp"
#include "daw/project.hpp"
#include "daw/score_midi.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <algorithm>

int main(int argc, char** argv) {
  std::string stage="arguments";
  try {
    const bool check=argc==3 && std::string(argv[1])=="--check";
    if(!check && argc!=4)throw std::runtime_error("usage: daw_performance_import SCORE.{musicxml,xml,mid,midi,dawproj} PIANO.aupreset NEW_DIRECTORY | --check SCORE");
    const std::filesystem::path source(argv[check?2:1]);
    auto extension=source.extension().string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    daw::PerformanceDocument d;std::string error;
    stage="score_read";bool ok=false;
    daw::MusicXmlImportReport repairs;
    if(extension==".xml"||extension==".musicxml")ok=daw::readMusicXmlFile(source.string(),&d.score,&error,&repairs);
    else if(extension==".mid"||extension==".midi")ok=daw::readMidiScoreFile(source.string(),&d.score,&error);
    else if(extension==".dawproj")ok=daw::readProjectFile(source.string(),&d.score,&error);
    else throw std::runtime_error("unsupported source extension (compressed .mxl is not supported)");
    if(!ok)throw std::runtime_error(error);
    stage="repair";daw::ScoreRepairReport fixes;daw::repairScoreForAudition(d.score,&fixes);
    stage="identity_mapping";daw::assignNoteIds(d.score);
    d.performances.push_back(daw::makePerformance(d.score,"Imported timing"));
    stage="performance_compile";
    const auto sequence=daw::compilePerformance(d.score,d.performances.front());
    if(!check) {
      stage="state_read";
      const auto size=std::filesystem::file_size(argv[2]);
      if(!size||size>16U*1024*1024)throw std::runtime_error("invalid piano state size");
      std::ifstream in(argv[2],std::ios::binary);
      d.piano_state.assign(std::istreambuf_iterator<char>(in),{});
      if(in.bad()||d.piano_state.size()!=size)throw std::runtime_error("state read failed or file changed");
      stage="save";daw::savePerformanceDocument(d,argv[3]);
    }
    std::size_t measures=0,notes=0;
    for(const auto& p:d.score.parts)for(const auto& m:p.measures){++measures;notes+=m.notes.size();}
    std::cout<<"PASS parts="<<d.score.parts.size()<<" measures="<<measures<<" notation_elements="<<notes
             <<" performed_notes="<<d.performances.front().mapping.size()<<" curves=0 frames="<<sequence.frames
             <<" plugins_opened=0 output_devices_opened=0\n";
    std::cout<<"Repairs: repeats_separated="<<fixes.repeats_separated
             <<" overlaps_trimmed="<<fixes.overlaps_trimmed
             <<" overlaps_silenced="<<fixes.overlaps_silenced
             <<" broken_ties_released="<<fixes.broken_tie_chains_released
             <<" grace_notes_timed="<<repairs.grace_notes_timed
             <<" grace_notes_dropped="<<repairs.grace_notes_dropped
             <<" conflicting_tempos_resolved="<<repairs.conflicting_tempos_resolved
             <<" extra_lyrics_dropped="<<repairs.extra_lyrics_dropped<<'\n';
    std::cout<<"Scope: supported written-note timeline, not engraving or repeat/ornament interpretation. No control curves invented; use curve-put to author a lane.\n";
  }catch(const std::exception& e){std::cerr<<"FAIL stage="<<stage<<" reason="<<e.what()<<'\n';return 1;}
}
