/// Premiere's Text panel, for captions: the captions on a track listed in time
/// order with their words to edit, the commands that make and divide them,
/// importing and exporting SubRip, and the track's style.
///
/// What a caption is lives in `core/captions.hpp`; this is where it meets the
/// panel. Every change is an edit, so undo takes it back, and the panel is
/// rebuilt from the document whenever the document or the selection changes.

#include "app.hpp"

#include "cutline/core/captions.hpp"

#include <fstream>
#include <iterator>

namespace cutline::tool {
namespace {

using cutline::ui::ColorSwatch;
using cutline::ui::NumericField;
using cutline::ui::ValueRange;

/// The caption tracks of the open sequence, top first.
[[nodiscard]] std::vector<const cutline::core::Track*> caption_tracks(const App& app) {
  std::vector<const cutline::core::Track*> found;
  for (const cutline::core::Track& track : app.session.project().sequence().tracks) {
    if (cutline::core::is_caption_track(track)) found.push_back(&track);
  }
  return found;
}

/// The caption track the panel is editing: the one chosen, or the first.
[[nodiscard]] const cutline::core::Track* active_track(const App& app) {
  const std::vector<const cutline::core::Track*> tracks = caption_tracks(app);
  for (const cutline::core::Track* track : tracks) {
    if (track->id == app.text_track) return track;
  }
  return tracks.empty() ? nullptr : tracks.front();
}

/// Applies an edit and shows it everywhere it shows.
void apply(App& app, cutline::core::Project next, std::string name = {}) {
  if (!name.empty()) app.session.name_next_edit(std::move(name));
  if (!app.session.apply(std::move(next))) return;
  refresh_timeline(app);
  invalidate_preview(app);
  mark_dirty(app);
}

/// The captions chosen on the timeline, on the active track.
[[nodiscard]] std::vector<std::string> chosen_captions(const App& app,
                                                       const cutline::core::Track& track) {
  std::vector<std::string> chosen;
  for (const cutline::core::Clip& clip : track.clips) {
    if (app.session.is_selected(clip.id)) chosen.push_back(clip.id);
  }
  return chosen;
}

/// A caption's words split at its first line break, for the two line fields.
[[nodiscard]] std::pair<std::string, std::string> lines_of(const std::string& text) {
  const std::size_t cut = text.find('\n');
  if (cut == std::string::npos) return {text, {}};
  std::string second = text.substr(cut + 1);
  // A third line, which a file can bring, stays on the second rather than
  // being lost.
  std::ranges::replace(second, '\n', ' ');
  return {text.substr(0, cut), std::move(second)};
}

[[nodiscard]] std::optional<std::filesystem::path> choose_captions_file(HWND owner, bool saving) {
  std::array<wchar_t, MAX_PATH> buffer{};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = L"SubRip captions\0*.srt\0All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrDefExt = L"srt";
  dialog.Flags = saving ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
                        : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
  const BOOL chosen = saving ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
  if (chosen == FALSE) return std::nullopt;
  return std::filesystem::path(buffer.data());
}

/// A track style with one change made to it, applied as an edit.
void restyle(App& app, const std::string& track_id,
             const std::function<void(cutline::core::CaptionStyle&)>& change) {
  const auto found = std::ranges::find(app.session.project().sequence().tracks, track_id,
                                       &cutline::core::Track::id);
  if (found == app.session.project().sequence().tracks.end()) return;
  cutline::core::CaptionStyle style = found->caption_style;
  change(style);
  apply(app, cutline::core::set_caption_style(app.session.project(), track_id, std::move(style)),
        "Caption Style");
}

/// The track's style: Premiere's caption formatting, for the whole track.
void build_style(App& app, Box& into, const cutline::core::Track& track) {
  const cutline::core::CaptionStyle& style = track.caption_style;
  const std::string id = track.id;
  into.emplace<Label>("Track Style").set_bold(true);

  auto& font_row = into.emplace<Box>(Axis::Horizontal);
  font_row.emplace<Label>("Font").set_small(true);
  auto& font = font_row.emplace<TextField>(style.look.font_family);
  font.set_columns(14);
  font.set_on_finish([&app, id, field = &font] {
    const std::string face = field->text();
    if (face.empty()) return;
    restyle(app, id, [&face](cutline::core::CaptionStyle& s) { s.look.font_family = face; });
  });
  auto& size = font_row.emplace<NumericField>(ValueRange{.minimum = 6.0, .maximum = 400.0},
                                              style.look.font_size);
  size.set_decimals(0);
  size.set_on_commit([&app, id](double v) {
    restyle(app, id, [v](cutline::core::CaptionStyle& s) { s.look.font_size = v; });
  });
  font_row.emplace<Spacer>();

  auto& face_row = into.emplace<Box>(Axis::Horizontal);
  auto& bold = face_row.emplace<Checkbox>("Bold", style.look.bold);
  bold.set_on_change([&app, id](bool on) {
    restyle(app, id, [on](cutline::core::CaptionStyle& s) { s.look.bold = on; });
  });
  auto& italic = face_row.emplace<Checkbox>("Italic", style.look.italic);
  italic.set_on_change([&app, id](bool on) {
    restyle(app, id, [on](cutline::core::CaptionStyle& s) { s.look.italic = on; });
  });
  face_row.emplace<Label>("Colour").set_small(true);
  auto& fill = face_row.emplace<ColorSwatch>(
      parse_color(style.look.fill, Color{1.0f, 1.0f, 1.0f, 1.0f}));
  fill.set_on_commit([&app, id](const Color& picked) {
    restyle(app, id, [hex = to_hex(picked)](cutline::core::CaptionStyle& s) { s.look.fill = hex; });
  });
  face_row.emplace<Spacer>();

  // A box behind the words, which is what makes a caption readable over any
  // picture, and an outline, which is the other way of doing the same.
  auto& box_row = into.emplace<Box>(Axis::Horizontal);
  auto& background = box_row.emplace<Checkbox>("Background", style.look.background.has_value());
  background.set_on_change([&app, id](bool on) {
    restyle(app, id, [on](cutline::core::CaptionStyle& s) {
      if (on) s.look.background = cutline::core::GraphicBackground{.padding = 10.0};
      else s.look.background.reset();
    });
  });
  if (style.look.background.has_value()) {
    auto& shade = box_row.emplace<ColorSwatch>(
        parse_color(style.look.background->color, Color{0.0f, 0.0f, 0.0f, 1.0f}));
    shade.set_on_commit([&app, id](const Color& picked) {
      restyle(app, id, [hex = to_hex(picked)](cutline::core::CaptionStyle& s) {
        if (s.look.background.has_value()) s.look.background->color = hex;
      });
    });
    auto& opacity = box_row.emplace<NumericField>(ValueRange{.minimum = 0.0, .maximum = 100.0},
                                                  style.look.background->opacity * 100.0);
    opacity.set_suffix("%");
    opacity.set_decimals(0);
    opacity.set_on_commit([&app, id](double v) {
      restyle(app, id, [v](cutline::core::CaptionStyle& s) {
        if (s.look.background.has_value()) s.look.background->opacity = v / 100.0;
      });
    });
  }
  box_row.emplace<Spacer>();

  auto& outline_row = into.emplace<Box>(Axis::Horizontal);
  auto& outline = outline_row.emplace<Checkbox>("Outline", !style.look.strokes.empty());
  outline.set_on_change([&app, id](bool on) {
    restyle(app, id, [on](cutline::core::CaptionStyle& s) {
      s.look.strokes.clear();
      if (on) s.look.strokes.push_back(cutline::core::GraphicStroke{.width = 3.0});
    });
  });
  outline_row.emplace<Spacer>();

  // Premiere's region: the foot of the frame, the middle or the top.
  auto& place_row = into.emplace<Box>(Axis::Horizontal);
  place_row.emplace<Label>("Position").set_small(true);
  const std::size_t at = style.y < 0.33 ? 2u : style.y < 0.75 ? 1u : 0u;
  auto& place = place_row.emplace<Dropdown>(std::vector<std::string>{"Bottom", "Middle", "Top"},
                                            at);
  place.set_on_change([&app, id](std::size_t index) {
    // Measured to the text's foot, so the top position is a line's height down.
    constexpr std::array<double, 3> kFoot{0.9, 0.58, 0.2};
    restyle(app, id, [y = kFoot[std::min<std::size_t>(index, 2)]](cutline::core::CaptionStyle& s) {
      s.y = y;
    });
  });
  place_row.emplace<Spacer>();
}

}  // namespace

void import_captions_file(App& app, const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto cues = cutline::core::parse_srt(text);
  if (!cues.has_value()) {
    complain(app.main.window, "Could not read captions from that file.\n\n" + cues.error());
    return;
  }
  // From the start of the sequence, Premiere's default Start Point.
  std::string made;
  apply(app, cutline::core::import_captions(app.session.project(), *cues, 0.0, &made),
        "Import Captions");
  app.text_track = made;
}

void refresh_text_panel(App& app) {
  if (app.text_body == nullptr) return;
  const std::uint64_t revision = app.session.revision();
  if (revision == app.text_revision && app.text_track == app.text_shown_track) return;
  app.text_revision = revision;
  app.text_shown_track = app.text_track;

  Box& body = *app.text_body;
  body.clear_children();

  const std::vector<const cutline::core::Track*> tracks = caption_tracks(app);
  const cutline::core::Track* track = active_track(app);

  auto& top = body.emplace<Box>(Axis::Horizontal);
  if (!tracks.empty()) {
    std::vector<std::string> names;
    std::size_t chosen = 0;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      const auto at = std::ranges::find(app.session.project().sequence().tracks, tracks[i]->id,
                                        &cutline::core::Track::id);
      names.push_back(cutline::editor::default_track_label(
          app.session.project(),
          static_cast<std::size_t>(at - app.session.project().sequence().tracks.begin())));
      if (tracks[i] == track) chosen = i;
    }
    auto& which = top.emplace<Dropdown>(std::move(names), chosen);
    which.set_on_change([&app](std::size_t index) {
      const std::vector<const cutline::core::Track*> all = caption_tracks(app);
      if (index < all.size()) app.text_track = all[index]->id;
      mark_dirty(app);
    });
  }
  top.emplace<Button>("New Caption Track", [&app] {
    std::string made;
    apply(app, cutline::core::add_caption_track(app.session.project(), &made),
          "New Caption Track");
    app.text_track = made;
  });
  top.emplace<Spacer>();

