/// The Lumetri Color panel: Premiere's grading controls for the selected clip,
/// section by section, editing its Lumetri Color effect.
///
/// The same effect Effect Controls lists, and the same numbers, with the
/// controls a grade is made with: wheels, curves and the secondary's range
/// sliders. A clip with no Lumetri Color shows the controls at rest, and the
/// first change adds one, as Premiere's panel does.

#include "app.hpp"

#include "cutline/editor/lumetri.hpp"
#include "cutline/core/nesting.hpp"
#include "cutline/render/effect_catalog.hpp"
#include "cutline/render/lumetri.hpp"
#include "cutline/render/plan.hpp"
#include "cutline/ui/color_picker.hpp"
#include "cutline/ui/grading.hpp"

namespace cutline::tool {
namespace {

using cutline::core::ClipEffect;
using cutline::core::CurvePoint;
using cutline::ui::ColorWheel;
using cutline::ui::CurveGraph;
using cutline::ui::CurvePointView;
using cutline::ui::NumericField;
using cutline::ui::RangeSlider;
using cutline::ui::ValueRange;
using Values = std::vector<std::pair<std::string, double>>;

/// The clip being graded and its grade as it stands at the playhead.
struct Target {
  std::string clip_id;
  std::string name;
  /// The clip's Lumetri Color, or one at rest when it has none yet.
  ClipEffect effect;
  double local_t = 0.0;
};

[[nodiscard]] const cutline::render::EffectParamSpec* spec_of(std::string_view key) {
  const cutline::render::EffectSpec* spec = cutline::render::find_effect_spec("lumetri");
  if (spec == nullptr) return nullptr;
  const auto found = std::ranges::find(spec->params, key, &cutline::render::EffectParamSpec::key);
  return found == spec->params.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<Target> target_of(App& app) {
  const auto selection = app.session.selection();
  if (selection.empty()) return std::nullopt;
  const std::string clip_id{selection.front()};
  const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), clip_id);
  if (clip == nullptr || clip->kind != cutline::core::TrackKind::Video) return std::nullopt;

