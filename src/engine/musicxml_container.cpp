// Compressed .mxl import: a Zip central-directory reader, a self-contained
// RFC 1951 inflate, and the OMA container.xml lookup that names the score.
// No third-party source is copied in, matching the engine's standing rule.
// Every gate here fails closed: an archive we do not fully understand is
// refused, never partially accepted.
#include "daw/musicxml_container.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <vector>

#include "daw/score.hpp"

namespace daw {
namespace {

constexpr std::size_t kMaxArchiveBytes = 64U * 1024 * 1024;
// Matches the plain-file reader's limit so .mxl and .musicxml stop together.
constexpr std::uint32_t kMaxDocumentBytes = 16U * 1024 * 1024;
constexpr std::uint32_t kMaxContainerBytes = 1U * 1024 * 1024;
constexpr std::size_t kMaxMembers = 4096;

std::uint16_t readLE16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint32_t>(p[1]) << 8));
}

std::uint32_t readLE32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint32_t crc32Of(const std::uint8_t* data, std::size_t size) {
  static const auto table = [] {
    struct Table {
      std::uint32_t value[256];
    } built{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
      built.value[i] = c;
    }
    return built;
  }();
  std::uint32_t c = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i)
    c = table.value[(c ^ data[i]) & 0xFFU] ^ (c >> 8);
  return c ^ 0xFFFFFFFFU;
}

std::string lowerAscii(std::string text) {
  for (char& c : text)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return text;
}

bool endsWithAnyIgnoreCase(const std::string& name, const char* const* suffixes, std::size_t count) {
  const std::string lowered = lowerAscii(name);
  for (std::size_t i = 0; i < count; ++i) {
    const std::string suffix = suffixes[i];
    if (lowered.size() >= suffix.size() &&
        lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) == 0)
      return true;
  }
  return false;
}

// ---------------------------------------------------------------- inflate

struct BitError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// LSB-first bit source, RFC 1951 order. Bytes enter the holding register only
// through refill, so a stored block after a Huffman block can still find its
// header bytes either in the register or in the stream, never in both halves
// of a torn word. Runs out loudly, never silently.
struct BitReader {
  const std::uint8_t* data;
  std::size_t size;
  std::size_t byte_position = 0;
  std::uint32_t holding = 0;
  int held_bits = 0;

  void refill(int want) {
    while (held_bits < want && byte_position < size) {
      holding |= static_cast<std::uint32_t>(data[byte_position++]) << held_bits;
      held_bits += 8;
    }
  }
  unsigned need(int bits) {
    refill(bits);
    if (held_bits < bits) throw BitError("deflate stream ended mid-code");
    const unsigned value = holding & ((1U << bits) - 1U);
    holding >>= bits;
    held_bits -= bits;
    return value;
  }
  void consume(int bits) {
    if (held_bits < bits) throw BitError("deflate stream ended mid-code");
    holding >>= bits;
    held_bits -= bits;
  }
  std::uint8_t readByte() { return static_cast<std::uint8_t>(need(8)); }
  // Bits available for a table probe, least-significant first. The register
  // can hold more than a probe's worth; the table only ever sees 15 bits.
  std::uint32_t peek(int* available) {
    refill(15);
    *available = held_bits;
    return holding & 0x7FFFU;
  }
};

// A full 15-bit forward table: 32768 entries of symbol | length << 9. A
// table decode cannot mis-walk a code boundary, which is where a
// hand-rolled canonical walk earns its bugs. Unary distance codes, the one
// legal incomplete code, bypass the table and consume no bits.
struct Huffman {
  std::vector<std::uint16_t> table;  // empty plus unary flags below
  bool unary = false;
  bool has_symbol = false;
  std::uint16_t unary_symbol = 0;

  static std::uint32_t reverseBits(std::uint32_t value, int length) {
    std::uint32_t reversed = 0;
    for (int i = 0; i < length; ++i) reversed = (reversed << 1) | ((value >> i) & 1U);
    return reversed;
  }

