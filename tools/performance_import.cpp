// Data-only bridge. Never instantiate a plugin or open an output device.
#include "daw/performance.hpp"
#include "daw/import_receipt.hpp"
#include "daw/project.hpp"
#include "daw/score_midi.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
#include <algorithm>

int main(int argc, char** argv) {
  std::string stage="arguments";
  try {
    // One part at a time is not multi-instrument support and does not pretend
    // to be. It is the difference between a quartet or a two-staff export being
    // unopenable and being work you can do a line at a time today.
    std::size_t wanted=0;
    std::vector<std::string> plain;
    for(int i=1;i<argc;++i) {
      if(std::string(argv[i])=="--part"&&i+1<argc){wanted=std::stoul(argv[++i]);continue;}
      plain.push_back(argv[i]);
    }
    const bool check=!plain.empty() && plain.front()=="--check";
    if(check?plain.size()!=2:plain.size()!=3)
      throw std::runtime_error("usage: daw_performance_import SCORE.{musicxml,xml,mid,midi,dawproj} PIANO.aupreset NEW_DIRECTORY [--part N] | --check SCORE [--part N]");
    const std::filesystem::path source(plain.at(check?1:0));
    auto extension=source.extension().string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    daw::PerformanceDocument d;std::string error;
    stage="source_fingerprint";const auto source_fingerprint=daw::fingerprintImportSource(source.string());
    stage="score_read";bool ok=false;
    daw::MusicXmlImportReport repairs;daw::ScoreRepairReport fixes;
    const bool is_xml=extension==".xml"||extension==".musicxml";
    if(is_xml)ok=daw::readMusicXmlFile(source.string(),&d.score,&error,&repairs,wanted);
    else if(extension==".mid"||extension==".midi")ok=daw::readMidiScoreFile(source.string(),&d.score,&error,&fixes);
    else if(extension==".dawproj")ok=daw::readProjectFile(source.string(),&d.score,&error);
    else throw std::runtime_error("unsupported source extension (compressed .mxl is not supported)");
    if(!ok)throw std::runtime_error(error);
    stage="select_part";
    if(wanted!=0 && !is_xml) {
      daw::selectScorePart(d.score,wanted);
    } else if(d.score.parts.size()>1) {
      std::string names;
      for(std::size_t i=0;i<d.score.parts.size();++i)
        names+="\n  --part "+std::to_string(i+1)+"  "+d.score.parts[i].id+"  "+d.score.parts[i].name;
      throw std::runtime_error("this score has "+std::to_string(d.score.parts.size())+
        " parts and the audition plays one. Choose one:"+names);
    }
    daw::ScoreRepairReport audition_repairs;std::vector<daw::ScoreRepairChange> changes;
    stage="repair";daw::repairScoreForAudition(d.score,&audition_repairs,&changes);
    stage="identity_mapping";daw::assignNoteIds(d.score);
    d.performances.push_back(daw::makePerformance(d.score,"Imported timing"));
    stage="performance_compile";
    const auto sequence=daw::compilePerformance(d.score,d.performances.front());
    // Ask exactly what a save asks, so --check cannot report a score that
    // only fails when someone tries to keep it.
    stage="validate";
    if(!daw::validateScore(d.score,&error))throw std::runtime_error(error);
    stage="import_receipt";
    const auto after_read=daw::fingerprintImportSource(source.string());
    if(after_read.bytes!=source_fingerprint.bytes||after_read.fnv1a64!=source_fingerprint.fnv1a64)
      throw std::runtime_error("source changed during import; retry with an unchanged file");
    d.import_receipt=daw::makeImportReceipt(source_fingerprint,wanted,repairs,fixes,audition_repairs,changes,d.score);
    if(!check) {
      stage="state_read";
      const auto size=std::filesystem::file_size(plain.at(1));
      if(!size||size>16U*1024*1024)throw std::runtime_error("invalid piano state size");
      std::ifstream in(plain.at(1),std::ios::binary);
      d.piano_state.assign(std::istreambuf_iterator<char>(in),{});
      if(in.bad()||d.piano_state.size()!=size)throw std::runtime_error("state read failed or file changed");
      stage="save";daw::savePerformanceDocument(d,plain.at(2));
    }
    std::size_t measures=0,notes=0;
    for(const auto& p:d.score.parts)for(const auto& m:p.measures){++measures;notes+=m.notes.size();}
    std::cout<<"PASS parts="<<d.score.parts.size()<<" measures="<<measures<<" notation_elements="<<notes
             <<" performed_notes="<<d.performances.front().mapping.size()<<" curves=0 frames="<<sequence.frames
             <<" plugins_opened=0 output_devices_opened=0\n";
    std::cout<<"Repairs: repeats_separated="<<(fixes.repeats_separated+audition_repairs.repeats_separated)
             <<" overlaps_trimmed="<<(fixes.overlaps_trimmed+audition_repairs.overlaps_trimmed)
             <<" overlaps_silenced="<<(fixes.overlaps_silenced+audition_repairs.overlaps_silenced)
             <<" broken_ties_released="<<(fixes.broken_tie_chains_released+audition_repairs.broken_tie_chains_released)
             <<" grace_notes_timed="<<repairs.grace_notes_timed
             <<" grace_notes_dropped="<<repairs.grace_notes_dropped
             <<" conflicting_tempos_resolved="<<repairs.conflicting_tempos_resolved
             <<" extra_lyrics_dropped="<<repairs.extra_lyrics_dropped
             <<" rounded_positions="<<repairs.rounded_positions
             <<" notes_widened_to_one_tick="<<repairs.notes_widened_to_one_tick
             <<" unsynchronized_parts="<<repairs.unsynchronized_parts
             <<" conflicting_part_meters="<<repairs.conflicting_part_meters<<'\n';
    if((repairs.unsynchronized_parts||repairs.conflicting_part_meters)&&wanted==0) {
      std::cout<<"Note: parts disagree about bar lines or meter, and a score keeps one global map, "
                 "so the bar numbers above are the longest part's. Use --part N to work from one part.\n";
    }
    std::cout<<"Import receipt: detailed_note_changes="<<changes.size()<<" reader_coverage=aggregate_only; use provenance in the editor to inspect saved records.\n";
    std::cout<<"Scope: supported written-note timeline, not engraving or repeat/ornament interpretation. No control curves invented; use curve-put to author a lane.\n";
  }catch(const std::exception& e){std::cerr<<"FAIL stage="<<stage<<" reason="<<e.what()<<'\n';return 1;}
}
