#include "cutline/editor/autosave.hpp"

#include "cutline/editor/document.hpp"

#include <cstdlib>
#include <format>
#include <system_error>

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

std::filesystem::path autosave_path_for(const std::filesystem::path& document) {
  const std::string name =
      document.empty() ? std::string("untitled")
                       : std::format("{}-{}", safe_stem(document), digest_of(document));
  return autosave_dir() / (name + std::string(kProjectExtension));
}

std::expected<void, std::string> write_autosave(const std::filesystem::path& document,
                                                const core::Project& project) {
  const std::filesystem::path target = autosave_path_for(document);

  std::error_code error;
  std::filesystem::create_directories(target.parent_path(), error);
  if (error) {
    return std::unexpected(std::format("cannot make {}: {}", target.parent_path().string(),
                                       error.message()));
  }
  // The same write-and-rename the real save uses: a recovery copy caught
  // half-written is worse than none, because it looks like something to
  // recover from.
  return write_project(target, project);
}

void discard_autosave(const std::filesystem::path& document) {
  std::error_code ignored;
  std::filesystem::remove(autosave_path_for(document), ignored);
}

std::optional<Recovery> find_recovery(const std::filesystem::path& document) {
  const std::filesystem::path copy = autosave_path_for(document);

  std::error_code error;
  if (!std::filesystem::exists(copy, error) || error) return std::nullopt;

  const std::filesystem::file_time_type written = std::filesystem::last_write_time(copy, error);
  if (error) return std::nullopt;

  // A document saved since the copy was made is ahead of it, and offering the
  // copy would be offering to go backwards.
  if (!document.empty() && std::filesystem::exists(document, error) && !error) {
    const std::filesystem::file_time_type saved =
        std::filesystem::last_write_time(document, error);
    if (!error && saved >= written) return std::nullopt;
  }

  return Recovery{.path = copy, .written_at = written};
}

}  // namespace cutline::editor
