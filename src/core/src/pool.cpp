#include "cutline/core/pool.hpp"

#include "cutline/core/id.hpp"

#include <algorithm>
#include <utility>

namespace cutline::core {
namespace {

[[nodiscard]] Bin* find_bin_mutable(Project& p, std::string_view bin_id) noexcept {
  if (bin_id.empty()) return nullptr;
  const auto it = std::ranges::find(p.bins, bin_id, &Bin::id);
  return it == p.bins.end() ? nullptr : &*it;
}

}  // namespace

const Bin* find_bin(const Project& p, std::string_view bin_id) noexcept {
  if (bin_id.empty()) return nullptr;
  const auto it = std::ranges::find(p.bins, bin_id, &Bin::id);
  return it == p.bins.end() ? nullptr : &*it;
}

Project create_bin(Project p, std::string name, std::string_view parent) {
  Bin bin;
  bin.id = new_id("bin");
  bin.name = std::move(name);
  // Only kept when it names something. A parent that has gone would otherwise
  // hide the new bin the moment it was made, which looks exactly like a button
  // that does nothing.
  if (find_bin(p, parent) != nullptr) bin.parent = std::string(parent);
  p.bins.push_back(std::move(bin));
  return p;
}

Project rename_bin(Project p, std::string_view bin_id, std::string name) {
  if (Bin* bin = find_bin_mutable(p, bin_id); bin != nullptr) bin->name = std::move(name);
  return p;
}

Project move_bin(Project p, std::string_view bin_id, std::string_view parent) {
  Bin* bin = find_bin_mutable(p, bin_id);
  if (bin == nullptr) return p;
  // Into itself, or into something already inside it. Either makes a ring the
  // drawing would walk for ever.
  if (bin_contains(p, bin_id, parent)) return p;
  bin->parent = find_bin(p, parent) != nullptr ? std::string(parent) : std::string{};
  return p;
}

Project file_media(Project p, std::string_view media_id, std::string_view bin_id) {
  const auto it = std::ranges::find(p.media, media_id, &Media::id);
  if (it == p.media.end()) return p;
  it->bin = find_bin(p, bin_id) != nullptr ? std::string(bin_id) : std::string{};
  return p;
}

Project remove_bins(Project p, std::string_view bin_id) {
  const std::vector<std::string> going = bins_within(p, bin_id);
  if (going.empty()) return p;
  std::erase_if(p.bins, [&](const Bin& bin) {
    return std::ranges::find(going, bin.id) != going.end();
  });
  // The media is deliberately left naming a bin that has gone, which reads as
  // top level. Rewriting it would be the same answer at more risk: a pass that
  // missed one would file it somewhere nobody can see.
  return p;
}

bool bin_contains(const Project& p, std::string_view bin_id,
                  std::string_view maybe_inside) noexcept {
  if (bin_id.empty()) return false;
  std::string_view at = maybe_inside;
  // Bounded by the number of bins: a file that arrived with a ring in it must
  // not be able to hang the application that opened it.
  for (std::size_t steps = 0; steps <= p.bins.size(); ++steps) {
    if (at.empty()) return false;
    if (at == bin_id) return true;
    const Bin* bin = find_bin(p, at);
    if (bin == nullptr) return false;
    at = bin->parent;
  }
  return false;
}

int bin_depth(const Project& p, std::string_view bin_id) noexcept {
  int depth = 0;
  const Bin* bin = find_bin(p, bin_id);
  while (bin != nullptr && !bin->parent.empty() &&
         depth <= static_cast<int>(p.bins.size())) {
    bin = find_bin(p, bin->parent);
    ++depth;
  }
  return depth;
}

std::vector<std::string> bins_within(const Project& p, std::string_view bin_id) {
  std::vector<std::string> found;
  if (find_bin(p, bin_id) == nullptr) return found;
  found.emplace_back(bin_id);
  for (const Bin& bin : p.bins) {
    if (bin.id != bin_id && bin_contains(p, bin_id, bin.id)) found.push_back(bin.id);
  }
  return found;
}

std::vector<std::string> media_in_bin(const Project& p, std::string_view bin_id) {
  std::vector<std::string> found;
  for (const Media& media : p.media) {
    // Compared against what the entry actually holds, so media naming a bin
    // that has gone is only ever found by asking for the top level.
    const bool at_top = find_bin(p, media.bin) == nullptr;
    if (at_top ? bin_id.empty() : media.bin == bin_id) found.push_back(media.id);
  }
  return found;
}

bool bin_is_empty(const Project& p, std::string_view bin_id) {
  const std::vector<std::string> within = bins_within(p, bin_id);
  if (within.empty()) return true;      // no such bin, so nothing in it
  if (within.size() > 1) return false;  // a bin inside it is something in it
  return media_in_bin(p, bin_id).empty();
}

}  // namespace cutline::core
