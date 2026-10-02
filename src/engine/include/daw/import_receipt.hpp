#pragma once
#include "daw/score.hpp"
namespace daw {
// Historical text, not an executable repair plan. Reader repairs only have
// aggregate coverage; this pass additionally has addressed before/after notes.
inline constexpr std::size_t kMaxImportReceiptBytes=16U*1024*1024;
struct ImportSourceFingerprint {
  std::string filename;
  std::uint64_t bytes=0;
  std::string fnv1a64; // accidental-change detection; NOT cryptographic proof
};
ImportSourceFingerprint fingerprintImportSource(const std::string& path);
std::string makeImportReceipt(const ImportSourceFingerprint& source, std::size_t selected_part,
    const MusicXmlImportReport& reader, const ScoreRepairReport& midi_reader,
    const ScoreRepairReport& audition, const std::vector<ScoreRepairChange>& changes,
    const Score& identified_score);
void validateImportReceipt(const std::string& receipt);
}