  void build(const std::uint8_t* lengths, std::size_t count, const char* what,
             bool allow_unary) {
    table.assign(32768, 0);
    std::uint32_t counts[16] = {0};
    std::size_t used = 0;
    for (std::size_t i = 0; i < count; ++i) {
      if (lengths[i] > 15) throw BitError(std::string(what) + " code length exceeds 15");
      ++counts[lengths[i]];
      if (lengths[i] > 0) ++used;
    }
    unary = false;
    has_symbol = used != 0;
    if (used == 0) return;  // Empty code: every lookup reports corruption.
    if (used == 1 && allow_unary) {
      unary = true;
      for (std::size_t i = 0; i < count; ++i)
        if (lengths[i] > 0) unary_symbol = static_cast<std::uint16_t>(i);
      return;
    }
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
      left = (left << 1) - static_cast<int>(counts[len]);
      if (left < 0) throw BitError(std::string(what) + " code is over-subscribed");
    }
    // An incomplete code is only ever legal as one symbol, handled above.
    if (left > 0) throw BitError(std::string(what) + " code is incomplete");
    std::uint32_t code[16] = {0};
    for (int len = 1; len < 15; ++len)
      code[len + 1] = (code[len] + counts[len]) << 1;
    std::uint32_t next[16] = {0};
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint32_t len = lengths[i];
      if (len == 0) continue;
      const std::uint32_t value = code[len] + next[len];
      ++next[len];
      const std::uint32_t probe = reverseBits(value, static_cast<int>(len));
      for (std::uint32_t fill = probe; fill < 32768; fill += 1U << len)
        table[fill] = static_cast<std::uint16_t>(i | (len << 9));
    }
  }

  std::uint16_t decode(BitReader& bits) const {
    if (unary) return unary_symbol;
    if (!has_symbol) throw BitError("symbol from an empty code");
    int available = 0;
    const std::uint32_t entry = table[bits.peek(&available)];
    const int length = static_cast<int>(entry >> 9);
    if (length == 0 || length > available)
      throw BitError("deflate code ran past the stream or the table");
    bits.consume(length);
    return static_cast<std::uint16_t>(entry & 0x1FFU);
  }
};

const std::uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,   9,   10,   11,   13,   15,   17,
                                       19, 23, 27, 31, 35, 43,  51,  59,   67,   83,   99,   115,
                                       131, 163, 195, 227, 258};
// Extra bits per length symbol 257..285 (RFC 1951 table 3.1.2). The pattern
// is eight zeros, four runs of four, then the 258 code again: it is not the
// distance table's pairwise pattern, and borrowing that here inflates every
// short match by a few bits and lands the distance read mid-code.
const std::uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2,  2,  2,  2,
                                       3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::uint16_t kDistanceBase[30] = {1,     2,     3,     4,     5,     7,     9,     13,
                                         17,    25,    33,    49,    65,    97,    129,   193,
                                         257,   385,   513,   769,   1025,  1537,  2049,  3073,
                                         4097,  6145,  8193,  12289, 16385, 24577};
const std::uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5,  5,  6,  6,
                                         7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void fixedTables(Huffman* literals, Huffman* distances) {
  static const std::vector<std::uint8_t> literal_lengths = [] {
    std::vector<std::uint8_t> lengths(288, 8);
    for (std::size_t i = 144; i < 256; ++i) lengths[i] = 9;
    for (std::size_t i = 256; i < 280; ++i) lengths[i] = 7;
    for (std::size_t i = 280; i < 288; ++i) lengths[i] = 8;
    return lengths;
  }();
  static const std::vector<std::uint8_t> distance_lengths(32, 5);
  literals->build(literal_lengths.data(), literal_lengths.size(), "fixed literal", false);
  distances->build(distance_lengths.data(), distance_lengths.size(), "fixed distance", false);
}

