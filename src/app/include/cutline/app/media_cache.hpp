#pragma once

/// What has already been read out of a file, kept between sessions.
///
/// Waveforms and filmstrips are derived from footage and cost real time to
/// derive: a ten-minute capture's filmstrip is a seek and a decode per frame,
/// measured at roughly half a second each. Nothing about that answer changes
/// while the file does not, and until now every one of them was computed again
/// from nothing on every launch — so opening yesterday's cut meant paying for
/// yesterday's scrolling a second time.
///
/// Premiere calls this the Media Cache and puts it under a folder you can move
/// or empty, which is what this is.
///
/// **Nothing here is required.** Every read may fail and every write may fail,
/// and both are silent: a cache that cannot be read is a slow session, and a
/// cache that cannot be written is a slow session next time. Neither is worth
/// a dialog, and neither may ever be allowed to fail an edit.
///
/// The layout is a directory per source and a file per thing in it, rather than
/// one file per source with an index. Frames arrive a few at a time as somebody
/// scrolls, and appending to an indexed file means rewriting the index — which
/// is a window in which the file describes frames that are not there yet.
/// Separate files have no such window: a frame either exists whole or does not
/// exist, and the worst a half-written one costs is one frame re-extracted.

#include "cutline/ui/timeline.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cutline::app {

/// Where the cache lives when nobody has chosen anywhere.
///
/// Under `LOCALAPPDATA` rather than `APPDATA`, and that is the whole decision:
/// the roaming folder follows a user between machines, and this is bulk data
/// that is worthless on another machine — every entry is keyed to a file path
/// on this one — and large enough to make a roaming profile painful.
[[nodiscard]] std::filesystem::path default_media_cache_dir();

/// The directory to use, given what the preference says. Empty means the
/// default, which is what a fresh install has.
[[nodiscard]] std::filesystem::path media_cache_dir(std::string_view configured);

/// What identifies a source's cached work: its path, its size, and when it was
/// last written.
///
/// Size and modification time are in the key rather than checked against a
/// stored copy, so a file that has been re-encoded or replaced simply misses
/// and is read again. The alternative — one key per path, validated on read —
/// has a failure mode this does not: a stale entry that *looks* valid gives
/// somebody a waveform belonging to footage they no longer have.
///
/// Empty when the file cannot be stat'ed, which reads as "do not cache this".
[[nodiscard]] std::string media_cache_key(const std::filesystem::path& source);

// ------------------------------------------------------------- waveforms --

/// The stored envelope for a source's stream, if there is one.
[[nodiscard]] std::optional<ui::Waveform> read_cached_waveform(
    const std::filesystem::path& dir, std::string_view key, int stream);

/// Stores one. Silent on failure.
void write_cached_waveform(const std::filesystem::path& dir, std::string_view key, int stream,
                           const ui::Waveform& wave);

// ------------------------------------------------------------ filmstrips --

/// A stretch of a source, stored whole.
///
/// Per stretch rather than per frame, and that is a correction: the extractor
/// spreads *n* frames evenly across whatever range it is given, so where the
/// frames land depends on the range asked for. Filing them under which step of
/// a fixed grid they fell nearest lost one whenever two landed in the same step
/// — which a short stretch does immediately, because a minimum frame count
/// makes it sample more finely than the grid.
///
/// So a stretch is the unit. It cannot collide, it needs no manifest saying
/// which frames to expect, and it matches how the timeline actually asks: the
/// same project reopened and looked at in the same place asks for the same
/// stretch and finds it. A different zoom misses and extracts, and then that
/// one is stored too.
struct CachedStrip {
  double from = 0.0;
  double to = 0.0;
  int count = 0;
  int height = 0;
};

/// The stored frames for exactly this stretch, if they are there.
[[nodiscard]] std::optional<std::vector<ui::FilmFrame>> read_cached_strip(
    const std::filesystem::path& dir, std::string_view key, const CachedStrip& what);

/// Stores them. Silent on failure.
void write_cached_strip(const std::filesystem::path& dir, std::string_view key,
                        const CachedStrip& what, const std::vector<ui::FilmFrame>& frames);

// ------------------------------------------------------------- housekeeping --

/// How much is on disk, in bytes. For saying something true on the settings
/// page next to a button that empties it.
[[nodiscard]] std::uintmax_t media_cache_bytes(const std::filesystem::path& dir);

/// Deletes everything in the cache. Returns how many bytes went.
///
/// Safe at any moment: everything in here is derived, so the worst emptying it
/// while something is being read can cost is that the read misses.
std::uintmax_t clear_media_cache(const std::filesystem::path& dir);

}  // namespace cutline::app
