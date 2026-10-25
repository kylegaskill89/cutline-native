/// The dialogs that work on clips and markers: Speed/Duration, Interpret
/// Footage, Paste Attributes, Fit to Fill and a marker's details.
///
/// Each is a `ui::Dialog` opened as a popup over the window.

#include "app.hpp"

namespace cutline::tool {

/// Speed and duration, Premiere's box, on the selection.
///
/// The two are one number seen two ways — a clip's length is its source span
/// divided by its rate — so the fields mirror each other as they are typed
/// rather than waiting to disagree until something is applied. Typing a
/// duration is the reason the box exists at all: "make this four seconds" is a
/// thing people want, and working out that it means 137% is not.
///
/// Both numbers are the *anchor's* — the first selected clip. A selection of
/// clips at different lengths has no single duration, and showing the first
/// one's is at least a number that came from something on screen.
void open_speed_dialog(App& app, std::span<const std::string> clips) {
  if (app.main.host == nullptr || clips.empty()) return;
  const cutline::core::Clip* anchor = cutline::core::find_clip(app.session.project(), clips[0]);
  if (anchor == nullptr) return;

  const double fps = app.session.project().sequence().fps;
  const double span = cutline::core::source_span(*anchor);
  const double speed = cutline::core::clip_speed(*anchor);

  auto dialog = std::make_unique<cutline::ui::Dialog>("Speed / Duration");

  auto& speed_row = dialog->row("Speed");
  auto& speed_field = speed_row.emplace<TextField>(std::format("{:.2f}", speed * 100.0));
  speed_field.set_columns(7);
  speed_row.emplace<Label>("%").set_small(true);

  auto& length_row = dialog->row("Duration");
  auto& length_field = length_row.emplace<TextField>(
      cutline::core::seconds_to_timecode(cutline::core::clip_duration(*anchor), fps,
                                         app.session.project().sequence().drop_frame));
  length_field.set_columns(11);

  auto& reverse = dialog->body().emplace<Checkbox>("Reverse speed", anchor->reverse);
  auto& ripple = dialog->body().emplace<Checkbox>("Ripple edit, shifting trailing clips", false);

  // Each field rewrites the other when its edit ends. `span` is fixed by the
  // trim, so it is the constant the two are computed through.
  const bool drop_frame = app.session.project().sequence().drop_frame;
  speed_field.set_on_finish([field = &speed_field, other = &length_field, span, fps, drop_frame] {
    double percent = 0.0;
    if (std::from_chars(field->text().data(), field->text().data() + field->text().size(),
                        percent)
            .ec != std::errc{} ||
        percent <= 0.0) {
      return;
    }
    const double rate = std::clamp(percent / 100.0, cutline::core::kMinSpeed,
                                   cutline::core::kMaxSpeed);
    other->set_text(cutline::core::seconds_to_timecode(span / rate, fps, drop_frame));
  });
  length_field.set_on_finish([field = &length_field, other = &speed_field, span, fps,
                              drop_frame] {
    const std::optional<double> wanted =
        cutline::core::timecode_to_seconds(field->text(), fps, drop_frame);
    if (!wanted.has_value() || *wanted <= 0.0) return;
    const double rate =
        std::clamp(span / *wanted, cutline::core::kMinSpeed, cutline::core::kMaxSpeed);
    other->set_text(std::format("{:.2f}", rate * 100.0));
  });

  dialog->set_accept(
      "OK", [&app, chosen = std::vector<std::string>(clips.begin(), clips.end()),
             field = &speed_field, rev = &reverse,
             rip = &ripple]() -> std::optional<std::string> {
        double percent = 0.0;
        if (std::from_chars(field->text().data(), field->text().data() + field->text().size(),
                            percent)
                .ec != std::errc{} ||
            percent <= 0.0) {
          // Said, and the dialog stays: closing on a typing mistake looked
          // exactly like the retime having worked.
          return "Speed has to be a number above 0.";
        }
        app.session.apply(cutline::core::set_clips_speed(app.session.project(), chosen,
                                                         percent / 100.0, rev->checked(),
                                                         rip->checked()));
        refresh_timeline(app);
        invalidate_preview(app);
        app.inspector_stale = true;
        mark_dirty(app);
        return std::nullopt;
      });

  app.main.host->open_popup(std::move(dialog), settings_anchor());
}

/// Premiere's Interpret Footage, which here is one question: what rate is this?
///
/// Footage shot fast to be shown slow is the whole of the daily use. Conforming
/// 60 to 24 makes the source two and a half times longer and every frame of it
/// a real one, which is a different picture from a 40% retime — that has to
/// invent the frames between or show each one two and a half times. The two
/// look alike in the timeline and do not look alike on screen.
///
/// The resulting length is shown and kept up to date as the number is typed,
/// because it is the thing anybody is actually checking: "does this give me the
/// ten seconds I need" is the question, and working out that it means 23.976 is
/// not.
///
/// Premiere's box also offers alpha and channel layout. Decoded video here is
/// NV12 or YUV 4:2:0 and carries no alpha at all, so those switches would be
/// switches on nothing; channel layout is `Clip::channel_map`, which is built
/// and lives per clip, where a camera's two microphones need deciding.
void open_interpret_dialog(App& app, const std::string& media_id) {
  if (app.main.host == nullptr) return;
  const auto found = std::ranges::find(app.session.project().media, media_id,
                                       &cutline::core::Media::id);
  if (found == app.session.project().media.end() || !cutline::core::can_interpret(*found)) {
    return;
  }

  const cutline::core::Media media = *found;
  const double file_rate = *media.fps;
  // The length with no conform on it, which is what every "how long would it
  // be" below is worked out from.
  const double file_length = media.file_duration.value_or(media.duration);
  const bool conformed = cutline::core::is_conformed(media);

  auto dialog = std::make_unique<cutline::ui::Dialog>("Interpret Footage");
  Box* panel = &dialog->body();
  panel->emplace<Label>(media.name).set_small(true);

  auto& choice = panel->emplace<RadioGroup>(
      std::vector<std::string>{std::format("Use the rate in the file: {:.3f} fps", file_rate),
                               "Assume this rate:"},
      conformed ? 1u : 0u);

  auto& rate_row = panel->emplace<Box>(Axis::Horizontal);
  auto& rate = rate_row.emplace<TextField>(
      std::format("{:.3f}", media.assumed_fps.value_or(file_rate)));
  rate.set_columns(8);
  rate_row.emplace<Label>("fps").set_small(true);

  const bool drop_frame = app.session.project().sequence().drop_frame;
  auto& length = panel->emplace<Label>("");
  length.set_small(true);

  // Premiere's Color Space Override, in the same box.
  constexpr std::array<const char*, 3> kSpaces{"Rec. 709", "Rec. 2100 PQ", "Rec. 2100 HLG"};
  const auto tagged = static_cast<std::size_t>(
      media.colour_space.value_or(cutline::core::WorkingColour::Rec709));
  auto& colour_row = panel->emplace<Box>(Axis::Horizontal);
  colour_row.emplace<Label>("Colour space").set_small(true);
  colour_row.emplace<Spacer>();
  const std::string as_tagged =
      std::format("As tagged: {}", kSpaces[std::min<std::size_t>(tagged, 2)]);
  const std::size_t overridden =
      media.colour_override.has_value() ? static_cast<std::size_t>(*media.colour_override) + 1 : 0u;
  auto& colour = colour_row.emplace<Dropdown>(
      std::vector<std::string>{as_tagged, kSpaces[0], kSpaces[1], kSpaces[2]}, overridden);

  // Kept in step from one place, so the radio and the field cannot disagree
  // about which length is on show.
  const auto show_length = [&length, &choice, &rate, file_rate, file_length, drop_frame] {
    double at = file_rate;
    if (choice.selected() == 1) {
      double typed = 0.0;
      if (std::from_chars(rate.text().data(), rate.text().data() + rate.text().size(), typed)
                  .ec == std::errc{} &&
          typed > 0.0) {
        at = typed;
      }
    }
    length.set_text("It would run " + cutline::core::seconds_to_timecode(
                                          file_length * file_rate / at, at, drop_frame));
  };
  show_length();
  choice.set_on_change([show_length](std::size_t) { show_length(); });
  rate.set_on_finish([show_length] { show_length(); });

  dialog->set_accept("OK", [&app, media_id, choice = &choice, rate = &rate,
                            colour = &colour]() -> std::optional<std::string> {
    std::optional<double> assumed;
    if (choice->selected() == 1) {
      double typed = 0.0;
      if (std::from_chars(rate->text().data(), rate->text().data() + rate->text().size(), typed)
                  .ec == std::errc{} &&
          typed > 0.0) {
        assumed = typed;
      } else {
        // Said, and the dialog stays, rather than conforming the source to
        // nothing -- which is a source of no length at all.
        return "The rate has to be a number above 0.";
      }
    }

    const std::size_t space = colour->selected();
    std::optional<cutline::core::WorkingColour> override;
    if (space > 0) {
      override = static_cast<cutline::core::WorkingColour>(std::min<std::size_t>(space - 1, 2));
    }
    // One edit, so one undo takes back both.
    app.session.apply(cutline::core::set_colour_override(
        cutline::core::interpret_media(app.session.project(), media_id, assumed), media_id,
        override));
    // Everything: the pool's duration column, the clips that changed length,
    // the picture, and the sound, which is now being read at another rate.
    refresh_all(app);
    stop_playback(app);
    invalidate_preview(app);
    return std::nullopt;
  });


  app.main.host->open_popup(std::move(dialog), settings_anchor());
}

/// Premiere's Paste Attributes: some of what was copied, not all of it.
///
/// A plain paste puts a whole clip down. This puts a *part* of one onto clips
/// that are already there — the framing off that shot, the grade, the level —
/// and the whole of the design is the question, because there is no default
/// worth guessing. "Everything" is Ctrl+V and already exists.
///
/// The groups are Premiere's, and each one is several fields: Motion is seven
/// numbers and the keyframes on all of them, because moving a shot and leaving
/// its animation behind gives you a clip that jumps back where it started the
/// moment it plays.
///
/// A group whose kind is not on the clipboard is shown greyed rather than
/// hidden. Copying a video-only clip and finding the sound half of the box
/// missing reads as a bug; finding it greyed says why.
void open_paste_attributes_dialog(App& app) {
  using cutline::editor::Command;
  if (app.main.host == nullptr) return;
  if (!cutline::editor::can_run(app.session, Command::PasteAttributes)) return;

  bool has_picture = false;
  bool has_sound = false;
  for (const cutline::core::ClipCopy& copy : app.session.clipboard()) {
    if (copy.clip.kind == cutline::core::TrackKind::Video) has_picture = true;
    else has_sound = true;
  }

  const cutline::core::ClipAttributes& was = app.paste_attributes;

  auto dialog = std::make_unique<cutline::ui::Dialog>("Paste Attributes");
  Box* panel = &dialog->body();

  // Ticked from what was chosen last time, which is what Premiere does and
  // what makes this usable on the second clip: pasting one look onto six
  // shots one at a time should not be six trips through the same six ticks.
  auto& picture = panel->emplace<Label>("Video");
  picture.set_small(true);
  auto& motion = panel->emplace<Checkbox>("Motion, and its keyframes", was.motion);
  auto& opacity = panel->emplace<Checkbox>("Opacity and fades", was.opacity);
  auto& blend = panel->emplace<Checkbox>("Blend mode", was.blend);
  auto& effects = panel->emplace<Checkbox>("Effects", was.effects);

  auto& sound = panel->emplace<Label>("Audio");
  sound.set_small(true);
  auto& volume = panel->emplace<Checkbox>("Volume and fades", was.volume);
  auto& pan = panel->emplace<Checkbox>("Pan", was.pan);
  auto& channels = panel->emplace<Checkbox>("Channel mapping", was.channels);
  auto& role = panel->emplace<Checkbox>("Role", was.role);
  auto& audio_effects = panel->emplace<Checkbox>("Audio effects", was.audio_effects);

  for (Checkbox* box : {&motion, &opacity, &blend, &effects}) box->set_enabled(has_picture);
  for (Checkbox* box : {&volume, &pan, &channels, &role, &audio_effects}) {
    box->set_enabled(has_sound);
  }

  auto& scale = panel->emplace<Checkbox>("Stretch keyframes to the clip they land on",
                                         was.scale_to_length);
  scale.set_tooltip(
      "A two-second push-in becomes a ten-second one on a ten-second shot, rather than "
      "staying two seconds long");

  dialog->set_accept("Paste", [&app, has_picture, has_sound, motion = &motion,
                               opacity = &opacity, blend = &blend, effects = &effects,
                               volume = &volume, pan = &pan, channels = &channels,
                               role = &role, audio_effects = &audio_effects,
                               scale = &scale]() -> std::optional<std::string> {
    // Read through what the clipboard actually holds, so a tick left over from
    // last time on a group that is greyed now cannot travel.
    const cutline::core::ClipAttributes which{
        .motion = has_picture && motion->checked(),
        .opacity = has_picture && opacity->checked(),
        .blend = has_picture && blend->checked(),
        .effects = has_picture && effects->checked(),
        .volume = has_sound && volume->checked(),
        .pan = has_sound && pan->checked(),
        .channels = has_sound && channels->checked(),
        .role = has_sound && role->checked(),
        .audio_effects = has_sound && audio_effects->checked(),
        .scale_to_length = scale->checked()};

    app.paste_attributes = which;

    app.session.apply(cutline::core::paste_attributes(app.session.project(),
                                                      app.session.clipboard(),
                                                      app.session.selected_group(), which));
    refresh_timeline(app);
    invalidate_preview(app);
    app.inspector_stale = true;
    mark_dirty(app);
    return std::nullopt;
  });


  app.main.host->open_popup(std::move(dialog), settings_anchor());
}

/// The five choices in `core::FitChoice`, in Premiere's own order.
///
/// Change Speed leads because it is the only one that keeps all four marks. The
/// two trims come next: giving up part of the source is a smaller thing than
/// giving up where the edit lands.
constexpr std::array<std::pair<std::string_view, cutline::core::FitChoice>, 5> kFitChoices{{
    {"Change clip speed (fit to fill)", cutline::core::FitChoice::ChangeSpeed},
    {"Trim source's head (left side)", cutline::core::FitChoice::TrimHead},
    {"Trim source's tail (right side)", cutline::core::FitChoice::TrimTail},
    {"Ignore sequence in point", cutline::core::FitChoice::IgnoreSequenceIn},
    {"Ignore sequence out point", cutline::core::FitChoice::IgnoreSequenceOut},
}};

/// Premiere's Fit Clip: what to give up when all four marks are set and they
/// disagree.
///
/// Four marks fix more than an edit has freedom for, and every way out gives up
/// exactly one of them. Which one matters least is a judgement about the cut,
/// and nothing in the model can make it — which is why this is a question
/// rather than a rule, and why all five are offered rather than the one that
/// was easiest to build.
void open_fit_dialog(App& app) {
  if (app.main.host == nullptr) return;
  if (!cutline::editor::can_run(app.session, cutline::editor::Command::FitToFill)) return;

  auto dialog = std::make_unique<cutline::ui::Dialog>("Fit Clip");
  Box* panel = &dialog->body();
  panel->emplace<Label>("The source and the marked span are different lengths.")
      .set_small(true);

  std::vector<std::string> names;
  names.reserve(kFitChoices.size());
  for (const auto& [label, choice] : kFitChoices) names.emplace_back(label);
  auto& choices = panel->emplace<cutline::ui::RadioGroup>(std::move(names), 0u);

  dialog->set_accept("OK", [&app, chosen = &choices]() -> std::optional<std::string> {
    const std::size_t index = chosen->selected();
    if (index >= kFitChoices.size()) return std::nullopt;

    // Read again rather than captured at the point the box opened: the marks,
    // the source and the targeted track can all move while it is up, and the
    // edit should be the one the sequence asks for now.
    if (!cutline::editor::can_run(app.session, cutline::editor::Command::FitToFill)) {
      return std::nullopt;
    }
    cutline::core::Project ready =
        cutline::core::match_sequence_to(app.session.project(), app.session.source_media());
    const std::string target = cutline::editor::edit_target(ready);
    const auto points = cutline::core::edit_points(ready, app.session.source_media(),
                                                   app.session.playhead());
    app.session.apply(cutline::core::fit_media(std::move(ready), app.session.source_media(),
                                               points, kFitChoices[index].second, target));
    refresh_all(app);
    invalidate_preview(app);
    mark_dirty(app);
    return std::nullopt;
  });


  app.main.host->open_popup(std::move(dialog), settings_anchor());
}

/// A marker's name, note, colour and length. Premiere's marker dialogue.
///
/// Opened from a double-click on the marker itself, which is the only gesture
/// on the ruler that is not already scrubbing — a single click there has to
/// keep moving the playhead, including over a marker, or the markers would be
/// holes in the one control that spans the whole sequence.
///
/// Delete is on it rather than only on a key, because a marker with a note in
/// it is a thing somebody made and the place to throw it away is the place they
/// are looking at it.
void open_marker_dialog(App& app, std::size_t index) {
  if (app.main.host == nullptr || app.timeline == nullptr) return;
  const std::vector<cutline::core::Marker>& markers = app.session.project().sequence().markers;
  if (index >= markers.size()) return;
  const cutline::core::Marker marker = markers[index];
  const double fps = app.session.project().sequence().fps;

  auto dialog = std::make_unique<cutline::ui::Dialog>(
      "Marker at " + cutline::core::seconds_to_timecode(
                         marker.time, fps, app.session.project().sequence().drop_frame));
  Box* panel = &dialog->body();

  auto& name_row = dialog->row("Name");
  auto& name = name_row.emplace<TextField>(marker.label);
  name.set_columns(18);

  auto& note_row = dialog->row("Comment");
  auto& comment = note_row.emplace<TextField>(marker.comment);
  comment.set_columns(28);

  auto& span_row = dialog->row("Duration");
  // A timecode rather than seconds, like every other length in the interface.
  // Zero reads as a point marker, which is what it writes back as too.
  auto& span = span_row.emplace<TextField>(
      cutline::core::seconds_to_timecode(marker.duration, fps,
                                         app.session.project().sequence().drop_frame));
  span.set_columns(11);

  panel->emplace<Label>("Colour").set_small(true);
  auto& colors = panel->emplace<Box>(Axis::Horizontal);
  // The label colours, which is the palette this application already has names
  // for. A second set of colours would be a second vocabulary to learn.
  auto chosen = std::make_shared<std::string>(marker.color);
  // Every swatch, so pressing one can light it and put the others out. Without
  // this the choice was silent: the colour was stored and the box went on
  // showing whatever the marker already was, so pressing Rose looked like
  // pressing nothing. Found by driving it — the code was right and the
  // interface said so nowhere.
  auto swatches = std::make_shared<std::vector<Button*>>();
  const auto choose = [&app, chosen, swatches](std::string color, Button* pressed) {
    *chosen = std::move(color);
    for (Button* swatch : *swatches) swatch->set_selected(swatch == pressed);
    if (app.main.host != nullptr) app.main.host->request_paint();
  };

  {
    auto& none = colors.emplace<Button>("None");
    none.set_on_click([choose, button = &none] { choose(std::string{}, button); });
    none.set_selected(marker.color.empty());
    swatches->push_back(&none);
  }
  for (const cutline::editor::ClipLabel& label : cutline::editor::clip_labels()) {
    auto& swatch = colors.emplace<Button>(std::string(label.name));
    swatch.set_on_click([choose, button = &swatch, color = std::string(label.color)] {
      choose(color, button);
    });
    swatch.set_selected(marker.color == label.color);
    swatches->push_back(&swatch);
  }

  dialog->set_accept("OK", [&app, id = marker.id, fps, chosen, n = &name, c = &comment,
                            d = &span]() -> std::optional<std::string> {
    // A duration that does not parse leaves the one it had rather than
    // becoming zero, which would silently turn a span back into a point.
    const std::optional<double> length =
        cutline::core::timecode_to_seconds(d->text(), fps, app.session.project().sequence().drop_frame);
    const cutline::core::Marker* was = nullptr;
    for (const cutline::core::Marker& m : app.session.project().sequence().markers) {
      if (m.id == id) was = &m;
    }
    const double duration = length.value_or(was != nullptr ? was->duration : 0.0);

    app.session.apply(cutline::core::set_marker(app.session.project(), id, n->text(),
                                                c->text(), *chosen, duration));
    refresh_timeline(app);
    mark_dirty(app);
    return std::nullopt;
  });
  dialog->add_button("Delete", [&app, id = marker.id] {
    if (app.main.host != nullptr) app.main.host->close_popup();
    app.session.apply(cutline::core::remove_marker(app.session.project(), id));
    refresh_timeline(app);
    mark_dirty(app);
  });

  // Beside the marker rather than under the menu bar. A dialogue about a thing
  // on the ruler that opens at the far corner of the window makes you look away
  // from what you are editing and then find your way back — and the marker is
  // usually the very thing you want to see while typing what it means.
  const Rect tab = app.timeline->marker_rect(index);

  app.main.host->open_popup(std::move(dialog),
                            tab.empty() ? settings_anchor() : Rect{tab.x, tab.bottom(), 0.0, 0.0});
}

}  // namespace cutline::tool
