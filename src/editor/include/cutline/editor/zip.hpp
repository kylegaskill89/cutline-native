#pragma once

/// ZIP archives, as far as templates need them: a Motion Graphics template
/// (.mogrt) is one, and so is the graphic inside it.
///
/// Reading takes the names and sizes from the central directory at the end of
/// the archive rather than from the header before each entry, because a
/// writer that streams (After Effects does) leaves those headers' sizes empty
/// and puts the real ones after the data.

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// Every file in an archive, by its name inside it, holding its contents.
/// Folders are left out. Stored and deflated entries are read; anything else,
/// an encrypted entry or a ZIP64 archive, is refused by name.
[[nodiscard]] std::expected<std::map<std::string, std::string>, std::string> read_zip(
    std::string_view bytes);

struct ZipEntry {
  std::string name;
  std::string contents;
};

/// An archive of `entries`, deflated or stored. Exposed for the tests, which
/// make their templates rather than carry someone else's.
[[nodiscard]] std::string write_zip(const std::vector<ZipEntry>& entries, bool deflate = true);

}  // namespace cutline::editor
