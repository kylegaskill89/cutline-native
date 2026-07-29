#pragma once

/// The project's media pool, as rows for the browser.
///
/// The same seam as `timeline_binding`: the core has no idea anything is being
/// listed, the browser has no idea a project exists, and this is the one place
/// that knows both. Sorting and searching live here rather than in the widget
/// because they are questions about media — how long a clip is, how many times
/// it is used — and a widget that could answer those would have to know what a
/// clip is.

#include "cutline/core/model.hpp"
#include "cutline/ui/browser.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// What kind of thing a pool entry is, from what the model records about it.
[[nodiscard]] ui::MediaKind media_kind(const core::Media& media) noexcept;

/// The right-hand column: a duration for anything with one, and what it is for
/// anything without.
[[nodiscard]] std::string media_detail(const core::Media& media, double fps);

/// How many clips in the sequence use this media.
///
/// Counted rather than stored, because a stored count is one more thing that
/// can disagree with the project after an undo.
[[nodiscard]] int media_uses(const core::Project& project, std::string_view media_id) noexcept;

/// Whether an entry matches a search. Case-insensitive, and every
/// whitespace-separated term has to appear somewhere in the name or the path —
/// so "boiler mp4" finds it and the order of the words does not matter.
[[nodiscard]] bool media_matches(const core::Media& media, std::string_view query);

enum class BrowserSort {
  /// The order they were imported in, which is what the pool actually is.
  Pool,
  Name,
  Kind,
  Duration,
  /// Least-used first, which is how unused media is found.
  Uses,
};

struct BrowserOptions {
  BrowserSort sort = BrowserSort::Pool;
  bool descending = false;
  std::string search;

  /// Paths the renderer could not open. Borrowed, so whoever passes this keeps
  /// them alive across the call — `ProjectPreview::missing_media()` owns the
  /// usual one.
  std::span<const std::string> offline;
};

/// Rows for the browser, filtered and ordered.
[[nodiscard]] std::vector<ui::MediaItem> browser_items(const core::Project& project,
                                                       const BrowserOptions& options = {});

/// Removes an entry from the pool, along with every clip that used it.
///
/// Both halves or neither: leaving the clips behind would give a sequence that
/// renders as holes and cannot be repaired, since the media they name is gone.
/// Ask `media_uses` first if that needs confirming.
[[nodiscard]] core::Project remove_media(core::Project project, std::string_view media_id);

/// Renames a pool entry. The file on disk is untouched — this is the name shown
/// in the browser and on the clips, which is a property of the project.
[[nodiscard]] core::Project rename_media(core::Project project, std::string_view media_id,
                                         std::string name);

}  // namespace cutline::editor
