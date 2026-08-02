#include "cutline/editor/bins.hpp"

#include "cutline/ui/effects_browser.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <ios>
#include <sstream>
#include <system_error>
#include <utility>

namespace cutline::editor {
namespace {

using nlohmann::json;

/// The separator the browser splits folder paths on. Taken from the widget
/// rather than written again, so a bin folder cannot drift out of step with the
/// tree that has to show it.
constexpr char kSeparator = ui::EffectsBrowser::kFolderSeparator;

[[nodiscard]] std::string shown(const std::filesystem::path& path) {
  return path.generic_string();
}

[[nodiscard]] bool usable_name(std::string_view name) {
  // A name with a separator in it would split into two folders in the tree, and
  // the outer one would belong to no bin — nothing could then delete it.
  return !name.empty() && name.find(kSeparator) == std::string_view::npos;
}

[[nodiscard]] Bin* mutable_bin(Bins& bins, std::string_view name) {
  const auto found = std::ranges::find(bins.named, name, &Bin::name);
  return found == bins.named.end() ? nullptr : &*found;
}

}  // namespace

const Bin* find_bin(const Bins& bins, std::string_view name) noexcept {
  const auto found = std::ranges::find(bins.named, name, &Bin::name);
  return found == bins.named.end() ? nullptr : &*found;
}

bool create_bin(Bins& bins, std::string name) {
  if (!usable_name(name) || find_bin(bins, name) != nullptr) return false;
  bins.named.push_back(Bin{.name = std::move(name)});
  return true;
}

bool remove_bin(Bins& bins, std::string_view name) {
  const auto found = std::ranges::find(bins.named, name, &Bin::name);
  if (found == bins.named.end()) return false;
  bins.named.erase(found);
  return true;
}

bool rename_bin(Bins& bins, std::string_view name, std::string renamed) {
  if (!usable_name(renamed)) return false;

  Bin* bin = mutable_bin(bins, name);
  if (bin == nullptr) return false;
  // A dialog dismissed with the text untouched is not an error, so the name it
  // already has is allowed through — but only for the bin that holds it.
  if (bin->name != renamed && find_bin(bins, renamed) != nullptr) return false;

  bin->name = std::move(renamed);
  return true;
}

bool add_to_bin(Bins& bins, std::string_view name, std::string id) {
  if (id.empty()) return false;

  Bin* bin = mutable_bin(bins, name);
  if (bin == nullptr) return false;
  // Twice over, the second copy is indistinguishable from the first and every
  // gesture on one of them would be ambiguous.
  if (std::ranges::find(bin->ids, id) != bin->ids.end()) return false;

  bin->ids.push_back(std::move(id));
  return true;
}

bool remove_from_bin(Bins& bins, std::string_view name, std::string_view id) {
  Bin* bin = mutable_bin(bins, name);
  if (bin == nullptr) return false;

  const auto found = std::ranges::find(bin->ids, id);
  if (found == bin->ids.end()) return false;
  bin->ids.erase(found);
  return true;
}

bool move_in_bin(Bins& bins, std::string_view name, std::size_t from, std::size_t to) {
  Bin* bin = mutable_bin(bins, name);
  if (bin == nullptr || from >= bin->ids.size()) return false;

  // Where it lands once it has been lifted out, so dragging the first entry to
  // the end reads as "to the end" rather than one short of it.
  to = std::min(to, bin->ids.size() - 1);
  if (from == to) return false;

  std::string moved = std::move(bin->ids[from]);
  bin->ids.erase(bin->ids.begin() + static_cast<std::ptrdiff_t>(from));
  bin->ids.insert(bin->ids.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
  return true;
}

std::string bin_folder(std::string_view name) {
  return std::string(kBinFolder) + kSeparator + std::string(name);
}

std::string_view bin_of_folder(std::string_view folder) noexcept {
  const std::string prefix = std::string(kBinFolder) + kSeparator;
  if (!folder.starts_with(prefix)) return {};

  std::string_view name = folder.substr(prefix.size());
  // Only one level down. A path deeper than that is inside something else and
  // is not a bin, whatever it is.
  return name.find(kSeparator) == std::string_view::npos ? name : std::string_view{};
}

std::vector<LibraryEntry> bin_entries(const Bins& bins,
                                      std::span<const LibraryEntry> catalogue) {
  std::vector<LibraryEntry> out;
  for (const Bin& bin : bins.named) {
    const std::string folder = bin_folder(bin.name);
    for (const std::string& id : bin.ids) {
      const auto found = std::ranges::find(catalogue, id, &LibraryEntry::id);
      // An id that names nothing any more — a deleted preset, an effect gone
      // from a later build. Left out of the list and left in the file.
      if (found == catalogue.end()) continue;
      out.push_back(LibraryEntry{.id = found->id, .name = found->name, .folder = folder});
    }
  }
  return out;
}

std::vector<std::string> bin_folders(const Bins& bins) {
  std::vector<std::string> out;
  out.reserve(bins.named.size());
  for (const Bin& bin : bins.named) out.push_back(bin_folder(bin.name));
  return out;
}

// ------------------------------------------------------------- persistence --

std::string to_json(const Bins& bins, int indent) {
  json out;
  out["version"] = kBinSchemaVersion;

  json named = json::array();
  for (const Bin& bin : bins.named) {
    json entry;
    entry["name"] = bin.name;
    entry["ids"] = bin.ids;
    named.push_back(std::move(entry));
  }
  out["bins"] = std::move(named);

  return out.dump(indent);
}

std::expected<Bins, std::string> bins_from_json(std::string_view text) {
  const json parsed = json::parse(text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return std::unexpected("bins file is not a JSON object");
  }

  const int version = parsed.value("version", 0);
  if (version > kBinSchemaVersion) {
    return std::unexpected("bins were written by a newer version of Cutline");
  }

  Bins bins;
  const auto named = parsed.find("bins");
  if (named == parsed.end() || !named->is_array()) return bins;

  for (const json& entry : *named) {
    if (!entry.is_object()) continue;

    Bin bin;
    bin.name = entry.value("name", std::string{});
    // A bin with no usable name cannot be shown or found again, and a second
    // one by a name already read would hide the first.
    if (!usable_name(bin.name) || find_bin(bins, bin.name) != nullptr) continue;

    if (const auto ids = entry.find("ids"); ids != entry.end() && ids->is_array()) {
      for (const json& id : *ids) {
        if (!id.is_string()) continue;
        std::string text_id = id.get<std::string>();
        if (text_id.empty() || std::ranges::find(bin.ids, text_id) != bin.ids.end()) continue;
        bin.ids.push_back(std::move(text_id));
      }
    }

    // An empty bin is kept, unlike an empty preset: it is a folder somebody made
    // and has not filled yet, and losing it on the next start would look like
    // the application forgetting.
    bins.named.push_back(std::move(bin));
  }
  return bins;
}

std::filesystem::path default_bins_path() {
  const char* roaming = std::getenv("APPDATA");
  std::filesystem::path base =
      roaming == nullptr ? std::filesystem::path{"."} : std::filesystem::path{roaming};
  return base / "Cutline" / "bins.json";
}

std::expected<Bins, std::string> read_bins(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  // Not an error: it means nobody has made one yet.
  if (!file) return Bins{};

  std::ostringstream text;
  text << file.rdbuf();
  if (file.bad()) return std::unexpected("could not read " + shown(path));

  return bins_from_json(text.str());
}

std::expected<void, std::string> write_bins(const std::filesystem::path& path,
                                            const Bins& bins) {
  std::error_code error;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      return std::unexpected("could not make " + shown(path.parent_path()) + ": " +
                             error.message());
    }
  }

  // Beside the target so the rename stays within one filesystem and is therefore
  // atomic, exactly as a project, a workspace and the presets are written.
  std::filesystem::path staging = path;
  staging += ".saving";
  {
    std::ofstream file(staging, std::ios::binary | std::ios::trunc);
    if (!file) return std::unexpected("could not write " + shown(staging));
    file << to_json(bins);
    if (!file) return std::unexpected("could not write " + shown(staging));
  }

  std::filesystem::rename(staging, path, error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
    return std::unexpected("could not replace " + shown(path) + ": " + error.message());
  }
  return {};
}

}  // namespace cutline::editor