void readDynamicTables(BitReader& bits, Huffman* literals, Huffman* distances) {
  const std::size_t literal_count = 257 + bits.need(5);
  const std::size_t distance_count = 1 + bits.need(5);
  const std::size_t order_count = 4 + bits.need(4);
  static const std::uint8_t kOrder[19] = {16, 17, 18, 0, 8,  7, 9, 6, 10, 5,
                                          11, 4,  12, 3, 13, 2, 14, 1, 15};
  std::uint8_t order_lengths[19] = {0};
  for (std::size_t i = 0; i < order_count; ++i)
    order_lengths[kOrder[i]] = static_cast<std::uint8_t>(bits.need(3));
  Huffman order_tree;
  order_tree.build(order_lengths, 19, "code-length", false);

  std::vector<std::uint8_t> lengths(literal_count + distance_count, 0);
  std::size_t filled = 0;
  while (filled < lengths.size()) {
    const std::uint16_t symbol = order_tree.decode(bits);
    if (symbol < 16) {
      lengths[filled++] = static_cast<std::uint8_t>(symbol);
    } else if (symbol == 16) {
      if (filled == 0) throw BitError("code-length repeat before any length");
      const std::size_t run = 3 + bits.need(2);
      if (filled + run > lengths.size()) throw BitError("code-length repeat overruns the table");
      for (std::size_t i = 0; i < run; ++i) lengths[filled + i] = lengths[filled - 1];
      filled += run;
    } else if (symbol == 17 || symbol == 18) {
      const std::size_t run = (symbol == 17) ? (3 + bits.need(3)) : (11 + bits.need(7));
      if (filled + run > lengths.size()) throw BitError("code-length zeros overrun the table");
      for (std::size_t i = 0; i < run; ++i) lengths[filled + i] = 0;
      filled += run;
    } else {
      throw BitError("code-length symbol out of range");
    }
  }
  literals->build(lengths.data(), literal_count, "literal", false);
  distances->build(lengths.data() + literal_count, distance_count, "distance", true);
}

// Decompress one raw deflate member. The output budget is the declared size,
// so a small member cannot lie about its expansion while it is being read.
std::vector<std::uint8_t> inflateRaw(const std::uint8_t* data, std::size_t size,
                                     std::size_t max_output) {
  BitReader bits{data, size};
  std::vector<std::uint8_t> output;
  output.reserve(64 * 1024);
  bool last = false;
  while (!last) {
    last = bits.need(1) != 0;
    const unsigned kind = bits.need(2);
    if (kind == 0) {
      bits.consume(bits.held_bits & 7);  // Discard the partial byte only.
      const std::uint32_t length =
          static_cast<std::uint32_t>(bits.readByte() | (bits.readByte() << 8));
      const std::uint32_t inverted =
          static_cast<std::uint32_t>(bits.readByte() | (bits.readByte() << 8));
      if ((length ^ inverted) != 0xFFFFU) throw BitError("stored block length check failed");
      if (output.size() + length > max_output) throw BitError("deflate output exceeds size limit");
      for (std::uint32_t i = 0; i < length; ++i) output.push_back(bits.readByte());
    } else if (kind == 3) {
      throw BitError("reserved deflate block type");
    } else {
      Huffman literals, distances;
      if (kind == 1) {
        fixedTables(&literals, &distances);
      } else {
        readDynamicTables(bits, &literals, &distances);
      }
      for (;;) {
        const std::uint16_t symbol = literals.decode(bits);
        if (symbol < 256) {
          if (output.size() + 1 > max_output) throw BitError("deflate output exceeds size limit");
          output.push_back(static_cast<std::uint8_t>(symbol));
        } else if (symbol == 256) {
          break;
        } else {
          const std::size_t index = symbol - 257U;
          if (index >= 29) throw BitError("length symbol out of range");
          const std::size_t length = kLengthBase[index] + bits.need(kLengthExtra[index]);
          const std::uint16_t distance_symbol = distances.decode(bits);
          if (distance_symbol >= 30) throw BitError("distance symbol out of range");
          const std::size_t distance =
              kDistanceBase[distance_symbol] + bits.need(kDistanceExtra[distance_symbol]);
          if (distance == 0 || distance > output.size())
            throw BitError("deflate copy reaches before the output");
          if (output.size() + length > max_output)
            throw BitError("deflate output exceeds size limit");
          const std::size_t from = output.size() - distance;
          for (std::size_t i = 0; i < length; ++i) output.push_back(output[from + i]);
        }
      }
    }
  }
  return output;
}


// ---------------------------------------------------------------- zip

struct Member {
  std::string name;
  std::uint16_t flags = 0;
  std::uint16_t method = 0;
  std::uint32_t crc = 0;
  std::uint32_t compressed_size = 0;
  std::uint32_t uncompressed_size = 0;
  std::uint32_t local_offset = 0;
};

