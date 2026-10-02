#include "daw/import_receipt.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace daw {
namespace {
std::string quote(const std::string& text) {
  std::ostringstream out;out<<'"';
  for(char c:text){const auto b=static_cast<unsigned char>(c);
    if(c=='"'||c=='\\')out<<'\\'<<c;
    else if(b<32)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<unsigned>(b);
    else out<<c;
  }out<<'"';return out.str();
}
void note(std::ostream& out,const ScoreNote& n) {
  out<<"{\"start_ticks\":"<<n.start<<",\"duration_ticks\":"<<n.duration
     <<",\"pitch\":"<<quote(std::string(1,n.pitch.step))<<",\"alter\":"<<n.pitch.alter<<",\"octave\":"<<n.pitch.octave
     <<",\"voice\":"<<n.voice<<",\"staff\":"<<n.staff<<",\"channel\":"<<n.midi_channel
     <<",\"rest\":"<<n.rest<<",\"chord\":"<<n.chord<<",\"tie_start\":"<<n.tie_start<<",\"tie_stop\":"<<n.tie_stop
     <<",\"lyric\":"<<quote(n.lyric)<<"}";
}
void counters(std::ostream& out,const ScoreRepairReport& r) {
  out<<"{\"repeats_separated\":"<<r.repeats_separated<<",\"overlaps_trimmed\":"<<r.overlaps_trimmed
    <<",\"overlaps_silenced\":"<<r.overlaps_silenced<<",\"broken_tie_chains_released\":"<<r.broken_tie_chains_released
    <<",\"dropped_silent_notes\":"<<r.dropped_silent_notes<<",\"dropped_orphan_releases\":"<<r.dropped_orphan_releases
    <<",\"merged_instrument_tracks\":"<<r.merged_instrument_tracks<<"}";
}
}
ImportSourceFingerprint fingerprintImportSource(const std::string& path) {
  namespace fs=std::filesystem;
  if(!fs::is_regular_file(path))throw std::invalid_argument("import source must be a regular file");
  std::ifstream in(path,std::ios::binary);
  if(!in)throw std::runtime_error("cannot fingerprint import source");
  std::uint64_t hash=14695981039346656037ULL,total=0;
  std::array<char,65536> buffer{};
  while(in){in.read(buffer.data(),buffer.size());const auto count=in.gcount();
    total+=static_cast<std::uint64_t>(count);
    if(total>256U*1024*1024)throw std::length_error("source fingerprint limit: 256 MiB");
    for(std::streamsize i=0;i<count;++i){hash^=static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);hash*=1099511628211ULL;}
  }
  if(!in.eof()||in.bad())throw std::runtime_error("source fingerprint read failed");
  std::ostringstream hex;hex<<std::hex<<std::setfill('0')<<std::setw(16)<<hash;
  return {fs::path(path).filename().string(),total,hex.str()};
}
void validateImportReceipt(const std::string& receipt) {
  if(receipt.size()>kMaxImportReceiptBytes)throw std::length_error("import receipt exceeds 16 MiB");
  // It is inert metadata, not a JSON interpreter or authenticity certificate.
  // Prevent control bytes from becoming terminal actions when shown by the CLI.
  for(char c:receipt){const auto b=static_cast<unsigned char>(c);
    if((b<32&&c!='\n'&&c!='\r'&&c!='\t')||b==127)throw std::invalid_argument("unsafe control byte in import receipt");}
}
std::string makeImportReceipt(const ImportSourceFingerprint& source,std::size_t selected_part,
    const MusicXmlImportReport& r,const ScoreRepairReport& midi_reader,const ScoreRepairReport& audition,
    const std::vector<ScoreRepairChange>& changes,const Score& score) {
  if(changes.size()>65536)throw std::length_error("detailed repair limit exceeded");
  std::ostringstream out;out.imbue(std::locale::classic());
  out<<"{\n  \"format\":\"classical-daw-import-receipt-1\",\n  \"policy\":\"audition-repair-2026-10-02\",\n"
     <<"  \"source\":{\"filename\":"<<quote(source.filename)<<",\"bytes\":"<<source.bytes<<",\"fnv1a64\":"<<quote(source.fnv1a64)<<"},\n"
     <<"  \"selected_part_one_based\":"<<selected_part<<",\n"
     <<"  \"coverage\":{\"reader\":\"aggregate_only\",\"audition_repair\":\"ordered_note_mutations\",\"source_offsets\":false},\n"
     <<"  \"musicxml_reader\":{\"grace_notes_timed\":"<<r.grace_notes_timed<<",\"grace_notes_dropped\":"<<r.grace_notes_dropped
     <<",\"conflicting_tempos_resolved\":"<<r.conflicting_tempos_resolved<<",\"extra_lyrics_dropped\":"<<r.extra_lyrics_dropped
     <<",\"rounded_positions\":"<<r.rounded_positions<<",\"notes_widened_to_one_tick\":"<<r.notes_widened_to_one_tick
     <<",\"unsynchronized_parts\":"<<r.unsynchronized_parts<<",\"conflicting_part_meters\":"<<r.conflicting_part_meters<<"},\n"
     <<"  \"midi_reader\":";counters(out,midi_reader);
  out<<",\n  \"audition_repair\":";counters(out,audition);
  out<<",\n  \"changes\":[\n";
  for(std::size_t i=0;i<changes.size();++i){const auto& c=changes[i];
    const auto& part=score.parts.at(c.part_index);const auto& measure=part.measures.at(c.measure_index);
    const auto& identified=measure.notes.at(c.note_index);
    if(!identified.id)throw std::invalid_argument("repair receipt needs assigned notation IDs");
    if(i)out<<",\n";
    out<<"    {\"reason\":"<<quote(c.reason)<<",\"part_id\":"<<quote(part.id)
       <<",\"measure_ordinal\":"<<c.measure_index+1<<",\"measure_label\":"<<quote(measure.label.empty()?std::to_string(measure.number):measure.label)
       <<",\"note_ordinal\":"<<c.note_index+1<<",\"notation_id_at_import\":"<<identified.id<<",\"before\":";
    note(out,c.before);out<<",\"after\":";note(out,c.after);out<<"}";
    if(out.tellp()>static_cast<std::streamoff>(kMaxImportReceiptBytes))throw std::length_error("import receipt exceeds 16 MiB");
  }
  out<<"\n  ]\n}\n";auto result=out.str();validateImportReceipt(result);return result;
}
}
