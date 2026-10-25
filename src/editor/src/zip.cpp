#include "cutline/editor/zip.hpp"

#include <zlib.h>

#include <cstdint>
#include <format>

namespace cutline::editor {
namespace {

constexpr std::uint32_t kLocalHeader = 0x04034b50;
constexpr std::uint32_t kCentralHeader = 0x02014b50;
constexpr std::uint32_t kEndOfDirectory = 0x06054b50;
constexpr std::uint16_t kStored = 0;
constexpr std::uint16_t kDeflated = 8;
/// The end record is 22 bytes, followed by a comment of up to this many.
constexpr std::size_t kLongestComment = 0xffff;

[[nodiscard]] std::uint16_t u16(std::string_view bytes, std::size_t at) {
  return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at]) |
                                    static_cast<unsigned char>(bytes[at + 1]) << 8);
}

[[nodiscard]] std::uint32_t u32(std::string_view bytes, std::size_t at) {
  return static_cast<std::uint32_t>(u16(bytes, at)) |
         static_cast<std::uint32_t>(u16(bytes, at + 2)) << 16;
}

void put16(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>(value & 0xff));
  out.push_back(static_cast<char>((value >> 8) & 0xff));
}

void put32(std::string& out, std::uint32_t value) {
  put16(out, value & 0xffff);
  put16(out, value >> 16);
}

[[nodiscard]] std::uint32_t crc_of(std::string_view bytes) {
  return static_cast<std::uint32_t>(
      crc32(0, reinterpret_cast<const Bytef*>(bytes.data()), static_cast<uInt>(bytes.size())));
}

/// Raw deflate, with no zlib header: what a ZIP entry holds.
[[nodiscard]] std::expected<std::string, std::string> inflate_raw(std::string_view packed,
                                                                  std::size_t size) {
  std::string out(size, '\0');
  z_stream stream{};
  if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return std::unexpected("could not start inflating");
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(packed.data()));
  stream.avail_in = static_cast<uInt>(packed.size());
  stream.next_out = reinterpret_cast<Bytef*>(out.data());
  stream.avail_out = static_cast<uInt>(out.size());
  const int result = inflate(&stream, Z_FINISH);
  const std::size_t made = stream.total_out;
  inflateEnd(&stream);
  if (result != Z_STREAM_END || made != size) return std::unexpected("damaged compressed data");
  return out;
}

[[nodiscard]] std::string deflate_raw(std::string_view bytes) {
  z_stream stream{};
  deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
  std::string out(deflateBound(&stream, static_cast<uLong>(bytes.size())), '\0');
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(bytes.data()));
  stream.avail_in = static_cast<uInt>(bytes.size());
  stream.next_out = reinterpret_cast<Bytef*>(out.data());
  stream.avail_out = static_cast<uInt>(out.size());
  deflate(&stream, Z_FINISH);
  out.resize(stream.total_out);
  deflateEnd(&stream);
  return out;
}

}  // namespace

