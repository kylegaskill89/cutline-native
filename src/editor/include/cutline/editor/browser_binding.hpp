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

  /// The bins showing what is inside them, by id. Everything else is closed.
  ///
  /// Open ones rather than closed ones, so a project opens with its bins shut
  /// and a pool of two hundred entries does not arrive as a wall. It is also the
  /// list that stays short: somebody working has a few bins open, not all but a
  /// few closed.
  ///
  /// Kept by whoever draws the panel rather than in the project, because which
  /// folders happen to be open is about the looking and not about the cut.
  std::span<const std::string> expanded;
};

/// Rows for the browser, filtered and ordered, with bins flattened in the order
/// they should be drawn.
///
/// A search flattens the tree entirely: what somebody typing wants is every
/// match, and a match hidden inside a closed bin is the one thing a search must
/// not do. Bins are dropped from the rows while it is running for the same
/// reason — a folder is not a thing anybody searched for.
[[nodiscard]] std::vector<ui::MediaItem> browser_items(const core::Project& project,
                                                       const BrowserOptions& options = {});

/// The row id of a bin, as the browser sees it.
///
/// Bins and media share one list, so their ids share one namespace and could
/// collide. Prefixing is what keeps "the selected row" answerable without the
/// panel having to remember which kind it last put there.
[[nodiscard]] std::string bin_row_id(std::string_view bin_id);

/// The bin a row id names, or empty when the row is not a bin.
[[nodiscard]] std::string bin_of_row(std::string_view row_id);

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

/// Removes a bin, the bins inside it, and everything filed in any of them —
/// clips included, as Premiere does.
///
/// Lives here rather than in `core::remove_bins` because taking the media means
/// taking the clips that used it, and that is the same "both halves or neither"
/// rule `remove_media` exists for. Ask `core::bin_is_empty` first if this should
/// be confirmed, which for anything holding footage it should.
[[nodiscard]] core::Project remove_bin(core::Project project, std::string_view bin_id);

}  // namespace cutline::editor
