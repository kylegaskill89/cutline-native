#include "cutline/core/graphic.hpp"

#include "cutline/core/keyframe.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace cutline::core {
namespace {

/// How deep pins may chain -- a bar pinned to a name pinned to a frame edge
/// is three -- before the rest are left where they were put. Pins that loop
/// are refused when they are made; this is what stops one from a file written
/// by hand from looping for ever.
constexpr int kPinDepth = 16;

[[nodiscard]] double eased(double fraction) noexcept {
  return ease_fraction(std::clamp(fraction, 0.0, 1.0), Interp::Ease);
}

/// One layer while it is being placed.
struct Working {
  bool resolved = false;
  bool resolving = false;
  Bounds content;
  double scale = 1.0;
  double width = 0.0;
  double height = 0.0;
  LayerExtent extent;
};

}  // namespace

double drawing_margin(const GraphicLayer& layer) noexcept {
  const GraphicStyle& style = layer.style;
  double margin = 2.0;
  for (const GraphicStroke& stroke : style.strokes) margin += std::max(0.0, stroke.width);
  if (layer.kind == GraphicKind::Text && style.background.has_value()) {
    margin = std::max(margin, std::max(0.0, style.background->padding) + 2.0);
  }
  if (style.shadow.has_value()) {
    const GraphicShadow& shadow = *style.shadow;
    margin += std::max(0.0, shadow.distance) + std::max(0.0, shadow.size) +
              2.0 * std::max(0.0, shadow.blur);
  }
  return std::ceil(margin);
}

LayerExtent measure_without_fonts(const GraphicLayer& layer) {
  LayerExtent extent;
  if (layer.kind == GraphicKind::Rectangle || layer.kind == GraphicKind::Ellipse) {
    extent.content = {std::max(0.0, layer.width), std::max(0.0, layer.height)};
  }
  const double margin = drawing_margin(layer);
  extent.left = extent.top = extent.right = extent.bottom = margin;
  return extent;
}

double graphic_time(const Graphic& graphic, double local_t, double duration) noexcept {
  const double intro = std::max(0.0, graphic.intro);
  const double outro = std::max(0.0, graphic.outro);
  if (graphic.length <= 0.0 || intro + outro <= 0.0 || duration <= 0.0) return local_t;
  if (local_t <= intro) return local_t;
  if (local_t >= duration - outro) return graphic.length - (duration - local_t);
  // The middle runs from the end of the intro to the start of the outro, in
  // both the clip as it is and as it was written.
  const double span = duration - outro - intro;
  const double written = graphic.length - outro - intro;
  if (span <= 0.0) return intro;
  return intro + (local_t - intro) * written / span;
}

double layer_value(const GraphicLayer& layer, std::string_view property, double t) noexcept {
  if (const auto found = layer.keyframes.find(std::string(property));
      found != layer.keyframes.end() && !found->second.empty()) {
    return eval_keyframes(found->second, t);
  }
  if (property == "x") return layer.x;
  if (property == "y") return layer.y;
  if (property == "scale") return layer.scale;
  if (property == "rotation") return layer.rotation;
  if (property == "opacity") return layer.opacity;
  return 0.0;
}

std::size_t character_count(std::string_view text) noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

std::string first_characters(std::string_view text, std::size_t count) {
  std::size_t seen = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
    if (seen == count) return std::string(text.substr(0, i));
    ++seen;
  }
  return std::string(text);
}