  auto& commands = body.emplace<Box>(Axis::Horizontal);
  auto& add = commands.emplace<Button>("Add Caption", [&app] {
    const cutline::core::Track* on = active_track(app);
    if (on == nullptr) return;
    std::string made;
    apply(app,
          cutline::core::add_caption(app.session.project(), on->id, app.session.playhead(),
                                     "New caption", cutline::core::kDefaultCaptionLength, &made),
          "Add Caption");
    if (!made.empty()) app.session.select_one(made);
  });
  add.set_tooltip("A caption at the playhead, three seconds long or up to the next");
  auto& split = commands.emplace<Button>("Split", [&app] {
    const cutline::core::Track* on = active_track(app);
    if (on == nullptr) return;
    std::vector<std::string> chosen = chosen_captions(app, *on);
    // The caption under the playhead when none is chosen.
    if (chosen.empty()) {
      for (const cutline::core::Clip& clip : on->clips) {
        if (clip.start <= app.session.playhead() &&
            app.session.playhead() < cutline::core::clip_end(clip)) {
          chosen.push_back(clip.id);
        }
      }
    }
    if (chosen.empty()) return;
    apply(app, cutline::core::split_caption(app.session.project(), chosen.front()),
          "Split Caption");
  });
  split.set_tooltip("Two captions, each half as long, both saying what it said");
  auto& merge = commands.emplace<Button>("Merge", [&app] {
    const cutline::core::Track* on = active_track(app);
    if (on == nullptr) return;
    apply(app, cutline::core::merge_captions(app.session.project(), chosen_captions(app, *on)),
          "Merge Captions");
  });
  merge.set_tooltip("The chosen captions as one, their words joined");
  commands.emplace<Spacer>();
  commands.emplace<Button>("Import...", [&app] {
    if (const auto file = choose_captions_file(app.main.window, false); file.has_value()) {
      import_captions_file(app, *file);
    }
  }).set_tooltip("A new caption track from a SubRip file");
  auto& save = commands.emplace<Button>("Export...", [&app] {
    const cutline::core::Track* on = active_track(app);
    if (on == nullptr) return;
    const std::string written = cutline::core::write_srt(*on);
    const auto file = choose_captions_file(app.main.window, true);
    if (!file.has_value()) return;
    std::ofstream out(*file, std::ios::binary);
    out << written;
    if (!out) complain(app.main.window, "Could not write the captions there.");
  });
  save.set_tooltip("This track as a SubRip file");
  for (Button* each : {&add, &split, &merge, &save}) each->set_enabled(track != nullptr);