std::string memberBody(const std::vector<std::uint8_t>& archive, const Member& member,
                       std::uint32_t limit) {
  const std::uint64_t header = member.local_offset;
  if (header + 30 > archive.size())
    throw std::runtime_error("member local header is outside the archive");
  const std::uint8_t* p = archive.data() + header;
  if (readLE32(p) != 0x04034b50U)
    throw std::runtime_error("member local header has a bad signature");
  // The local header repeats sizes the central directory already vouched
  // for; its name and extra lengths are the only fields the data offset
  // depends on, and they may legitimately differ from the central entry.
  const std::size_t local_name = readLE16(p + 26);
  const std::size_t local_extra = readLE16(p + 28);
  if (header + 30 + local_name + local_extra > archive.size())
    throw std::runtime_error("member local header runs past the archive");
  if (member.compressed_size > archive.size())
    throw std::runtime_error("member sizes disagree with the archive");
  const std::uint8_t* body =
      archive.data() + header + 30 + local_name + local_extra;
  if (body + member.compressed_size > archive.data() + archive.size())
    throw std::runtime_error("member body runs past the archive");
  const std::size_t declared_name = member.name.size();
  if (local_name != declared_name ||
      std::memcmp(body - local_extra - local_name, member.name.data(), declared_name) != 0)
    throw std::runtime_error("member name differs between headers");

  if (member.method == 0) {
    if (member.compressed_size != member.uncompressed_size)
      throw std::runtime_error("stored member sizes disagree");
    if (member.uncompressed_size > limit)
      throw std::runtime_error("stored member exceeds the size limit");
    return std::string(reinterpret_cast<const char*>(body), member.compressed_size);
  }
  if (member.uncompressed_size > limit)
    throw std::runtime_error("member expands past the size limit");
  // A 200x ratio already needs real work to fake; 1024x stops zip bombs
  // without tripping over a few seconds of silences compressing to nothing.
  if (member.compressed_size < member.uncompressed_size / 1024U)
    throw std::runtime_error("member compression ratio is implausible");
  const std::vector<std::uint8_t> expanded =
      inflateRaw(body, member.compressed_size, member.uncompressed_size);
  if (expanded.size() != member.uncompressed_size)
    throw std::runtime_error("deflate member changed size on expansion");
  return std::string(reinterpret_cast<const char*>(expanded.data()), expanded.size());
}

std::string xmlAttribute(const std::string& text, const std::size_t tag_start,
                         const std::string& attribute) {
  const std::size_t end = text.find('>', tag_start);
  if (end == std::string::npos) return std::string();
  // Both quote styles are legal XML, and real writers use both.
  for (const char quote : {'"', '\''}) {
    const std::string key = attribute + "=" + quote;
    const std::size_t start = text.find(key, tag_start);
    if (start == std::string::npos || start > end) continue;
    const std::size_t value_start = start + key.size();
    const std::size_t value_end = text.find(quote, value_start);
    if (value_end == std::string::npos || value_end > end) continue;
    std::string value = text.substr(value_start, value_end - value_start);
    if (value.find('&') != std::string::npos)
      throw std::runtime_error("entity escapes in a container.xml path are unsupported");
    return value;
  }
  return std::string();
}

// Placeholder replaced below; keeps the two-phase lookup readable.
}  // namespace

bool looksLikeZipArchive(const std::string& bytes) {
  return bytes.size() >= 4 && static_cast<std::uint8_t>(bytes[0]) == 0x50 &&
         static_cast<std::uint8_t>(bytes[1]) == 0x4b &&
         (static_cast<std::uint8_t>(bytes[2]) == 0x03 &&
          static_cast<std::uint8_t>(bytes[3]) == 0x04);
}

namespace {

std::vector<std::uint8_t> readFileBytes(const std::string& path, std::string* error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (error) *error = "cannot open archive file";
    return {};
  }
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
  if (input.bad()) {
    if (error) *error = "archive file read failed";
    return {};
  }
  return bytes;
}