  Target target;
  target.clip_id = clip_id;
  target.local_t = std::clamp(app.session.playhead() - clip->start, 0.0,
                              cutline::core::clip_duration(*clip));
  const auto media = std::ranges::find(app.session.project().media, clip->media_id,
                                       &cutline::core::Media::id);
  target.name = media != app.session.project().media.end() ? media->name : clip_id;
  if (const auto index = cutline::editor::lumetri_on(app.session.project(), clip_id);
      index.has_value()) {
    // As it is at the playhead, so an animated value shows where it has got to.
    target.effect = cutline::core::resolved_effects(*clip, target.local_t)[*index];
  } else {
    target.effect.type = "lumetri";
  }
  return target;
}

[[nodiscard]] double value_of(const Target& target, std::string_view key) {
  const auto found = target.effect.params.find(std::string(key));
  if (found != target.effect.params.end()) return found->second;
  const cutline::render::EffectParamSpec* spec = spec_of(key);
  return spec == nullptr ? 0.0 : spec->fallback;
}

/// Shows a change on the picture while a control is dragged, without writing it.
void preview(App& app, const std::string& clip_id, const Values& values) {
  app.live_gesture = true;
  app.live_project =
      cutline::editor::set_lumetri_params(app.session.project(), clip_id, values,
                                          local_playhead_of(app, clip_id));
  invalidate_preview(app);
}

void finish(App& app) {
  app.live_gesture = false;
  app.live_project.reset();
  refresh_timeline(app);
  invalidate_preview(app);
  invalidate_scopes(app);
  app.inspector_stale = true;
}

void commit(App& app, const std::string& clip_id, const Values& values) {
  app.session.apply(cutline::editor::set_lumetri_params(app.session.project(), clip_id, values,
                                                        local_playhead_of(app, clip_id)));
  finish(app);
}

[[nodiscard]] std::vector<CurvePointView> view_of(const std::vector<CurvePoint>& points) {
  std::vector<CurvePointView> out;
  for (const CurvePoint& point : points) out.push_back({.x = point.x, .y = point.y});
  return out;
}

[[nodiscard]] std::vector<CurvePoint> points_of(const std::vector<CurvePointView>& view) {
  std::vector<CurvePoint> out;
  for (const CurvePointView& point : view) out.push_back({.x = point.x, .y = point.y});
  return out;
}

void heading(Box& body, std::string text) {
  body.emplace<Label>(std::move(text)).set_bold(true);
}

/// A slider and its number, Premiere's row: the name, the slider, the value.
void slider_row(App& app, Box& body, const Target& target, std::string_view key) {
  const cutline::render::EffectParamSpec* spec = spec_of(key);
  if (spec == nullptr) return;
  const ValueRange range{.minimum = spec->minimum, .maximum = spec->maximum};
  const double value = value_of(target, key);

  auto& row = body.emplace<Box>(Axis::Horizontal);
  auto& label = row.emplace<Label>(std::string(spec->name));
  label.set_small(true);
  label.set_min_width(96.0);
  auto& slider = row.emplace<Slider>(range, value);
  slider.set_default_value(spec->fallback);
  auto& number = row.emplace<NumericField>(range, value);
  number.set_decimals(std::abs(spec->maximum - spec->minimum) <= 10.0 ? 2 : 1);
  number.set_suffix(std::string(spec->suffix));
  number.set_default_value(spec->fallback);

  const std::string clip_id = target.clip_id;
  const std::string name(key);
  slider.set_on_change([&app, clip_id, name, field = &number](double v) {
    field->set_value(v);
    preview(app, clip_id, {{name, v}});
  });
  slider.set_on_commit([&app, clip_id, name](double v) { commit(app, clip_id, {{name, v}}); });
  number.set_on_commit([&app, clip_id, name](double v) { commit(app, clip_id, {{name, v}}); });
}

/// A wheel's colours: grey, with as much of each hue as the grade would add.
/// From the grade's own arithmetic, so the wheel and the picture agree.
[[nodiscard]] ColorWheel::Shade wheel_shade() {
  return [](double x, double y) {
    const std::array<float, 3> offset = cutline::render::wheel_offset(x, y);
    return Color{0.42f + offset[0] * 0.9f, 0.42f + offset[1] * 0.9f, 0.42f + offset[2] * 0.9f,
                 1.0f};
  };
}

/// A wheel and, under it, its name and its luma slider.
void wheel_column(App& app, Box& row, const Target& target, std::string prefix,
                  std::string title, bool with_luma, double diameter) {
  auto& column = row.emplace<Box>(Axis::Vertical);
  column.emplace<Label>(std::move(title)).set_small(true);
  auto& wheel = column.emplace<ColorWheel>(diameter);
  wheel.set_shade(wheel_shade());
  wheel.set_position(value_of(target, prefix + "_x") / 100.0,
                     value_of(target, prefix + "_y") / 100.0);
  const std::string clip_id = target.clip_id;
  const auto values = [prefix](double x, double y) {
    return Values{{prefix + "_x", std::round(x * 1000.0) / 10.0},
                  {prefix + "_y", std::round(y * 1000.0) / 10.0}};
  };
  wheel.set_on_change([&app, clip_id, values](double x, double y) {
    preview(app, clip_id, values(x, y));
  });
  wheel.set_on_commit([&app, clip_id, values](double x, double y) {
    commit(app, clip_id, values(x, y));
  });
  if (with_luma) {
    const std::string key = prefix + "_luma";
    const ValueRange range{.minimum = -100.0, .maximum = 100.0};
    auto& luma = column.emplace<Slider>(range, value_of(target, key));
    luma.set_default_value(0.0);
    luma.set_on_change([&app, clip_id, key](double v) { preview(app, clip_id, {{key, v}}); });
    luma.set_on_commit([&app, clip_id, key](double v) { commit(app, clip_id, {{key, v}}); });
  }
}

/// One curve, edited in place: the samples it draws are the grade's own.
void curve(App& app, Box& body, const Target& target, std::string_view key,
           CurveGraph::Backdrop backdrop, Color line, double height) {
  const auto index = static_cast<std::size_t>(
      std::ranges::find(cutline::render::kLumetriCurveKeys, key) -
      cutline::render::kLumetriCurveKeys.begin());
  const bool diagonal = index < 4 || key == "sat_sat";
  const bool periodic = key.starts_with("hue_");
  const auto rest = diagonal ? cutline::render::CurveRest::Diagonal
                             : cutline::render::CurveRest::Middle;

  auto& graph = body.emplace<CurveGraph>(backdrop, line, diagonal, periodic, height);
  const auto found = target.effect.curves.find(std::string(key));
  const std::vector<CurvePoint> points =
      found == target.effect.curves.end() ? std::vector<CurvePoint>{} : found->second;
  graph.set_points(view_of(points));
  graph.set_samples(cutline::render::curve_table(points, rest, periodic));

  const std::string clip_id = target.clip_id;
  const std::string name(key);
  graph.set_on_change(
      [&app, clip_id, name, rest, periodic, view = &graph](const std::vector<CurvePointView>& now) {
        const std::vector<CurvePoint> moved = points_of(now);
        view->set_samples(cutline::render::curve_table(moved, rest, periodic));
        app.live_gesture = true;
        app.live_project =
            cutline::editor::set_lumetri_curve(app.session.project(), clip_id, name, moved);
        invalidate_preview(app);
      });
  graph.set_on_commit([&app, clip_id, name](const std::vector<CurvePointView>& now) {
    app.session.apply(cutline::editor::set_lumetri_curve(app.session.project(), clip_id, name,
                                                         points_of(now)));
    finish(app);
  });
}

/// A LUT, chosen from a list with Browse at the end: Input LUT and Look.
void lut_row(App& app, Box& body, const Target& target, std::string label, std::string key,
             const std::vector<std::filesystem::path>& offered) {
  const auto file = target.effect.files.find(key);
  const std::filesystem::path current =
      file == target.effect.files.end() ? std::filesystem::path{} : file->second;

  std::vector<std::filesystem::path> paths{std::filesystem::path{}};
  paths.insert(paths.end(), offered.begin(), offered.end());
  if (!current.empty() && std::ranges::find(paths, current) == paths.end()) {
    paths.push_back(current);
  }
  std::vector<std::string> names;
  std::size_t chosen = 0;
  for (std::size_t i = 0; i < paths.size(); ++i) {
    names.push_back(paths[i].empty() ? "None" : paths[i].stem().string());
    if (paths[i] == current) chosen = i;
  }
  names.emplace_back("Browse...");

  auto& row = body.emplace<Box>(Axis::Horizontal);
  auto& name = row.emplace<Label>(std::move(label));
  name.set_small(true);
  name.set_min_width(96.0);
  auto& menu = row.emplace<Dropdown>(names, chosen);
  const std::string clip_id = target.clip_id;
  menu.set_on_change([&app, clip_id, key, paths](std::size_t index) {
    std::string path;
    if (index < paths.size()) {
      path = paths[index].string();
    } else {
      const auto browsed = choose_lut_to_open(app.main.window);
      if (!browsed.has_value()) {
        app.lumetri_stale = true;
        return;
      }
      if (auto lut = cutline::render::load_lut(*browsed); !lut.has_value()) {
        complain(app.main.window, lut.error());
        app.lumetri_stale = true;
        return;
      }
      path = browsed->string();
    }
    app.session.apply(cutline::editor::set_lumetri_file(app.session.project(), clip_id, key, path));
    finish(app);
    app.lumetri_stale = true;
  });
  row.emplace<Spacer>();
}

/// A section's heading: the twirl, and its switch.
[[nodiscard]] bool section(App& app, Box& body, const Target& target, std::string title,
                           std::string key) {
  const bool open = app.lumetri_open.contains(title);
  auto& head = body.emplace<Box>(Axis::Horizontal);
  head.emplace<IconButton>(IconButton::Icon::Disclosure, [&app, title] {
        if (!app.lumetri_open.erase(title)) app.lumetri_open.insert(title);
        app.lumetri_stale = true;
      })
      .set_selected(open);
  auto& on = head.emplace<Checkbox>(title, value_of(target, key) >= 0.5);
  const std::string clip_id = target.clip_id;
  on.set_on_change([&app, clip_id, key](bool checked) {
    commit(app, clip_id, {{key, checked ? 1.0 : 0.0}});
  });
  head.emplace<Spacer>();
  return open;
}

void build_basic(App& app, Box& body, const Target& target) {
  lut_row(app, body, target, "Input LUT", "input_lut", {});

  heading(body, "White Balance");
  auto& picker = body.emplace<Box>(Axis::Horizontal);
  picker.emplace<Label>("WB Selector").set_small(true);
  auto& pick = picker.emplace<Button>("Pick from Picture", [&app] {
    app.lumetri_picking = app.lumetri_picking == LumetriPick::WhiteBalance
                              ? LumetriPick::None
                              : LumetriPick::WhiteBalance;
    app.lumetri_stale = true;
  });
  pick.set_selected(app.lumetri_picking == LumetriPick::WhiteBalance);
  pick.set_tooltip("Click something white or grey in the Program Monitor to make it neutral");
  picker.emplace<Spacer>();
  slider_row(app, body, target, "temperature");
  slider_row(app, body, target, "tint");

  heading(body, "Tone");
  for (const char* key : {"exposure", "contrast", "highlights", "shadows", "whites", "blacks"}) {
    slider_row(app, body, target, key);
  }
  // Only where there is anything above white to reach, as in Premiere.
  if (app.session.project().sequence().colour != cutline::core::WorkingColour::Rec709) {
    slider_row(app, body, target, "hdr_specular");
  }
  auto& buttons = body.emplace<Box>(Axis::Horizontal);
  buttons.emplace<Spacer>();
  const std::string clip_id = target.clip_id;
  buttons.emplace<Button>("Reset", [&app, clip_id] {
    commit(app, clip_id,
           {{"exposure", 0.0}, {"contrast", 0.0}, {"highlights", 0.0}, {"shadows", 0.0},
            {"whites", 0.0}, {"blacks", 0.0}, {"saturation", 100.0}});
  }).set_tooltip("Put the tone and saturation back as they were");
  buttons.emplace<Button>("Auto", [&app, clip_id] { auto_tone_clip(app, clip_id); })
      .set_tooltip("Set the tone from what is in the picture");
  slider_row(app, body, target, "saturation");
}

void build_creative(App& app, Box& body, const Target& target) {
  const std::filesystem::path folder = cutline::editor::default_looks_folder();
  cutline::editor::write_built_in_looks(folder);
  lut_row(app, body, target, "Look", "look", cutline::editor::looks_in(folder));
  slider_row(app, body, target, "intensity");
  heading(body, "Adjustments");
  for (const char* key : {"faded_film", "sharpen", "vibrance", "creative_saturation"}) {
    slider_row(app, body, target, key);
  }
  auto& wheels = body.emplace<Box>(Axis::Horizontal);
  wheels.emplace<Spacer>();
  wheel_column(app, wheels, target, "shadow_tint", "Shadow Tint", false, 84.0);
  wheels.emplace<Spacer>();
  wheel_column(app, wheels, target, "highlight_tint", "Highlight Tint", false, 84.0);
  wheels.emplace<Spacer>();
  slider_row(app, body, target, "tint_balance");
}

void build_curves(App& app, Box& body, const Target& target) {
  heading(body, "RGB Curves");
  constexpr std::array<std::pair<const char*, const char*>, 4> kChannels{
      {{"rgb_master", "Master"}, {"rgb_red", "Red"}, {"rgb_green", "Green"}, {"rgb_blue", "Blue"}}};
  constexpr std::array<Color, 4> kLines{Color{0.92f, 0.92f, 0.92f, 1.0f},
                                        Color{0.95f, 0.35f, 0.32f, 1.0f},
                                        Color{0.35f, 0.85f, 0.4f, 1.0f},
                                        Color{0.4f, 0.55f, 1.0f, 1.0f}};
  auto& channels = body.emplace<Box>(Axis::Horizontal);
  for (std::size_t i = 0; i < kChannels.size(); ++i) {
    auto& button = channels.emplace<Button>(kChannels[i].second, [&app, i] {
      app.lumetri_channel = i;
      app.lumetri_stale = true;
    });
    button.set_selected(app.lumetri_channel == i);
  }
  channels.emplace<Spacer>();
  const std::size_t channel = std::min<std::size_t>(app.lumetri_channel, 3);
  curve(app, body, target, kChannels[channel].first, CurveGraph::Backdrop::Tone, kLines[channel],
        180.0);

  heading(body, "Hue Saturation Curves");
  const Color line{0.92f, 0.92f, 0.92f, 1.0f};
  const std::array<std::tuple<const char*, const char*, CurveGraph::Backdrop>, 5> kHue{{
      {"hue_sat", "Hue vs Sat", CurveGraph::Backdrop::Hue},
      {"hue_hue", "Hue vs Hue", CurveGraph::Backdrop::Hue},
      {"hue_luma", "Hue vs Luma", CurveGraph::Backdrop::Hue},
      {"luma_sat", "Luma vs Sat", CurveGraph::Backdrop::Tone},
      {"sat_sat", "Sat vs Sat", CurveGraph::Backdrop::Saturation},
  }};
  for (const auto& [key, name, backdrop] : kHue) {
    body.emplace<Label>(name).set_small(true);
    curve(app, body, target, key, backdrop, line, 110.0);
  }
}

void capture_reference(App& app);
void apply_match(App& app, const std::string& clip_id);

void build_wheels(App& app, Box& body, const Target& target) {
  // Premiere's Color Match first, as its section is named for both: a frame
  // elsewhere in the sequence to match this one to.
  heading(body, "Color Match");
  auto& match = body.emplace<Box>(Axis::Horizontal);
  auto& compare = match.emplace<Button>("Comparison View", [&app] { toggle_comparison(app); });
  compare.set_selected(app.lumetri_comparing);
  compare.set_tooltip("Show a reference frame beside the picture in the Program Monitor");
  const std::string clip_id = target.clip_id;
  auto& apply = match.emplace<Button>("Apply Match", [&app, clip_id] { apply_match(app, clip_id); });
  apply.set_enabled(app.lumetri_comparing);
  apply.set_tooltip("Set the wheels and saturation to match the reference");
  match.emplace<Spacer>();
  if (app.lumetri_comparing) {
    const double length =
        std::max(0.0, cutline::core::sequence_duration(app.session.project().sequence()));
    auto& row = body.emplace<Box>(Axis::Horizontal);
    auto& name = row.emplace<Label>("Reference");
    name.set_small(true);
    name.set_min_width(96.0);
    auto& where = row.emplace<Slider>(ValueRange{.minimum = 0.0, .maximum = length},
                                      std::clamp(app.lumetri_reference_time, 0.0, length));
    where.set_on_commit([&app](double t) {
      app.lumetri_reference_time = t;
      capture_reference(app);
      show_comparison(app);
    });
  }

  auto& wheels = body.emplace<Box>(Axis::Horizontal);
  wheel_column(app, wheels, target, "shadows", "Shadows", true, 92.0);
  wheels.emplace<Spacer>();
  wheel_column(app, wheels, target, "midtones", "Midtones", true, 92.0);
  wheels.emplace<Spacer>();
  wheel_column(app, wheels, target, "highlights", "Highlights", true, 92.0);
}

/// A key range: the slider, and the three numbers it sets.
void key_range(App& app, Box& body, const Target& target, std::string name, std::string label,
               RangeSlider::Shade shade, bool hue) {
  const std::string key = "key_" + name;
  const double scale = hue ? 360.0 : 100.0;
  auto& row = body.emplace<Box>(Axis::Horizontal);
  row.emplace<Label>(std::move(label)).set_small(true);
  auto& range = row.emplace<RangeSlider>(std::move(shade), hue);
  range.set_range(value_of(target, key + "_lo") / scale, value_of(target, key + "_hi") / scale,
                  value_of(target, key + "_soft") / scale);
  const std::string clip_id = target.clip_id;
  const double most_soft = hue ? 90.0 : 50.0;
  const auto values = [key, scale, most_soft](double low, double high, double soft) {
    return Values{{key + "_lo", std::round(low * scale * 10.0) / 10.0},
                  {key + "_hi", std::round(high * scale * 10.0) / 10.0},
                  {key + "_soft", std::min(most_soft, std::round(soft * scale * 10.0) / 10.0)}};
  };
  range.set_on_change([&app, clip_id, values](double low, double high, double soft) {
    preview(app, clip_id, values(low, high, soft));
  });
  range.set_on_commit([&app, clip_id, values](double low, double high, double soft) {
    commit(app, clip_id, values(low, high, soft));
  });
}

void build_secondary(App& app, Box& body, const Target& target) {
  heading(body, "Key");
  auto& picker = body.emplace<Box>(Axis::Horizontal);
  auto& pick = picker.emplace<Button>("Set Colour", [&app] {
    app.lumetri_picking =
        app.lumetri_picking == LumetriPick::Key ? LumetriPick::None : LumetriPick::Key;
    app.lumetri_stale = true;
  });
  pick.set_selected(app.lumetri_picking == LumetriPick::Key);
  pick.set_tooltip("Click the colour to select in the Program Monitor");
  // Premiere's preset swatches: a hue each, at full saturation and luma range.
  const std::string clip_id = target.clip_id;
  constexpr std::array<std::pair<const char*, double>, 6> kPresets{
      {{"R", 0.0}, {"Y", 60.0}, {"G", 120.0}, {"C", 180.0}, {"B", 240.0}, {"M", 300.0}}};
  for (const auto& [letter, hue] : kPresets) {
    picker.emplace<Button>(letter, [&app, clip_id, hue] {
      commit(app, clip_id,
             {{"key_hue_lo", std::fmod(hue + 335.0, 360.0)},
              {"key_hue_hi", std::fmod(hue + 25.0, 360.0)},
              {"key_hue_soft", 15.0},
              {"key_sat_lo", 15.0},
              {"key_sat_hi", 100.0},
              {"key_sat_soft", 10.0},
              {"key_luma_lo", 0.0},
              {"key_luma_hi", 100.0},
              {"key_luma_soft", 0.0}});
    }).set_tooltip("Select this colour");
  }
  picker.emplace<Spacer>();

  key_range(app, body, target, "hue", "H",
            [](double at) { return cutline::ui::from_hsv({.h = at * 360.0, .s = 0.9, .v = 0.9}); },
            true);
  key_range(app, body, target, "sat", "S",
            [](double at) { return cutline::ui::from_hsv({.h = 0.0, .s = at, .v = 0.85}); },
            false);
  key_range(app, body, target, "luma", "L",
            [](double at) {
              const auto v = static_cast<float>(at);
              return Color{v, v, v, 1.0f};
            },
            false);

  auto& checks = body.emplace<Box>(Axis::Horizontal);
  for (const auto& [key, name] :
       {std::pair{"show_mask", "Show Mask"}, std::pair{"invert_key", "Invert Mask"}}) {
    auto& box = checks.emplace<Checkbox>(name, value_of(target, key) >= 0.5);
    box.set_on_change([&app, clip_id, key = std::string(key)](bool on) {
      commit(app, clip_id, {{key, on ? 1.0 : 0.0}});
    });
  }
  checks.emplace<Spacer>();

  heading(body, "Refine");
  slider_row(app, body, target, "denoise");
  slider_row(app, body, target, "blur");

  heading(body, "Correction");
  auto& wheel = body.emplace<Box>(Axis::Horizontal);
  wheel.emplace<Spacer>();
  wheel_column(app, wheel, target, "sec", "Midtones", true, 92.0);
  wheel.emplace<Spacer>();
  for (const char* key :
       {"sec_temperature", "sec_tint", "sec_contrast", "sec_sharpen", "sec_saturation"}) {
    slider_row(app, body, target, key);
  }
}

void build_vignette(App& app, Box& body, const Target& target) {
  for (const char* key : {"vig_amount", "vig_midpoint", "vig_roundness", "vig_feather"}) {
    slider_row(app, body, target, key);
  }
}

/// The picture under the playhead with this clip's grade partly or wholly off,
/// as the eyedroppers and Auto see it: they measure what the correction they
/// set will be applied to.
[[nodiscard]] std::optional<cutline::gpu::Image> ungraded_picture(
    App& app, const std::string& clip_id, std::vector<std::string> sections) {
#if CUTLINE_HAVE_PREVIEW
  if (app.preview == nullptr) return std::nullopt;
  cutline::core::Project project =
      cutline::editor::without_lumetri(app.session.project(), clip_id, std::move(sections));
  if (app.preview_scale != 1.0) {
    project = cutline::render::scaled_canvas(project, app.preview_scale);
  }
  auto frame = app.preview->capture(std::make_shared<const cutline::core::Project>(std::move(project)),
                                    app.session.playhead());
  if (!frame.has_value() || frame->empty()) return std::nullopt;
  return std::move(*frame);
#else
  (void)app;
  (void)clip_id;
  (void)sections;
  return std::nullopt;
#endif
}

void capture_reference(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.preview == nullptr) return;
  // The sequence as it is, graded, at the reference's moment: what this clip
  // is to be matched to.
  cutline::core::Project project = app.session.project();
  if (app.preview_scale != 1.0) project = cutline::render::scaled_canvas(project, app.preview_scale);
  auto frame = app.preview->capture(
      std::make_shared<const cutline::core::Project>(std::move(project)),
      app.lumetri_reference_time);
  if (!frame.has_value() || frame->empty()) {
    app.lumetri_reference.clear();
    return;
  }
  app.lumetri_reference = std::move(frame->pixels);
  app.lumetri_reference_width = frame->width;
  app.lumetri_reference_height = frame->height;
#else
  (void)app;
#endif
}

