#include "cutline/editor/autosave.hpp"

#include "cutline/editor/document.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <string_view>
#include <system_error>
#include <vector>

namespace cutline::editor {
namespace {

/// A digest of the document's path, so two projects with the same name in
/// different folders do not share a recovery file.
///
/// FNV-1a. Not a security question — this only has to separate two paths a
/// person is plausibly editing at once, and being short enough to read is
/// worth more here than being hard to collide.
[[nodiscard]] std::string digest_of(const std::filesystem::path& path) {
  constexpr std::uint64_t kOffset = 14695981039346656037ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;

  std::uint64_t hash = kOffset;
  for (const char c : path.generic_string()) {
    hash ^= static_cast<unsigned char>(c);
    hash *= kPrime;
  }
  return std::format("{:016x}", hash);
}

/// Filesystem-safe, and short enough that the whole name stays under any
/// sensible limit. Anything unusual becomes a dash rather than being dropped,
/// so two names cannot collapse into one.
[[nodiscard]] std::string safe_stem(const std::filesystem::path& path) {
  std::string out;
  for (const char c : path.stem().string()) {
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_';
    out.push_back(plain ? c : '-');
    if (out.size() >= 40) break;
  }
  return out.empty() ? "untitled" : out;
}

/// What `autosave_path_for` adds to a prefix: a dash, a date and a time.
/// Named because `autosave_copies` recognises a copy by exactly this much
/// following the prefix, and a format change that did not move this with it
/// would quietly stop finding anything.
constexpr std::size_t kStampWidth = std::string_view("-20260806-140309").size();

}  // namespace

bool autosave_due(const AutosaveState& state, bool modified, std::uint64_t revision,
                  std::chrono::steady_clock::time_point now,
                  std::chrono::seconds interval) noexcept {
  // Nothing to recover that is not already on disk.
  if (!modified) return false;
  // Already written for exactly this state. A document left alone would
  // otherwise be rewritten every interval for as long as it stayed open.
  if (state.ever_written && state.written_revision == revision) return false;
  // The first change after opening is written straight away rather than a
  // minute later: the window most likely to be lost is the one right after
  // somebody starts working.
  if (!state.ever_written) return true;
  return now - state.written_at >= interval;
}

std::filesystem::path autosave_dir() {
  // Beside the workspace file, which is already where this application keeps
  // what belongs to the machine rather than to the project.
  const char* roaming = std::getenv("APPDATA");
  const std::filesystem::path base =
      roaming == nullptr ? std::filesystem::path{"."} : std::filesystem::path{roaming};
  return base / "Cutline" / "recovery";
}

std::string autosave_prefix(const std::filesystem::path& document) {
  if (document.empty()) return "untitled";
  return std::format("{}-{}", safe_stem(document), digest_of(document));
}

std::filesystem::path autosave_path_for(const std::filesystem::path& document,
                                        std::chrono::system_clock::time_point when) {
  // Fixed width, so the name sorts chronologically as plain text. Seconds are
  // as fine as this needs to go: the shortest interval anybody can ask for is
  // fifteen of them, so two copies of one document cannot share a stamp.
  const std::string stamp =
      std::format("{:%Y%m%d-%H%M%S}", std::chrono::floor<std::chrono::seconds>(when));
  return autosave_dir() /
         std::format("{}-{}{}", autosave_prefix(document), stamp, kProjectExtension);
}

std::expected<void, std::string> write_autosave(const std::filesystem::path& document,
                                                const core::Project& project, int versions,
                                                std::chrono::system_clock::time_point when) {
  const std::filesystem::path target = autosave_path_for(document, when);

  std::error_code error;
  std::filesystem::create_directories(target.parent_path(), error);
  if (error) {
    return std::unexpected(std::format("cannot make {}: {}", target.parent_path().string(),
                                       error.message()));
  }
  // The same write-and-rename the real save uses: a recovery copy caught
  // half-written is worse than none, because it looks like something to
  // recover from.
  if (auto written = write_project(target, project); !written.has_value()) return written;

  // Only now. Making room first would mean a failed write had thrown away a
  // good copy to fit one that never arrived.
  const std::vector<Recovery> copies = autosave_copies(document);
  const auto keep = static_cast<std::size_t>(std::max(1, versions));
  for (std::size_t i = keep; i < copies.size(); ++i) {
    std::error_code ignored;
    std::filesystem::remove(copies[i].path, ignored);
  }
  return {};
}

void discard_autosave(const std::filesystem::path& document) {
  for (const Recovery& copy : autosave_copies(document)) {
    std::error_code ignored;
    std::filesystem::remove(copy.path, ignored);
  }
}

std::vector<Recovery> autosave_copies(const std::filesystem::path& document) {
  const std::string prefix = autosave_prefix(document);

  std::error_code error;
  std::filesystem::directory_iterator entries(autosave_dir(), error);
  if (error) return {};

  std::vector<Recovery> found;
  for (const std::filesystem::directory_entry& entry : entries) {
    if (!entry.is_regular_file(error) || error) continue;
    if (entry.path().extension() != kProjectExtension) continue;

    // Matching the prefix is not enough: what follows it has to be a whole
    // stamp and nothing else. A document that has never been saved has the
    // prefix "untitled", which is *also* the start of every copy of a saved
    // project called "untitled" — so a looser test would hand one document's
    // copies to another, and the two are the pair most likely to both exist.
    //
    // Nothing at all is the copy written before the stamps existed.
    const std::string stem = entry.path().stem().string();
    if (!stem.starts_with(prefix)) continue;
    if (stem.size() != prefix.size() && stem.size() != prefix.size() + kStampWidth) continue;

    const std::filesystem::file_time_type written = entry.last_write_time(error);
    found.push_back(Recovery{.path = entry.path(),
                             .written_at = error ? std::filesystem::file_time_type{} : written});
  }

  // Newest first, by the stamp in the name. A copy from before the stamps
  // existed is the shortest name and therefore sorts last, which is where the
  // oldest belongs.
  std::ranges::sort(found, [](const Recovery& a, const Recovery& b) {
    return a.path.stem().string() > b.path.stem().string();
  });
  return found;
}

std::optional<Recovery> find_recovery(const std::filesystem::path& document) {
  const std::vector<Recovery> copies = autosave_copies(document);
  if (copies.empty()) return std::nullopt;
  const Recovery& newest = copies.front();

  // A document saved since the copy was made is ahead of it, and offering the
  // copy would be offering to go backwards.
  std::error_code error;
  if (!document.empty() && std::filesystem::exists(document, error) && !error) {
    const std::filesystem::file_time_type saved =
        std::filesystem::last_write_time(document, error);
    if (!error && saved >= newest.written_at) return std::nullopt;
  }

  return newest;
}

}  // namespace cutline::editor
