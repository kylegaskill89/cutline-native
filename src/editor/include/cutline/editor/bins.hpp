#pragma once

/// User bins: folders you make yourself, to gather the things you actually use.
///
/// Premiere calls these custom bins and puts them at the top of the Effects
/// panel, above the catalogue. The reason they exist is that the catalogue is
/// organised the way a catalogue has to be — by what a thing *is* — and nobody
/// works that way. A person cutting a documentary reaches for the same six
/// entries all week, and those six live in five different folders.
///
/// A bin holds **ids, not copies**. An effect gathered into a bin is the same
/// effect that is still under its category, and a preset gathered into one still
/// follows the preset when it is saved over. That is the difference between a
/// bin and a preset: a preset is a *thing*, a bin is a *shortcut to things*.
///
/// The consequence is that an id in a bin can go stale — a preset that has since
/// been deleted, or an effect gone from a later build. That is handled where the
/// list is made rather than here: a bin keeps what it was told to keep, and
/// `bin_entries` shows only what still exists. Dropping it from the file instead
/// would mean a build that temporarily lacked an effect silently emptied
/// somebody's bins.
///
/// Saved beside the presets, and for the same reason: which effects you keep to
/// hand is a fact about you, not about whichever project happened to be open.

#include "cutline/editor/effects_binding.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// Bumped when the on-disk shape changes incompatibly. A file from the future is
/// refused rather than half-read.
inline constexpr int kBinSchemaVersion = 1;

/// The folder user bins are shown under, so they sit together and above the
/// catalogue rather than scattered through it.
inline constexpr std::string_view kBinFolder = "Bins";

/// One bin: a name and the ids gathered into it, in the order they should show.
struct Bin {
  std::string name;
  std::vector<std::string> ids;

  friend bool operator==(const Bin&, const Bin&) = default;
};

/// Every bin, in the order they should be offered.
struct Bins {
  std::vector<Bin> named;

  friend bool operator==(const Bins&, const Bins&) = default;
};

[[nodiscard]] const Bin* find_bin(const Bins& bins, std::string_view name) noexcept;

/// Makes an empty one. Refuses an empty name, and refuses a name already taken —
/// two bins called Favourites are indistinguishable once the panel has drawn
/// them, and the second would look like the first had lost its contents.
///
/// A name containing the folder separator is refused too: it would split into
/// two folders in the tree, one of which nothing could then delete.
bool create_bin(Bins& bins, std::string name);

/// Removes one, contents and all. Reports whether it was there.
///
/// Nothing is lost that cannot be gathered again — a bin holds ids, and the
/// effects themselves are untouched — which is why this needs no confirmation of
/// its own beyond the one the caller asks for.
bool remove_bin(Bins& bins, std::string_view name);

/// Renames one, keeping its place and its contents. Refuses the same names
/// `create_bin` does, and refuses renaming one that is not there.
///
/// Renaming to the name it already has succeeds and changes nothing, so a dialog
/// dismissed with the text untouched is not an error.
bool rename_bin(Bins& bins, std::string_view name, std::string renamed);

/// Puts an id in a bin. Reports whether anything changed.
///
/// An id already in that bin is refused rather than added twice: the second copy
/// is indistinguishable from the first and every gesture on one would be
/// ambiguous. The same id in *two* bins is fine, and expected.
bool add_to_bin(Bins& bins, std::string_view name, std::string id);

/// Takes one out again.
bool remove_from_bin(Bins& bins, std::string_view name, std::string_view id);

/// Moves an entry within its bin, to sit at `to` once it has been lifted out.
///
/// The order in a bin is the whole point of gathering things by hand, so it is
/// something you can set rather than something that happens to you. An index
/// past the end lands at the end; a bin that is not there changes nothing.
bool move_in_bin(Bins& bins, std::string_view name, std::size_t from, std::size_t to);

/// The library rows the bins add, ready to be put in front of the catalogue.
///
/// Names come from `catalogue`, so an entry shows the name it shows everywhere
/// else and a renamed preset is renamed here too. An id that names nothing any
/// more is left out — see the note at the top on why it stays in the file.
///
/// An empty bin has no entries and so appears nowhere in a tree built out of
/// paths, which would make a bin you have just made look as though it failed to
/// be created. `bin_folders` is the other half: the folders that exist whether
/// or not anything is in them.
[[nodiscard]] std::vector<LibraryEntry> bin_entries(const Bins& bins,
                                                    std::span<const LibraryEntry> catalogue);

/// Every bin's folder path, empty ones included.
[[nodiscard]] std::vector<std::string> bin_folders(const Bins& bins);

/// The folder path a bin is shown at: `Bins/Favourites`.
[[nodiscard]] std::string bin_folder(std::string_view name);

/// The bin named by a folder path, or empty when the path is not a bin's. What
/// turns a right-click on a row into an operation on a bin.
[[nodiscard]] std::string_view bin_of_folder(std::string_view folder) noexcept;

// ------------------------------------------------------------- persistence --

[[nodiscard]] std::string to_json(const Bins& bins, int indent = 2);
[[nodiscard]] std::expected<Bins, std::string> bins_from_json(std::string_view text);

/// Where the file lives: beside the presets and the rest of the settings.
[[nodiscard]] std::filesystem::path default_bins_path();

/// Reads it. A missing file is not an error — it means nobody has made a bin
/// yet — and gives back an empty set.
[[nodiscard]] std::expected<Bins, std::string> read_bins(const std::filesystem::path& path);

/// Writes it, through a staging file and a rename, so an interrupted save cannot
/// leave a half-written one behind.
[[nodiscard]] std::expected<void, std::string> write_bins(const std::filesystem::path& path,
                                                          const Bins& bins);

}  // namespace cutline::editor