void apply_match(App& app, const std::string& clip_id) {
  if (app.lumetri_reference.empty()) return;
  const auto picture = ungraded_picture(app, clip_id, {"wheels"});
  if (!picture.has_value()) return;
  const std::optional<Target> target = target_of(app);
  const double saturation = target.has_value() ? value_of(*target, "saturation") : 100.0;
  const Values values = cutline::editor::colour_match(
      app.lumetri_reference.data(), app.lumetri_reference_width, app.lumetri_reference_height,
      picture->pixels.data(), picture->width, picture->height, saturation);
  if (!values.empty()) commit(app, clip_id, values);
  app.lumetri_stale = true;
}

}  // namespace

void toggle_comparison(App& app) {
  app.lumetri_comparing = !app.lumetri_comparing;
  if (app.lumetri_comparing) capture_reference(app);
  show_comparison(app);
  app.lumetri_stale = true;
}

void show_comparison(App& app) {
  if (app.monitor == nullptr) return;
  if (app.lumetri_comparing && !app.lumetri_reference.empty()) {
    app.monitor->set_reference(cutline::ui::ImageView{
        app.lumetri_reference.data(), app.lumetri_reference_width,
        app.lumetri_reference_height, app.lumetri_reference_width * 4});
  } else {
    app.monitor->set_reference({});
  }
  for (Shell* shell : app.shells()) {
    if (shell->host != nullptr) shell->host->request_layout();
  }
  mark_dirty(app);
}