std::string displayed_text(const GraphicLayer& layer) {
  std::string text = layer.text;
  if (layer.style.all_caps) {
    // ASCII only. A byte-wise upper-casing of UTF-8 would corrupt it, and the
    // fonts that matter for titles render their own small capitals anyway.
    for (char& c : text) {
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return text;
}

std::vector<PlacedLayer> place_graphic(const Graphic& graphic, double local_t, double duration,
                                       double canvas_w, double canvas_h,
                                       const LayerMeasurer& measure) {
  const double t = graphic_time(graphic, local_t, duration);
  const std::size_t count = graphic.layers.size();
  std::vector<Working> working(count);
  const Bounds frame{0.0, 0.0, canvas_w, canvas_h};

  const auto index_of = [&](std::string_view id) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < count; ++i) {
      if (graphic.layers[i].id == id) return i;
    }
    return std::nullopt;
  };

  // Where each layer is before anything moves it for an animation: its own
  // position, then its pins. A pinned layer needs whatever it is pinned to
  // placed first, so this recurses, and a loop stops at the depth limit.
  const auto resolve = [&](this const auto& self, std::size_t i, int depth) -> const Working& {
    Working& w = working[i];
    if (w.resolved || w.resolving) return w;
    w.resolving = true;
    const GraphicLayer& layer = graphic.layers[i];

    w.scale = std::max(0.0, layer_value(layer, "scale", t));
    w.width = layer.width;
    w.height = layer.height;
    w.extent = measure ? measure(layer) : measure_without_fonts(layer);
    const double cw = w.extent.content.width * w.scale;
    const double ch = w.extent.content.height * w.scale;
    const double ax = layer_value(layer, "x", t) * canvas_w;
    const double ay = layer_value(layer, "y", t) * canvas_h;
    w.content = Bounds{ax - layer.anchor_x * cw, ay - layer.anchor_y * ch, cw, ch};

    const GraphicPin& pin = layer.pin;
    if (pin.any() && depth < kPinDepth) {
      Bounds parent = frame;
      bool found = true;
      if (!pin.to.empty()) {
        const std::optional<std::size_t> to = index_of(pin.to);
        found = to.has_value() && *to != i;
        if (found) parent = self(*to, depth + 1).content;
      }
      if (found) {
        const bool stretches =
            layer.kind == GraphicKind::Rectangle || layer.kind == GraphicKind::Ellipse;
        // Each axis on its own. Both edges pinned stretches a shape between
        // them; anything else -- one edge, or text, which has no width to
        // give -- moves the layer so the pinned edge keeps its distance.
        const auto settle = [&](bool lead, bool trail, double lead_at, double trail_at,
                                double& start, double& size, double& own) {
          if (lead && trail && stretches && w.scale > 0.0) {
            start = lead_at;
            size = std::max(0.0, trail_at - lead_at);
            own = size / w.scale;
          } else if (lead) {
            start = lead_at;
          } else if (trail) {
            start = trail_at - size;
          }
        };
        settle(pin.left, pin.right, parent.left + pin.left_offset,
               parent.right() + pin.right_offset, w.content.left, w.content.width, w.width);
        settle(pin.top, pin.bottom, parent.top + pin.top_offset,
               parent.bottom() + pin.bottom_offset, w.content.top, w.content.height, w.height);
        if (stretches) {
          // The drawing is measured again at the size the pins gave it.
          GraphicLayer stretched = layer;
          stretched.width = w.width;
          stretched.height = w.height;
          w.extent = measure ? measure(stretched) : measure_without_fonts(stretched);
        }
      }
    }

    w.resolving = false;
    w.resolved = true;
    return w;
  };

  for (std::size_t i = 0; i < count; ++i) resolve(i, 0);

  // Roll and crawl move everything together, by how far the whole graphic
  // has to travel: from below the frame to above it, or from its right to its
  // left, unless it is to start or end where it was put.
  double scroll_x = 0.0;
  double scroll_y = 0.0;
  if (graphic.scroll.mode != ScrollMode::None) {
    std::optional<Bounds> all;
    for (std::size_t i = 0; i < count; ++i) {
      if (!graphic.layers[i].visible) continue;
      const Bounds& b = working[i].content;
      if (!all) {
        all = b;
        continue;
      }
      const double left = std::min(all->left, b.left);
      const double top = std::min(all->top, b.top);
      all = Bounds{left, top, std::max(all->right(), b.right()) - left,
                   std::max(all->bottom(), b.bottom()) - top};
    }
    if (all.has_value()) {
      const GraphicScroll& scroll = graphic.scroll;
      const double moving = std::max(0.0, duration - scroll.preroll - scroll.postroll);
      double progress = moving > 0.0 ? std::clamp((local_t - scroll.preroll) / moving, 0.0, 1.0)
                                     : 1.0;
      // Eased at either end over the seconds asked for, and steady between.
      if (moving > 0.0 && (scroll.ease_in > 0.0 || scroll.ease_out > 0.0)) {
        const double in = std::clamp(scroll.ease_in / moving, 0.0, 0.5);
        const double out = std::clamp(scroll.ease_out / moving, 0.0, 0.5);
        // A ramp up, a steady middle and a ramp down, scaled so it still ends
        // at one: the area under a trapezoid.
        const double speed = 1.0 / (1.0 - in / 2.0 - out / 2.0);
        const double p = progress;
        if (p < in) {
          progress = speed * p * p / (2.0 * in);
        } else if (p > 1.0 - out) {
          const double left = 1.0 - p;
          progress = 1.0 - speed * left * left / (2.0 * out);
        } else {
          progress = speed * (p - in / 2.0);
        }
      }
      if (scroll.mode == ScrollMode::Roll) {
        const double from = scroll.start_off ? canvas_h - all->top : 0.0;
        const double to = scroll.end_off ? -all->bottom() : canvas_h - all->bottom();
        scroll_y = from + (to - from) * progress;
      } else {
        const double from = scroll.start_off ? canvas_w - all->left : 0.0;
        const double to = scroll.end_off ? -all->right() : canvas_w - all->right();
        scroll_x = from + (to - from) * progress;
      }
    }
  }

  std::vector<PlacedLayer> placed;
  placed.reserve(count);
  // Bottom first: the list is top first, as the panel shows it.
  for (std::size_t back = count; back-- > 0;) {
    const GraphicLayer& layer = graphic.layers[back];
    if (!layer.visible) continue;
    const Working& w = working[back];

    PlacedLayer out;
    out.layer = &layer;
    out.index = back;
    out.content = w.content;
    out.content.left += scroll_x;
    out.content.top += scroll_y;
    out.width = w.width;
    out.height = w.height;
    out.opacity = std::clamp(layer_value(layer, "opacity", t), 0.0, 1.0);
    double rotation = layer_value(layer, "rotation", t);
    double grow = 1.0;

    // The reveals, by the clip's own time rather than the keyframes': an
    // entrance lasts what it says however the middle was stretched.
    const auto reveal = [&](const GraphicReveal& reveal, double fraction) {
      const double p = eased(fraction);
      switch (reveal.kind) {
        case RevealKind::None: break;
        case RevealKind::Fade: out.opacity *= p; break;
        case RevealKind::SlideLeft:
          out.content.left -= (out.content.right()) * (1.0 - p);
          break;
        case RevealKind::SlideRight:
          out.content.left += (canvas_w - out.content.left) * (1.0 - p);
          break;
        case RevealKind::SlideUp:
          out.content.top += (canvas_h - out.content.top) * (1.0 - p);
          break;
        case RevealKind::SlideDown:
          out.content.top -= (out.content.bottom()) * (1.0 - p);
          break;
        case RevealKind::Scale: grow *= p; break;
        case RevealKind::Wipe: out.wipe = std::min(out.wipe, p); break;
        case RevealKind::Typewriter: {
          const std::size_t all = character_count(displayed_text(layer));
          const auto shown = static_cast<std::size_t>(std::floor(fraction * all + 1e-9));
          out.characters = std::min(out.characters, std::min(all, shown));
          break;
        }
      }
    };
    if (layer.reveal_in.kind != RevealKind::None && layer.reveal_in.duration > 0.0) {
      const double fraction = local_t / layer.reveal_in.duration;
      if (fraction < 1.0) reveal(layer.reveal_in, fraction);
    }
    if (layer.reveal_out.kind != RevealKind::None && layer.reveal_out.duration > 0.0) {
      const double fraction = (duration - local_t) / layer.reveal_out.duration;
      if (fraction < 1.0) reveal(layer.reveal_out, fraction);
    }

    // The anchor in canvas pixels: the point scale and rotation happen about.
    const double anchor_x = out.content.left + layer.anchor_x * out.content.width;
    const double anchor_y = out.content.top + layer.anchor_y * out.content.height;
    if (grow != 1.0) {
      out.content = Bounds{anchor_x - (anchor_x - out.content.left) * grow,
                           anchor_y - (anchor_y - out.content.top) * grow,
                           out.content.width * grow, out.content.height * grow};
    }

    // The whole drawing, margins included, turned about the anchor.
    const double s = w.scale * grow;
    const double drawn_left = out.content.left - w.extent.left * s;
    const double drawn_top = out.content.top - w.extent.top * s;
    const double drawn_w = out.content.width + (w.extent.left + w.extent.right) * s;
    const double drawn_h = out.content.height + (w.extent.top + w.extent.bottom) * s;
    const double cx = drawn_left + drawn_w / 2.0 - anchor_x;
    const double cy = drawn_top + drawn_h / 2.0 - anchor_y;
    const double radians = rotation * std::numbers::pi / 180.0;
    out.box = LayerBox{anchor_x + cx * std::cos(radians) - cy * std::sin(radians),
                       anchor_y + cx * std::sin(radians) + cy * std::cos(radians), drawn_w,
                       drawn_h, rotation};
    placed.push_back(out);
  }
  return placed;
}

