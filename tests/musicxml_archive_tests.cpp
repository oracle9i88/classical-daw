// Compressed .mxl import. The positive fixtures were packed by Python's
// zipfile (scripts/make_musicxml_archive_fixture.py), an independent writer
// that shares no code with this reader. The fail-closed archives are
// assembled in-place below, so each refusal names its own reason.
#include "daw/musicxml_container.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool value, const std::string& why) {
  if (!value) throw std::runtime_error(why);
}

const char* const kXml =
    "<score-partwise><part-list><score-part id='P1'><part-name>Piano</part-name></score-part>"
    "</part-list><part id='P1'><measure number='1'>"
    "<note><pitch><step>C</step><octave>4</octave></pitch><duration>480</duration></note>"
    "<note><pitch><step>E</step><octave>4</octave></pitch><duration>480</duration></note>"
    "</measure></part></score-partwise>";
const char* const kContainer =
    "<?xml version='1.0' encoding='UTF-8'?>"
    "<container version='1.0' xmlns='http://www.idpf.org/2011/opf/'>"
    "<rootfiles><rootfile full-path='score.musicxml' "
    "media-type='application/vnd.recordare.musicxml'/></rootfiles></container>";

#include "musicxml_archive_fixtures.inc"

std::uint32_t tableCrc(const std::string& data) {
  std::uint32_t c = 0xFFFFFFFFU;
  for (const char byte : data) {
    c ^= static_cast<unsigned char>(byte);
    for (int k = 0; k < 8; ++k)
      c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
  }
  return c ^ 0xFFFFFFFFU;
}

void put16(std::string& out, std::uint16_t value) {
  out += static_cast<char>(value & 0xFFU);
  out += static_cast<char>((value >> 8) & 0xFFU);
}

void put32(std::string& out, std::uint32_t value) {
  put16(out, static_cast<std::uint16_t>(value & 0xFFFFU));
  put16(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFFU));
}

struct Piece {
  std::string name;
  std::string payload;
  std::uint16_t method = 0;
  std::uint16_t flags = 0;
};

// Stored archives are simple enough to build here instead of shipping a
// second fixture per refusal.
std::string buildArchive(const std::vector<Piece>& pieces) {
  std::string body, directory;
  for (const Piece& piece : pieces) {
    const std::uint32_t offset = static_cast<std::uint32_t>(body.size());
    const std::uint32_t crc = tableCrc(piece.payload);
    put32(body, 0x04034b50U);
    put16(body, 20);
    put16(body, piece.flags);
    put16(body, piece.method);
    put16(body, 0);
    put16(body, 0);
    put32(body, crc);
    put32(body, static_cast<std::uint32_t>(piece.payload.size()));
    put32(body, static_cast<std::uint32_t>(piece.payload.size()));
    put16(body, static_cast<std::uint16_t>(piece.name.size()));
    put16(body, 0);
    body += piece.name;
    body += piece.payload;
    put32(directory, 0x02014b50U);
    put16(directory, 20);
    put16(directory, 20);
    put16(directory, piece.flags);
    put16(directory, piece.method);
    put16(directory, 0);
    put16(directory, 0);
    put32(directory, crc);
    put32(directory, static_cast<std::uint32_t>(piece.payload.size()));
    put32(directory, static_cast<std::uint32_t>(piece.payload.size()));
    put16(directory, static_cast<std::uint16_t>(piece.name.size()));
    put16(directory, 0);
    put16(directory, 0);
    put16(directory, 0);
    put16(directory, 0);
    put32(directory, 0);
    put32(directory, offset);
    directory += piece.name;
  }
  std::string out = body + directory;
  put32(out, 0x06054b50U);
  put16(out, 0);
  put16(out, 0);
  put16(out, static_cast<std::uint16_t>(pieces.size()));
  put16(out, static_cast<std::uint16_t>(pieces.size()));
  put32(out, static_cast<std::uint32_t>(directory.size()));
  put32(out, static_cast<std::uint32_t>(body.size()));
  put16(out, 0);
  return out;
}

void patch16(std::string& archive, std::size_t offset, std::uint16_t value) {
  archive[offset] = static_cast<char>(value & 0xFFU);
  archive[offset + 1] = static_cast<char>((value >> 8) & 0xFFU);
}

