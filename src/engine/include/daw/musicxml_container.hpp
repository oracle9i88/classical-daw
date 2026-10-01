#pragma once
// Reading a compressed .mxl MusicXML container. The archive is a transport,
// not a project format: the member it names is parsed by readMusicXmlString
// exactly as a plain .musicxml file would be, under the same fail-closed
// gates. Nothing here opens a plugin or an output device.
#include <string>

#include "daw/score.hpp"

namespace daw {

// What the container layer decided, for callers that report evidence rather
// than a bare pass/fail. Sizes are declared values from the archive, not
// allocations; the decompressed byte count is what the parse actually saw.
struct MusicXmlArchiveReport {
  std::uint64_t archive_bytes = 0;
  std::uint64_t members = 0;
  // Chosen MusicXML member, verbatim from the central directory.
  std::string chosen_member;
  // Deflate rather than stored. False means the member was stored.
  bool document_deflate = false;
  // The member was named by META-INF/container.xml. Without one, a lone
  // .xml/.musicxml member is accepted; more than one is refused.
  bool used_container_xml = false;
  std::uint32_t decompressed_bytes = 0;
};

bool readMusicXmlArchiveFile(const std::string& path, Score* score,
                             std::string* error = nullptr,
                             MusicXmlImportReport* report = nullptr,
                             MusicXmlArchiveReport* archive = nullptr);

// Archive bytes already in memory, for callers that already own the bytes and
// for tests that build archives member by member. Same gates either way.
bool readMusicXmlArchiveBytes(const std::string& bytes, Score* score,
                              std::string* error = nullptr,
                              MusicXmlImportReport* report = nullptr,
                              MusicXmlArchiveReport* archive = nullptr);

// Cheap magic-number sniff so a caller can pick a reader before a full read.
// It only inspects the leading bytes of a Zip local file header.
bool looksLikeZipArchive(const std::string& bytes);

}  // namespace daw
