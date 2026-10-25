#include "cutline/editor/graphics.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/id.hpp"
#include "cutline/core/keyframe.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <set>
#include <utility>

namespace cutline::editor {
namespace {

using core::Graphic;
using core::GraphicKind;
using core::GraphicLayer;

[[nodiscard]] core::Media* find_media(core::Project& project, std::string_view media_id) {
  const auto found = std::ranges::find(project.media, media_id, &core::Media::id);
  return found == project.media.end() ? nullptr : &*found;
}

/// Shortened for a list: long enough to identify, short enough to fit.
[[nodiscard]] std::string shortened(std::string text) {
  constexpr std::size_t kMaxName = 32;
  if (core::character_count(text) > kMaxName) {
    text = core::first_characters(text, kMaxName - 1) + "...";
  }
  return text;
}

/// What a graphic is called in the project panel: the first line of its first
/// text layer, which is what anyone would call it.
[[nodiscard]] std::string name_for(const Graphic& graphic) {
  for (const GraphicLayer& layer : graphic.layers) {
    if (layer.kind != GraphicKind::Text) continue;
    std::string first = layer.text.substr(0, layer.text.find('\n'));
    if (!first.empty()) return shortened(std::move(first));
  }
  return "Graphic";
}

[[nodiscard]] GraphicLayer* layer_in(Graphic& graphic, std::string_view id) {
  return core::find_layer(graphic, id);
}

/// An id no layer of the graphic has.
[[nodiscard]] std::string fresh_layer_id(const Graphic& graphic) {
  for (int n = 1;; ++n) {
    std::string id = std::format("layer{}", n);
    if (core::find_layer(graphic, id) == nullptr) return id;
  }
}

/// Premiere's names for a new shape: "Shape 01", "Shape 02", counting the
/// ones already there.
[[nodiscard]] std::string fresh_name(const Graphic& graphic, std::string_view stem) {
  for (int n = 1;; ++n) {
    std::string name = std::format("{} {:02}", stem, n);
    if (std::ranges::none_of(graphic.layers,
                             [&](const GraphicLayer& layer) { return layer.name == name; })) {
      return name;
    }
  }
}

/// Where a new graphic goes when no track is named, as Premiere puts one: on
/// the first video track above everything that is showing over the span it
/// will cover, so it lands on top of the picture rather than over a clip of it.
/// A track is added at the top when every one is taken.
[[nodiscard]] std::string free_track_above(core::Project& project, double from, double to) {
  // Stored top first, so walking backwards goes up from the bottom.
  std::vector<std::size_t> video;
  for (std::size_t i = 0; i < project.sequence().tracks.size(); ++i) {
    if (project.sequence().tracks[i].kind == core::TrackKind::Video) video.push_back(i);
  }
  std::optional<std::size_t> highest_busy;
  for (std::size_t k = video.size(); k-- > 0;) {
    const core::Track& track = project.sequence().tracks[video[k]];
    const bool busy = std::ranges::any_of(track.clips, [&](const core::Clip& clip) {
      return clip.start < to && core::clip_end(clip) > from;
    });
    if (busy) highest_busy = k;
  }
  if (!highest_busy.has_value()) {
    return video.empty() ? std::string{} : project.sequence().tracks[video.back()].id;
  }
  if (*highest_busy > 0) return project.sequence().tracks[video[*highest_busy - 1]].id;
  project = core::add_video_track(std::move(project));
  return project.sequence().tracks.front().id;
}

/// Whether following the pins from `from` reaches `target`.
[[nodiscard]] bool pinned_through(const Graphic& graphic, std::string_view from,
                                  std::string_view target) {
  std::set<std::string> seen;
  std::string at(from);
  while (!at.empty() && seen.insert(at).second) {
    if (at == target) return true;
    const GraphicLayer* layer = core::find_layer(graphic, at);
    if (layer == nullptr || !layer->pin.any()) return false;
    at = layer->pin.to;
  }
  return false;
}

}  // namespace

// ----------------------------------------------------------------- graphics --

Graphic default_title() {
  GraphicLayer layer = text_layer("Title");
  layer.id = "layer1";
  Graphic graphic;
  graphic.layers.push_back(std::move(layer));
  return graphic;
}

core::Project add_graphic(core::Project project, Graphic graphic, std::string* id) {
  core::Media media;
  media.id = core::new_id("graphic");
  media.name = name_for(graphic);
  media.is_graphic = true;
  // `has_video` means this contributes picture, which a graphic does — it is
  // what `place_media` looks at to decide whether anything goes on a video
  // track, so without it a graphic could be made and never placed.
  media.has_video = true;
  // Nothing on disk, and no inherent length. The duration is what a placement
  // uses when there is no source to take one from.
  media.duration = kDefaultTitleLength;
  media.graphic = std::move(graphic);

  if (id != nullptr) *id = media.id;
  project.media.push_back(std::move(media));
  return project;
}

core::Project add_graphic_at(core::Project project, Graphic graphic, double at,
                             std::string_view video_track_id, std::string* clip_id) {
  std::string media_id;
  project = add_graphic(std::move(project), std::move(graphic), &media_id);
  std::string on_track(video_track_id);
  if (on_track.empty()) on_track = free_track_above(project, at, at + kDefaultTitleLength);

  std::vector<std::string> existing;
  for (const core::Track& track : project.sequence().tracks) {
    for (const core::Clip& clip : track.clips) existing.push_back(clip.id);
  }

  project = core::place_media(std::move(project), media_id, at, on_track);

  // The placement decides the clip's id; finding it is how a caller can select
  // what it just made. Comparing against what was there beats guessing at the
  // id generator's next value.
  if (clip_id != nullptr) {
    for (const core::Track& track : project.sequence().tracks) {
      for (const core::Clip& clip : track.clips) {
        if (clip.media_id != media_id) continue;
        if (std::ranges::find(existing, clip.id) != existing.end()) continue;
        *clip_id = clip.id;
      }
    }
  }
  return project;
}

const Graphic* graphic_of(const core::Project& project, std::string_view media_id) noexcept {
  const auto found = std::ranges::find(project.media, media_id, &core::Media::id);
  if (found == project.media.end() || !found->is_graphic || !found->graphic.has_value()) {
    return nullptr;
  }
  return &*found->graphic;
}

const Graphic* clip_graphic(const core::Project& project, std::string_view clip_id) noexcept {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return nullptr;
  return graphic_of(project, clip->media_id);
}

core::Project set_graphic(core::Project project, std::string_view media_id, Graphic graphic) {
  core::Media* media = find_media(project, media_id);
  if (media == nullptr || !media->is_graphic) return project;
  if (media->graphic.has_value() && *media->graphic == graphic) return project;

  // The name follows the words, unless someone has renamed it to something the
  // text no longer explains. Comparing against the old name is what tells
  // those two apart.
  if (!media->graphic.has_value() || media->name == name_for(*media->graphic)) {
    media->name = name_for(graphic);
  }
  media->graphic = std::move(graphic);
  return project;
}

// ------------------------------------------------------------------- layers --

GraphicLayer text_layer(std::string text) {
  GraphicLayer layer;
  layer.kind = GraphicKind::Text;
  layer.text = std::move(text);
  return layer;
}

GraphicLayer shape_layer(GraphicKind kind) {
  GraphicLayer layer;
  layer.kind = kind == GraphicKind::Ellipse ? GraphicKind::Ellipse : GraphicKind::Rectangle;
  layer.text.clear();
  // Premiere's new shape is a mid grey, so it shows over black and white
  // alike until it is given a colour.
  layer.style.fill = "#a0a0a0";
  return layer;
}

GraphicLayer image_layer(std::string path) {
  GraphicLayer layer;
  layer.kind = GraphicKind::Image;
  layer.text.clear();
  layer.path = std::move(path);
  return layer;
}

Graphic add_layer(Graphic graphic, GraphicLayer layer, std::string* id) {
  if (layer.id.empty() || core::find_layer(graphic, layer.id) != nullptr) {
    layer.id = fresh_layer_id(graphic);
  }
  if (layer.name.empty() && layer.kind != GraphicKind::Text) {
    if (layer.kind == GraphicKind::Image) {
      const std::size_t slash = layer.path.find_last_of("/\\");
      layer.name = slash == std::string::npos ? layer.path : layer.path.substr(slash + 1);
    } else {
      layer.name = fresh_name(graphic, "Shape");
    }
  }
  if (id != nullptr) *id = layer.id;
  graphic.layers.insert(graphic.layers.begin(), std::move(layer));
  return graphic;
}

Graphic remove_layer(Graphic graphic, std::string_view id) {
  std::erase_if(graphic.layers, [&](const GraphicLayer& layer) { return layer.id == id; });
  // Whatever was pinned to it is pinned to nothing now, and stays where it is.
  for (GraphicLayer& layer : graphic.layers) {
    if (layer.pin.to == id) layer.pin = core::GraphicPin{};
  }
  return graphic;
}

Graphic duplicate_layer(Graphic graphic, std::string_view id, std::string* copy_id) {
  const auto found = std::ranges::find(graphic.layers, id, &GraphicLayer::id);
  if (found == graphic.layers.end()) return graphic;
  GraphicLayer copy = *found;
  copy.id = fresh_layer_id(graphic);
  if (!copy.name.empty()) copy.name += " copy";
  if (copy_id != nullptr) *copy_id = copy.id;
  graphic.layers.insert(found, std::move(copy));
  return graphic;
}

Graphic move_layer(Graphic graphic, std::string_view id, std::size_t to) {
  const auto found = std::ranges::find(graphic.layers, id, &GraphicLayer::id);
  if (found == graphic.layers.end()) return graphic;
  GraphicLayer layer = std::move(*found);
  graphic.layers.erase(found);
  to = std::min(to, graphic.layers.size());
  graphic.layers.insert(graphic.layers.begin() + static_cast<std::ptrdiff_t>(to),
                        std::move(layer));
  return graphic;
}

Graphic set_layer(Graphic graphic, GraphicLayer layer) {
  if (GraphicLayer* at = layer_in(graphic, layer.id); at != nullptr) *at = std::move(layer);
  return graphic;
}

std::string layer_label(const GraphicLayer& layer) {
  if (!layer.name.empty()) return layer.name;
  if (layer.kind == GraphicKind::Text) {
    std::string first = layer.text.substr(0, layer.text.find('\n'));
    if (!first.empty()) return shortened(std::move(first));
  }
  return "Layer";
}

// ---------------------------------------------------------------- keyframes --

Graphic set_layer_keyframe(Graphic graphic, std::string_view id, std::string_view property,
                           double t, double value) {
  if (GraphicLayer* layer = layer_in(graphic, id); layer != nullptr) {
    core::upsert_keyframe(layer->keyframes[std::string(property)], t, value);
  }
  return graphic;
}

Graphic remove_layer_keyframe(Graphic graphic, std::string_view id, std::string_view property,
                              double t) {
  if (GraphicLayer* layer = layer_in(graphic, id); layer != nullptr) {
    const auto found = layer->keyframes.find(std::string(property));
    if (found != layer->keyframes.end()) {
      core::remove_keyframe_near(found->second, t);
      if (found->second.empty()) layer->keyframes.erase(found);
    }
  }
  return graphic;
}

Graphic clear_layer_keyframes(Graphic graphic, std::string_view id, std::string_view property,
                              double t) {
  GraphicLayer* layer = layer_in(graphic, id);
  if (layer == nullptr) return graphic;
  const double value = core::layer_value(*layer, property, t);
  layer->keyframes.erase(std::string(property));
  return set_layer_value(std::move(graphic), id, property, t, value);
}

Graphic set_layer_value(Graphic graphic, std::string_view id, std::string_view property, double t,
                        double value) {
  GraphicLayer* layer = layer_in(graphic, id);
  if (layer == nullptr) return graphic;
  if (const auto found = layer->keyframes.find(std::string(property));
      found != layer->keyframes.end() && !found->second.empty()) {
    core::upsert_keyframe(found->second, t, value);
    return graphic;
  }
  if (property == "x") layer->x = value;
  if (property == "y") layer->y = value;
  if (property == "scale") layer->scale = std::max(0.0, value);
  if (property == "rotation") layer->rotation = value;
  if (property == "opacity") layer->opacity = std::clamp(value, 0.0, 1.0);
  return graphic;
}

// -------------------------------------------------------- responsive design --

std::expected<Graphic, std::string> pin_layer(Graphic graphic, std::string_view id,
                                              std::string_view to, PinEdges edges,
                                              double canvas_w, double canvas_h,
                                              const core::LayerMeasurer& measure) {
  GraphicLayer* layer = layer_in(graphic, id);
  if (layer == nullptr) return std::unexpected("there is no such layer");
  if (!to.empty()) {
    if (core::find_layer(graphic, to) == nullptr) return std::unexpected("there is no such layer");
    if (to == id || pinned_through(graphic, to, id)) {
      return std::unexpected("a layer cannot be pinned to one pinned to it");
    }
  }

  // Where both are now, before the pin, at the start of the clip: the
  // distances to keep.
  const std::vector<core::PlacedLayer> placed =
      core::place_graphic(graphic, 0.0, 0.0, canvas_w, canvas_h, measure);
  const auto bounds_of = [&](std::string_view which) -> std::optional<core::Bounds> {
    for (const core::PlacedLayer& at : placed) {
      if (at.layer->id == which) return at.content;
    }
    return std::nullopt;
  };
  const std::optional<core::Bounds> own = bounds_of(id);
  const std::optional<core::Bounds> parent =
      to.empty() ? std::optional(core::Bounds{0.0, 0.0, canvas_w, canvas_h}) : bounds_of(to);
  if (!own.has_value() || !parent.has_value()) {
    return std::unexpected("a hidden layer cannot be pinned, or pinned to");
  }

  core::GraphicPin pin;
  pin.to = std::string(to);
  pin.left = edges.left;
  pin.top = edges.top;
  pin.right = edges.right;
  pin.bottom = edges.bottom;
  pin.left_offset = own->left - parent->left;
  pin.top_offset = own->top - parent->top;
  pin.right_offset = own->right() - parent->right();
  pin.bottom_offset = own->bottom() - parent->bottom();
  layer->pin = pin;
  return graphic;
}

// ---------------------------------------------------------------- alignment --

Graphic align_layers(Graphic graphic, const std::vector<std::string>& ids, Align align,
                     double canvas_w, double canvas_h, const core::LayerMeasurer& measure) {
  if (ids.empty() || canvas_w <= 0.0 || canvas_h <= 0.0) return graphic;
  const std::vector<core::PlacedLayer> placed =
      core::place_graphic(graphic, 0.0, 0.0, canvas_w, canvas_h, measure);

  std::vector<const core::PlacedLayer*> chosen;
  for (const core::PlacedLayer& at : placed) {
    if (std::ranges::find(ids, at.layer->id) != ids.end()) chosen.push_back(&at);
  }
  if (chosen.empty()) return graphic;

  // One layer lines up with the frame; several with the box round them all.
  core::Bounds against{0.0, 0.0, canvas_w, canvas_h};
  if (chosen.size() > 1) {
    double left = std::numeric_limits<double>::max();
    double top = left;
    double right = std::numeric_limits<double>::lowest();
    double bottom = right;
    for (const core::PlacedLayer* at : chosen) {
      left = std::min(left, at->content.left);
      top = std::min(top, at->content.top);
      right = std::max(right, at->content.right());
      bottom = std::max(bottom, at->content.bottom());
    }
    against = core::Bounds{left, top, right - left, bottom - top};
  }

  for (const core::PlacedLayer* at : chosen) {
    GraphicLayer* layer = layer_in(graphic, at->layer->id);
    if (layer == nullptr) continue;
    double dx = 0.0;
    double dy = 0.0;
    switch (align) {
      case Align::Left: dx = against.left - at->content.left; break;
      case Align::CentreX:
        dx = (against.left + against.width / 2.0) - (at->content.left + at->content.width / 2.0);
        break;
      case Align::Right: dx = against.right() - at->content.right(); break;
      case Align::Top: dy = against.top - at->content.top; break;
      case Align::CentreY:
        dy = (against.top + against.height / 2.0) - (at->content.top + at->content.height / 2.0);
        break;
      case Align::Bottom: dy = against.bottom() - at->content.bottom(); break;
    }
    // The whole of a movement, when it is animated: every keyframe shifts by
    // the same amount, so the layer still travels the way it did.
    const auto shift = [&](std::string_view property, double& value, double by) {
      const auto found = layer->keyframes.find(std::string(property));
      if (found == layer->keyframes.end() || found->second.empty()) {
        value += by;
        return;
      }
      for (core::Keyframe& key : found->second) key.v += by;
    };
    shift("x", layer->x, dx / canvas_w);
    shift("y", layer->y, dy / canvas_h);
  }
  return graphic;
}

// ------------------------------------------------------------ master styles --

Graphic apply_master_style(Graphic graphic, std::string_view id,
                           const core::GraphicMasterStyle& style) {
  if (GraphicLayer* layer = layer_in(graphic, id); layer != nullptr) {
    layer->style = style.style;
    layer->master_style = style.name;
  }
  return graphic;
}

core::Project create_master_style(core::Project project, std::string_view media_id,
                                  std::string_view layer_id, std::string name) {
  const Graphic* graphic = graphic_of(project, media_id);
  if (graphic == nullptr || name.empty()) return project;
  const GraphicLayer* layer = core::find_layer(*graphic, layer_id);
  if (layer == nullptr) return project;

  core::GraphicMasterStyle style{.name = std::move(name), .style = layer->style};
  // A name already taken is replaced, as saving over a style in Premiere is.
  const auto found = std::ranges::find(project.graphic_styles, style.name,
                                       &core::GraphicMasterStyle::name);
  if (found != project.graphic_styles.end()) {
    *found = style;
  } else {
    project.graphic_styles.push_back(style);
  }
  Graphic linked = apply_master_style(*graphic, layer_id, style);
  project = editor::set_graphic(std::move(project), media_id, std::move(linked));
  return set_master_style(std::move(project), style);
}

core::Project set_master_style(core::Project project, core::GraphicMasterStyle style) {
  const auto found = std::ranges::find(project.graphic_styles, style.name,
                                       &core::GraphicMasterStyle::name);
  if (found == project.graphic_styles.end()) return project;
  *found = style;
  for (core::Media& media : project.media) {
    if (!media.graphic.has_value()) continue;
    for (GraphicLayer& layer : media.graphic->layers) {
      if (layer.master_style == style.name) layer.style = style.style;
    }
  }
  return project;
}

bool follows_master(const core::Project& project, const GraphicLayer& layer) noexcept {
  if (layer.master_style.empty()) return false;
  const auto found = std::ranges::find(project.graphic_styles, layer.master_style,
                                       &core::GraphicMasterStyle::name);
  return found != project.graphic_styles.end() && found->style == layer.style;
}

}  // namespace cutline::editor
