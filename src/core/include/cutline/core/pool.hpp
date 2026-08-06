#pragma once

/// The media pool, and the folders it is filed into.
///
/// Called a pool rather than a bin file to keep it apart from `editor::Bins`,
/// which gathers *effects* into folders in the Effects panel. Same idea, none of
/// the same code and none of the same lifetime: these live in the project and
/// are saved with it, those are a fact about the person using the application.
///
/// A bin holds nothing. It is a name and a place in a tree, and what is in it is
/// whatever names it — the only arrangement where filing a clip cannot leave two
/// lists disagreeing about where it is. Everything here is structure: what
/// happens to the *media* in a bin that goes away is a question about clips, and
/// clips are the editor's business.

#include "cutline/core/model.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cutline::core {

/// The bin with this id, or null. An empty id is the top level, which is not a
/// bin and so has none.
[[nodiscard]] const Bin* find_bin(const Project& p, std::string_view bin_id) noexcept;

/// Makes a bin and appends it, so the caller can find it as `p.bins.back()` —
/// which is what selecting or renaming a bin just made needs.
///
/// A parent that does not exist puts it at the top level rather than refusing.
/// The tree is a way of arranging things, not a constraint to be enforced
/// against its own callers.
[[nodiscard]] Project create_bin(Project p, std::string name, std::string_view parent = {});

[[nodiscard]] Project rename_bin(Project p, std::string_view bin_id, std::string name);

/// Moves a bin inside another, or to the top level with an empty parent.
///
/// A move that would put a bin inside itself, however far down, changes
/// nothing. Refused rather than corrected because there is no sensible
/// correction, and a cycle is not a tree that draws oddly — it is a walk that
/// does not end.
[[nodiscard]] Project move_bin(Project p, std::string_view bin_id, std::string_view parent);

/// Files a pool entry in a bin, or at the top level with an empty bin.
[[nodiscard]] Project file_media(Project p, std::string_view media_id, std::string_view bin_id);

/// Removes a bin and every bin inside it, leaving their media where it was.
///
/// Media whose bin no longer exists reads as top level, so nothing has to be
/// rewritten and nothing is lost. Removing what was *in* the bin is a separate
/// decision, and a destructive one — see `editor::remove_bin`.
[[nodiscard]] Project remove_bins(Project p, std::string_view bin_id);

/// Whether `maybe_inside` is `bin_id` or sits anywhere beneath it.
///
/// Walks upward and gives up after as many steps as there are bins, so a file
/// that arrived with a cycle in it cannot hang the application that opened it.
[[nodiscard]] bool bin_contains(const Project& p, std::string_view bin_id,
                                std::string_view maybe_inside) noexcept;

/// How deep a bin sits, with a top-level bin at zero.
[[nodiscard]] int bin_depth(const Project& p, std::string_view bin_id) noexcept;

/// The ids of every bin at or beneath `bin_id`, itself first.
[[nodiscard]] std::vector<std::string> bins_within(const Project& p, std::string_view bin_id);

/// The ids of the media filed directly in a bin — not in the bins beneath it.
[[nodiscard]] std::vector<std::string> media_in_bin(const Project& p, std::string_view bin_id);

/// Whether a bin holds nothing at all, counting what is beneath it. What tells a
/// "delete this bin" whether it needs to ask first.
[[nodiscard]] bool bin_is_empty(const Project& p, std::string_view bin_id);

}  // namespace cutline::core
