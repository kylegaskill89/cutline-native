#pragma once

/// Premiere's preview files and the render bar above the timeline.
///
/// The sequence's picture is cut into pieces at every edit, and each piece is
/// judged: plain footage plays straight from its source and needs nothing;
/// anything composited may need rendering, and Premiere draws that yellow, or
/// red where it will not keep up. A rendered piece is green and plays from its
/// preview file.
///
/// A preview file is named for what is in its piece: the clips over it, where
/// they sit, what is done to them, and the sequence's frame. So nothing needs
/// telling when a render goes stale. An edit changes what a piece is called,
/// and the file of the old name is simply no longer asked for. Undoing the edit
/// brings the old name, and its file, back.

#include "cutline/core/model.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace cutline::editor {

enum class RenderStatus {
  /// Plain footage, played from its source: no bar.
  None,
  /// Composited, and probably kept up with: yellow.
  Unrendered,
  /// Composited, and unlikely to play in real time: red.
  Heavy,
  /// A preview file is there for it: green.
  Rendered,
};

struct RenderPiece {
  double start = 0.0;
  double end = 0.0;
  RenderStatus status = RenderStatus::None;
  /// What the piece is called, from what is in it; empty for a plain one.
  std::string key;

  friend bool operator==(const RenderPiece&, const RenderPiece&) = default;
};

/// The open sequence's picture, cut into pieces at every edit and transition,
/// each judged and named. Gaps with nothing on any track are left out. A
/// piece whose file is in `folder` is Rendered.
[[nodiscard]] std::vector<RenderPiece> render_pieces(const core::Project& project,
                                                     const std::filesystem::path& folder);

/// Where a piece's preview file is, or would be.
[[nodiscard]] std::filesystem::path preview_file(const std::filesystem::path& folder,
                                                 const std::string& key);

/// Where preview files are kept: beside the other caches, as Premiere keeps
/// them apart from the project.
[[nodiscard]] std::filesystem::path default_previews_folder();

/// The pieces a Render In to Out would render: the unrendered ones that
/// overlap [from, to), whole.
[[nodiscard]] std::vector<RenderPiece> pieces_to_render(const std::vector<RenderPiece>& pieces,
                                                        double from, double to);

/// Takes every preview file away. How many went.
std::size_t delete_preview_files(const std::filesystem::path& folder);

}  // namespace cutline::editor