double local_playhead_of(const App& app, const std::string& clip_id) {
  const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), clip_id);
  if (clip == nullptr) return 0.0;
  return std::clamp(app.session.playhead() - clip->start, 0.0,
                    cutline::core::clip_duration(*clip));
}

void auto_tone_clip(App& app, const std::string& clip_id) {
  const auto picture = ungraded_picture(app, clip_id, {});
  if (!picture.has_value()) return;
  const Values values =
      cutline::editor::auto_tone(picture->pixels.data(), picture->width, picture->height);
  if (!values.empty()) commit(app, clip_id, values);
  app.lumetri_stale = true;
}

bool pick_for_lumetri(App& app, double x, double y) {
  if (app.lumetri_picking == LumetriPick::None) return false;
  const LumetriPick picking = app.lumetri_picking;
  app.lumetri_picking = LumetriPick::None;
  app.lumetri_stale = true;
  const std::optional<Target> target = target_of(app);
  if (!target.has_value()) return true;

  const auto picture = ungraded_picture(
      app, target->clip_id,
      picking == LumetriPick::WhiteBalance ? std::vector<std::string>{}
                                           : std::vector<std::string>{"secondary", "vignette"});
  if (!picture.has_value()) return true;
  // The press comes as a fraction of the picture across and down.
  const int px = std::clamp(static_cast<int>(x * picture->width), 0, picture->width - 1);
  const int py = std::clamp(static_cast<int>(y * picture->height), 0, picture->height - 1);
  // A few pixels round the click, so grain does not decide it.
  std::array<double, 3> sum{};
  int count = 0;
  for (int dy = -2; dy <= 2; ++dy) {
    for (int dx = -2; dx <= 2; ++dx) {
      const int sx = std::clamp(px + dx, 0, picture->width - 1);
      const int sy = std::clamp(py + dy, 0, picture->height - 1);
      const std::uint8_t* p =
          picture->pixels.data() + (static_cast<std::size_t>(sy) * picture->width + sx) * 4;
      for (int c = 0; c < 3; ++c) sum[static_cast<std::size_t>(c)] += p[c];
      ++count;
    }
  }
  const auto channel = [&](std::size_t c) {
    return static_cast<std::uint8_t>(std::lround(sum[c] / count));
  };
  if (picking == LumetriPick::WhiteBalance) {
    const auto [temperature, tint] =
        cutline::editor::white_balance_for(channel(0), channel(1), channel(2));
    commit(app, target->clip_id,
           {{"temperature", std::round(temperature * 10.0) / 10.0},
            {"tint", std::round(tint * 10.0) / 10.0}});
  } else {
    commit(app, target->clip_id, cutline::editor::key_for(channel(0), channel(1), channel(2)));
  }
  return true;
}

