/// The Essential Graphics panel: the layers of the selected graphic, and every
/// property of the one chosen.
///
/// Premiere's Edit tab, top to bottom in its order: the layer list and the New
/// Layer buttons; Responsive Design, Position; Align and Transform; Master
/// Styles; Text; Appearance. With no layer chosen, the graphic itself:
/// Responsive Design, Time, and Roll and Crawl. And the in and out animations,
/// which Premiere leaves to keyframes and this offers as presets as well.
///
/// Rebuilt whenever the document or the selection changes, as the inspector
/// is. A control writes one change -- one undo step -- when it is let go, and
/// shows the change on the picture while it is being dragged.

#include "app.hpp"

#if CUTLINE_HAVE_TEXT
#include "cutline/text/raster.hpp"
#endif

namespace cutline::tool {
namespace {

using cutline::core::Graphic;
using cutline::core::GraphicKind;
using cutline::core::GraphicLayer;
using cutline::ui::NumericField;
using cutline::ui::ValueRange;

/// The graphic being edited: the selected clip's.
struct Target {
  std::string clip_id;
  std::string media_id;
  Graphic graphic;
  /// The playhead, in the clip's own seconds and in the graphic's keyframe
  /// time, which differ once its intro and outro are protected.
  double local = 0.0;
  double key_time = 0.0;
  double duration = 0.0;
};

[[nodiscard]] std::optional<Target> target_of(const App& app) {
  const auto selection = app.session.selection();
  if (selection.empty()) return std::nullopt;
  const cutline::core::Project& project = app.session.project();
  const cutline::core::Clip* clip = cutline::core::find_clip(project, selection.front());
  if (clip == nullptr) return std::nullopt;
  const Graphic* graphic = cutline::editor::graphic_of(project, clip->media_id);
  if (graphic == nullptr) return std::nullopt;
  Target target{.clip_id = clip->id, .media_id = clip->media_id, .graphic = *graphic};
  target.duration = cutline::core::clip_duration(*clip);
  target.local = std::clamp(app.session.playhead() - clip->start, 0.0, target.duration);
  target.key_time = cutline::core::graphic_time(*graphic, target.local, target.duration);
  return target;
}

[[nodiscard]] cutline::core::LayerMeasurer measurer() { return layer_measurer(); }

/// Writes a changed graphic: one undo step, and everything showing it told.
void commit(App& app, const std::string& media_id, Graphic graphic) {
  app.live_gesture = false;
  app.live_project.reset();
  app.session.apply(
      cutline::editor::set_graphic(app.session.project(), media_id, std::move(graphic)));
  refresh_browser(app);
  refresh_timeline(app);
  invalidate_preview(app);
  app.inspector_stale = true;
}

/// Shows a changed graphic on the picture while a control is being dragged,
/// without writing it. See `App::live_project`.
void preview(App& app, const std::string& media_id, const Graphic& graphic) {
  app.live_gesture = true;
  app.live_project = cutline::editor::set_graphic(app.session.project(), media_id, graphic);
  invalidate_preview(app);
  refresh_handles(app);
}

/// A change to one layer of the graphic, worked out afresh from the document
/// each time: a control holds the layer's id, never a copy that could be
/// stale by the time it is used.
using LayerChange = std::function<void(GraphicLayer&, double)>;

[[nodiscard]] std::optional<Graphic> changed_layer(const App& app, const std::string& layer_id,
                                                   const LayerChange& change, double value) {
  const std::optional<Target> target = target_of(app);
  if (!target.has_value()) return std::nullopt;
  Graphic graphic = target->graphic;
  GraphicLayer* layer = cutline::core::find_layer(graphic, layer_id);
  if (layer == nullptr) return std::nullopt;
  change(*layer, value);
  return graphic;
}

void heading(Box& into, std::string text) {
  auto& label = into.emplace<Label>(std::move(text));
  label.set_bold(true);
}

/// A labelled number that edits one layer.
NumericField& layer_number(App& app, Box& into, std::string label, ValueRange range,
                           double value, std::string suffix, int decimals,
                           const std::string& layer_id, LayerChange change) {
  auto& row = into.emplace<Box>(Axis::Horizontal);
  row.emplace<Label>(std::move(label)).set_small(true);
  auto& number = row.emplace<NumericField>(range, value);
  number.set_suffix(std::move(suffix));
  number.set_decimals(decimals);
  number.set_on_change([&app, layer_id, change](double v) {
    const std::optional<Target> target = target_of(app);
    if (const auto graphic = changed_layer(app, layer_id, change, v);
        graphic.has_value() && target.has_value()) {
      preview(app, target->media_id, *graphic);
    }
  });
  number.set_on_commit([&app, layer_id, change](double v) {
    const std::optional<Target> target = target_of(app);
    if (const auto graphic = changed_layer(app, layer_id, change, v);
        graphic.has_value() && target.has_value()) {
      commit(app, target->media_id, *graphic);
    }
  });
  row.emplace<Spacer>();
  return number;
}

/// A checkbox that edits one layer.
Checkbox& layer_check(App& app, Box& into, std::string label, bool on,
                      const std::string& layer_id, LayerChange change) {
  auto& box = into.emplace<Checkbox>(std::move(label), on);
  box.set_on_change([&app, layer_id, change](bool now) {
    const std::optional<Target> target = target_of(app);
    if (const auto graphic = changed_layer(app, layer_id, change, now ? 1.0 : 0.0);
        graphic.has_value() && target.has_value()) {
      commit(app, target->media_id, *graphic);
    }
  });
  return box;
}

/// A colour that edits one layer. The panel is not rebuilt as it is picked:
/// rebuilding destroys the swatch, and with it the picker hanging open above.
ColorSwatch& layer_colour(App& app, Box& into, std::string label, const std::string& colour,
                          const std::string& layer_id,
                          std::function<void(GraphicLayer&, const std::string&)> change) {
  auto& row = into.emplace<Box>(Axis::Horizontal);
  row.emplace<Label>(std::move(label)).set_small(true);
  auto& swatch = row.emplace<ColorSwatch>(parse_color(colour, Color{1.0f, 1.0f, 1.0f, 1.0f}));
  swatch.set_on_commit([&app, layer_id, change](const Color& picked) {
    const std::optional<Target> target = target_of(app);
    if (!target.has_value()) return;
    Graphic graphic = target->graphic;
    GraphicLayer* layer = cutline::core::find_layer(graphic, layer_id);
    if (layer == nullptr) return;
    change(*layer, to_hex(picked));
    app.session.apply(
        cutline::editor::set_graphic(app.session.project(), target->media_id, std::move(graphic)));
    refresh_browser(app);
    invalidate_preview(app);
  });
  row.emplace<Spacer>();
  return swatch;
}

/// A number that edits the graphic as a whole.
NumericField& graphic_number(App& app, Box& into, std::string label, ValueRange range,
                             double value, std::string suffix, int decimals,
                             std::function<void(Graphic&, double, double)> change) {
  auto& row = into.emplace<Box>(Axis::Horizontal);
  row.emplace<Label>(std::move(label)).set_small(true);
  auto& number = row.emplace<NumericField>(range, value);
  number.set_suffix(std::move(suffix));
  number.set_decimals(decimals);
  number.set_on_commit([&app, change](double v) {
    std::optional<Target> target = target_of(app);
    if (!target.has_value()) return;
    change(target->graphic, v, target->duration);
    commit(app, target->media_id, std::move(target->graphic));
  });
  row.emplace<Spacer>();
  return number;
}

constexpr std::array<std::pair<cutline::core::RevealKind, const char*>, 9> kReveals{{
    {cutline::core::RevealKind::None, "None"},
    {cutline::core::RevealKind::Fade, "Fade"},
    {cutline::core::RevealKind::SlideLeft, "Slide from left"},
    {cutline::core::RevealKind::SlideRight, "Slide from right"},
    {cutline::core::RevealKind::SlideUp, "Slide from below"},
    {cutline::core::RevealKind::SlideDown, "Slide from above"},
    {cutline::core::RevealKind::Scale, "Scale"},
    {cutline::core::RevealKind::Wipe, "Wipe"},
    {cutline::core::RevealKind::Typewriter, "Typewriter"},
}};

[[nodiscard]] std::string kind_mark(GraphicKind kind) {
  switch (kind) {
    case GraphicKind::Text: return "T";
    case GraphicKind::Rectangle: return "▭";
    case GraphicKind::Ellipse: return "○";
    case GraphicKind::Image: return "▣";
  }
  return "?";
}

// ------------------------------------------------------------- the layers --

void build_layer_list(App& app, Box& body, const Target& target) {
  for (const GraphicLayer& layer : target.graphic.layers) {
    auto& row = body.emplace<Box>(Axis::Horizontal);
    // The eye: shown or not, without losing the layer.
    auto& eye = row.emplace<Checkbox>("", layer.visible);
    eye.set_tooltip("Show or hide this layer");
    eye.set_on_change([&app, id = layer.id](bool on) {
      std::optional<Target> now = target_of(app);
      if (!now.has_value()) return;
      if (GraphicLayer* l = cutline::core::find_layer(now->graphic, id); l != nullptr) {
        l->visible = on;
        commit(app, now->media_id, std::move(now->graphic));
      }
    });
    auto& pick = row.emplace<Button>(kind_mark(layer.kind) + "  " +
                                     cutline::editor::layer_label(layer));
    pick.set_selected(layer.id == app.graphic_layer);
    pick.set_on_click([&app, id = layer.id] {
      // Choosing the chosen layer again goes back to the graphic as a whole,
      // which is where its timing is set.
      app.graphic_layer = app.graphic_layer == id ? std::string{} : id;
      app.graphics_stale = true;
      refresh_handles(app);
      mark_dirty(app);
    });
    row.emplace<Spacer>();
  }

  // Premiere's New Layer menu, as buttons: the panel is narrow but tall.
  auto& make = body.emplace<Box>(Axis::Horizontal);
  const auto add = [&app](GraphicLayer layer) {
    std::optional<Target> now = target_of(app);
    if (!now.has_value()) return;
    std::string id;
    Graphic graphic = cutline::editor::add_layer(std::move(now->graphic), std::move(layer), &id);
    app.graphic_layer = id;
    commit(app, now->media_id, std::move(graphic));
  };
  make.emplace<Button>("Text", [add] { add(cutline::editor::text_layer()); })
      .set_tooltip("A new text layer, on top");
  make.emplace<Button>("Rectangle", [add] {
    add(cutline::editor::shape_layer(GraphicKind::Rectangle));
  }).set_tooltip("A new rectangle, on top");
  make.emplace<Button>("Ellipse", [add] {
    add(cutline::editor::shape_layer(GraphicKind::Ellipse));
  }).set_tooltip("A new ellipse, on top");
  make.emplace<Button>("Image...", [&app, add] {
    const auto chosen = choose_picture_to_open(app.main.window);
    if (chosen.has_value()) add(cutline::editor::image_layer(chosen->string()));
  }).set_tooltip("A picture from a file, on top");
  make.emplace<Spacer>();

  if (app.graphic_layer.empty()) return;
  const auto index = static_cast<std::size_t>(
      std::ranges::find(target.graphic.layers, app.graphic_layer, &GraphicLayer::id) -
      target.graphic.layers.begin());
  auto& order = body.emplace<Box>(Axis::Horizontal);
  const auto move = [&app, index](int by) {
    std::optional<Target> now = target_of(app);
    if (!now.has_value()) return;
    const auto to = static_cast<std::size_t>(std::max<std::ptrdiff_t>(
        0, static_cast<std::ptrdiff_t>(index) + by));
    commit(app, now->media_id,
           cutline::editor::move_layer(std::move(now->graphic), app.graphic_layer, to));
  };
  auto& up = order.emplace<IconButton>(IconButton::Icon::ArrowUp, [move] { move(-1); });
  up.set_tooltip("Bring this layer forward");
  up.set_enabled(index > 0);
  auto& down = order.emplace<IconButton>(IconButton::Icon::ArrowDown, [move] { move(1); });
  down.set_tooltip("Send this layer back");
  down.set_enabled(index + 1 < target.graphic.layers.size());
  order.emplace<Button>("Duplicate", [&app] {
    std::optional<Target> now = target_of(app);
    if (!now.has_value()) return;
    std::string copy;
    Graphic graphic =
        cutline::editor::duplicate_layer(std::move(now->graphic), app.graphic_layer, &copy);
    app.graphic_layer = copy;
    commit(app, now->media_id, std::move(graphic));
  });
  auto& remove = order.emplace<IconButton>(IconButton::Icon::Cross, [&app] {
    std::optional<Target> now = target_of(app);
    if (!now.has_value()) return;
    Graphic graphic = cutline::editor::remove_layer(std::move(now->graphic), app.graphic_layer);
    app.graphic_layer.clear();
    commit(app, now->media_id, std::move(graphic));
  });
  remove.set_tooltip("Delete this layer");
  order.emplace<Spacer>();
}

// ------------------------------------------------------ the graphic itself --

void build_graphic_timing(App& app, Box& body, const Target& target) {
  const Graphic& graphic = target.graphic;
  heading(body, "Responsive Design - Time");
  // Setting either records how long the graphic is now: the length its
  // keyframes were written against, which the protected ends keep to.
  const auto protect = [](bool intro) {
    return [intro](Graphic& g, double v, double duration) {
      (intro ? g.intro : g.outro) = std::max(0.0, v);
      g.length = (g.intro > 0.0 || g.outro > 0.0) ? duration : 0.0;
    };
  };
  graphic_number(app, body, "Intro", ValueRange{.minimum = 0.0, .maximum = 60.0, .step = 0.1},
                 graphic.intro, " s", 2, protect(true));
  graphic_number(app, body, "Outro", ValueRange{.minimum = 0.0, .maximum = 60.0, .step = 0.1},
                 graphic.outro, " s", 2, protect(false));

  heading(body, "Roll and Crawl");
  auto& mode_row = body.emplace<Box>(Axis::Horizontal);
  mode_row.emplace<Label>("Movement").set_small(true);
  auto& mode = mode_row.emplace<Dropdown>(std::vector<std::string>{"Still", "Roll", "Crawl"},
                                          static_cast<std::size_t>(graphic.scroll.mode));
  mode.set_on_change([&app](std::size_t index) {
    std::optional<Target> now = target_of(app);
    if (!now.has_value()) return;
    now->graphic.scroll.mode = static_cast<cutline::core::ScrollMode>(std::min<std::size_t>(index, 2));
    commit(app, now->media_id, std::move(now->graphic));
  });
  mode_row.emplace<Spacer>();
  if (graphic.scroll.mode == cutline::core::ScrollMode::None) return;

  const auto scroll_flag = [&app](bool start) {
    return [&app, start](bool on) {
      std::optional<Target> now = target_of(app);
      if (!now.has_value()) return;
      (start ? now->graphic.scroll.start_off : now->graphic.scroll.end_off) = on;
      commit(app, now->media_id, std::move(now->graphic));
    };
  };
  body.emplace<Checkbox>("Start off screen", graphic.scroll.start_off)
      .set_on_change(scroll_flag(true));
  body.emplace<Checkbox>("End off screen", graphic.scroll.end_off).set_on_change(scroll_flag(false));
  const ValueRange seconds{.minimum = 0.0, .maximum = 60.0, .step = 0.1};
  graphic_number(app, body, "Preroll", seconds, graphic.scroll.preroll, " s", 2,
                 [](Graphic& g, double v, double) { g.scroll.preroll = std::max(0.0, v); });
  graphic_number(app, body, "Postroll", seconds, graphic.scroll.postroll, " s", 2,
                 [](Graphic& g, double v, double) { g.scroll.postroll = std::max(0.0, v); });
  graphic_number(app, body, "Ease in", seconds, graphic.scroll.ease_in, " s", 2,
                 [](Graphic& g, double v, double) { g.scroll.ease_in = std::max(0.0, v); });
  graphic_number(app, body, "Ease out", seconds, graphic.scroll.ease_out, " s", 2,
                 [](Graphic& g, double v, double) { g.scroll.ease_out = std::max(0.0, v); });
}

// ---------------------------------------------------------- the one layer --

void build_pins(App& app, Box& body, const Target& target, const GraphicLayer& layer) {
  heading(body, "Responsive Design - Position");
  std::vector<std::string> names{"Video Frame"};
  std::vector<std::string> ids{""};
  for (const GraphicLayer& other : target.graphic.layers) {
    if (other.id == layer.id) continue;
    names.push_back(cutline::editor::layer_label(other));
    ids.push_back(other.id);
  }
  const auto at = std::ranges::find(ids, layer.pin.to);
  const auto chosen = static_cast<std::size_t>(at == ids.end() ? 0 : at - ids.begin());

  // Pinning works out the distances from where both layers are now, so every
  // change pins afresh rather than editing the stored offsets.
  const auto repin = [&app, id = layer.id](std::string to, cutline::editor::PinEdges edges) {
    std::optional<Target> now = target_of(app);
    if (!now.has_value()) return;
    const cutline::core::Sequence& sequence = app.session.project().sequence();
    if (!edges.left && !edges.top && !edges.right && !edges.bottom) {
      if (GraphicLayer* l = cutline::core::find_layer(now->graphic, id); l != nullptr) {
        l->pin = cutline::core::GraphicPin{};
      }
      commit(app, now->media_id, std::move(now->graphic));
      return;
    }
    auto pinned = cutline::editor::pin_layer(now->graphic, id, to, edges, sequence.canvas_w,
                                             sequence.canvas_h, measurer());
    if (!pinned.has_value()) {
      complain(app.main.window, pinned.error());
      return;
    }
    commit(app, now->media_id, std::move(*pinned));
  };
  const cutline::editor::PinEdges edges{.left = layer.pin.left, .top = layer.pin.top,
                                        .right = layer.pin.right, .bottom = layer.pin.bottom};

  auto& to_row = body.emplace<Box>(Axis::Horizontal);
  to_row.emplace<Label>("Pin to").set_small(true);
  auto& to = to_row.emplace<Dropdown>(std::move(names), chosen);
  to.set_on_change([repin, ids, edges](std::size_t index) {
    if (index < ids.size()) repin(ids[index], edges);
  });
  to_row.emplace<Spacer>();

  auto& sides = body.emplace<Box>(Axis::Horizontal);
  const auto edge = [&sides, repin, edges, to_id = layer.pin.to](
                        const char* name, bool on, bool cutline::editor::PinEdges::*which) {
    sides.emplace<Checkbox>(name, on).set_on_change([repin, edges, to_id, which](bool now) {
      cutline::editor::PinEdges changed = edges;
      changed.*which = now;
      repin(to_id, changed);
    });
  };
  edge("Left", layer.pin.left, &cutline::editor::PinEdges::left);
  edge("Top", layer.pin.top, &cutline::editor::PinEdges::top);
  edge("Right", layer.pin.right, &cutline::editor::PinEdges::right);
  edge("Bottom", layer.pin.bottom, &cutline::editor::PinEdges::bottom);
  sides.emplace<Spacer>();
}

void build_transform(App& app, Box& body, const Target& target, const GraphicLayer& layer) {
  heading(body, "Align and Transform");
  const cutline::core::Sequence& sequence = app.session.project().sequence();
  const double width = sequence.canvas_w;
  const double height = sequence.canvas_h;

  // One layer lines up with the frame, as Premiere's does with one selected.
  const auto align = [&app, id = layer.id, width, height](cutline::editor::Align how) {
    return [&app, id, width, height, how] {
      std::optional<Target> now = target_of(app);
      if (!now.has_value()) return;
      commit(app, now->media_id,
             cutline::editor::align_layers(std::move(now->graphic), {id}, how, width, height,
                                           measurer()));
    };
  };
  auto& across = body.emplace<Box>(Axis::Horizontal);
  across.emplace<Button>("Left", align(cutline::editor::Align::Left))
      .set_tooltip("Align the layer's left edge with the frame's");
  across.emplace<Button>("Centre", align(cutline::editor::Align::CentreX))
      .set_tooltip("Centre the layer across the frame");
  across.emplace<Button>("Right", align(cutline::editor::Align::Right))
      .set_tooltip("Align the layer's right edge with the frame's");
  across.emplace<Spacer>();
  auto& down = body.emplace<Box>(Axis::Horizontal);
  down.emplace<Button>("Top", align(cutline::editor::Align::Top))
      .set_tooltip("Align the layer's top edge with the frame's");
  down.emplace<Button>("Middle", align(cutline::editor::Align::CentreY))
      .set_tooltip("Centre the layer down the frame");
  down.emplace<Button>("Bottom", align(cutline::editor::Align::Bottom))
      .set_tooltip("Align the layer's bottom edge with the frame's");
  down.emplace<Spacer>();

  // At the playhead, keyed there when the property is animated: Premiere's rule.
  const double t = target.key_time;
  const auto keyed = [t](std::string property, double scale) -> LayerChange {
    return [t, property, scale](GraphicLayer& l, double v) {
      Graphic one;
      one.layers = {l};
      one = cutline::editor::set_layer_value(std::move(one), l.id, property, t, v / scale);
      l = one.layers.front();
    };
  };
  const ValueRange pixels{.minimum = -100000.0, .maximum = 100000.0, .step = 1.0};
  layer_number(app, body, "Position X", pixels, cutline::core::layer_value(layer, "x", t) * width,
               " px", 1, layer.id, keyed("x", width));
  layer_number(app, body, "Position Y", pixels, cutline::core::layer_value(layer, "y", t) * height,
               " px", 1, layer.id, keyed("y", height));
  layer_number(app, body, "Scale", ValueRange{.minimum = 0.0, .maximum = 10000.0, .step = 1.0},
               cutline::core::layer_value(layer, "scale", t) * 100.0, "%", 1, layer.id,
               keyed("scale", 100.0));
  layer_number(app, body, "Rotation", ValueRange{.minimum = -3600.0, .maximum = 3600.0, .step = 1.0},
               cutline::core::layer_value(layer, "rotation", t), "°", 1, layer.id,
               keyed("rotation", 1.0));
  layer_number(app, body, "Opacity", ValueRange{.minimum = 0.0, .maximum = 100.0, .step = 1.0},
               cutline::core::layer_value(layer, "opacity", t) * 100.0, "%", 1, layer.id,
               keyed("opacity", 100.0));
}

void open_master_style_prompt(App& app, std::string layer_id) {
  if (app.main.host == nullptr) return;
  auto dialog = std::make_unique<cutline::ui::Dialog>("Create Master Text Style");
  auto& row = dialog->row("Name");
  auto& name = row.emplace<TextField>("");
  name.set_columns(22);
  dialog->set_accept("OK", [&app, layer_id, field = &name]() -> std::optional<std::string> {
    if (field->text().empty()) return std::string("A style needs a name.");
    std::optional<Target> now = target_of(app);
    if (!now.has_value()) return std::nullopt;
    app.session.apply(cutline::editor::create_master_style(app.session.project(), now->media_id,
                                                           layer_id, field->text()));
    refresh_browser(app);
    invalidate_preview(app);
    app.inspector_stale = true;
    return std::nullopt;
  });
  dialog->set_cancel("Cancel");
  app.main.host->open_popup(std::move(dialog), settings_anchor());
}

void build_master_styles(App& app, Box& body, const GraphicLayer& layer) {
  heading(body, "Master Styles");
  const cutline::core::Project& project = app.session.project();
  std::vector<std::string> names{"None"};
  for (const auto& style : project.graphic_styles) names.push_back(style.name);
  std::size_t chosen = 0;
  for (std::size_t i = 0; i < project.graphic_styles.size(); ++i) {
    if (project.graphic_styles[i].name == layer.master_style) chosen = i + 1;
  }
  // Said when the layer has been changed since it took the style, as
  // Premiere marks it: following, but not matching.
  if (chosen > 0 && !cutline::editor::follows_master(project, layer)) {
    names[chosen] += " (modified)";
  }
  auto& row = body.emplace<Box>(Axis::Horizontal);
  auto& pick = row.emplace<Dropdown>(std::move(names), chosen);
  pick.set_on_change([&app, id = layer.id](std::size_t index) {
    std::optional<Target> now = target_of(app);
    if (!now.has_value()) return;
    const auto& styles = app.session.project().graphic_styles;
    if (index == 0) {
      if (GraphicLayer* l = cutline::core::find_layer(now->graphic, id); l != nullptr) {
        l->master_style.clear();
      }
      commit(app, now->media_id, std::move(now->graphic));
    } else if (index - 1 < styles.size()) {
      commit(app, now->media_id,
             cutline::editor::apply_master_style(std::move(now->graphic), id, styles[index - 1]));
    }
  });
  row.emplace<Spacer>();
  auto& actions = body.emplace<Box>(Axis::Horizontal);
  actions.emplace<Button>("Create Style...", [&app, id = layer.id] {
    open_master_style_prompt(app, id);
  }).set_tooltip("Keep this layer's look as a master style, and follow it");
  // Pushing a layer's own changes back into its style, which then carries
  // them to every other layer following it.
  if (!layer.master_style.empty() && !cutline::editor::follows_master(project, layer)) {
    actions.emplace<Button>("Update Style", [&app, id = layer.id] {
      std::optional<Target> now = target_of(app);
      if (!now.has_value()) return;
      const GraphicLayer* l = cutline::core::find_layer(now->graphic, id);
      if (l == nullptr) return;
      app.session.apply(cutline::editor::set_master_style(
          app.session.project(), cutline::core::GraphicMasterStyle{.name = l->master_style,
                                                                   .style = l->style}));
      refresh_browser(app);
      invalidate_preview(app);
      app.inspector_stale = true;
    }).set_tooltip("Change the style to this, and every layer following it");
  }
  actions.emplace<Spacer>();
}

void build_text(App& app, Box& body, const GraphicLayer& layer) {
  heading(body, "Text");
  auto& words = body.emplace<TextField>(layer.text);
  words.set_multiline(true);
  words.set_min_lines(2);
  words.set_placeholder("Title text");
  words.set_on_commit([&app, id = layer.id](const std::string& text) {
    std::optional<Target> now = target_of(app);
    if (!now.has_value()) return;
    if (GraphicLayer* l = cutline::core::find_layer(now->graphic, id); l != nullptr) {
      l->text = text;
      commit(app, now->media_id, std::move(now->graphic));
    }
  });

#if CUTLINE_HAVE_TEXT
  {
    // The machine's own fonts, as Premiere lists them, with the one in use
    // kept at the top when this machine does not have it.
    static const std::vector<std::string> installed = cutline::text::font_families();
    std::vector<std::string> fonts = installed;
    auto found = std::ranges::find(fonts, layer.style.font_family);
    if (found == fonts.end()) {
      fonts.insert(fonts.begin(), layer.style.font_family);
      found = fonts.begin();
    }
    auto& row = body.emplace<Box>(Axis::Horizontal);
    row.emplace<Label>("Font").set_small(true);
    auto& font = row.emplace<Dropdown>(fonts, static_cast<std::size_t>(found - fonts.begin()));
    font.set_on_change([&app, id = layer.id, fonts](std::size_t index) {
      std::optional<Target> now = target_of(app);
      if (!now.has_value() || index >= fonts.size()) return;
      if (GraphicLayer* l = cutline::core::find_layer(now->graphic, id); l != nullptr) {
        l->style.font_family = fonts[index];
        commit(app, now->media_id, std::move(now->graphic));
      }
    });
    row.emplace<Spacer>();
  }
#endif

  layer_number(app, body, "Size", ValueRange{.minimum = 1.0, .maximum = 1000.0, .step = 1.0},
               layer.style.font_size, " px", 0, layer.id,
               [](GraphicLayer& l, double v) { l.style.font_size = std::max(1.0, v); });

  auto& align_row = body.emplace<Box>(Axis::Horizontal);
  align_row.emplace<Label>("Align").set_small(true);
  auto& align = align_row.emplace<Dropdown>(std::vector<std::string>{"Left", "Centre", "Right"},
                                            static_cast<std::size_t>(layer.style.align));
  align.set_on_change([&app, id = layer.id](std::size_t index) {
    std::optional<Target> now = target_of(app);
    if (!now.has_value()) return;
    if (GraphicLayer* l = cutline::core::find_layer(now->graphic, id); l != nullptr) {
      l->style.align = static_cast<cutline::core::TextAlign>(std::min<std::size_t>(index, 2));
      commit(app, now->media_id, std::move(now->graphic));
    }
  });
  align_row.emplace<Spacer>();

  layer_number(app, body, "Tracking", ValueRange{.minimum = -1000.0, .maximum = 1000.0, .step = 5.0},
               layer.style.tracking, "", 0, layer.id,
               [](GraphicLayer& l, double v) { l.style.tracking = v; });
  layer_number(app, body, "Leading", ValueRange{.minimum = -500.0, .maximum = 500.0, .step = 1.0},
               layer.style.leading, " px", 0, layer.id,
               [](GraphicLayer& l, double v) { l.style.leading = v; });

  auto& faces = body.emplace<Box>(Axis::Horizontal);
  layer_check(app, faces, "Bold", layer.style.bold, layer.id,
              [](GraphicLayer& l, double v) { l.style.bold = v != 0.0; });
  layer_check(app, faces, "Italic", layer.style.italic, layer.id,
              [](GraphicLayer& l, double v) { l.style.italic = v != 0.0; });
  faces.emplace<Spacer>();
  auto& marks = body.emplace<Box>(Axis::Horizontal);
  layer_check(app, marks, "Underline", layer.style.underline, layer.id,
              [](GraphicLayer& l, double v) { l.style.underline = v != 0.0; });
  layer_check(app, marks, "All caps", layer.style.all_caps, layer.id,
              [](GraphicLayer& l, double v) { l.style.all_caps = v != 0.0; });
  marks.emplace<Spacer>();

  // Premiere's paragraph text: a box the lines wrap within.
  layer_check(app, body, "Paragraph text", layer.box_width.has_value(), layer.id,
              [](GraphicLayer& l, double v) {
                if (v != 0.0) {
                  l.box_width = 600.0;
                } else {
                  l.box_width.reset();
                }
              });
  if (layer.box_width.has_value()) {
    layer_number(app, body, "Box width", ValueRange{.minimum = 10.0, .maximum = 10000.0, .step = 1.0},
                 *layer.box_width, " px", 0, layer.id,
                 [](GraphicLayer& l, double v) { l.box_width = std::max(10.0, v); });
  }
}

void build_shape(App& app, Box& body, const GraphicLayer& layer) {
  heading(body, layer.kind == GraphicKind::Ellipse ? "Ellipse" : "Rectangle");
  const ValueRange size{.minimum = 1.0, .maximum = 20000.0, .step = 1.0};
  layer_number(app, body, "Width", size, layer.width, " px", 0, layer.id,
               [](GraphicLayer& l, double v) { l.width = std::max(1.0, v); });
  layer_number(app, body, "Height", size, layer.height, " px", 0, layer.id,
               [](GraphicLayer& l, double v) { l.height = std::max(1.0, v); });
  if (layer.kind == GraphicKind::Rectangle) {
    layer_number(app, body, "Corners", ValueRange{.minimum = 0.0, .maximum = 5000.0, .step = 1.0},
                 layer.corner_radius, " px", 0, layer.id,
                 [](GraphicLayer& l, double v) { l.corner_radius = std::max(0.0, v); });
  }
}

void build_image(App& app, Box& body, const GraphicLayer& layer) {
  heading(body, "Image");
  auto& name = body.emplace<Label>(std::filesystem::path(layer.path).filename().string());
  name.set_small(true);
  body.emplace<Button>("Replace...", [&app, id = layer.id] {
    const auto chosen = choose_picture_to_open(app.main.window);
    if (!chosen.has_value()) return;
    std::optional<Target> now = target_of(app);
    if (!now.has_value()) return;
    if (GraphicLayer* l = cutline::core::find_layer(now->graphic, id); l != nullptr) {
      l->path = chosen->string();
      commit(app, now->media_id, std::move(now->graphic));
    }
  });
}

void build_appearance(App& app, Box& body, const GraphicLayer& layer) {
  heading(body, "Appearance");
  const cutline::core::GraphicStyle& style = layer.style;
  const std::string& id = layer.id;

  if (layer.kind != GraphicKind::Image) {
    layer_check(app, body, "Fill", style.fill_enabled, id,
                [](GraphicLayer& l, double v) { l.style.fill_enabled = v != 0.0; });
    if (style.fill_enabled) {
      layer_colour(app, body, "Fill colour", style.fill, id,
                   [](GraphicLayer& l, const std::string& c) { l.style.fill = c; });
    }

    for (std::size_t i = 0; i < style.strokes.size(); ++i) {
      auto& row = body.emplace<Box>(Axis::Horizontal);
      row.emplace<Label>(i == 0 ? "Stroke" : "Stroke " + std::to_string(i + 1)).set_small(true);
      auto& swatch = row.emplace<ColorSwatch>(parse_color(style.strokes[i].color));
      swatch.set_on_commit([&app, id, i](const Color& picked) {
        std::optional<Target> now = target_of(app);
        if (!now.has_value()) return;
        GraphicLayer* l = cutline::core::find_layer(now->graphic, id);
        if (l == nullptr || i >= l->style.strokes.size()) return;
        l->style.strokes[i].color = to_hex(picked);
        app.session.apply(cutline::editor::set_graphic(app.session.project(), now->media_id,
                                                       std::move(now->graphic)));
        invalidate_preview(app);
      });
      auto& width = row.emplace<NumericField>(
          ValueRange{.minimum = 0.0, .maximum = 500.0, .step = 1.0}, style.strokes[i].width);
      width.set_suffix(" px");
      width.set_decimals(0);
      width.set_on_commit([&app, id, i](double v) {
        if (const auto graphic = changed_layer(
                app, id,
                [i](GraphicLayer& l, double w) {
                  if (i < l.style.strokes.size()) l.style.strokes[i].width = std::max(0.0, w);
                },
                v);
            graphic.has_value()) {
          commit(app, target_of(app)->media_id, *graphic);
        }
      });
      auto& remove = row.emplace<IconButton>(IconButton::Icon::Cross, [&app, id, i] {
        if (const auto graphic = changed_layer(
                app, id,
                [i](GraphicLayer& l, double) {
                  if (i < l.style.strokes.size()) {
                    l.style.strokes.erase(l.style.strokes.begin() + static_cast<std::ptrdiff_t>(i));
                  }
                },
                0.0);
            graphic.has_value()) {
          commit(app, target_of(app)->media_id, *graphic);
        }
      });
      remove.set_tooltip("Take this stroke away");
      row.emplace<Spacer>();
    }
    body.emplace<Button>("Add Stroke", [&app, id] {
      if (const auto graphic = changed_layer(
              app, id, [](GraphicLayer& l, double) { l.style.strokes.emplace_back(); }, 0.0);
          graphic.has_value()) {
        commit(app, target_of(app)->media_id, *graphic);
      }
    }).set_tooltip("Another outline, outside the ones there are");
  }

  if (layer.kind == GraphicKind::Text) {
    layer_check(app, body, "Background", style.background.has_value(), id,
                [](GraphicLayer& l, double v) {
                  if (v != 0.0) {
                    l.style.background = cutline::core::GraphicBackground{};
                  } else {
                    l.style.background.reset();
                  }
                });
    if (style.background.has_value()) {
      layer_colour(app, body, "Background colour", style.background->color, id,
                   [](GraphicLayer& l, const std::string& c) {
                     if (l.style.background) l.style.background->color = c;
                   });
      layer_number(app, body, "Opacity", ValueRange{.minimum = 0.0, .maximum = 100.0, .step = 1.0},
                   style.background->opacity * 100.0, "%", 0, id, [](GraphicLayer& l, double v) {
                     if (l.style.background) l.style.background->opacity = std::clamp(v / 100.0, 0.0, 1.0);
                   });
      layer_number(app, body, "Size", ValueRange{.minimum = 0.0, .maximum = 1000.0, .step = 1.0},
                   style.background->padding, " px", 0, id, [](GraphicLayer& l, double v) {
                     if (l.style.background) l.style.background->padding = std::max(0.0, v);
                   });
      layer_number(app, body, "Corners", ValueRange{.minimum = 0.0, .maximum = 1000.0, .step = 1.0},
                   style.background->corner_radius, " px", 0, id, [](GraphicLayer& l, double v) {
                     if (l.style.background) l.style.background->corner_radius = std::max(0.0, v);
                   });
    }
  }

  layer_check(app, body, "Shadow", style.shadow.has_value(), id, [](GraphicLayer& l, double v) {
    if (v != 0.0) {
      l.style.shadow = cutline::core::GraphicShadow{};
    } else {
      l.style.shadow.reset();
    }
  });
  if (style.shadow.has_value()) {
    const cutline::core::GraphicShadow& shadow = *style.shadow;
    layer_colour(app, body, "Shadow colour", shadow.color, id,
                 [](GraphicLayer& l, const std::string& c) {
                   if (l.style.shadow) l.style.shadow->color = c;
                 });
    const auto shadow_number = [&](std::string label, ValueRange range, double value,
                                   std::string suffix, double scale,
                                   double cutline::core::GraphicShadow::*field) {
      layer_number(app, body, std::move(label), range, value * scale, std::move(suffix), 0, id,
                   [field, scale](GraphicLayer& l, double v) {
                     if (l.style.shadow) (*l.style.shadow).*field = v / scale;
                   });
    };
    shadow_number("Opacity", ValueRange{.minimum = 0.0, .maximum = 100.0, .step = 1.0},
                  shadow.opacity, "%", 100.0, &cutline::core::GraphicShadow::opacity);
    shadow_number("Angle", ValueRange{.minimum = -360.0, .maximum = 360.0, .step = 1.0},
                  shadow.angle, "°", 1.0, &cutline::core::GraphicShadow::angle);
    shadow_number("Distance", ValueRange{.minimum = 0.0, .maximum = 1000.0, .step = 1.0},
                  shadow.distance, " px", 1.0, &cutline::core::GraphicShadow::distance);
    shadow_number("Size", ValueRange{.minimum = 0.0, .maximum = 500.0, .step = 1.0}, shadow.size,
                  " px", 1.0, &cutline::core::GraphicShadow::size);
    shadow_number("Blur", ValueRange{.minimum = 0.0, .maximum = 500.0, .step = 1.0}, shadow.blur,
                  " px", 1.0, &cutline::core::GraphicShadow::blur);
  }
}

void build_animation(App& app, Box& body, const GraphicLayer& layer) {
  heading(body, "Animate");
  std::vector<std::string> names;
  for (const auto& [kind, name] : kReveals) names.emplace_back(name);
  const auto index_of = [](cutline::core::RevealKind kind) {
    const auto found = std::ranges::find(kReveals, kind, &std::pair<cutline::core::RevealKind,
                                                                    const char*>::first);
    return static_cast<std::size_t>(found == kReveals.end() ? 0 : found - kReveals.begin());
  };
  const auto reveal_row = [&](const char* label, const cutline::core::GraphicReveal& reveal,
                              cutline::core::GraphicReveal GraphicLayer::*which) {
    auto& row = body.emplace<Box>(Axis::Horizontal);
    row.emplace<Label>(label).set_small(true);
    auto& kind = row.emplace<Dropdown>(names, index_of(reveal.kind));
    kind.set_on_change([&app, id = layer.id, which](std::size_t index) {
      if (index >= kReveals.size()) return;
      if (const auto graphic = changed_layer(
              app, id,
              [which, kind = kReveals[index].first](GraphicLayer& l, double) {
                (l.*which).kind = kind;
              },
              0.0);
          graphic.has_value()) {
        commit(app, target_of(app)->media_id, *graphic);
      }
    });
    row.emplace<Spacer>();
    if (reveal.kind != cutline::core::RevealKind::None) {
      layer_number(app, body, "Duration", ValueRange{.minimum = 0.05, .maximum = 60.0, .step = 0.05},
                   reveal.duration, " s", 2, layer.id, [which](GraphicLayer& l, double v) {
                     (l.*which).duration = std::max(0.05, v);
                   });
    }
  };
  reveal_row("In", layer.reveal_in, &GraphicLayer::reveal_in);
  reveal_row("Out", layer.reveal_out, &GraphicLayer::reveal_out);
}

// ------------------------------------------------------ on the picture --

/// The graphic clip a new layer drawn on the picture goes into: the selected
/// one, when it is a graphic on screen; otherwise a new graphic at the
/// playhead, made in `project`, as Premiere's tools make one.
[[nodiscard]] std::pair<std::string, std::string> graphic_to_draw_in(
    const App& app, cutline::core::Project& project) {
  const auto selection = app.session.selection();
  const double t = app.session.playhead();
  if (!selection.empty()) {
    const cutline::core::Clip* clip = cutline::core::find_clip(project, selection.front());
    if (clip != nullptr && t >= clip->start && t < cutline::core::clip_end(*clip) &&
        cutline::editor::graphic_of(project, clip->media_id) != nullptr) {
      return {clip->id, clip->media_id};
    }
  }
  std::string clip_id;
  project = cutline::editor::add_graphic_at(std::move(project), Graphic{}, t, {}, &clip_id);
  const cutline::core::Clip* clip = cutline::core::find_clip(project, clip_id);
  return {clip_id, clip != nullptr ? clip->media_id : std::string{}};
}

/// After a layer is made or edited on the picture: the clip selected, the
/// layer chosen, and everything showing it told.
void settle_on_layer(App& app, const std::string& clip_id, const std::string& layer_id) {
  app.session.select_one(clip_id);
  app.graphic_layer = layer_id;
  refresh_all(app);
  refresh_handles(app);
}

/// The project with one text layer's words replaced.
[[nodiscard]] cutline::core::Project with_text(cutline::core::Project project,
                                               const std::string& media_id,
                                               const std::string& layer_id, std::string text) {
  const Graphic* graphic = cutline::editor::graphic_of(project, media_id);
  if (graphic == nullptr) return project;
  Graphic changed = *graphic;
  if (GraphicLayer* layer = cutline::core::find_layer(changed, layer_id); layer != nullptr) {
    layer->text = std::move(text);
  }
  return cutline::editor::set_graphic(std::move(project), media_id, std::move(changed));
}

/// Types into a text layer, in a field over it on the picture, the picture
/// following every keystroke. `base` is the project the typing is done in,
/// which for a layer the Type tool has just made is one the session has not
/// seen yet: the making and the typing are one undo step, and typing nothing
/// leaves nothing behind.
void type_into(App& app, cutline::core::Project base, const std::string& clip_id,
               const std::string& layer_id, bool made) {
  if (app.main.host == nullptr || app.monitor == nullptr) return;
  const cutline::core::Clip* clip = cutline::core::find_clip(base, clip_id);
  if (clip == nullptr) return;
  const std::string media_id = clip->media_id;
  const Graphic* graphic = cutline::editor::graphic_of(base, media_id);
  const GraphicLayer* layer =
      graphic == nullptr ? nullptr : cutline::core::find_layer(*graphic, layer_id);
  if (layer == nullptr) return;

  app.live_gesture = false;
  app.typing_on_picture = true;
  app.live_project = base;
  app.graphic_layer = layer_id;
  invalidate_preview(app);

  auto dialog = std::make_unique<cutline::ui::Dialog>("Text");
  auto& field = dialog->body().emplace<TextField>(layer->text);
  field.set_multiline(true);
  field.set_min_lines(2);
  field.set_columns(24);
  field.set_placeholder("Type here");
  field.set_on_change([&app, base, media_id, layer_id](const std::string& text) {
    app.live_project = with_text(base, media_id, layer_id, text);
    invalidate_preview(app);
    refresh_handles(app);
  });
  dialog->set_accept("OK", [&app, base, clip_id, media_id, layer_id, made,
                            words = &field]() -> std::optional<std::string> {
    app.typing_on_picture = false;
    app.live_project.reset();
    const std::string text = words->text();
    if (text.empty()) {
      // A text layer with nothing in it is no layer, as in Premiere. One made
      // by this gesture simply never happens.
      if (!made) {
        const Graphic* now = cutline::editor::graphic_of(app.session.project(), media_id);
        if (now != nullptr) {
          app.session.apply(cutline::editor::set_graphic(
              app.session.project(), media_id, cutline::editor::remove_layer(*now, layer_id)));
        }
      }
      app.graphic_layer.clear();
      refresh_all(app);
      return std::nullopt;
    }
    app.session.apply(with_text(base, media_id, layer_id, text));
    settle_on_layer(app, clip_id, layer_id);
    return std::nullopt;
  });
  dialog->set_cancel("Cancel", [&app] {
    app.typing_on_picture = false;
    app.live_project.reset();
    invalidate_preview(app);
    refresh_handles(app);
  });

  // Under the picture, level with the layer: near enough to read as the
  // layer's, and clear of the frame so what is typed can be seen arriving in
  // it. Over the picture, where the first attempt put it, it covered the very
  // words it was for.
  Rect anchor = settings_anchor();
  const Rect picture = app.monitor->picture();
  if (const auto box = cutline::editor::graphic_layer_box(base, clip_id, layer_id,
                                                          app.session.playhead(), layer_measurer());
      box.has_value() && !picture.empty()) {
    anchor = Rect{picture.x + (box->x - box->width / 2.0) * picture.width,
                  picture.bottom() + 4.0, 0.0, 0.0};
  }
  app.main.host->open_popup(std::move(dialog), anchor);
  app.main.host->set_focus(&field);
  refresh_handles(app);
}

}  // namespace

bool press_on_picture(App& app, double x, double y, int clicks) {
  const double t = app.session.playhead();
  const std::optional<cutline::editor::LayerHit> hit = cutline::editor::graphic_layer_at(
      app.session.project(), t, x, y, layer_measurer());
  const auto is_text = [&app](const cutline::editor::LayerHit& at) {
    const Graphic* graphic = cutline::editor::clip_graphic(app.session.project(), at.clip_id);
    const GraphicLayer* layer =
        graphic == nullptr ? nullptr : cutline::core::find_layer(*graphic, at.layer_id);
    return layer != nullptr && layer->kind == GraphicKind::Text;
  };

  if (app.tool == cutline::ui::Tool::Type) {
    // On existing text, the Type tool edits it; anywhere else it starts new.
    if (hit.has_value() && is_text(*hit)) {
      type_into(app, app.session.project(), hit->clip_id, hit->layer_id, false);
      return true;
    }
    cutline::core::Project project = app.session.project();
    const auto [clip_id, media_id] = graphic_to_draw_in(app, project);
    if (clip_id.empty()) return true;
    const auto at = cutline::editor::frame_to_graphic(project, clip_id, t, x, y);
    const Graphic* graphic = cutline::editor::graphic_of(project, media_id);
    if (!at.has_value() || graphic == nullptr) return true;
    // Point text, starting where the click was, as Premiere's Type tool puts
    // it: left-aligned, with the click on its left edge.
    GraphicLayer layer = cutline::editor::text_layer("");
    layer.style.align = cutline::core::TextAlign::Left;
    layer.anchor_x = 0.0;
    layer.x = at->first / project.sequence().canvas_w;
    layer.y = at->second / project.sequence().canvas_h;
    std::string layer_id;
    project = cutline::editor::set_graphic(
        std::move(project), media_id, cutline::editor::add_layer(*graphic, std::move(layer), &layer_id));
    type_into(app, std::move(project), clip_id, layer_id, true);
    return true;
  }

  if (app.tool != cutline::ui::Tool::Selection) return false;
  if (!hit.has_value()) {
    // A click off every layer goes back to the graphic as a whole.
    if (app.graphic_layer.empty()) return false;
    app.graphic_layer.clear();
    app.graphics_stale = true;
    refresh_handles(app);
    return true;
  }
  const auto selection = app.session.selection();
  const bool chosen = !selection.empty() && selection.front() == hit->clip_id &&
                      app.graphic_layer == hit->layer_id;
  if (chosen && clicks >= 2 && is_text(*hit)) {
    type_into(app, app.session.project(), hit->clip_id, hit->layer_id, false);
    return true;
  }
  if (!chosen) {
    app.session.select_one(hit->clip_id);
    app.graphic_layer = hit->layer_id;
    refresh_timeline(app);
    app.inspector_stale = true;
    refresh_handles(app);
  }
  // Not taken: the same press goes on to move the layer just chosen.
  return false;
}

void box_drawn_on_picture(App& app, const cutline::ui::MonitorBox& box, bool done) {
  if (!done) return;
  const cutline::core::Sequence& sequence = app.session.project().sequence();
  // A click rather than a drag draws nothing.
  if (box.width * sequence.canvas_w < 4.0 || box.height * sequence.canvas_h < 4.0) return;

  cutline::core::Project project = app.session.project();
  const auto [clip_id, media_id] = graphic_to_draw_in(app, project);
  const double t = app.session.playhead();
  const auto from = cutline::editor::frame_to_graphic(project, clip_id, t, box.x - box.width / 2.0,
                                                      box.y - box.height / 2.0);
  const auto to = cutline::editor::frame_to_graphic(project, clip_id, t, box.x + box.width / 2.0,
                                                    box.y + box.height / 2.0);
  const Graphic* graphic = cutline::editor::graphic_of(project, media_id);
  if (!from.has_value() || !to.has_value() || graphic == nullptr) return;

  GraphicLayer layer = cutline::editor::shape_layer(
      app.tool == cutline::ui::Tool::Ellipse ? GraphicKind::Ellipse : GraphicKind::Rectangle);
  layer.width = std::abs(to->first - from->first);
  layer.height = std::abs(to->second - from->second);
  layer.x = (from->first + to->first) / 2.0 / sequence.canvas_w;
  layer.y = (from->second + to->second) / 2.0 / sequence.canvas_h;
  std::string layer_id;
  project = cutline::editor::set_graphic(
      std::move(project), media_id, cutline::editor::add_layer(*graphic, std::move(layer), &layer_id));
  app.session.apply(std::move(project));
  settle_on_layer(app, clip_id, layer_id);
}

cutline::core::LayerMeasurer layer_measurer() {
#if CUTLINE_HAVE_TEXT
  return [](const GraphicLayer& layer) { return cutline::text::measure(layer); };
#else
  return [](const GraphicLayer& layer) { return cutline::core::measure_without_fonts(layer); };
#endif
}

void refresh_graphics_panel(App& app) {
  if (app.graphics_body == nullptr) return;
  app.graphics_body->clear_children();
  Box& body = *app.graphics_body;

  const std::optional<Target> target = target_of(app);
  if (!target.has_value()) {
    app.graphic_layer.clear();
    body.emplace<Label>("Select a graphic in the timeline, or make one with File > New > Title.")
        .set_small(true);
    body.emplace<Spacer>();
  } else {
    // A layer chosen in another graphic means nothing in this one.
    const GraphicLayer* chosen = cutline::core::find_layer(target->graphic, app.graphic_layer);
    if (chosen == nullptr) app.graphic_layer.clear();

    build_layer_list(app, body, *target);
    if (chosen == nullptr) {
      build_graphic_timing(app, body, *target);
    } else {
      build_pins(app, body, *target, *chosen);
      build_transform(app, body, *target, *chosen);
      if (chosen->kind == GraphicKind::Text) {
        build_master_styles(app, body, *chosen);
        build_text(app, body, *chosen);
      } else if (chosen->kind == GraphicKind::Image) {
        build_image(app, body, *chosen);
      } else {
        build_shape(app, body, *chosen);
      }
      build_appearance(app, body, *chosen);
      build_animation(app, body, *chosen);
    }
  }

  for (Shell* shell : app.shells()) {
    if (shell->host != nullptr) shell->host->request_layout();
  }
  mark_dirty(app);
}

[[nodiscard]] std::unique_ptr<Widget> make_graphics_panel(App* app) {
  auto panel = std::make_unique<Panel>();
  auto& scroll = panel->emplace<ScrollView>(Axis::Vertical);
  auto& body = static_cast<Box&>(scroll.set_content(std::make_unique<Box>(Axis::Vertical)));
  body.set_spacing(6.0);
  if (app != nullptr) {
    app->graphics_body = &body;
    refresh_graphics_panel(*app);
  } else {
    body.emplace<Label>("Select a graphic.").set_small(true);
  }
  return panel;
}

}  // namespace cutline::tool