Graphic graphic_from_text(const TextSpec& spec) {
  GraphicLayer layer;
  layer.id = "text";
  layer.name = spec.content;
  layer.kind = GraphicKind::Text;
  layer.text = spec.content;
  GraphicStyle& style = layer.style;
  style.font_family = spec.font_family;
  style.font_size = spec.font_size;
  style.bold = spec.bold;
  style.italic = spec.italic;
  style.align = spec.align;
  style.fill = spec.color;
  if (spec.stroke_color.has_value() && spec.stroke_width > 0.0) {
    style.strokes.push_back(GraphicStroke{.color = *spec.stroke_color, .width = spec.stroke_width});
  }
  if (spec.background.has_value()) {
    // The old title filled its whole image, which reached past the text by
    // the stroke and two pixels.
    style.background = GraphicBackground{
        .color = *spec.background, .opacity = 1.0,
        .padding = (spec.stroke_color.has_value() ? std::max(0.0, spec.stroke_width) : 0.0) + 2.0};
  }
  if (spec.shadow) {
    // The old shadow: black at 60%, down and right by 4% of the font size
    // each way, blurred by 6%.
    style.shadow = GraphicShadow{.color = "#000000",
                                 .opacity = 0.6,
                                 .angle = 135.0,
                                 .distance = spec.font_size * 0.04 * std::numbers::sqrt2,
                                 .size = 0.0,
                                 .blur = spec.font_size * 0.06};
  }
  Graphic graphic;
  graphic.layers.push_back(std::move(layer));
  return graphic;
}