void refresh_lumetri_panel(App& app) {
  if (app.lumetri_body == nullptr) return;
  app.lumetri_body->clear_children();
  Box& body = *app.lumetri_body;

  const std::optional<Target> target = target_of(app);
  if (!target.has_value()) {
    body.emplace<Label>("Select a clip on the timeline to grade it.").set_small(true);
  } else {
    auto& title = body.emplace<Box>(Axis::Horizontal);
    title.emplace<Label>(target->name).set_bold(true);
    title.emplace<Spacer>();
    if (!cutline::editor::lumetri_on(app.session.project(), target->clip_id).has_value()) {
      body.emplace<Label>("Changing anything adds a Lumetri Color effect to the clip.")
          .set_small(true);
    }
    if (section(app, body, *target, "Basic Correction", "basic")) build_basic(app, body, *target);
    if (section(app, body, *target, "Creative", "creative")) build_creative(app, body, *target);
    if (section(app, body, *target, "Curves", "curves")) build_curves(app, body, *target);
    if (section(app, body, *target, "Color Wheels", "wheels")) build_wheels(app, body, *target);
    if (section(app, body, *target, "HSL Secondary", "secondary")) {
      build_secondary(app, body, *target);
    }
    if (section(app, body, *target, "Vignette", "vignette")) build_vignette(app, body, *target);
  }

  for (Shell* shell : app.shells()) {
    if (shell->host != nullptr) shell->host->request_layout();
  }
  mark_dirty(app);
}

std::unique_ptr<Widget> make_lumetri_panel(App* app) {
  auto panel = std::make_unique<Panel>();
  auto& scroll = panel->emplace<ScrollView>(Axis::Vertical);
  auto& body = static_cast<Box&>(scroll.set_content(std::make_unique<Box>(Axis::Vertical)));
  body.set_spacing(6.0);
  if (app != nullptr) {
    app->lumetri_body = &body;
    refresh_lumetri_panel(*app);
  } else {
    body.emplace<Label>("Select a clip on the timeline to grade it.").set_small(true);
  }
  return panel;
}

}  // namespace cutline::tool
