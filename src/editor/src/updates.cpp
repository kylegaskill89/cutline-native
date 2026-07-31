#include "cutline/editor/updates.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>

#include <windows.h>

#include <bcrypt.h>

namespace cutline::editor {
namespace {

using nlohmann::json;

/// A SHA-256 digest is thirty-two bytes, so sixty-four hex characters. Checked
/// rather than assumed: a manifest with a truncated digest would otherwise
/// compare equal to nothing and fail confusingly instead of clearly.
constexpr std::size_t kDigestChars = 64;

[[nodiscard]] bool is_hex(std::string_view text) noexcept {
  return std::ranges::all_of(text, [](unsigned char c) { return std::isxdigit(c) != 0; });
}

[[nodiscard]] char lowered(char c) noexcept {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// Reads one dot-separated number, and says where it stopped.
[[nodiscard]] const char* read_part(const char* begin, const char* end, int& out) noexcept {
  const auto result = std::from_chars(begin, end, out);
  if (result.ec != std::errc{} || out < 0) return nullptr;
  return result.ptr;
}

}  // namespace

std::string Version::to_string() const {
  return std::format("{}.{}.{}", major, minor, patch);
}

std::expected<Version, std::string> parse_version(std::string_view text) {
  // A leading "v" is how a git tag spells it, and the tag is what a release is
  // named after — so both forms arrive and both mean the same thing.
  if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
  if (text.empty()) return std::unexpected("a version cannot be empty");

  Version out;
  const char* at = text.data();
  const char* const end = text.data() + text.size();

  at = read_part(at, end, out.major);
  if (at == nullptr || at == end || *at != '.') {
    return std::unexpected(std::format("'{}' is not a version", text));
  }
  at = read_part(at + 1, end, out.minor);
  if (at == nullptr || at == end || *at != '.') {
    return std::unexpected(std::format("'{}' is not a version", text));
  }
  at = read_part(at + 1, end, out.patch);
  // Trailing anything is refused rather than ignored. "1.2.3-beta" and "1.2.3"
  // are different releases, and this cannot tell which is newer, so it must not
  // pretend to.
  if (at == nullptr || at != end) {
    return std::unexpected(std::format("'{}' is not a version", text));
  }
  return out;
}

std::expected<Release, std::string> parse_release_manifest(std::string_view json_text) {
  const json document = json::parse(json_text, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return std::unexpected("the update manifest is not readable");
  }

  Release out;

  const auto version = document.find("version");
  if (version == document.end() || !version->is_string()) {
    return std::unexpected("the update manifest has no version");
  }
  auto parsed = parse_version(version->get<std::string>());
  if (!parsed.has_value()) return std::unexpected(parsed.error());
  out.version = *parsed;

  const auto installer = document.find("installer");
  if (installer == document.end() || !installer->is_string()) {
    return std::unexpected("the update manifest has no installer");
  }
  out.installer = installer->get<std::string>();

  // https, and nothing else. An installer fetched over plain http can be
  // replaced in transit by anybody on the path between here and the server,
  // and this one is about to be run.
  if (!out.installer.starts_with("https://")) {
    return std::unexpected("the installer must be an https address");
  }

  const auto digest = document.find("sha256");
  if (digest == document.end() || !digest->is_string()) {
    return std::unexpected("the update manifest has no sha256");
  }
  out.sha256 = digest->get<std::string>();
  if (out.sha256.size() != kDigestChars || !is_hex(out.sha256)) {
    return std::unexpected("the sha256 in the update manifest is not a digest");
  }

  // The only optional field: a release with no notes is a release, and refusing
  // one over a missing paragraph would be refusing an update over prose.
  if (const auto notes = document.find("notes");
      notes != document.end() && notes->is_string()) {
    out.notes = notes->get<std::string>();
  }

  return out;
}

bool update_available(const Version& current, const Version& latest) noexcept {
  return latest > current;
}

std::string sha256_hex(const std::vector<std::uint8_t>& bytes) {
  // The system's implementation rather than one written here. This decides
  // whether an executable is the one that was published, and a hash function
  // is exactly the kind of thing that is subtly wrong in a way no ordinary
  // test finds.
  std::array<std::uint8_t, 32> digest{};
  const NTSTATUS status = BCryptHash(
      BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
      const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(bytes.data())),
      static_cast<ULONG>(bytes.size()), digest.data(),
      static_cast<ULONG>(digest.size()));
  if (status < 0) return {};

  std::string out;
  out.reserve(digest.size() * 2);
  for (const std::uint8_t byte : digest) out += std::format("{:02x}", byte);
  return out;
}

bool digest_matches(std::string_view expected, std::string_view actual) noexcept {
  if (expected.size() != actual.size() || expected.empty()) return false;
  return std::ranges::equal(expected, actual,
                            [](char a, char b) { return lowered(a) == lowered(b); });
}

}  // namespace cutline::editor
