#include "cutline/core/subclips.hpp"

#include "cutline/core/id.hpp"
#include "cutline/core/query.hpp"

#include <algorithm>

namespace cutline::core {
namespace {

/// Shorter than any frame a source is cut at, so a span this short is a
/// mistake rather than a shot.
constexpr double kShortest = 1e-3;

}  // namespace

bool can_make_subclip(const Media& media) noexcept {
  return media.sequence_id.empty() && !is_still_like(media) && media.duration > 0.0;
}

std::string subclip_name(const Media& source) { return source.name + ".Subclip"; }

Project make_subclip(Project p, std::string_view media_id, std::string name, double start,
                     double end, bool restrict_trims, std::string_view bin,
                     std::string* made_id) {
  const auto it = std::ranges::find(p.media, media_id, &Media::id);
  if (it == p.media.end() || !can_make_subclip(*it)) return p;
  start = std::clamp(start, 0.0, it->duration);
  end = std::clamp(end, 0.0, it->duration);
  if (end - start < kShortest) return p;

  Media sub = *it;
  sub.id = new_id("media");
  sub.name = std::move(name);
  sub.bin = std::string(bin);
  sub.subclip = Subclip{.master = it->subclip.has_value() && !it->subclip->master.empty()
                                      ? it->subclip->master
                                      : it->id,
                        .start = start,
                        .end = end,
                        .restrict_trims = restrict_trims};
  std::erase_if(sub.markers,
                [&](const Marker& m) { return m.time < start - 1e-9 || m.time > end + 1e-9; });
  sub.in_point.reset();
  sub.out_point.reset();
  if (!restrict_trims) {
    sub.in_point = start;
    sub.out_point = end;
  }
  if (made_id != nullptr) *made_id = sub.id;
  p.media.push_back(std::move(sub));
  return p;
}

Project edit_subclip(Project p, std::string_view media_id, double start, double end,
                     bool restrict_trims) {
  const auto it = std::ranges::find(p.media, media_id, &Media::id);
  if (it == p.media.end() || !it->subclip.has_value()) return p;
  start = std::clamp(start, 0.0, it->duration);
  end = std::clamp(end, 0.0, it->duration);
  if (end - start < kShortest) return p;
  const bool moved = start != it->subclip->start || end != it->subclip->end;
  it->subclip->start = start;
  it->subclip->end = end;
  it->subclip->restrict_trims = restrict_trims;
  // The marks follow a new span, as Premiere resets them to it; and are kept
  // inside a restricted one.
  if (moved || restrict_trims) {
    it->in_point.reset();
    it->out_point.reset();
    if (!restrict_trims) {
      it->in_point = start;
      it->out_point = end;
    }
  }
  return p;
}

Project convert_to_master_clip(Project p, std::string_view media_id) {
  const auto it = std::ranges::find(p.media, media_id, &Media::id);
  if (it == p.media.end() || !it->subclip.has_value()) return p;
  it->subclip.reset();
  return p;
}

}  // namespace cutline::core