Graphic scaled_graphic(Graphic graphic, double factor) {
  if (!(factor > 0.0) || factor == 1.0) return graphic;
  for (GraphicLayer& layer : graphic.layers) {
    GraphicStyle& style = layer.style;
    style.font_size *= factor;
    style.leading *= factor;
    for (GraphicStroke& stroke : style.strokes) stroke.width *= factor;
    if (style.background) {
      style.background->padding *= factor;
      style.background->corner_radius *= factor;
    }
    if (style.shadow) {
      style.shadow->distance *= factor;
      style.shadow->size *= factor;
      style.shadow->blur *= factor;
    }
    if (layer.box_width) *layer.box_width *= factor;
    layer.width *= factor;
    layer.height *= factor;
    layer.corner_radius *= factor;
    layer.pin.left_offset *= factor;
    layer.pin.top_offset *= factor;
    layer.pin.right_offset *= factor;
    layer.pin.bottom_offset *= factor;
  }
  return graphic;
}

const GraphicLayer* find_layer(const Graphic& graphic, std::string_view id) noexcept {
  for (const GraphicLayer& layer : graphic.layers) {
    if (layer.id == id) return &layer;
  }
  return nullptr;
}

GraphicLayer* find_layer(Graphic& graphic, std::string_view id) noexcept {
  for (GraphicLayer& layer : graphic.layers) {
    if (layer.id == id) return &layer;
  }
  return nullptr;
}

}  // namespace cutline::core