  if (track == nullptr) {
    body.emplace<Label>("No caption track. Make one, or import a SubRip file.").set_small(true);
    mark_dirty(app);
    return;
  }

  auto& scroll = body.emplace<ScrollView>(Axis::Vertical);
  auto& list = static_cast<Box&>(scroll.set_content(std::make_unique<Box>(Axis::Vertical)));
  const cutline::core::Sequence& sequence = app.session.project().sequence();
  if (track->clips.empty()) {
    list.emplace<Label>("No captions yet. Add Caption puts one at the playhead.").set_small(true);
  }
  for (const cutline::core::Clip& clip : track->clips) {
    auto& row = list.emplace<Box>(Axis::Horizontal);
    // The start, as a button that goes there and chooses it.
    auto& when = row.emplace<Button>(
        cutline::core::seconds_to_timecode(clip.start, sequence.fps, sequence.drop_frame),
        [&app, id = clip.id, at = clip.start] {
          app.session.select_one(id);
          go_to_time(app, at);
          refresh_timeline(app);
        });
    when.set_selected(app.session.is_selected(clip.id));
    auto& words = row.emplace<Box>(Axis::Vertical);
    const auto [first, second] = lines_of(clip.caption);
    auto& line1 = words.emplace<TextField>(first);
    line1.set_columns(32);
    auto& line2 = words.emplace<TextField>(second);
    line2.set_columns(32);
    line2.set_placeholder("Second line");
    const auto commit = [&app, id = clip.id, a = &line1, b = &line2] {
      std::string text = a->text();
      if (!b->text().empty()) text += "\n" + b->text();
      apply(app, cutline::core::set_caption_text(app.session.project(), id, std::move(text)),
            "Caption Text");
    };
    line1.set_on_finish(commit);
    line2.set_on_finish(commit);
  }

  // In the scroll with the captions, so a short panel scrolls to the style
  // rather than squeezing the list to nothing to make room for it.
  build_style(app, list, *track);
  mark_dirty(app);
}

[[nodiscard]] std::unique_ptr<Widget> make_text_panel(App* app) {
  auto panel = std::make_unique<Panel>();
  auto& body = panel->emplace<Box>(Axis::Vertical);
  // Something to show before there is an application to fill it from, as the
  // other panels have: a gallery or a layout check builds them without one.
  if (app == nullptr) body.emplace<Label>("Captions").set_small(true);
  if (app != nullptr) {
    app->text_body = &body;
    app->text_revision = ~std::uint64_t{0};
    refresh_text_panel(*app);
  }
  return panel;
}

}  // namespace cutline::tool
