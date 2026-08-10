#include "cutline/editor/browser_binding.hpp"

#include "cutline/core/pool.hpp"
#include "cutline/core/time.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace cutline::editor {
namespace {

[[nodiscard]] std::string lowered(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

/// Compares two names the way a person reads a list: case-insensitively, so
/// `Boiler.mp4` and `beach.mov` are not separated by every capital letter
/// sorting before every lower-case one.
[[nodiscard]] bool name_before(const ui::MediaItem& a, const ui::MediaItem& b) {
  return lowered(a.name) < lowered(b.name);
}

}  // namespace

ui::MediaKind media_kind(const core::Media& media) noexcept {
  // Ordered from most specific: a title is a generated still, and an adjustment
  // layer has neither video nor audio, so testing the flags in the other order
  // would call both of them something else.
  if (media.is_adjustment) return ui::MediaKind::Adjustment;
  if (media.is_text) return ui::MediaKind::Title;
  if (media.is_color) return ui::MediaKind::Color;
  if (media.is_image) return ui::MediaKind::Image;
  if (!media.has_video && media.audio_stream_count > 0) return ui::MediaKind::Audio;
  return ui::MediaKind::Video;
}

std::string media_detail(const core::Media& media, double fps, bool drop_frame) {
  switch (media_kind(media)) {
    case ui::MediaKind::Adjustment: return "adjustment";
    case ui::MediaKind::Title: return "title";
    case ui::MediaKind::Color: return "color";
    case ui::MediaKind::Image:
      // An animated still has a real running time; a plain one does not, and
      // showing its default placement length as though it did would be a lie.
      return media.is_animated && media.duration > 0.0
                 ? core::seconds_to_timecode(media.duration, fps, drop_frame)
                 : "still";
    case ui::MediaKind::Audio:
    case ui::MediaKind::Video: break;
  }
  if (media.duration <= 0.0) return {};
  return core::seconds_to_timecode(media.duration, fps, drop_frame);
}

int media_uses(const core::Project& project, std::string_view media_id) noexcept {
  if (media_id.empty()) return 0;
  int count = 0;
  for (const core::Track& track : project.sequence().tracks) {
    for (const core::Clip& clip : track.clips) {
      if (clip.media_id == media_id) ++count;
    }
  }
  return count;
}

bool media_matches(const core::Media& media, std::string_view query) {
  const std::string terms = lowered(query);
  // Searched together so a term can match either. Two fields checked
  // separately would fail on "boiler mp4" when the name has no extension.
  const std::string haystack = lowered(media.name) + '\n' + lowered(media.path);

  std::size_t at = 0;
  while (at < terms.size()) {
    while (at < terms.size() && std::isspace(static_cast<unsigned char>(terms[at]))) ++at;
    const std::size_t begin = at;
    while (at < terms.size() && !std::isspace(static_cast<unsigned char>(terms[at]))) ++at;
    if (begin == at) break;

    if (haystack.find(terms.substr(begin, at - begin)) == std::string::npos) return false;
  }
  return true;
}

namespace {

/// What a bin row's id looks like. A prefix no generated id starts with, so the
/// two kinds of row can never be mistaken for one another.
constexpr std::string_view kBinRowPrefix = "bin:";

/// Orders a run of media rows by whichever column is chosen. Applied per bin
/// rather than across the pool, because sorting across bins would take entries
/// out of the folders they were put in.
void order(std::vector<ui::MediaItem>& rows, const BrowserOptions& options) {
  // Stable throughout, so entries that compare equal — two stills, two clips of
  // the same length — stay in the order they were imported rather than
  // shuffling every time the list is rebuilt.
  switch (options.sort) {
    case BrowserSort::Pool: break;
    case BrowserSort::Name: std::ranges::stable_sort(rows, name_before); break;
    case BrowserSort::Kind:
      std::ranges::stable_sort(rows, [](const ui::MediaItem& a, const ui::MediaItem& b) {
        return a.kind < b.kind;
      });
      break;
    case BrowserSort::Duration:
      std::ranges::stable_sort(rows, [](const ui::MediaItem& a, const ui::MediaItem& b) {
        return a.duration < b.duration;
      });
      break;
    case BrowserSort::Uses:
      std::ranges::stable_sort(rows, [](const ui::MediaItem& a, const ui::MediaItem& b) {
        return a.uses < b.uses;
      });
      break;
  }
  if (options.descending) std::ranges::reverse(rows);
}

[[nodiscard]] ui::MediaItem media_row(const core::Project& project, const core::Media& media,
                                      const BrowserOptions& options) {
  ui::MediaItem item;
  item.id = media.id;
  // The pool is allowed to hold a nameless entry — a path is enough to import
  // one — and a row with no text at all looks broken.
  item.name = media.name.empty() ? media.path : media.name;
  item.kind = media_kind(media);
  item.duration = media.duration;
  item.detail = media_detail(media, project.sequence().fps, project.sequence().drop_frame);
  item.uses = media_uses(project, media.id);
  item.label_color = media.label_color;
  item.offline = !media.path.empty() &&
                 std::ranges::find(options.offline, media.path) != options.offline.end();
  return item;
}

/// How much a bin holds directly, for the line at the right of its row.
[[nodiscard]] std::string bin_detail(const core::Project& project, std::string_view bin_id) {
  std::size_t count = core::media_in_bin(project, bin_id).size();
  for (const core::Bin& bin : project.bins) {
    if (bin.parent == bin_id) ++count;
  }
  if (count == 0) return "empty";
  return count == 1 ? "1 item" : std::to_string(count) + " items";
}

/// Appends a bin's contents, then recurses into the bins inside it.
///
/// Bins before media, which is where Premiere puts them and what stops the
/// folders of a large pool being scattered down a list of files. Depth is
/// bounded by the number of bins, so a ring hand-edited into a project file
/// cannot recurse for ever.
void emit(const core::Project& project, const BrowserOptions& options, std::string_view bin_id,
          int depth, std::vector<ui::MediaItem>& out) {
  if (depth > static_cast<int>(project.bins.size())) return;

  std::vector<const core::Bin*> children;
  for (const core::Bin& bin : project.bins) {
    if (bin.parent == bin_id) children.push_back(&bin);
  }
  // Always by name, whatever the media is sorted by. A folder has no duration
  // and no uses, so the other orderings would leave the bins in whichever order
  // they happened to be made.
  std::ranges::stable_sort(children, [](const core::Bin* a, const core::Bin* b) {
    return name_before(ui::MediaItem{.name = a->name}, ui::MediaItem{.name = b->name});
  });

  for (const core::Bin* bin : children) {
    const bool open = std::ranges::find(options.expanded, bin->id) != options.expanded.end();
    ui::MediaItem row;
    row.id = bin_row_id(bin->id);
    row.name = bin->name.empty() ? "Bin" : bin->name;
    row.detail = bin_detail(project, bin->id);
    row.depth = depth;
    row.is_bin = true;
    row.expanded = open;
    out.push_back(std::move(row));

    if (open) emit(project, options, bin->id, depth + 1, out);
  }

  std::vector<ui::MediaItem> rows;
  for (const core::Media& media : project.media) {
    // Compared through the project, so an entry naming a bin that has gone is
    // found by asking for the top level rather than vanishing from the panel.
    const bool at_top = core::find_bin(project, media.bin) == nullptr;
    if (at_top ? !bin_id.empty() : media.bin != bin_id) continue;
    rows.push_back(media_row(project, media, options));
  }
  order(rows, options);
  for (ui::MediaItem& row : rows) {
    row.depth = depth;
    out.push_back(std::move(row));
  }
}

}  // namespace

std::string bin_row_id(std::string_view bin_id) {
  return std::string(kBinRowPrefix) + std::string(bin_id);
}

std::string bin_of_row(std::string_view row_id) {
  if (!row_id.starts_with(kBinRowPrefix)) return {};
  return std::string(row_id.substr(kBinRowPrefix.size()));
}

std::vector<ui::MediaItem> browser_items(const core::Project& project,
                                         const BrowserOptions& options) {
  std::vector<ui::MediaItem> items;
  items.reserve(project.media.size());

  if (!options.search.empty()) {
    // Flat while searching. A match hidden inside a closed bin is the one thing
    // a search must not do, and there is no sensible half measure — showing the
    // folders that happen to contain matches makes the result depend on where
    // things were filed rather than on what was typed.
    for (const core::Media& media : project.media) {
      if (!media_matches(media, options.search)) continue;
      items.push_back(media_row(project, media, options));
    }
    order(items, options);
    return items;
  }

  emit(project, options, "", 0, items);
  return items;
}

core::Project remove_media(core::Project project, std::string_view media_id) {
  if (media_id.empty()) return project;

  for (core::Track& track : project.sequence().tracks) {
    std::erase_if(track.clips,
                  [media_id](const core::Clip& clip) { return clip.media_id == media_id; });
  }
  std::erase_if(project.media,
                [media_id](const core::Media& media) { return media.id == media_id; });
  return project;
}

core::Project remove_bin(core::Project project, std::string_view bin_id) {
  const std::vector<std::string> going = core::bins_within(project, bin_id);
  if (going.empty()) return project;

  // The media first, while the bins still exist to say what was in them.
  // Removing the bins first would leave everything reading as top level, and
  // this would take the whole pool.
  for (const std::string& within : going) {
    for (const std::string& media_id : core::media_in_bin(project, within)) {
      project = remove_media(std::move(project), media_id);
    }
  }
  return core::remove_bins(std::move(project), bin_id);
}

core::Project rename_media(core::Project project, std::string_view media_id, std::string name) {
  if (name.empty()) return project;
  const auto found = std::ranges::find(project.media, media_id, &core::Media::id);
  if (found != project.media.end()) found->name = std::move(name);
  return project;
}

}  // namespace cutline::editor
