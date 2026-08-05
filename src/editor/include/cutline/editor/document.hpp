#pragma once

/// Reading and writing project files.
///
/// `core::serialize` turns a project into text and back; this is the part that
/// touches the disk, which is deliberately not in the core — the model should
/// be testable without a filesystem, and the failure modes here are about
/// permissions and half-written files rather than about projects.

#include "cutline/core/serialize.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace cutline::editor {

/// The extension a project is saved with, dot included.
inline constexpr std::string_view kProjectExtension = ".cutline";

/// Reads and parses a project file.
///
/// Warnings from the parse — media that no longer resolves — are carried
/// through rather than raised, so a project whose footage has moved still
/// opens and can be relinked.
[[nodiscard]] std::expected<core::LoadedProject, std::string> read_project(
    const std::filesystem::path& path);

/// Writes a project.
///
/// Written to a temporary beside the target and renamed over it, so an
/// interruption partway through leaves the previous file intact. Saving over
/// an afternoon's work with half a file is not a failure anyone recovers from.
[[nodiscard]] std::expected<void, std::string> write_project(const std::filesystem::path& path,
                                                             const core::Project& project);

/// `path` with the project extension, if it has none. What a save dialog
/// should be handed when the user typed a bare name.
[[nodiscard]] std::filesystem::path with_project_extension(std::filesystem::path path);

/// Whether a path names a project, by its extension and case-insensitively.
///
/// The counterpart of the above, and what tells a file named on the command
/// line from a media file named there — which are the two things worth doing
/// with one and want opposite treatment.
[[nodiscard]] bool has_project_extension(const std::filesystem::path& path);

}  // namespace cutline::editor