bool parseAndParse(const std::vector<std::uint8_t>& archive, Score* score, std::string* error,
                   MusicXmlImportReport* report, MusicXmlArchiveReport* archive_report) {
  auto fail = [&](const std::string& message) {
    if (error) *error = message;
    return false;
  };
  try {
    if (archive.empty()) return fail("archive is empty");
    if (archive.size() > kMaxArchiveBytes)
      return fail("archive exceeds the Alpha size limit");
    if (archive.size() < 22) return fail("archive is smaller than an end record");

    // End of central directory, searched from the tail so a trailing comment
    // of any legal length is tolerated. The comment length has to reconcile
    // with the file tail exactly, so a stray copy inside a comment cannot
    // move the answer.
    const std::size_t floor =
        archive.size() < 22U + 65535U ? 0U : archive.size() - (22U + 65535U);
    std::size_t found = archive.size();
    for (std::size_t i = archive.size() - 22 + 1; i-- > floor;) {
      if (readLE32(archive.data() + i) == 0x06054b50U) {
        const std::uint32_t comment = readLE16(archive.data() + i + 20);
        if (static_cast<std::uint64_t>(i) + 22 + comment == archive.size()) {
          found = i;
          break;
        }
      }
    }
    if (found == archive.size())
      return fail("no end-of-central-directory record at the archive tail");
    const std::uint8_t* eocd = archive.data() + found;
    if (found >= 20 && readLE32(eocd - 20) == 0x07064b50U)
      return fail("Zip64 archives are unsupported");
    const std::uint32_t total_entries = readLE16(eocd + 10);
    const std::uint32_t disk_entries = readLE16(eocd + 8);
    const std::uint32_t directory_size = readLE32(eocd + 12);
    const std::uint32_t directory_offset = readLE32(eocd + 16);
    if (total_entries == 0xFFFFU || directory_size == 0xFFFFFFFFU ||
        directory_offset == 0xFFFFFFFFU)
      return fail("Zip64 archives are unsupported");
    if (readLE16(eocd + 4) != 0 || readLE16(eocd + 6) != 0)
      return fail("multi-disk archives are unsupported");
    if (disk_entries != total_entries)
      return fail("multi-disk archives are unsupported");
    if (total_entries == 0 || total_entries > kMaxMembers)
      return fail("archive member count is out of range");
    if (static_cast<std::uint64_t>(directory_offset) + directory_size >
        static_cast<std::uint64_t>(found))
      return fail("central directory overlaps the end record");

    std::map<std::string, Member> members;
    std::vector<std::string> order;
    std::size_t cursor = directory_offset;
    const std::size_t directory_end = directory_offset + directory_size;
    for (std::uint32_t i = 0; i < total_entries; ++i) {
      if (cursor + 46 > directory_end || directory_end > archive.size())
        return fail("central directory entry runs past the archive");
      const std::uint8_t* entry = archive.data() + cursor;
      if (readLE32(entry) != 0x02014b50U)
        return fail("central directory entry has a bad signature");
      Member member;
      member.flags = readLE16(entry + 8);
      member.method = readLE16(entry + 10);
      member.crc = readLE32(entry + 16);
      member.compressed_size = readLE32(entry + 20);
      member.uncompressed_size = readLE32(entry + 24);
      const std::size_t name_size = readLE16(entry + 28);
      const std::size_t extra_size = readLE16(entry + 30);
      const std::size_t comment_size = readLE16(entry + 32);
      member.local_offset = readLE32(entry + 42);
      if (readLE16(entry + 34) != 0)
        return fail("multi-disk archives are unsupported");
      if (member.local_offset >= directory_offset)
        return fail("member local header is misplaced");
      if (cursor + 46 + name_size + extra_size + comment_size > directory_end)
        return fail("central directory entry name runs past the directory");
      const char* raw_name = reinterpret_cast<const char*>(entry + 46);
      member.name.assign(raw_name, name_size);
      cursor += 46 + name_size + extra_size + comment_size;
      if (!name_size || name_size > 1024) return fail("member name length is out of range");
      if (member.name.back() == '/') continue;  // Directory entry.
      if (member.name.find('\\') != std::string::npos || member.name.front() == '/' ||
          member.name.find(':') != std::string::npos ||
          member.name == ".." || member.name.rfind("../", 0) == 0 ||
          member.name.find("/../") != std::string::npos)
        return fail("member name is not a safe relative path: " + member.name);
      // Encryption and every reserved flag bit are refused outright.
      // UTF-8 names (bit 11) and data descriptors (bit 3, whose central
      // sizes this reader trusts) are the only tolerated extras.
      if ((member.flags & ~0x0808U) != 0)
        return fail("archive member uses unsupported general flags");
      if (member.method != 0 && member.method != 8)
        return fail("archive member uses an unsupported compression method");
      if (member.compressed_size > kMaxArchiveBytes ||
          member.uncompressed_size > kMaxDocumentBytes)
        return fail("archive member sizes are out of range");
      std::string key = member.name;
      while (!key.empty() && key.front() == '/') key.erase(key.begin());
      if (members.count(key)) return fail("archive repeats a member name: " + key);
      members.emplace(key, member);
      order.push_back(key);
    }
    if (cursor != directory_end) return fail("central directory trailing bytes are unexpected");

    // Which member is the score?
    MusicXmlArchiveReport local;
    local.archive_bytes = archive.size();
    local.members = members.size();
    bool used_container = false;
    std::string key;
    const auto container = members.find("META-INF/container.xml");
    if (container != members.end()) {
      used_container = true;
      const std::string body = memberBody(archive, container->second, kMaxContainerBytes);
      // Only the first rootfile decides: that is the OMA Packaging order of
      // preference, and a second MusicXML rootfile is a different score, not
      // a second choice for the reader to quietly make. The tag must end at
      // whitespace or '/'; a bare "<rootfile" would otherwise land on the
      // <rootfiles> wrapper that actually carries the list.
      std::size_t rootfile = std::string::npos;
      for (std::size_t at = body.find("<rootfile"); at != std::string::npos;
           at = body.find("<rootfile", at + 1)) {
        const std::size_t name_end = at + std::string("<rootfile").size();
        const char next = name_end < body.size() ? body[name_end] : '\0';
        if (next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == '/') {
          rootfile = at;
          break;
        }
      }
      if (rootfile == std::string::npos)
        return fail("container.xml has no rootfile element");
      const std::string path = xmlAttribute(body, rootfile, "full-path");
      if (path.empty()) return fail("container.xml rootfile has no full-path");
      const std::string media = xmlAttribute(body, rootfile, "media-type");
      if (!media.empty() && media.rfind("application/vnd.recordare.musicxml", 0) != 0)
        return fail("container.xml rootfile has an unsupported media type: " + media);
      std::string looked_up = path;
      while (!looked_up.empty() && looked_up.front() == '/')
        looked_up.erase(looked_up.begin());
      if (!members.count(looked_up))
        return fail("container.xml rootfile is not an archive member: " + path);
      key = looked_up;
    } else {
      const char* const suffixes[] = {".xml", ".musicxml"};
      for (const std::string& name : order) {
        if (endsWithAnyIgnoreCase(name, suffixes, 2)) {
          if (!key.empty())
            return fail(
                "archive has no META-INF/container.xml to choose between two MusicXML "
                "members");
          key = name;
        }
      }
      if (key.empty())
        return fail("archive has neither META-INF/container.xml nor a MusicXML member");
    }
    const Member& document = members.find(key)->second;
    if (document.uncompressed_size == 0) return fail("MusicXML member is empty");
    std::string xml = memberBody(archive, document, kMaxDocumentBytes);
    if (crc32Of(reinterpret_cast<const std::uint8_t*>(xml.data()), xml.size()) != document.crc)
      return fail("MusicXML member does not match its recorded CRC");

    local.chosen_member = key;
    local.document_deflate = document.method == 8;
    local.used_container_xml = used_container;
    local.decompressed_bytes = document.uncompressed_size;

    if (!readMusicXmlString(xml, score, error, report)) return false;
    if (archive_report) *archive_report = local;
    return true;
  } catch (const std::exception& exception) {
    return fail(exception.what());
  }
}

}  // namespace

bool readMusicXmlArchiveBytes(const std::string& bytes, Score* score, std::string* error,
                              MusicXmlImportReport* report,
                              MusicXmlArchiveReport* archive_report) {
  if (score == nullptr) {
    if (error) *error = "score output pointer is null";
    return false;
  }
  const std::vector<std::uint8_t> archive(bytes.begin(), bytes.end());
  return parseAndParse(archive, score, error, report, archive_report);
}

bool readMusicXmlArchiveFile(const std::string& path, Score* score, std::string* error,
                             MusicXmlImportReport* report,
                             MusicXmlArchiveReport* archive_report) {
  if (score == nullptr) {
    if (error) *error = "score output pointer is null";
    return false;
  }
  const std::vector<std::uint8_t> archive = readFileBytes(path, error);
  if (archive.empty()) {
    if (error && error->empty()) *error = "archive file is empty";
    return false;
  }
  return parseAndParse(archive, score, error, report, archive_report);
}

}  // namespace daw