std::expected<std::map<std::string, std::string>, std::string> read_zip(std::string_view bytes) {
  if (bytes.size() < 22) return std::unexpected("not a ZIP archive");
  // Searched for from the end, as the comment after it can be any length.
  std::size_t end = std::string_view::npos;
  const std::size_t lowest = bytes.size() > 22 + kLongestComment ? bytes.size() - 22 - kLongestComment
                                                                   : 0;
  for (std::size_t at = bytes.size() - 22;; --at) {
    if (u32(bytes, at) == kEndOfDirectory) {
      end = at;
      break;
    }
    if (at == lowest) break;
  }
  if (end == std::string_view::npos) return std::unexpected("not a ZIP archive");

  const std::uint16_t count = u16(bytes, end + 10);
  const std::uint32_t directory = u32(bytes, end + 16);
  if (count == 0xffff || directory == 0xffffffff) return std::unexpected("ZIP64 archives are not read");

  std::map<std::string, std::string> files;
  std::size_t at = directory;
  for (std::uint16_t i = 0; i < count; ++i) {
    if (at + 46 > bytes.size() || u32(bytes, at) != kCentralHeader) {
      return std::unexpected("damaged ZIP directory");
    }
    const std::uint16_t flags = u16(bytes, at + 8);
    const std::uint16_t method = u16(bytes, at + 10);
    const std::uint32_t crc = u32(bytes, at + 16);
    const std::uint32_t packed = u32(bytes, at + 20);
    const std::uint32_t size = u32(bytes, at + 24);
    const std::uint16_t name_length = u16(bytes, at + 28);
    const std::uint16_t extra = u16(bytes, at + 30);
    const std::uint16_t comment = u16(bytes, at + 32);
    const std::uint32_t local = u32(bytes, at + 42);
    if (at + 46 + name_length > bytes.size()) return std::unexpected("damaged ZIP directory");
    std::string name(bytes.substr(at + 46, name_length));
    at += 46 + name_length + extra + comment;

    if (name.empty() || name.back() == '/') continue;
    if ((flags & 1) != 0) return std::unexpected(std::format("{} is encrypted", name));
    if (local + 30 > bytes.size() || u32(bytes, local) != kLocalHeader) {
      return std::unexpected(std::format("damaged ZIP entry {}", name));
    }
    const std::size_t data = local + 30 + u16(bytes, local + 26) + u16(bytes, local + 28);
    if (data + packed > bytes.size()) return std::unexpected(std::format("{} is cut short", name));
    const std::string_view stored = bytes.substr(data, packed);

    std::string contents;
    if (method == kStored) {
      contents = std::string(stored);
    } else if (method == kDeflated) {
      auto unpacked = inflate_raw(stored, size);
      if (!unpacked.has_value()) return std::unexpected(std::format("{}: {}", name, unpacked.error()));
      contents = std::move(*unpacked);
    } else {
      return std::unexpected(std::format("{} is compressed in a way that is not read", name));
    }
    if (contents.size() != size || crc_of(contents) != crc) {
      return std::unexpected(std::format("{} is damaged", name));
    }
    files.emplace(std::move(name), std::move(contents));
  }
  return files;
}

std::string write_zip(const std::vector<ZipEntry>& entries, bool deflate) {
  std::string out;
  std::string directory;
  for (const ZipEntry& entry : entries) {
    const std::uint32_t offset = static_cast<std::uint32_t>(out.size());
    const std::string packed = deflate ? deflate_raw(entry.contents) : entry.contents;
    const std::uint16_t method = deflate ? kDeflated : kStored;
    const std::uint32_t crc = crc_of(entry.contents);
    const auto header = [&](std::string& to, bool central) {
      put32(to, central ? kCentralHeader : kLocalHeader);
      if (central) put16(to, 20);  // made by
      put16(to, 20);               // needed to extract
      put16(to, 0);                // flags
      put16(to, method);
      put32(to, 0);  // time and date
      put32(to, crc);
      put32(to, static_cast<std::uint32_t>(packed.size()));
      put32(to, static_cast<std::uint32_t>(entry.contents.size()));
      put16(to, static_cast<std::uint32_t>(entry.name.size()));
      put16(to, 0);  // extra
      if (central) {
        put16(to, 0);  // comment
        put16(to, 0);  // disk
        put16(to, 0);  // internal attributes
        put32(to, 0);  // external attributes
        put32(to, offset);
      }
      to += entry.name;
    };
    header(out, false);
    out += packed;
    header(directory, true);
  }
  const std::uint32_t start = static_cast<std::uint32_t>(out.size());
  out += directory;
  put32(out, kEndOfDirectory);
  put16(out, 0);
  put16(out, 0);
  put16(out, static_cast<std::uint32_t>(entries.size()));
  put16(out, static_cast<std::uint32_t>(entries.size()));
  put32(out, static_cast<std::uint32_t>(directory.size()));
  put32(out, start);
  put16(out, 0);
  return out;
}

}  // namespace cutline::editor
