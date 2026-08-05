#include "cutline/editor/document.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ios>
#include <sstream>
#include <system_error>

namespace cutline::editor {
namespace {

/// Rendered rather than relied on: `std::filesystem::path` on Windows holds
/// wide characters, and putting one straight into a message mangles anything
/// outside ASCII.
[[nodiscard]] std::string shown(const std::filesystem::path& path) {
  return path.generic_string();
}

}  // namespace

std::expected<core::LoadedProject, std::string> read_project(
    const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::unexpected("could not open " + shown(path));

  std::ostringstream text;
  text << file.rdbuf();
  if (file.bad()) return std::unexpected("could not read " + shown(path));

  // Parse failures come back with the parser's own message, which says what
  // was wrong with the document rather than merely that it would not open.
  return core::from_json(text.str());
}

std::expected<void, std::string> write_project(const std::filesystem::path& path,
                                               const core::Project& project) {
  const std::string text = core::to_json(project);

  // Beside the target rather than in the system temp directory, so the rename
  // is within one filesystem and therefore atomic. A rename across volumes
  // silently becomes a copy, which is exactly what this exists to avoid.
  std::filesystem::path staging = path;
  staging += ".saving";

  {
    std::ofstream file(staging, std::ios::binary | std::ios::trunc);
    if (!file) return std::unexpected("could not write " + shown(staging));
    file << text;
    file.flush();
    if (!file) return std::unexpected("could not write " + shown(staging));
  }

  std::error_code error;
  std::filesystem::rename(staging, path, error);
  if (error) {
    // Leaving the staging file behind would be worse than the failure itself:
    // the directory fills with debris nobody knows the meaning of.
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
    return std::unexpected("could not replace " + shown(path) + ": " + error.message());
  }
  return {};
}

std::filesystem::path with_project_extension(std::filesystem::path path) {
  if (path.empty() || path.has_extension()) return path;
  path += kProjectExtension;
  return path;
}

bool has_project_extension(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  // Case-insensitively, because Windows does not care and a project saved from
  // a shell that upper-cased the name is still a project.
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension == kProjectExtension;
}

}  // namespace cutline::editor
