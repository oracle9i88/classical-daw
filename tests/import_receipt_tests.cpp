#include "daw/import_receipt.hpp"
#include "daw/performance_fixture.hpp"
#include "daw/performance_recovery.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace fs=std::filesystem;
void need(bool value,const char* why){if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}need(caught,"invalid input accepted");}
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
bool same(const daw::ScoreNote& a,const daw::ScoreNote& b){return a.start==b.start&&a.duration==b.duration&&a.rest==b.rest&&a.chord==b.chord&&a.tie_start==b.tie_start&&a.tie_stop==b.tie_stop&&a.lyric==b.lyric;}
daw::Score make(const std::vector<daw::ScoreNote>& notes){daw::Score score;daw::ScorePart part;daw::ScoreMeasure bar;bar.number=1;bar.label="X1";bar.duration=3840;bar.notes=notes;part.measures.push_back(bar);score.parts.push_back(part);return score;}
daw::ScoreNote note(daw::Tick start,daw::Tick duration){daw::ScoreNote n;n.start=start;n.duration=duration;return n;}
int main(){const auto root=fs::temp_directory_path()/("daw-import-receipt-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));try{
  fs::create_directory(root);
  // Ordered deltas replay all mutations, including several operations on one
  // note. A chain counter is intentionally NOT the number of affected segments.
  auto head=note(0,240);head.tie_start=true;
  auto body=note(240,2880);body.tie_start=true;body.tie_stop=true;
  auto clash=note(1920,480);clash.voice=2;
  auto score=make({head,body,clash});const auto before=score;
  daw::ScoreRepairReport report;std::vector<daw::ScoreRepairChange> changes;
  daw::repairScoreForAudition(score,&report,&changes);
  need(report.broken_tie_chains_released==1 && report.repeats_separated==1 && report.overlaps_trimmed==1,"fixture repairs changed");
  auto replay=before;
  for(const auto& c:changes){auto& n=replay.parts.at(c.part_index).measures.at(c.measure_index).notes.at(c.note_index);
    need(same(n,c.before),"change is not the immediate pre-mutation state");n=c.after;}
  for(std::size_t i=0;i<3;++i)need(same(replay.parts[0].measures[0].notes[i],score.parts[0].measures[0].notes[i]),"ordered records do not explain final notes");
  need(changes.size()==4,"missing per-segment tie record");
  daw::assignNoteIds(score);
  const auto receipt=daw::makeImportReceipt({"study.xml",42,"1234"},0,{}, {},report,changes,score);
  need(receipt.find("\"notation_id_at_import\":2")!=std::string::npos && receipt.find("\"measure_label\":\"X1\"")!=std::string::npos,"cannot address repair");

  auto shorter=note(0,240);shorter.lyric="verse\n\"two\"";shorter.chord=true;
  auto longer=note(0,960);longer.voice=2;
  auto unison=make({shorter,longer});changes.clear();report={};
  daw::repairScoreForAudition(unison,&report,&changes);
  need(changes.size()==1&&changes[0].note_index==0&&changes[0].reason=="overlap_silenced","silenced the wrong owner in audit");
  need(changes[0].before.lyric==shorter.lyric&&changes[0].before.chord&&!changes[0].after.chord&&changes[0].after.lyric.empty()&&changes[0].after.rest,"silencing losses not recorded");
  daw::assignNoteIds(unison);
  const auto escaped=daw::makeImportReceipt({"a\n\".xml",1,"00"},0,{}, {},report,changes,unison);
  need(escaped.find("verse\\u000a\\\"two\\\"")!=std::string::npos,"receipt strings are not JSON escaped");
  auto untouched=before;changes.clear();daw::repairScoreForAudition(untouched,nullptr,&changes);
  need(changes.empty()&&same(untouched.parts[0].measures[0].notes[0],head),"null report changed data/audit");

  // Known noncryptographic checksum vector plus source-change detection.
  const auto input=root/"source.xml";std::ofstream(input)<<"hello";
  const auto fp=daw::fingerprintImportSource(input.string());
  need(fp.filename=="source.xml"&&fp.bytes==5&&fp.fnv1a64=="a430d84680aabd0b","fingerprint mismatch");
  std::ofstream(input)<<"world";need(daw::fingerprintImportSource(input.string()).fnv1a64!=fp.fnv1a64,"same-size change not detected");

  auto d=daw::performanceFixture({1});d.import_receipt=receipt;
  const auto source=root/"document";daw::savePerformanceDocument(d,source.string());
  need(read(source/"performances.dawperformance").find("CLASSICAL_DAW_PERFORMANCE 3\n")==0,"missing version 3");
  auto loaded=daw::loadPerformanceDocument(source.string());need(loaded.import_receipt==receipt,"reopen lost receipt");
  daw::WorkEditor editor(loaded);editor.setGain(-18);editor.setPitch(2,{'F',0,4});editor.undo();editor.redo();
  editor.copyTake("audited copy");editor.undo();
  need(editor.document().import_receipt==receipt,"editing rewrote historical receipt");
  daw::savePerformanceDocument(editor.document(),(root/"edited").string());
  need(daw::loadPerformanceDocument((root/"edited").string()).import_receipt==receipt,"save lost history");
  daw::PerformanceRecovery recovery(source.string());recovery.checkpoint(editor.document(),editor.revision());
  need(daw::readPerformanceRecovery(source.string(),recovery.directory()).import_receipt==receipt,"recovery lost receipt");
  auto invalid=d;invalid.import_receipt="bad\x1b[2J";rejects([&]{daw::savePerformanceDocument(invalid,(root/"bad").string());});
  need(!fs::exists(root/"bad"),"unsafe receipt left an output");
  rejects([]{daw::validateImportReceipt(std::string(daw::kMaxImportReceiptBytes+1,'a'));});
  const auto savedBytes=read(source/"performances.dawperformance");
  std::ostringstream quoted;quoted<<std::quoted(receipt);
  const auto offset=savedBytes.find(quoted.str());need(offset!=std::string::npos,"receipt field not located");
  auto oversized=savedBytes;oversized.replace(offset,quoted.str().size(),"\""+std::string(daw::kMaxImportReceiptBytes+1,'x')+"\"");
  std::ofstream(source/"performances.dawperformance",std::ios::binary)<<oversized;
  rejects([&]{daw::loadPerformanceDocument(source.string());});
  std::ofstream(source/"performances.dawperformance",std::ios::binary)<<savedBytes;
  auto truncated=read(source/"performances.dawperformance");
  const auto cut=truncated.find("classical-daw-import-receipt-1");
  need(cut!=std::string::npos,"missing receipt fixture");truncated.resize(cut+10);
  std::ofstream(source/"performances.dawperformance",std::ios::binary)<<truncated;
  rejects([&]{daw::loadPerformanceDocument(source.string());});
  d.import_receipt.clear();daw::savePerformanceDocument(d,(root/"legacy").string());
  need(read(root/"legacy"/"performances.dawperformance").find("CLASSICAL_DAW_PERFORMANCE 1\n")==0,"legacy layout changed");
  fs::remove_all(root);std::cout<<"import receipt tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<" fixtures="<<root<<'\n';return 1;}}