bool refuses(const std::string& archive, const std::string& reason_fragment) {
  daw::Score score;
  score.bpm = 77;
  std::string error;
  const bool ok = daw::readMusicXmlArchiveBytes(archive, &score, &error);
  require(score.bpm == 77, "a refused archive changed the caller's score: " + error);
  require(!ok, "archive was accepted where it should refuse: " + error);
  if (error.find(reason_fragment) == std::string::npos) {
    std::cout << "  note: refusal said \"" << error << "\" while looking for \""
              << reason_fragment << "\"\n";
    return false;
  }
  return true;
}

void expectScore(const daw::Score& score, const std::string& where) {
  require(score.parts.size() == 1 && score.parts[0].measures.size() == 1,
          "container lost structure: " + where);
  const auto& notes = score.parts[0].measures[0].notes;
  require(notes.size() == 2, "container lost notes: " + where);
  require(notes[0].pitch.step == 'C' && notes[1].pitch.step == 'E',
          "container lost pitches: " + where);
}

}  // namespace

int main() {
  try {
    // The three generated archives must parse to the same score as the plain
    // member, through both container lookup and lone-member fallback.
    struct Positive {
      const unsigned char* bytes;
      std::size_t size;
      const char* name;
      bool deflate;
      bool container;
    };
    const Positive positives[] = {
        {kMxlDeflate, sizeof(kMxlDeflate), "deflate+container", true, true},
        {kMxlDeflateNoContainer, sizeof(kMxlDeflateNoContainer), "deflate+lone-member", true,
         false},
        {kMxlStored, sizeof(kMxlStored), "stored+container", false, true},
    };
    for (const Positive& positive : positives) {
      const std::string archive(
          reinterpret_cast<const char*>(positive.bytes), positive.size);
      daw::Score score;
      std::string error;
      daw::MusicXmlArchiveReport report;
      // Materialize before building the message: argument evaluation order is
      // unspecified, and a message built early captures an empty error.
      const bool parsed =
          daw::readMusicXmlArchiveBytes(archive, &score, &error, nullptr, &report);
      require(parsed, std::string(positive.name) + ": " + error);
      expectScore(score, positive.name);
      require(report.chosen_member == "score.musicxml",
              std::string(positive.name) + ": wrong member chosen");
      require(report.document_deflate == positive.deflate,
              std::string(positive.name) + ": wrong method reported");
      require(report.used_container_xml == positive.container,
              std::string(positive.name) + ": wrong provenance reported");
      require(report.decompressed_bytes == std::strlen(kXml),
              std::string(positive.name) + ": wrong size reported");
    }
    // Two harder members exercise paths the three above miss: stored
    // (uncompressed) deflate blocks, and a stream large enough to split
    // across dynamic blocks with long distances.
    {
      const std::string archive(
          reinterpret_cast<const char*>(kMxlStoredBlocks), sizeof(kMxlStoredBlocks));
      daw::Score score;
      std::string error;
      const bool parsed = daw::readMusicXmlArchiveBytes(archive, &score, &error);
      require(parsed, "stored-blocks member: " + error);
      expectScore(score, "stored-blocks");
    }
    {
      const std::string archive(
          reinterpret_cast<const char*>(kMxlMultiBlock), sizeof(kMxlMultiBlock));
      daw::Score score;
      std::string error;
      const bool parsed = daw::readMusicXmlArchiveBytes(archive, &score, &error);
      require(parsed, "multi-block member: " + error);
      require(score.parts.size() == 1 && score.parts[0].measures.size() == 900,
              "multi-block member lost measures: " + error);
      std::size_t notes = 0;
      for (const auto& part : score.parts)
        for (const auto& measure : part.measures) notes += measure.notes.size();
      require(notes == 901, "multi-block member lost notes");
    }
    require(daw::looksLikeZipArchive(std::string(kMxlDeflate, kMxlDeflate + 4)),
            "magic sniff missed a local header");
    require(!daw::looksLikeZipArchive("<score-partwise>"), "magic sniff saw a score");

    // A reader-written archive must also read: the builder above is not the
    // fixture generator, and neither may be load-bearing alone.
    const std::string homegrown = buildArchive(
        {{"META-INF/container.xml", kContainer}, {"score.musicxml", kXml}});
    {
      daw::Score score;
      std::string error;
      const bool parsed = daw::readMusicXmlArchiveBytes(homegrown, &score, &error);
      require(parsed, "home-grown stored archive: " + error);
      expectScore(score, "home-grown");
    }

    // One refusal per reason.
    require(refuses("", "empty"), "empty refusal wording");
    require(refuses(std::string(21, 'x'), "smaller than an end record"), "tiny refusal wording");
    require(refuses(std::string(kMxlDeflate, kMxlDeflate + 400), "end-of-central-directory"),
            "truncation refusal wording");
    {
      // Stored bytes give this a deterministic outcome: the member inflates
      // to its declared size and only the recorded CRC can catch it.
      const std::string lone = buildArchive({{"score.musicxml", kXml}});
      std::string broken = lone;
      broken[50] = static_cast<char>(broken[50] ^ 0x5A);  // inside the stored payload
      require(refuses(broken, "CRC"), "payload corruption wording");
    }
    {
      std::string zip64 = homegrown;
      // The end record is the last 22 bytes; entry count sits at offset 10.
      patch16(zip64, zip64.size() - 22 + 10, 0xFFFFU);
      require(refuses(zip64, "Zip64"), "zip64 refusal wording");
    }
    require(refuses(buildArchive({{"score.musicxml", kXml},
                                  {"second.musicxml", kXml}}),
                    "container.xml"),
            "two candidates refusal wording");
    require(refuses(buildArchive({{"score.musicxml", kXml}, {"score.musicxml", kXml}}),
                    "repeats a member name"),
            "duplicate member refusal wording");
    require(refuses(buildArchive({{"../escape.musicxml", kXml}}), "safe relative path"),
            "escape refusal wording");
    require(refuses(buildArchive({{"score.musicxml", kXml, 99, 0}}), "compression method"),
            "method refusal wording");
    require(refuses(buildArchive({{"score.musicxml", kXml, 0, 1}}), "general flags"),
            "encryption refusal wording");
    require(refuses(buildArchive({{"META-INF/container.xml",
                                   "<?xml version='1.0'?><container><rootfiles/> </container>"}}),
                    "rootfile"),
            "empty container refusal wording");
    require(refuses(buildArchive({{"META-INF/container.xml",
                                   "<?xml version='1.0'?><container><rootfile "
                                   "full-path='missing.xml'/></container>"}}),
                    "not an archive member"),
            "missing rootfile refusal wording");
    require(refuses(buildArchive({{"META-INF/container.xml",
                                   "<?xml version='1.0'?><container><rootfile "
                                   "full-path='score.musicxml' media-type='application/zip'/>"
                                   "</container>"},
                                  {"score.musicxml", kXml}}),
                    "media type"),
            "media type refusal wording");
    {
      // container.xml pointing at a stored member whose central sizes lie.
      // The name appears three times in the archive: inside container.xml,
      // in the local header, then in the central directory, whose csize sits
      // 26 bytes before its name.
      std::string archive = homegrown;
      const std::size_t central = archive.rfind("score.musicxml");
      require(central != std::string::npos, "fixture lost its central entry");
      patch16(archive, central - 26, 4U);  // compressed size, in the 46-byte header
      require(refuses(archive, "stored"), "stored size refusal wording");
    }
    {
      // The ceiling applies to the file before its bytes are read: a
      // 400 MB candidate used to be paid for in full just to be refused.
      const auto path = std::filesystem::temp_directory_path() / "classical_daw_oversize.mxl";
      {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.put('P');
        require(static_cast<bool>(out), "oversize fixture write failed");
        out.flush();
        std::filesystem::resize_file(path, 64U * 1024 * 1024 + 1);  // sparse
      }
      daw::Score score;
      std::string error;
      require(!daw::readMusicXmlArchiveFile(path.string(), &score, &error),
              "oversized archive was accepted");
      require(error.find("size limit") != std::string::npos,
              "oversized archive refusal wording: " + error);
      std::filesystem::remove(path);
    }
    std::cout << "PASS deflate+stored .mxl through container and lone-member paths, "
                 "refused truncation, zip64, encryption, unknown methods, escaping and "
                 "duplicate names, lying sizes, and mislabelled rootfiles\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
