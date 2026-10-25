/// The Effect Controls panel's contents: what the selected clip is doing.
///
/// Rebuilt from the session whenever the selection or the clip changes --
/// the motion and its keyframes, the effect stack, a transition's settings,
/// a title's or a matte's -- rather than patched in place.

#include "app.hpp"

namespace cutline::tool {

/// The scale axis tied to this one by the aspect lock, if it is a scale at all.
[[nodiscard]] std::optional<cutline::editor::ClipParam> other_scale_axis(
    cutline::editor::ClipParam param) noexcept {
  using cutline::editor::ClipParam;
  if (param == ClipParam::ScaleX) return ClipParam::ScaleY;
  if (param == ClipParam::ScaleY) return ClipParam::ScaleX;
  return std::nullopt;
}

/// The other component of a two-part property, given its first.
///
/// Premiere's Position and Anchor Point are each one property with an x and a y,
/// and both halves belong on one row with one stopwatch. Scale is deliberately
/// not in here: Premiere keeps Scale and Scale Width apart too, because Uniform
/// Scale is what joins them.
[[nodiscard]] std::optional<cutline::editor::ClipParam> paired_axis(
    cutline::editor::ClipParam param) noexcept {
  using cutline::editor::ClipParam;
  if (param == ClipParam::X) return ClipParam::Y;
  if (param == ClipParam::AnchorX) return ClipParam::AnchorY;
  return std::nullopt;
}

/// Whether a row is the *second* half of a pair, and so has no row of its own.
[[nodiscard]] bool is_paired_follower(cutline::editor::ClipParam param) noexcept {
  using cutline::editor::ClipParam;
  return param == ClipParam::Y || param == ClipParam::AnchorY;
}

/// What a paired row is called, which is neither half's name.
[[nodiscard]] std::string_view pair_name(cutline::editor::ClipParam param) noexcept {
  using cutline::editor::ClipParam;
  return param == ClipParam::AnchorX ? "Anchor Point" : "Position";
}

/// A heading inside the inspector, over the group of controls it names.
void inspector_heading(App& app, std::string text) {
  auto& label = app.inspector->emplace<Label>(std::move(text));
  label.set_bold(true);
}

/// Where the playhead is within a clip, in the seconds keyframes are stored in.
///
/// Clip-local: keyframe times run from the clip's start, which is also what the
/// renderer resolves them against. Clamped to the clip, so a keyframe cannot be
/// dropped outside the span it belongs to.
[[nodiscard]] double local_playhead(const App& app, const std::string& clip_id) {
  const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), clip_id);
  if (clip == nullptr) return 0.0;
  return std::clamp(app.session.playhead() - clip->start, 0.0,
                    cutline::core::clip_duration(*clip));
}

/// The keyframes on one property, or nothing when it is not animated.
///
/// Through `clip_keyframes` rather than reaching into the model, so a lane in
/// the view and a row in the inspector cannot disagree about what is animated.
[[nodiscard]] std::vector<cutline::core::Keyframe> lane_keys(
    const App& app, const std::string& clip_id, const cutline::editor::ParamRef& ref) {
  const cutline::editor::KeyframeModel model =
      cutline::editor::clip_keyframes(app.session.project(), clip_id);
  for (const cutline::editor::KeyframeLane& lane : model.lanes) {
    if (lane.ref == ref) return lane.keys;
  }
  return {};
}

/// Moves the playhead to the keyframe either side of it on one property.
///
/// In timeline time, because that is what the playhead is in. The lane's own
/// times are clip-local, so the clip's start has to go back on.
void step_to_keyframe(App& app, const std::string& clip_id,
                      const cutline::editor::ParamRef& ref, int direction) {
  const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), clip_id);
  if (clip == nullptr) return;

  const std::vector<cutline::core::Keyframe> keys = lane_keys(app, clip_id, ref);
  const double here = local_playhead(app, clip_id);
  const std::optional<double> target = direction < 0
                                           ? cutline::editor::keyframe_before(keys, here)
                                           : cutline::editor::keyframe_after(keys, here);
  if (!target.has_value()) return;

  app.session.set_playhead(clip->start + *target);
  refresh_timeline(app);
  invalidate_preview(app);
  app.inspector_stale = true;
}

/// Scrolls the timeline to keep the playhead in view.
///
/// Only from playback and the shuttle. Scrubbing deliberately does not scroll:
/// the pointer is already where the answer is, and moving the view out from
/// under a drag makes it impossible to aim.
void scroll_to_playhead(App& app) {
  if (app.timeline == nullptr) return;
  // The view is asked whether it moved rather than told to repaint, because the
  // playhead spends nearly every frame of a playback somewhere already visible
  // and a repaint per frame for nothing is the difference this loop is careful
  // about everywhere else.
  if (app.timeline->follow_playhead()) mark_dirty(app);
}

/// Rebuilds the inspector when moving the playhead changes what it shows.
///
/// Which is only when the selected clip animates something: an animated row
/// reads its value at the playhead and a static one does not care where it is.
/// Guarded rather than unconditional because a rebuild is dozens of widgets and
/// playback moves the playhead thirty times a second.
/// Puts the playhead's time in the readout.
///
/// Not while it has the keyboard: it is a field now, and overwriting what
/// somebody is halfway through typing — which playback would do thirty times a
/// second — is the one thing a field must never do to them.
void show_playhead(App& app) {
  if (app.readout == nullptr) return;
  if (app.main.host != nullptr && app.main.host->focused() == app.readout) return;
  app.readout->set_text(
      cutline::core::seconds_to_timecode(app.session.playhead(), app.session.project().sequence().fps,
                                         app.session.project().sequence().drop_frame));
}

void follow_playhead(App& app) {
  const auto selection = app.session.selection();
  if (selection.empty()) return;

  const cutline::core::Clip* clip =
      cutline::core::find_clip(app.session.project(), selection.front());
  if (clip == nullptr) return;

  const bool animated =
      cutline::core::clip_has_effect_keyframes(*clip) ||
      cutline::core::is_gain_animated(*clip) ||
      std::ranges::any_of(cutline::core::kAnimProps, [clip](cutline::core::AnimProp prop) {
        return cutline::core::is_animated(*clip, prop);
      });
  if (animated) app.inspector_stale = true;
}

/// One parameter, in Premiere's arrangement: the stopwatch that turns animation
/// on, the name, a marker saying whether there is a keyframe at the playhead,
/// and the control itself underneath.
struct ParamRow {
  std::string name;
  std::string suffix;
  /// What the disclosure triangle's state is remembered against. Has to be
  /// unique within the panel and stable across a rebuild — the effect's index
  /// and the parameter's key, since two effects can both have an "Amount".
  std::string key;
  ValueRange range;
  double value = 0.0;
  double fallback = 0.0;
  bool animatable = false;
  bool animated = false;
  bool keyed_here = false;
  /// Whether there is a keyframe to go back or forward to, which is what greys
  /// the ◀ and ▶ at the ends of the list.
  bool has_previous = false;
  bool has_next = false;
  /// A second number on the same row.
  ///
  /// Premiere's Position is *one* property with two components, on one row,
  /// behind one stopwatch. Ours are two `AnimProp`s with two keyframe lists, so
  /// the row's animation chrome fans out to both — which is what makes the two
  /// stay in step and the pairing honest rather than cosmetic.
  struct Partner {
    std::string suffix;
    ValueRange range;
    double value = 0.0;
    double fallback = 0.0;
    std::function<void(double)> commit;
    std::function<cutline::core::Project(double)> preview;
  };
  std::optional<Partner> partner;

  /// Governed by another control, so the number is shown but cannot be set.
  /// Premiere's Uniform Scale greys Scale Width the same way; ours left both
  /// live and tied, which reads as two independent controls that mysteriously
  /// move together.
  bool governed = false;
  cutline::core::Interp interp = cutline::core::Interp::Linear;
};

/// How many digits after the point a range deserves.
///
/// A stepped range needs exactly its step's precision and nothing more: a
/// control that moves in whole numbers showing a decimal place is a decimal
/// place nobody can put anything in.
///
/// A continuous one always needs at least one, whatever its span. This is the
/// part that is easy to get wrong — none of the transform parameters declares
/// a step, so a rule derived from the nudge gives Opacity no decimals at all,
/// and a scrub that lands on 100.4 then reads as 100. A number that rounds
/// what it is showing is worse than a slider, which at least never claimed to
/// be exact. Two places below a span of 100, where the units are seconds or a
/// speed multiplier and a tenth is a coarse thing to be stuck with.
[[nodiscard]] int decimals_for(const ValueRange& range) {
  if (range.step > 0.0) {
    if (range.step >= 1.0) return 0;
    return range.step >= 0.1 ? 1 : 2;
  }
  return std::abs(range.maximum - range.minimum) >= 100.0 ? 1 : 2;
}

/// What a parameter row can do, in one bundle.
///
/// A struct rather than a list of arguments because only `commit` is always
/// there: a row with nowhere to keep a keyframe — an audio effect's — has no
/// stopwatch, no marker and no chip, and one that changes only the sound has
/// nothing to preview. Naming them at each call site is also what keeps five
/// lambdas in a row readable.
struct ParamHandlers {
  /// Writes the value to the document. The end of a gesture.
  std::function<void(double)> commit;

  /// The project this value *would* make, without applying it.
  ///
  /// Called on every change during a drag, and what the preview renders until
  /// the pointer comes up. Returning the project rather than applying it is the
  /// whole point: one gesture has to be one entry in the undo stack, so
  /// scrubbing Scale from 100 to 180 must be one thing to undo and not eighty.
  ///
  /// Absent for anything that changes no pixels, which is every audio effect.
  std::function<cutline::core::Project(double)> preview;

  std::function<void(bool)> animate;
  std::function<void()> keyframe;
  std::function<void(cutline::core::Interp)> interp;

  /// Moves the playhead to this property's previous (-1) or next (+1)
  /// keyframe. Premiere's ◀ ▶ either side of the marker, and the only way to
  /// *reach* a keyframe from a parameter row — the marker says one is here and
  /// offers no way to get to one that is not.
  std::function<void(int)> step;
};

void build_param_row(App& app, const ParamRow& row, ParamHandlers handlers) {
  auto& head = app.inspector->emplace<Box>(Axis::Horizontal);

  if (row.animatable) {
    auto& watch = head.emplace<IconButton>(
        IconButton::Icon::Stopwatch,
        [animate = std::move(handlers.animate), animated = row.animated] { animate(!animated); });
    watch.set_selected(row.animated);
  }

  // Shown while the pointer is down and thrown away when it comes up. See
  // `ParamHandlers::preview`.
  const auto live = [&app, preview = handlers.preview](double value) {
    if (!preview) return;
    app.live_gesture = true;
    app.live_project = preview(value);
    invalidate_preview(app);
    // The overlay is part of the picture, so it follows too.
    refresh_handles(app);
    // Deliberately not marking the inspector stale: rebuilding it would destroy
    // the control the pointer is still holding.
  };

  // Premiere's disclosure triangle, and the slider behind it. The number is
  // what a parameter is read and set by; the slider is for the rarer case of
  // wanting to sweep it and watch.
  const bool expanded = app.expanded_params.contains(row.key);
  auto& reveal = head.emplace<IconButton>(IconButton::Icon::Disclosure, [&app, key = row.key] {
    if (!app.expanded_params.erase(key)) app.expanded_params.insert(key);
    app.inspector_stale = true;
  });
  reveal.set_selected(expanded);

  auto& label = head.emplace<Label>(row.name);
  label.set_small(true);

  // The value, immediately after the name and before everything else, which is
  // where Premiere puts it and where it is read from.
  auto& number = head.emplace<cutline::ui::NumericField>(row.range, row.value);
  number.set_decimals(decimals_for(row.range));
  number.set_suffix(row.suffix);
  number.set_default_value(row.fallback);
  number.set_on_change(live);
  number.set_on_commit(handlers.commit);
  // Shown but not settable. The value still has to be *readable* — a locked
  // aspect means Scale Y is whatever Scale X is, and hiding it would leave
  // nowhere to read the number the picture is actually at.
  number.set_enabled(!row.governed);

  if (row.partner.has_value()) {
    const ParamRow::Partner& other = *row.partner;
    auto& second = head.emplace<cutline::ui::NumericField>(other.range, other.value);
    second.set_decimals(decimals_for(other.range));
    second.set_suffix(other.suffix);
    second.set_default_value(other.fallback);
    second.set_on_change([&app, preview = other.preview](double value) {
      if (!preview) return;
      app.live_gesture = true;
      app.live_project = preview(value);
      invalidate_preview(app);
      refresh_handles(app);
    });
    second.set_on_commit(other.commit);
  }

  head.emplace<Spacer>();

  // Premiere's reset, a circular arrow at the end of the row. Double-clicking
  // the number does the same thing and always has, but nothing on screen said
  // so — an affordance nobody can see is one nobody uses.
  //
  // Not while the property is animated. Two reasons, and either would do. The
  // room: an animated paired row also carries ◀ ◆ ▶, and with the reset as well
  // it does not fit a narrow panel — the property's own name was squeezed away
  // to nothing. And the meaning: on an animated property this button writes a
  // keyframe at the playhead holding the default, which is not what anybody
  // reads "reset" as. Premiere's reset is per effect, not per keyframe.
  const bool moved = row.value != row.fallback ||
                     (row.partner.has_value() && row.partner->value != row.partner->fallback);
  if (!row.governed && !row.animated && moved) {
    auto& reset = head.emplace<IconButton>(IconButton::Icon::Reset);
    reset.set_name("reset");
    reset.set_on_click([commit = handlers.commit, fallback = row.fallback,
                        partner = row.partner] {
      // Both halves of a paired row. Resetting Position to its default and
      // leaving the other axis where it was is not what the button says.
      if (partner.has_value() && partner->commit) partner->commit(partner->fallback);
      commit(fallback);
    });
  }

  // Only once animation is on. Before that there is no list to add to, and a
  // marker that silently started one would mean the same as the stopwatch.
  if (row.animated) {
    // Premiere's interpolation chip, as a button that cycles rather than a
    // dropdown of three. Three is short enough to walk round, and a dropdown
    // beside a stopwatch and a diamond on a narrow row is a lot of chrome for
    // a setting with three values.
    // Premiere's ◀ ◆ ▶. The marker alone says a keyframe is here; these are
    // what get to one that is not, and without them a keyframe placed off the
    // playhead can only be found by scrubbing until the diamond lights up.
    //
    // All three narrow, the way Premiere draws them: one navigator rather than
    // three controls. Three square buttons here plus a stopwatch, a triangle
    // and two numbers do not fit a parameter row, and what got dropped to make
    // room was the property's own name.
    auto& previous = head.emplace<IconButton>(IconButton::Icon::ArrowLeft);
    previous.set_narrow(true);
    previous.set_enabled(row.has_previous);
    if (handlers.step) previous.set_on_click([step = handlers.step] { step(-1); });

    auto& mark =
        head.emplace<IconButton>(IconButton::Icon::Diamond, std::move(handlers.keyframe));
    mark.set_narrow(true);
    mark.set_selected(row.keyed_here);

    auto& next = head.emplace<IconButton>(IconButton::Icon::ArrowRight);
    next.set_narrow(true);
    next.set_enabled(row.has_next);
    if (handlers.step) next.set_on_click([step = std::move(handlers.step)] { step(1); });
  }

  if (!expanded) return;

  auto& more = app.inspector->emplace<Box>(Axis::Horizontal);

  // The interpolation chip, behind the triangle rather than on the head row.
  //
  // It was on the row until ◀ ▶ joined it there, and the two together left the
  // property's own *name* squeezed to nothing — which `--check` caught and a
  // glance would not have. The row is what a parameter is read by and has to
  // stay legible; the chip is a setting nobody touches twice.
  //
  // Premiere has no chip at all: interpolation there is a right-click on the
  // keyframe, per keyframe. That is where this is going, and the chip goes when
  // it arrives. Until then it is the only way to set a curve at all.
  if (row.animated) {
    more.emplace<Button>(std::string(cutline::editor::interp_name(row.interp)),
                         [interp = std::move(handlers.interp), mode = row.interp] {
                           if (interp) interp(cutline::editor::next_interp(mode));
                         });
  }

  auto& slider = more.emplace<Slider>(row.range, row.value);
  slider.set_default_value(row.fallback);
  // The picture follows the drag; the document is written once, at the end of
  // it. Sweeping a slider and watching nothing happen until the button comes up
  // is the case this control exists for.
  slider.set_on_change(live);
  slider.set_on_commit(std::move(handlers.commit));
}

/// Offers the catalogue, and adds what is chosen.
///
/// On the popup layer rather than in the panel, for the reason every menu is:
/// the inspector is a scroll view, and a list opened at the bottom of it would
/// be clipped to the panel that holds it.
void open_effect_menu(App& app, const std::string& clip_id, const Rect& anchor, bool audio) {
  if (app.main.host == nullptr) return;

  const std::vector<cutline::editor::EffectChoice> choices =
      audio ? cutline::editor::addable_audio_effects() : cutline::editor::addable_effects();
  std::vector<std::string> names;
  names.reserve(choices.size());
  for (const cutline::editor::EffectChoice& choice : choices) names.push_back(choice.name);

  auto list = std::make_unique<MenuList>(std::move(names));
  list->set_on_choose([&app, clip_id, choices, audio](std::size_t index) {
    if (index >= choices.size()) return;

    // The whole selection, exactly as the library's double-click and drop
    // already do. The panel shows one clip because a stack is a clip's, but
    // *adding* one is the same gesture wherever it is reached from, and this
    // was the last place it meant only the clip on show. A clip the effect
    // does not suit is passed over rather than given one it cannot run.
    const auto selection = app.session.selection();
    std::vector<std::string> clip_ids(selection.begin(), selection.end());
    // The panel's clip is what the button belongs to, so it takes the effect
    // even when the selection has moved on beneath it.
    if (std::ranges::find(clip_ids, clip_id) == clip_ids.end()) clip_ids.push_back(clip_id);

    app.session.apply(cutline::editor::add_effect_to(app.session.project(), clip_ids,
                                                     choices[index].type, audio));
    if (app.main.host != nullptr) app.main.host->close_popup();
    app.inspector_stale = true;
    refresh_timeline(app);
    invalidate_preview(app);
  });

  app.main.host->open_popup(std::move(list), anchor);
}

/// Where a dragged effect card would land, by the y it was released at.
///
/// The stack is a column of cards in the inspector, so "which effect is under
/// the pointer" is answered by walking the header rows the panel just built.
/// They are found by name because the panel is rebuilt from nothing on every
/// edit and there is no stable widget to hold on to.
[[nodiscard]] std::optional<std::size_t> effect_row_at(const App& app, double y) {
  if (app.inspector == nullptr) return std::nullopt;

  std::size_t seen = 0;
  std::optional<std::size_t> found;
  for (const auto& child : app.inspector->children()) {
    const auto* row = dynamic_cast<const cutline::ui::GrabRow*>(child.get());
    if (row == nullptr) continue;
    // The nearest header above the pointer, so releasing anywhere over a card —
    // header or parameters — means that card.
    if (row->bounds().y <= y) found = seen;
    ++seen;
  }
  return found;
}

/// Outlines the card a drop would land on. Cleared with no index.
void show_effect_drop(App& app, double y) {
  const std::optional<std::size_t> target = effect_row_at(app, y);
  if (app.effect_drop_target == target) return;
  app.effect_drop_target = target;

  for (std::size_t i = 0; i < app.effect_rows.size(); ++i) {
    app.effect_rows[i]->set_selected(target.has_value() && *target == i);
  }
  mark_dirty(app);
}

/// Moves an effect from `index` to wherever the pointer was let go.
void drop_effect(App& app, std::size_t index, double y) {
  const std::optional<std::size_t> target = effect_row_at(app, y);
  app.effect_drop_target.reset();
  for (cutline::ui::GrabRow* row : app.effect_rows) row->set_selected(false);
  if (!target.has_value() || *target == index) {
    mark_dirty(app);
    return;
  }

  const auto selection = app.session.selection();
  if (selection.empty()) return;
  const std::string clip_id{selection.front()};
  const bool audio = !cutline::editor::clip_audio_effects(app.session.project(), clip_id).empty();

  // Walked one step at a time, against one project, applied once. The core
  // moves an effect by a single place — order affects the render, so each step
  // is a real edit — and a reorder is still one gesture and one undo entry.
  cutline::core::Project next = app.session.project();
  const int direction = *target > index ? 1 : -1;
  for (std::size_t at = index; at != *target; at = static_cast<std::size_t>(
                                    static_cast<std::ptrdiff_t>(at) + direction)) {
    next = audio ? cutline::core::move_audio_effect(std::move(next), clip_id, at, direction)
                 : cutline::core::move_clip_effect(std::move(next), clip_id, at, direction);
  }

  app.session.apply(std::move(next));
  invalidate_preview(app);
  app.inspector_stale = true;
}

/// The row an effect card starts with: its name as an on/off checkbox, then
/// move up, move down and remove.
///
/// Shared by the visual and audio stacks. The two differ in which operations
/// they call and in what a parameter row can do — not in this, and writing it
/// twice is how the two would come to disagree about what a disabled effect
/// looks like.
///
/// Each handler returns the edited project rather than applying it, so the
/// applying, the rebuilding and the invalidating happen once here.
void build_effect_header(App& app, const cutline::editor::EffectRow& row, std::size_t count,
                         std::function<cutline::core::Project()> on_toggle,
                         std::function<cutline::core::Project(int)> on_move,
                         std::function<cutline::core::Project()> on_remove,
                         std::function<cutline::core::Project()> on_reset = {},
                         std::function<void()> on_copy = {}) {
  const auto applied = [&app](cutline::core::Project next) {
    app.session.apply(std::move(next));
    app.inspector_stale = true;
    invalidate_preview(app);
  };

  auto& line = app.inspector->emplace<cutline::ui::GrabRow>(Axis::Horizontal);

  // Dragged up or down to reorder, which is how Premiere does it and what the
  // arrows below are the keyboard-reachable version of.
  //
  // The row is told where the pointer is; which effect that lands on is worked
  // out here, because the row knows nothing of the stack it is in.
  line.set_on_drag([&app](double, double y) { show_effect_drop(app, y); });
  app.effect_rows.push_back(&line);
  line.set_on_drop([&app, index = row.index](double, double y) {
    drop_effect(app, index, y);
  });
  // Everything the card's own controls do, and Copy, which has no control on
  // the card. Premiere's right-click on an effect is the same list.
  line.set_on_context_menu([&app, applied, on_toggle, on_move, on_remove, on_reset, on_copy,
                            enabled = row.enabled, first = row.index == 0,
                            last = row.index + 1 >= count](double x, double y) {
    if (app.main.host == nullptr) return;
    std::vector<MenuEntry> rows;
    if (on_copy) {
      rows.push_back(action_entry("Copy", on_copy));
      rows.push_back(MenuEntry::divider());
    }
    rows.push_back(switch_entry("Effect Enabled", enabled, [applied, on_toggle] {
      applied(on_toggle());
    }));
    if (on_reset) {
      rows.push_back(action_entry("Reset Parameters", [applied, on_reset] { applied(on_reset()); }));
    }
    rows.push_back(MenuEntry::divider());
    rows.push_back(action_entry("Move Up", [applied, on_move] { applied(on_move(-1)); }, {},
                                !first));
    rows.push_back(action_entry("Move Down", [applied, on_move] { applied(on_move(1)); }, {},
                                !last));
    rows.push_back(MenuEntry::divider());
    rows.push_back(action_entry("Clear", [applied, on_remove] { applied(on_remove()); }));
    app.main.host->open_popup(std::make_unique<MenuList>(std::move(rows)), Rect{x, y, 0.0, 0.0});
  });

  // The checkbox is the name as well: a disabled effect stays in the stack and
  // stays visible, which is the whole point of disabling rather than removing.
  auto& on = line.emplace<Checkbox>(row.name, row.enabled);
  on.set_on_change([&app, applied, on_toggle](bool) { applied(on_toggle()); });

  line.emplace<Spacer>();

  // Moving is by button rather than by dragging the row. Dragging is what
  // Premiere does and what this should eventually do; a button is what can be
  // reached from the keyboard and what can be tested without a pointer.
  // Every parameter back to its catalogue default, in one press and one undo
  // entry. Doing it a row at a time is what the per-row buttons are for; an
  // effect somebody has pushed too far wants all of them at once.
  if (on_reset) {
    auto& reset = line.emplace<IconButton>(IconButton::Icon::Reset);
    reset.set_on_click([applied, on_reset] { applied(on_reset()); });
  }

  auto& up = line.emplace<IconButton>(IconButton::Icon::ArrowUp);
  up.set_enabled(row.index > 0);
  up.set_on_click([applied, on_move] { applied(on_move(-1)); });

  auto& down = line.emplace<IconButton>(IconButton::Icon::ArrowDown);
  down.set_enabled(row.index + 1 < count);
  down.set_on_click([applied, on_move] { applied(on_move(1)); });

  line.emplace<IconButton>(IconButton::Icon::Cross,
                           [applied, on_remove] { applied(on_remove()); });
}

/// What the effects library should list, presets included.
[[nodiscard]] std::vector<cutline::ui::EffectEntry> library_entries(const App& app) {
  std::vector<std::string> preset_names;
  for (const cutline::editor::EffectPreset& preset : app.presets.named) {
    preset_names.push_back(preset.name);
  }

  const std::vector<cutline::editor::LibraryEntry> catalogue =
      cutline::editor::effect_library(preset_names);

  std::vector<cutline::ui::EffectEntry> entries;
  // The bins first, so what somebody gathered by hand sits above the catalogue
  // it was gathered from. Their names come from the catalogue, which is what
  // holding an id rather than a copy buys: a renamed preset is renamed here too.
  for (const cutline::editor::LibraryEntry& entry :
       cutline::editor::bin_entries(app.bins, catalogue)) {
    entries.push_back(cutline::ui::EffectEntry{
        .id = entry.id, .name = entry.name, .folder = entry.folder});
  }
  for (const cutline::editor::LibraryEntry& entry : catalogue) {
    entries.push_back(cutline::ui::EffectEntry{
        .id = entry.id, .name = entry.name, .folder = entry.folder});
  }
  return entries;
}

/// Puts the library's list back, which is how a saved preset appears in it.
///
/// Setting the items keeps whichever folders are open — they are remembered by
/// name — so this does not close the tree somebody was working in.
void refresh_library(App& app) {
  if (app.library == nullptr) return;
  app.library->set_items(library_entries(app));
  // And the bins as *folders*, because an empty one has no entries to imply it
  // and a bin you have just made and cannot see looks like one that failed.
  app.library->set_folders(cutline::editor::bin_folders(app.bins));
}

/// Keeps the bins, and says so if it could not.
///
/// Written on every change rather than on exit, like the presets: a folder you
/// spent a morning filling and lost to a crash is worse than one never offered.
void keep_bins(App& app) {
  if (const auto ok =
          cutline::editor::write_bins(cutline::editor::default_bins_path(), app.bins);
      !ok) {
    complain(app.main.window, "Could not save the bins.\n\n" + ok.error());
  }
  refresh_library(app);
}

/// Names what is on a clip and keeps it.
///
/// Written to disk immediately rather than on exit: a preset is worth having
/// because it survives, and one lost to a crash an hour later is worse than one
/// that was never offered.
void save_preset(App& app, const std::string& clip_id, std::string name) {
  if (name.empty()) return;

  if (!cutline::editor::save_preset(app.presets, app.session.project(), clip_id,
                                    std::move(name))) {
    complain(app.main.window, "There is nothing on this clip to save.");
    return;
  }

  if (const auto ok = cutline::editor::write_presets(cutline::editor::default_presets_path(),
                                                     app.presets);
      !ok) {
    complain(app.main.window, "Could not save the preset.\n\n" + ok.error());
  }

  refresh_library(app);
  app.inspector_stale = true;
}

/// The effect stack: a header per effect, its parameters beneath it.
///
/// Copy and paste a whole stack, above whichever stack the clip has.
///
/// One row for both stacks rather than a pair of buttons on each heading. A
/// clipboard holds a *clip's* effects, video and audio together, because an A/V
/// pair is two linked clips in this model — so copying the video half and
/// pasting onto a selection that includes the audio half has to do the right
/// thing with each, and one control is what says that is what happens.
void build_effect_clipboard_row(App& app, const std::string& clip_id) {
  auto& row = app.inspector->emplace<Box>(Axis::Horizontal);
  row.emplace<Label>("Effects").set_bold(true);
  row.emplace<Spacer>();

  row.emplace<Button>("Copy", [&app, clip_id] {
    app.effect_clipboard = cutline::editor::copy_effects(app.session.project(), clip_id);
    // Rebuilt so Paste stops being greyed out. Marked rather than done, since
    // this handler belongs to a button the rebuild destroys.
    app.inspector_stale = true;
  });

  auto& paste = row.emplace<Button>("Paste", [&app] {
    // Onto everything selected, not only the clip the inspector is showing:
    // matching a look across several shots is the reason anybody copies a
    // stack, and doing them one at a time is the thing to avoid.
    const auto selection = app.session.selection();
    app.session.apply(cutline::editor::paste_effects(app.session.project(), selection,
                                                     app.effect_clipboard));
    refresh_timeline(app);
    invalidate_preview(app);
    app.inspector_stale = true;
  });
  // Nothing has been copied yet. A button that silently does nothing is worse
  // than one that says it cannot.
  paste.set_enabled(!app.effect_clipboard.empty());

  auto& clear = row.emplace<Button>("Clear", [&app, clip_id] {
    // The whole selection, because Paste beside it already takes the whole
    // selection and the two are the same gesture in opposite directions:
    // putting a look on several shots, and taking it off again. Clearing one
    // at a time was the fiddle that made stripping a sequence back a chore.
    const auto selection = app.session.selection();
    std::vector<std::string> clip_ids(selection.begin(), selection.end());
    if (std::ranges::find(clip_ids, clip_id) == clip_ids.end()) clip_ids.push_back(clip_id);

    app.session.apply(cutline::editor::clear_effects_on(app.session.project(), clip_ids));
    refresh_timeline(app);
    invalidate_preview(app);
    app.inspector_stale = true;
  });
  const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), clip_id);
  clear.set_enabled(clip != nullptr &&
                    !(clip->effects.empty() && clip->audio_effects.empty()));

  // Saving one, on a row of its own. A name typed in place rather than asked
  // for in a dialog: naming a thing is part of making it, and a modal in the
  // middle of that is a modal in the way.
  auto& saving = app.inspector->emplace<Box>(Axis::Horizontal);
  auto& name = saving.emplace<TextField>(app.preset_name);
  name.set_placeholder("Preset name");
  name.set_on_change([&app](const std::string& text) { app.preset_name = text; });

  auto& save = saving.emplace<Button>("Save");
  // Greyed only for a clip with nothing on it. Deliberately *not* greyed for an
  // empty name: enabling it would need the panel rebuilt on every keystroke,
  // and the rebuild would destroy the field being typed into.
  save.set_enabled(clip != nullptr &&
                   !(clip->effects.empty() && clip->audio_effects.empty()));
  save.set_on_click([&app, clip_id, field = &name] {
    // Read from the control rather than from the copy on `App`, which is only
    // there so the text survives a rebuild.
    const std::string wanted = field->text();
    if (wanted.empty()) return;
    app.preset_name.clear();
    save_preset(app, clip_id, wanted);
  });
}

/// Where one effect applies.
///
/// A mask belongs to a single effect, which is what Premiere means by one and
/// what the flat effect struct could not express at all. The shape chooser is
/// always there; everything else appears only once there is a shape, because a
/// feather on nothing is a control that cannot do anything.
///
/// No overlay on the monitor yet: this is numbers, and dragging the shape on
/// the picture is the obvious next thing and a different piece of work.
void build_effect_mask(App& app, const std::string& clip_id,
                       const cutline::editor::EffectRow& row) {
  const cutline::editor::EffectMaskRow& mask = row.mask;

  auto& line = app.inspector->emplace<Box>(Axis::Horizontal);
  line.emplace<Label>("Mask").set_small(true);
  line.emplace<Spacer>();

  std::vector<std::string> names;
  std::size_t chosen = 0;
  for (const cutline::core::MaskShape shape : cutline::editor::mask_shapes()) {
    if (shape == mask.shape) chosen = names.size();
    names.emplace_back(cutline::editor::mask_shape_name(shape));
  }

  auto& shapes = line.emplace<Dropdown>(std::move(names), chosen);
  shapes.set_on_change([&app, clip_id, index = row.index, current = mask](std::size_t at) {
    const std::span<const cutline::core::MaskShape> offered = cutline::editor::mask_shapes();
    if (at >= offered.size()) return;

    cutline::editor::EffectMaskRow next = current;
    next.shape = offered[at];
    app.session.apply(
        cutline::editor::set_effect_mask(app.session.project(), clip_id, index, next));
    invalidate_preview(app);
    // The shape is drawn on the picture as well as described here, and the
    // monitor is handed its shapes rather than working them out.
    refresh_handles(app);
    app.inspector_stale = true;
  });

  if (mask.shape == cutline::core::MaskShape::None) return;

  // What the pen can do, written down where the shape was chosen. None of it is
  // guessable — a modifier held over a picture leaves no trace until it is
  // tried — and a tool nobody can find is a tool that is not there.
  if (mask.shape == cutline::core::MaskShape::Path) {
    auto& pen_line = app.inspector->emplace<Box>(Axis::Horizontal);
    auto& draw = pen_line.emplace<Button>("Draw Path");
    draw.set_on_click([&app, index = row.index] {
      if (app.monitor == nullptr) return;
      // Which shape on the monitor belongs to this effect. The widget is handed
      // shapes and told nothing about what they are for, so the mapping lives
      // here — the same one every other mask callback uses.
      for (std::size_t at = 0; at < app.mask_effects.size(); ++at) {
        if (app.mask_effects[at] != index) continue;
        app.monitor->begin_mask_drawing(at);
        break;
      }
    });
    pen_line.emplace<Spacer>();

    auto& pen = app.inspector->emplace<Box>(Axis::Vertical);
    pen.emplace<Label>("Draw Path clears the shape: click the picture to place")
        .set_small(true);
    pen.emplace<Label>("points, hold and drag as you click to curve one, and")
        .set_small(true);
    pen.emplace<Label>("click the first point again to close.").set_small(true);
    pen.emplace<Label>("Then: drag a point to move it, double-click to round it")
        .set_small(true);
    pen.emplace<Label>("off or square it up, Ctrl-click the outline to add a")
        .set_small(true);
    pen.emplace<Label>("point, Alt-click one to remove it. Alt while pulling a")
        .set_small(true);
    pen.emplace<Label>("handle breaks the pair, for a corner instead of a curve.")
        .set_small(true);
  }

  // Every number on the mask is an ordinary animatable parameter row, under the
  // reserved keys `core::mask_param_keys` names. That is the whole of mask
  // animation: the stopwatch, the keyframe navigator, the curve picker and the
  // marks on the clip are the ones effect parameters already had, and not one
  // of them had to learn what a mask is.
  //
  // The panel shows fractions of the layer as percentages, so each value is
  // scaled on the way out and back — the model keeps one unit and the screen
  // shows another, exactly as position and scale do.
  for (const cutline::editor::EffectParamRow& param : row.mask_params) {
    const double scale = cutline::editor::mask_param_scale(param.key);
    const cutline::editor::ParamRef ref{.effect = row.index, .key = param.key};
    const std::vector<cutline::core::Keyframe> keys =
        param.animated ? lane_keys(app, clip_id, ref) : std::vector<cutline::core::Keyframe>{};
    const double here = local_playhead(app, clip_id);

    const ParamRow control{
        .name = param.name,
        .suffix = param.suffix,
        // Keyed by the effect's place in the stack too: two effects can both
        // carry a mask, and both would call a row Mask Width.
        .key = "fx." + std::to_string(row.index) + "." + param.key,
        .range = param.range,
        .value = param.value,
        .fallback = param.fallback,
        .animatable = true,
        .animated = param.animated,
        .keyed_here = param.keyed_here,
        .has_previous = cutline::editor::keyframe_before(keys, here).has_value(),
        .has_next = cutline::editor::keyframe_after(keys, here).has_value(),
        .interp = param.interp};

    const auto edited = [&app, clip_id, index = row.index, key = param.key, scale](double set) {
      return cutline::editor::set_effect_parameter(app.session.project(), clip_id, index, key,
                                                   set / scale, local_playhead(app, clip_id));
    };

    build_param_row(
        app, control,
        {.commit =
             [&app, edited](double set) {
               app.session.apply(edited(set));
               invalidate_preview(app);
               // The shape is drawn on the picture as well as described here.
               refresh_handles(app);
               app.inspector_stale = true;
             },
         .preview = edited,
         .animate =
             [&app, clip_id, index = row.index, key = param.key](bool animated) {
               app.session.apply(cutline::editor::set_effect_parameter_animated(
                   app.session.project(), clip_id, index, key, animated,
                   local_playhead(app, clip_id)));
               invalidate_preview(app);
               app.inspector_stale = true;
             },
         .keyframe =
             [&app, clip_id, index = row.index, key = param.key] {
               app.session.apply(cutline::editor::toggle_effect_keyframe(
                   app.session.project(), clip_id, index, key, local_playhead(app, clip_id)));
               refresh_timeline(app);
               invalidate_preview(app);
               app.inspector_stale = true;
             },
         .interp =
             [&app, clip_id, index = row.index, key = param.key](cutline::core::Interp mode) {
               app.session.apply(cutline::editor::set_effect_parameter_interp(
                   app.session.project(), clip_id, index, key, mode));
               invalidate_preview(app);
               app.inspector_stale = true;
             },
         .step = [&app, clip_id, ref](int direction) {
           step_to_keyframe(app, clip_id, ref, direction);
         }});
  }

  auto& inverted = app.inspector->emplace<Checkbox>("Mask Inverted", mask.inverted);
  inverted.set_on_change([&app, clip_id, index = row.index, mask](bool on) {
    cutline::editor::EffectMaskRow next = mask;
    next.inverted = on;
    app.session.apply(
        cutline::editor::set_effect_mask(app.session.project(), clip_id, index, next));
    invalidate_preview(app);
    refresh_handles(app);
    app.inspector_stale = true;
  });
}

/// The order of the rows is the order the effects apply in, which is why moving
/// one is an edit rather than a view preference — a blur before a crop and a
/// blur after it are different pictures.
void build_effect_controls(App& app, const std::string& clip_id) {
  auto& header = app.inspector->emplace<Box>(Axis::Horizontal);
  header.emplace<Label>("Video Effects").set_bold(true);
  header.emplace<Spacer>();

  auto& add = header.emplace<Button>("Add Effect");
  add.set_on_click([&app, clip_id, control = &add] {
    open_effect_menu(app, clip_id, control->bounds(), false);
  });

  const std::vector<cutline::editor::EffectRow> rows =
      cutline::editor::clip_effects(app.session.project(), clip_id,
                                    local_playhead(app, clip_id));
  if (rows.empty()) {
    app.inspector->emplace<Label>("No effects").set_small(true);
    return;
  }

  const std::size_t count = rows.size();
  for (const cutline::editor::EffectRow& row : rows) {
    build_effect_header(
        app, row, count,
        [&app, clip_id, index = row.index] {
          return cutline::core::toggle_clip_effect(app.session.project(), clip_id, index);
        },
        [&app, clip_id, index = row.index](int direction) {
          return cutline::core::move_clip_effect(app.session.project(), clip_id, index,
                                                 direction);
        },
        [&app, clip_id, index = row.index] {
          return cutline::core::remove_clip_effect(app.session.project(), clip_id, index);
        },
        [&app, clip_id, index = row.index] {
          return cutline::editor::reset_effect(app.session.project(), clip_id, index);
        },
        [&app, clip_id, index = row.index] {
          app.effect_clipboard =
              cutline::editor::copy_one_effect(app.session.project(), clip_id, index);
          app.inspector_stale = true;
        });

    if (row.unknown) {
      app.inspector->emplace<Label>("Not available in this version").set_small(true);
      continue;
    }

    for (const cutline::editor::EffectParamRow& param : row.params) {
      if (param.toggle) {
        auto& box = app.inspector->emplace<Checkbox>(param.name, param.value >= 0.5);
        box.set_on_change([&app, clip_id, index = row.index, key = param.key](bool checked) {
          app.session.apply(cutline::core::set_clip_effect_param(
              app.session.project(), clip_id, index, key, checked ? 1.0 : 0.0));
          app.inspector_stale = true;
          invalidate_preview(app);
        });
        continue;
      }

      const cutline::editor::ParamRef ref{.effect = row.index, .key = param.key};
      const std::vector<cutline::core::Keyframe> keys =
          param.animated ? lane_keys(app, clip_id, ref)
                         : std::vector<cutline::core::Keyframe>{};
      const double here = local_playhead(app, clip_id);

      const ParamRow control{
          .name = param.name,
          .suffix = param.suffix,
          .key = "fx." + std::to_string(row.index) + "." + param.key,
          .range = param.range,
          .value = param.value,
          .fallback = param.fallback,
          .animatable = true,
          .animated = param.animated,
          .keyed_here = param.keyed_here,
          .has_previous = cutline::editor::keyframe_before(keys, here).has_value(),
          .has_next = cutline::editor::keyframe_after(keys, here).has_value(),
          .interp = param.interp};

      build_param_row(
          app, control,
          {.commit =
               [&app, clip_id, index = row.index, key = param.key](double value) {
                 app.session.apply(cutline::editor::set_effect_parameter(
                     app.session.project(), clip_id, index, key, value,
                     local_playhead(app, clip_id)));
                 invalidate_preview(app);
                 app.inspector_stale = true;
               },
           .preview =
               [&app, clip_id, index = row.index, key = param.key](double value) {
                 return cutline::editor::set_effect_parameter(
                     app.session.project(), clip_id, index, key, value,
                     local_playhead(app, clip_id));
               },
           .animate =
               [&app, clip_id, index = row.index, key = param.key](bool animated) {
                 app.session.apply(cutline::editor::set_effect_parameter_animated(
                     app.session.project(), clip_id, index, key, animated,
                     local_playhead(app, clip_id)));
                 invalidate_preview(app);
                 app.inspector_stale = true;
               },
           .keyframe =
               [&app, clip_id, index = row.index, key = param.key] {
                 app.session.apply(cutline::editor::toggle_effect_keyframe(
                     app.session.project(), clip_id, index, key, local_playhead(app, clip_id)));
                 refresh_timeline(app);
                 invalidate_preview(app);
                 app.inspector_stale = true;
               },
           .interp =
               [&app, clip_id, index = row.index, key = param.key](cutline::core::Interp mode) {
                 app.session.apply(cutline::editor::set_effect_parameter_interp(
                     app.session.project(), clip_id, index, key, mode));
                 invalidate_preview(app);
                 app.inspector_stale = true;
               },
           .step = [&app, clip_id, ref](int direction) {
             step_to_keyframe(app, clip_id, ref, direction);
           }});
    }

    for (const cutline::editor::EffectColorRow& color : row.colors) {
      auto& colour_line = app.inspector->emplace<Box>(Axis::Horizontal);
      colour_line.emplace<Label>(color.name).set_small(true);

      auto& swatch = colour_line.emplace<ColorSwatch>(parse_color(color.value));
      // No alpha strip. The only effect colour there is is the keyer's, which
      // is matched on hue; a transparency the shader discards is worse than
      // one that was never offered.
      swatch.set_alpha_enabled(false);
      swatch.set_on_commit(
          [&app, clip_id, index = row.index, key = color.key](const Color& picked) {
            app.session.apply(cutline::core::set_clip_effect_color(
                app.session.project(), clip_id, index, key, to_hex(picked)));
            invalidate_preview(app);
            // Deliberately *not* marking the inspector stale. Rebuilding it
            // would destroy the swatch, which closes the picker still open
            // above it — and nothing else in the panel depends on this value.
          });
    }

    build_effect_mask(app, clip_id, row);
  }
}

/// The audio effect stack, for an audio clip.
///
/// Shorter than the visual one by exactly one thing: the audio registry has no
/// colour parameters, so there is no swatch to build. Everything else — the
/// header, the parameter rows, the stopwatch, the keyframe navigator — is the
/// same code over the same structs.
void build_audio_effect_controls(App& app, const std::string& clip_id) {
  auto& header = app.inspector->emplace<Box>(Axis::Horizontal);
  header.emplace<Label>("Audio Effects").set_bold(true);
  header.emplace<Spacer>();

  auto& add = header.emplace<Button>("Add Effect");
  add.set_on_click([&app, clip_id, control = &add] {
    open_effect_menu(app, clip_id, control->bounds(), true);
  });

  const std::vector<cutline::editor::EffectRow> rows = cutline::editor::clip_audio_effects(
      app.session.project(), clip_id, local_playhead(app, clip_id));
  if (rows.empty()) {
    app.inspector->emplace<Label>("No effects").set_small(true);
    return;
  }

  const std::size_t count = rows.size();
  for (const cutline::editor::EffectRow& row : rows) {
    build_effect_header(
        app, row, count,
        [&app, clip_id, index = row.index] {
          return cutline::core::toggle_audio_effect(app.session.project(), clip_id, index);
        },
        [&app, clip_id, index = row.index](int direction) {
          return cutline::core::move_audio_effect(app.session.project(), clip_id, index,
                                                  direction);
        },
        [&app, clip_id, index = row.index] {
          return cutline::core::remove_audio_effect(app.session.project(), clip_id, index);
        },
        [&app, clip_id, index = row.index] {
          return cutline::editor::reset_audio_effect(app.session.project(), clip_id, index);
        },
        [&app, clip_id, index = row.index] {
          app.effect_clipboard =
              cutline::editor::copy_one_effect(app.session.project(), clip_id, index);
          app.inspector_stale = true;
        });

    if (row.unknown) {
      app.inspector->emplace<Label>("Not available in this version").set_small(true);
      continue;
    }

    for (const cutline::editor::EffectParamRow& param : row.params) {
      const cutline::editor::ParamRef ref{
          .effect = row.index, .audio = true, .key = param.key};
      const std::vector<cutline::core::Keyframe> keys =
          param.animated ? lane_keys(app, clip_id, ref)
                         : std::vector<cutline::core::Keyframe>{};
      const double here = local_playhead(app, clip_id);

      const ParamRow control{.name = param.name,
                             .suffix = param.suffix,
                             .key = "afx." + std::to_string(row.index) + "." + param.key,
                             .range = param.range,
                             .value = param.value,
                             .fallback = param.fallback,
                             .animatable = true,
                             .animated = param.animated,
                             .keyed_here = param.keyed_here,
                             .has_previous =
                                 cutline::editor::keyframe_before(keys, here).has_value(),
                             .has_next = cutline::editor::keyframe_after(keys, here).has_value(),
                             .interp = param.interp};

      // No `preview`. A live gesture on this changes the *sound*, and there is
      // no way to hear a value the document does not hold yet — the mixer plays
      // the session's project. Committing on release is the whole gesture.
      build_param_row(
          app, control,
          {.commit =
               [&app, clip_id, index = row.index, key = param.key](double value) {
                 app.session.apply(cutline::editor::set_audio_effect_parameter(
                     app.session.project(), clip_id, index, key, value,
                     local_playhead(app, clip_id)));
                 // No `invalidate_preview`: this changes the sound and not one
                 // pixel. The player notices the project has moved on by
                 // itself, on the next frame.
                 app.inspector_stale = true;
               },
           .animate =
               [&app, clip_id, index = row.index, key = param.key](bool animated) {
                 app.session.apply(cutline::editor::set_audio_effect_parameter_animated(
                     app.session.project(), clip_id, index, key, animated,
                     local_playhead(app, clip_id)));
                 app.inspector_stale = true;
               },
           .keyframe =
               [&app, clip_id, index = row.index, key = param.key] {
                 app.session.apply(cutline::editor::toggle_audio_effect_keyframe(
                     app.session.project(), clip_id, index, key,
                     local_playhead(app, clip_id)));
                 app.inspector_stale = true;
               },
           .interp =
               [&app, clip_id, index = row.index,
                key = param.key](cutline::core::Interp mode) {
                 app.session.apply(cutline::editor::set_audio_effect_parameter_interp(
                     app.session.project(), clip_id, index, key, mode));
                 app.inspector_stale = true;
               },
           .step = [&app, clip_id, ref](int direction) {
             step_to_keyframe(app, clip_id, ref, direction);
           }});
    }
  }
}

/// Every animated property on the clip, laid out in time.
///
/// The other half of Premiere's Effect Controls panel. A parameter row says a
/// keyframe is at the playhead; this says where all of them are, which is what
/// shaping an animation needs — and it is the only place a keyframe can be
/// grabbed and moved, an operation the model has always supported and nothing
/// has ever offered.
///
/// Below the stacks rather than beside them, because the panel is a narrow
/// column. Premiere's is a second pane to the right of the parameters; the same
/// arrangement here would leave both halves too narrow to read.
/// How finely a lane's graph is sampled.
///
/// Enough that an ease reads as a curve rather than as three straight lines,
/// and few enough that a clip with six animated properties is a few hundred
/// line segments rather than a few thousand. The panel is about three hundred
/// pixels wide, so this is roughly one sample per pixel.
constexpr std::size_t kCurveSamples = 256;

void build_keyframe_lanes(App& app, const std::string& clip_id) {
  const cutline::editor::KeyframeModel model =
      cutline::editor::clip_keyframes(app.session.project(), clip_id);
  // Nothing animated, nothing to show. An empty lane view in a column this
  // narrow is a heading and a strip of dead space.
  if (model.empty()) return;

  inspector_heading(app, "Keyframes");

  cutline::ui::KeyframeView::Model shown;
  shown.duration = model.duration;
  shown.lanes.reserve(model.lanes.size());
  for (const cutline::editor::KeyframeLane& lane : model.lanes) {
    std::vector<double> times;
    times.reserve(lane.keys.size());
    for (const cutline::core::Keyframe& key : lane.keys) times.push_back(key.t);

    // The curve, sampled through the model's own evaluator rather than
    // reimplemented in the view. Whatever easing means is decided in one place,
    // and the graph is drawn from the same numbers the renderer uses.
    std::vector<double> curve;
    curve.reserve(kCurveSamples);
    for (std::size_t i = 0; i < kCurveSamples; ++i) {
      const double t = model.duration * static_cast<double>(i) /
                       static_cast<double>(kCurveSamples - 1);
      curve.push_back(cutline::core::eval_keyframes(lane.keys, t));
    }

    // The keyframes' own values and handles, so the graph can draw the bezier
    // controls and a drag can be converted back into the model's units. The
    // view knows what a handle looks like and nothing about what it means.
    std::vector<double> values;
    std::vector<cutline::ui::KeyframeHandles> handles;
    values.reserve(lane.keys.size());
    handles.reserve(lane.keys.size());
    for (const cutline::core::Keyframe& key : lane.keys) {
      values.push_back(key.v);
      handles.push_back(cutline::ui::KeyframeHandles{
          .out_x = key.out_x,
          .out_y = key.out_y,
          .in_x = key.in_x,
          .in_y = key.in_y,
          .bezier = key.e == cutline::core::Interp::Bezier});
    }

    shown.lanes.push_back(cutline::ui::KeyframeView::Lane{.name = lane.name,
                                                          .times = std::move(times),
                                                          .curve = std::move(curve),
                                                          .values = std::move(values),
                                                          .handles = std::move(handles)});
  }

  auto& view = app.inspector->emplace<cutline::ui::KeyframeView>();
  view.set_model(std::move(shown));
  view.set_playhead(local_playhead(app, clip_id));

  // What was selected, put back — matched by name and time, so a keyframe that
  // has moved is still the same keyframe and one that has gone quietly drops
  // out of the selection rather than taking whichever point inherited its
  // index.
  {
    std::vector<cutline::ui::KeyframeHit> picked;
    for (std::size_t lane = 0; lane < model.lanes.size(); ++lane) {
      const cutline::editor::KeyframeLane& row = model.lanes[lane];
      for (std::size_t at = 0; at < row.keys.size(); ++at) {
        const auto wanted = std::pair{row.name, row.keys[at].t};
        if (app.keyframe_selection.contains(wanted)) {
          picked.push_back(cutline::ui::KeyframeHit{.lane = lane, .index = at, .found = true});
        }
      }
    }
    if (!picked.empty()) view.set_selection(std::move(picked));
  }

  // Which graphs were open, put back. The panel is rebuilt from nothing on
  // every edit, so state left in the widget would mean easing a keyframe closed
  // the graph you were easing it in — which is exactly what it did.
  for (const std::string& name : app.expanded_lanes) view.set_expanded(name, true);
  view.set_on_expand([&app](const std::string& name, bool expanded) {
    if (expanded) {
      app.expanded_lanes.insert(name);
    } else {
      app.expanded_lanes.erase(name);
    }
  });

  // The lane order is the view's only handle on which property a row is, so it
  // is captured rather than looked up again: the project can change underneath
  // a drag, and a lane index resolved against a newer model is a different
  // property.
  const std::vector<cutline::editor::KeyframeLane> lanes = model.lanes;

  // Remembered as it changes, so the next rebuild can put it back.
  view.set_on_select([&app, lanes_for_selection = model.lanes, control = &view] {
    app.keyframe_selection.clear();
    for (const cutline::ui::KeyframeHit& hit : control->selection()) {
      if (hit.lane >= lanes_for_selection.size()) continue;
      const cutline::editor::KeyframeLane& row = lanes_for_selection[hit.lane];
      if (hit.index >= row.keys.size()) continue;
      app.keyframe_selection.emplace(row.name, row.keys[hit.index].t);
    }
  });

  view.set_on_scrub([&app, clip_id](double t) {
    const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), clip_id);
    if (clip == nullptr) return;
    app.session.set_playhead(clip->start + t);
    refresh_timeline(app);
    invalidate_preview(app);
    app.inspector_stale = true;
  });

  view.set_on_move([&app, clip_id, lanes, control = &view](std::size_t lane, std::size_t index,
                                                           double to) {
    if (lane >= lanes.size()) return;
    // The same bargain as a scrubbed number: rendered against a copy so the
    // picture follows the drag, and written to the document once, on release.
    app.live_gesture = true;
    app.live_project = cutline::editor::move_keyframe(
        app.session.project(), clip_id, lanes[lane].ref, lanes[lane].keys[index].t, to);
    // And the diamond follows the pointer. Without this the view goes on
    // drawing the keyframe where it was until the button comes up.
    control->nudge(lane, index, to);
    invalidate_preview(app);
    refresh_handles(app);
  });

  // What the right-click menu offers, and what each entry does to one keyframe.
  // Applied across the whole selection in one go, which is the point of being
  // able to select several: ease out of the first, hold through the middle,
  // ease into the last is three menu choices rather than thirty.
  struct KeyframeAction {
    std::string name;
    cutline::core::Interp mode = cutline::core::Interp::Linear;
    bool removes = false;
  };
  static const std::vector<KeyframeAction> kActions{
      {.name = "Linear", .mode = cutline::core::Interp::Linear},
      {.name = "Hold", .mode = cutline::core::Interp::Hold},
      {.name = "Ease", .mode = cutline::core::Interp::Ease},
      {.name = "Delete", .removes = true}};

  // One project, edited once per selected keyframe, applied once. Applying each
  // separately would put one entry in the undo stack per keyframe, and undoing
  // "ease these five" would take five presses.
  const auto apply_to_selection = [&app, clip_id, lanes](const KeyframeAction& action) {
    if (app.keyframes == nullptr) return;
    cutline::core::Project next = app.session.project();
    for (const cutline::ui::KeyframeHit& hit : app.keyframes->selection()) {
      if (!hit.found || hit.lane >= lanes.size()) continue;
      const cutline::editor::KeyframeLane& lane = lanes[hit.lane];
      if (hit.index >= lane.keys.size()) continue;
      const double at = lane.keys[hit.index].t;
      next = action.removes
                 ? cutline::editor::remove_keyframe(std::move(next), clip_id, lane.ref, at)
                 : cutline::editor::set_keyframe_interp(std::move(next), clip_id, lane.ref, at,
                                                        action.mode);
    }
    app.session.apply(std::move(next));
    refresh_timeline(app);
    invalidate_preview(app);
    app.inspector_stale = true;
  };

  app.keyframes = &view;

  view.set_on_context_menu([&app, apply_to_selection](double x, double y) {
    if (app.main.host == nullptr) return;
    std::vector<std::string> names;
    names.reserve(kActions.size());
    for (const KeyframeAction& action : kActions) names.push_back(action.name);

    auto list = std::make_unique<MenuList>(std::move(names));
    list->set_on_choose([&app, apply_to_selection](std::size_t index) {
      if (app.main.host != nullptr) app.main.host->close_popup();
      if (index < kActions.size()) apply_to_selection(kActions[index]);
    });
    // Anchored on the pointer rather than on a control: there is no widget
    // under a right-click to hang a menu from, only a position.
    app.main.host->open_popup(std::move(list), Rect{x, y, 0.0, 0.0});
  });

  view.set_on_delete([apply_to_selection] { apply_to_selection(kActions.back()); });

  // Where the selection is, in terms the editor understands. The view knows
  // lane indices and the editor knows properties, and this is the one place
  // the two meet.
  const auto selected_addresses = [&app, lanes] {
    std::vector<cutline::editor::KeyframeAddress> out;
    if (app.keyframes == nullptr) return out;
    for (const cutline::ui::KeyframeHit& hit : app.keyframes->selection()) {
      if (!hit.found || hit.lane >= lanes.size()) continue;
      if (hit.index >= lanes[hit.lane].keys.size()) continue;
      out.push_back(cutline::editor::KeyframeAddress{.ref = lanes[hit.lane].ref,
                                                     .t = lanes[hit.lane].keys[hit.index].t});
    }
    return out;
  };

  view.set_on_copy([&app, clip_id, selected_addresses] {
    app.keyframe_clipboard =
        cutline::editor::copy_keyframes(app.session.project(), clip_id, selected_addresses());
  });

  view.set_on_paste([&app, clip_id] {
    if (app.keyframe_clipboard.empty()) return;
    // At the playhead, which is where a paste means something: the clipboard
    // holds the shape of the animation, and this is where it goes.
    app.session.apply(cutline::editor::paste_keyframes(
        app.session.project(), clip_id, app.keyframe_clipboard, local_playhead(app, clip_id)));
    refresh_timeline(app);
    invalidate_preview(app);
    app.inspector_stale = true;
  });

  view.set_on_move_commit(
      [&app, clip_id, lanes](std::size_t lane, std::size_t index, double, double to) {
        if (lane >= lanes.size()) return;
        // From the captured list rather than from the view: the view's model
        // has been nudged during the drag and no longer says where the keyframe
        // started, which is what the edit has to find it by.
        app.session.apply(cutline::editor::move_keyframe(
            app.session.project(), clip_id, lanes[lane].ref, lanes[lane].keys[index].t, to));
        refresh_timeline(app);
        invalidate_preview(app);
        app.inspector_stale = true;
      });

  // A handle dragged on the graph. The same bargain as everything else that is
  // dragged here: rendered against a copy while the button is down, written to
  // the document once on release, so the whole gesture is one undo entry.
  const auto handle_side = [](cutline::ui::KeyframeHandle side) {
    return side == cutline::ui::KeyframeHandle::Out ? cutline::editor::HandleSide::Out
                                                    : cutline::editor::HandleSide::In;
  };

  view.set_on_handle([&app, clip_id, lanes, handle_side](std::size_t lane, std::size_t index,
                                                         cutline::ui::KeyframeHandle side,
                                                         double x, double y) {
    if (lane >= lanes.size() || index >= lanes[lane].keys.size()) return;
    app.live_gesture = true;
    app.live_project = cutline::editor::set_keyframe_handle(
        app.session.project(), clip_id, lanes[lane].ref, lanes[lane].keys[index].t,
        handle_side(side), x, y);
    invalidate_preview(app);
    refresh_handles(app);
  });

  view.set_on_handle_commit([&app, clip_id, lanes, handle_side](
                                std::size_t lane, std::size_t index,
                                cutline::ui::KeyframeHandle side, double x, double y) {
    if (lane >= lanes.size() || index >= lanes[lane].keys.size()) return;
    app.session.apply(cutline::editor::set_keyframe_handle(
        app.session.project(), clip_id, lanes[lane].ref, lanes[lane].keys[index].t,
        handle_side(side), x, y));
    refresh_timeline(app);
    invalidate_preview(app);
    app.inspector_stale = true;
  });
}

/// The four the model renders, in the order the dropdown offers them.
constexpr std::array kTransitionKinds{
    cutline::core::TransitionKind::Dissolve, cutline::core::TransitionKind::DipBlack,
    cutline::core::TransitionKind::Push, cutline::core::TransitionKind::Slide};

/// The transition at a clip's out-edge, when there is a cut for one to sit on.
///
/// Only shown when another clip abuts this one. The model would happily keep a
/// transition at the end of a track and the renderer would ignore it, so
/// offering the control there would be a control that does nothing.
void build_transition_controls(App& app, const std::string& clip_id) {
  const cutline::editor::TransitionRow row =
      cutline::editor::clip_transition(app.session.project(), clip_id);
  if (!row.joins) return;

  inspector_heading(app, "Transition");

  // "None" first, so removing one is the same control as choosing one rather
  // than a separate button to take it away.
  std::vector<std::string> names{"None"};
  for (const cutline::core::TransitionKind kind : kTransitionKinds) {
    std::string name{cutline::editor::transition_name(kind)};
    // Said out loud rather than left to be discovered. A dissolve overlaps the
    // two clips, and a clip trimmed to the last frame of its footage has
    // nothing to lend — the slider would move and the picture would not.
    if (row.handles_exhausted && kind != cutline::core::TransitionKind::DipBlack) {
      name += " (no handles)";
    }
    names.push_back(std::move(name));
  }

  std::size_t selected = 0;
  if (row.present) {
    const auto found = std::ranges::find(kTransitionKinds, row.kind);
    if (found != kTransitionKinds.end()) {
      selected = static_cast<std::size_t>(found - kTransitionKinds.begin()) + 1;
    }
  }

  auto& kind_row = app.inspector->emplace<Box>(Axis::Horizontal);
  kind_row.emplace<Label>("Kind").set_small(true);
  auto& choice = kind_row.emplace<Dropdown>(std::move(names), selected);
  choice.set_on_change([&app, clip_id](std::size_t index) {
    const std::optional<cutline::core::TransitionKind> kind =
        index == 0 || index > kTransitionKinds.size()
            ? std::nullopt
            : std::optional(kTransitionKinds[index - 1]);
    // A length of its own when one is first added, and the length it already
    // had when the kind is only being changed.
    const cutline::editor::TransitionRow was =
        cutline::editor::clip_transition(app.session.project(), clip_id);
    const double length =
        was.present && kind.has_value()
            ? was.duration
            : (kind.has_value() ? cutline::editor::default_transition_length(
                                      app.session.project(), clip_id, *kind,
                                      app.settings.transition_length)
                                : 0.0);

    app.session.apply(
        cutline::editor::set_transition(app.session.project(), clip_id, kind, length));
    refresh_timeline(app);
    invalidate_preview(app);
    app.inspector_stale = true;
  });

  if (!row.present || row.longest <= 0.0) return;

  app.inspector->emplace<Label>("Duration").set_small(true);
  auto& length = app.inspector->emplace<Slider>(
      ValueRange{.minimum = 0.0, .maximum = row.longest}, row.duration);
  length.set_on_commit([&app, clip_id, kind = row.kind](double value) {
    app.session.apply(
        cutline::editor::set_transition(app.session.project(), clip_id, kind, value));
    refresh_timeline(app);
    invalidate_preview(app);
    // The model may have clamped it, and dragging to zero removes the
    // transition entirely — which changes what the rest of this section shows.
    app.inspector_stale = true;
  });
}

/// A colour matte's fill, when the selected clip is one.
///
/// Two swatches and an angle, and the second swatch is what turns a flat fill
/// into a gradient — there is no separate switch, because a gradient with one
/// colour is a solid and saying so twice invites the two to disagree.
void build_matte_controls(App& app, const std::string& clip_id,
                          const cutline::editor::MatteFill& fill) {
  inspector_heading(app, "Colour Matte");

  // Every edit starts from what the matte *is now*, read back out of the
  // project, rather than from the value this panel was built with.
  //
  // The swatches deliberately do not rebuild the panel — rebuilding destroys
  // the swatch, and a destroyed swatch closes the picker hanging open above it
  // — so a captured copy goes stale the moment anything is changed, and the
  // next edit writes it back over the top. Setting a colour and then turning on
  // Gradient put the old colour back; setting the second colour then put the
  // first one back again.
  const auto change = [&app, clip_id](const auto& edit, bool rebuild_panel) {
    const cutline::core::Clip* clip =
        cutline::core::find_clip(app.session.project(), clip_id);
    if (clip == nullptr) return;
    std::optional<cutline::editor::MatteFill> now =
        cutline::editor::clip_matte_fill(app.session.project(), clip_id);
    if (!now.has_value()) return;

    // Copied, not pointed at. Applying replaces the whole project, and `clip`
    // points into the one being replaced.
    const std::string media_id = clip->media_id;

    edit(*now);
    app.session.apply(
        cutline::editor::set_matte_fill(app.session.project(), media_id, *std::move(now)));
    refresh_browser(app);
    refresh_timeline(app);
    invalidate_preview(app);
    if (rebuild_panel) {
      app.inspector_stale = true;
    } else if (app.clip_heading != nullptr) {
      // The panel is not being rebuilt, so the heading has to be told: a
      // matte's name follows its colour, and the pool and the timeline have
      // both just been given the new one.
      const auto media = std::ranges::find(app.session.project().media, media_id,
                                           &cutline::core::Media::id);
      if (media != app.session.project().media.end()) {
        app.clip_heading->set_text(media->name);
      }
    }
  };

  auto& colour_row = app.inspector->emplace<Box>(Axis::Horizontal);
  colour_row.emplace<Label>("Colour").set_small(true);
  auto& colour = colour_row.emplace<ColorSwatch>(parse_color(fill.color));
  colour.set_on_commit([change](const Color& picked) {
    change([&picked](cutline::editor::MatteFill& fill) { fill.color = to_hex(picked); }, false);
  });

  auto& ramp = app.inspector->emplace<Box>(Axis::Horizontal);
  auto& gradient = ramp.emplace<Checkbox>("Gradient", fill.gradient.has_value());
  gradient.set_on_change([change](bool on) {
    change(
        [on](cutline::editor::MatteFill& fill) {
          if (!on) {
            fill.gradient.reset();
            return;
          }
          // Black by default, which is the one second colour that reads as a
          // ramp whatever the first one is. Turned on twice — which the panel
          // can do, since a rebuild sets the checkbox from the model — the one
          // already there is kept rather than reset to black.
          if (!fill.gradient.has_value()) {
            fill.gradient = cutline::core::MatteGradient{.color2 = "#000000", .angle = 0.0};
          }
        },
        // This one does rebuild: turning it on adds two controls below.
        true);
  });
  ramp.emplace<Spacer>();

  if (!fill.gradient.has_value()) return;

  auto& second_row = app.inspector->emplace<Box>(Axis::Horizontal);
  second_row.emplace<Label>("To").set_small(true);
  auto& second = second_row.emplace<ColorSwatch>(parse_color(fill.gradient->color2));
  second.set_on_commit([change](const Color& picked) {
    change(
        [&picked](cutline::editor::MatteFill& fill) {
          // Only when it is still a ramp. The checkbox above could have been
          // turned off while the picker was open, and a second colour on a flat
          // fill would turn it back into a gradient nobody asked for.
          if (fill.gradient.has_value()) fill.gradient->color2 = to_hex(picked);
        },
        false);
  });

  app.inspector->emplace<Label>("Angle (deg)").set_small(true);
  auto& angle = app.inspector->emplace<Slider>(ValueRange{.minimum = 0.0, .maximum = 360.0},
                                               fill.gradient->angle);
  angle.set_on_commit([change](double value) {
    change(
        [value](cutline::editor::MatteFill& fill) {
          if (fill.gradient.has_value()) fill.gradient->angle = value;
        },
        false);
  });
}

/// A title's own text and styling, when the selected clip is one.
///
/// Above Motion rather than below the effects: what a title *says* is the first
/// thing anyone wants to change about it, and Premiere puts the same controls at
/// the top of its own panel.
void build_title_controls(App& app, const std::string& clip_id,
                          const cutline::core::TextSpec& spec) {
  inspector_heading(app, "Text");

  // Written to the document when the field is done with — Enter, or the
  // keyboard leaving. On every keystroke it would be one undo entry per letter.
  //
  // `rebuild_panel` is off for the colour swatches only. Rebuilding destroys
  // the swatch, and a destroyed swatch closes the picker hanging open above it,
  // so a colour could only ever be chosen once per opening.
  const auto write = [&app, clip_id](cutline::core::TextSpec changed,
                                     bool rebuild_panel = true) {
    const cutline::core::Clip* clip =
        cutline::core::find_clip(app.session.project(), clip_id);
    if (clip == nullptr) return;
    app.session.apply(cutline::editor::set_title_spec(app.session.project(), clip->media_id,
                                                      std::move(changed)));
    refresh_browser(app);
    refresh_timeline(app);
    invalidate_preview(app);
    if (rebuild_panel) app.inspector_stale = true;
  };

  auto& content = app.inspector->emplace<TextField>(spec.content);
  content.set_multiline(true);
  content.set_min_lines(2);
  content.set_placeholder("Title text");
  content.set_on_commit([write, spec](const std::string& text) {
    cutline::core::TextSpec changed = spec;
    changed.content = text;
    write(std::move(changed));
  });

  app.inspector->emplace<Label>("Size").set_small(true);
  auto& size = app.inspector->emplace<Slider>(ValueRange{.minimum = 8.0, .maximum = 400.0},
                                              spec.font_size);
  size.set_default_value(96.0);
  size.set_on_commit([write, spec](double value) {
    cutline::core::TextSpec changed = spec;
    changed.font_size = value;
    write(std::move(changed));
  });

  auto& colour_row = app.inspector->emplace<Box>(Axis::Horizontal);
  colour_row.emplace<Label>("Colour").set_small(true);
  auto& colour =
      colour_row.emplace<ColorSwatch>(parse_color(spec.color, Color{1.0f, 1.0f, 1.0f, 1.0f}));
  colour.set_on_commit([write, spec](const Color& picked) {
    cutline::core::TextSpec changed = spec;
    changed.color = to_hex(picked);
    write(std::move(changed), false);
  });

  auto& style = app.inspector->emplace<Box>(Axis::Horizontal);
  auto& bold = style.emplace<Checkbox>("Bold", spec.bold);
  bold.set_on_change([write, spec](bool on) {
    cutline::core::TextSpec changed = spec;
    changed.bold = on;
    write(std::move(changed));
  });
  auto& italic = style.emplace<Checkbox>("Italic", spec.italic);
  italic.set_on_change([write, spec](bool on) {
    cutline::core::TextSpec changed = spec;
    changed.italic = on;
    write(std::move(changed));
  });
  style.emplace<Spacer>();

  auto& decoration = app.inspector->emplace<Box>(Axis::Horizontal);
  auto& shadow = decoration.emplace<Checkbox>("Shadow", spec.shadow);
  shadow.set_on_change([write, spec](bool on) {
    cutline::core::TextSpec changed = spec;
    changed.shadow = on;
    write(std::move(changed));
  });
  // An outline is a colour and a width, and either one alone does nothing. One
  // checkbox turns both on, at a width that can be seen.
  auto& outline = decoration.emplace<Checkbox>("Outline", spec.stroke_color.has_value());
  outline.set_on_change([write, spec](bool on) {
    cutline::core::TextSpec changed = spec;
    if (on) {
      changed.stroke_color = "#000000";
      if (changed.stroke_width <= 0.0) changed.stroke_width = 3.0;
    } else {
      changed.stroke_color.reset();
    }
    write(std::move(changed));
  });
  decoration.emplace<Spacer>();

  // Only when there is an outline to colour. A swatch for a property that is
  // switched off is a control that does nothing, which is worse than a control
  // that is not there.
  if (spec.stroke_color.has_value()) {
    auto& stroke_row = app.inspector->emplace<Box>(Axis::Horizontal);
    stroke_row.emplace<Label>("Outline colour").set_small(true);
    auto& stroke = stroke_row.emplace<ColorSwatch>(parse_color(*spec.stroke_color));
    stroke.set_on_commit([write, spec](const Color& picked) {
      cutline::core::TextSpec changed = spec;
      changed.stroke_color = to_hex(picked);
      write(std::move(changed), false);
    });
  }

  auto& align_row = app.inspector->emplace<Box>(Axis::Horizontal);
  align_row.emplace<Label>("Align").set_small(true);
  constexpr std::array kAligns{cutline::core::TextAlign::Left, cutline::core::TextAlign::Center,
                               cutline::core::TextAlign::Right};
  const auto found = std::ranges::find(kAligns, spec.align);
  auto& align = align_row.emplace<Dropdown>(
      std::vector<std::string>{"Left", "Centre", "Right"},
      found == kAligns.end() ? 1u : static_cast<std::size_t>(found - kAligns.begin()));
  align.set_on_change([write, spec](std::size_t index) {
    cutline::core::TextSpec changed = spec;
    changed.align = index < kAligns.size() ? kAligns[index] : cutline::core::TextAlign::Center;
    write(std::move(changed));
  });
}

/// Rebuilds the inspector for whatever is selected.
///
/// A loop over `clip_parameters` rather than a hand-built form, so a property
/// added to the model turns up here without anyone remembering to come and add
/// a control for it.
void refresh_inspector(App& app) {
  if (app.inspector == nullptr || app.main.host == nullptr) return;

  // Rebuilt from nothing each time. `clear_children` tells the host to drop
  // hover, focus and capture first, so a slider that was mid-drag when the
  // selection changed is not freed underneath the drag.
  app.keyframes = nullptr;
  app.effect_rows.clear();
  app.effect_drop_target.reset();
  app.inspector->clear_children();

  const auto selection = app.session.selection();
  if (selection.empty()) {
    app.inspector->emplace<Label>("Nothing selected").set_small(true);
    app.inspector->emplace<Spacer>();
    app.main.host->request_layout();
    mark_dirty(app);
    return;
  }

  const std::string clip_id{selection.front()};
  const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), clip_id);
  const bool visual = clip != nullptr && clip->kind == cutline::core::TrackKind::Video;

  // A title's words come first: it is the thing anyone wants to change about a
  // title, and everything below is the same for every clip.
  if (const cutline::core::TextSpec* spec =
          cutline::editor::clip_title_spec(app.session.project(), clip_id);
      spec != nullptr) {
    build_title_controls(app, clip_id, *spec);
  } else if (const std::optional<cutline::editor::MatteFill> fill =
                 cutline::editor::clip_matte_fill(app.session.project(), clip_id);
             fill.has_value()) {
    build_matte_controls(app, clip_id, *fill);
  } else if (cutline::editor::clip_is_adjustment(app.session.project(), clip_id)) {
    // Said out loud. An adjustment layer draws nothing of its own, and a clip
    // showing black where a picture should be looks like one that is broken
    // rather than one that is working exactly as intended.
    inspector_heading(app, "Adjustment Layer");
    app.inspector->emplace<Label>("Its effects apply to everything below it.").set_small(true);
  }

  // Which clip this is. Premiere titles the panel with the clip and the
  // sequence, and with a timeline full of similar takes it is the only thing
  // saying whether the numbers below belong to the one you meant.
  {
    const auto media = std::ranges::find(app.session.project().media,
                                         clip == nullptr ? std::string{} : clip->media_id,
                                         &cutline::core::Media::id);
    auto& name = app.inspector->emplace<Label>(
        media == app.session.project().media.end() ? clip_id : media->name);
    name.set_bold(true);
    // Held, because a matte's name follows its colour and the swatches
    // deliberately do not rebuild this panel — a rebuild would destroy the
    // swatch and close the picker hanging open above it. Without this the
    // heading keeps the colour the clip used to be.
    app.clip_heading = &name;
  }

  // Premiere's own heading for the built-in transform, and the honest one for
  // an audio clip, which has no geometry to move.
  inspector_heading(app, visual ? "Motion" : "Audio");

  // Premiere's "Uniform Scale", beside the rows it governs. Only for a clip
  // that has geometry — an audio clip has no shape to keep.
  if (visual) {
    auto& lock = app.inspector->emplace<Checkbox>("Lock aspect", app.aspect_locked);
    lock.set_on_change([&app](bool locked) {
      app.aspect_locked = locked;
      if (app.monitor != nullptr) app.monitor->set_aspect_locked(locked);
      save_settings(app);
      mark_dirty(app);
    });
  }

  const std::vector<cutline::editor::ParamSpec> specs =
      cutline::editor::clip_parameters(app.session.project(), clip_id,
                                       local_playhead(app, clip_id));

  for (const cutline::editor::ParamSpec& spec : specs) {
    // The second half of a pair has no row of its own: it rides on the first
    // half's, the way Premiere's Position and Anchor Point are each one
    // property with two components.
    if (is_paired_follower(spec.param)) continue;

    const cutline::editor::ParamRef ref{.param = spec.param};
    const std::vector<cutline::core::Keyframe> keys =
        spec.animated ? lane_keys(app, clip_id, ref) : std::vector<cutline::core::Keyframe>{};
    const double here = local_playhead(app, clip_id);

    // The paired half, when there is one.
    const std::optional<cutline::editor::ClipParam> mate = paired_axis(spec.param);
    std::optional<ParamRow::Partner> partner;
    if (mate.has_value()) {
      const auto found =
          std::ranges::find(specs, *mate, &cutline::editor::ParamSpec::param);
      if (found != specs.end()) {
        partner = ParamRow::Partner{
            .suffix = found->suffix,
            .range = found->range,
            .value = found->value,
            .fallback = found->fallback,
            .commit =
                [&app, clip_id, mate = *mate](double value) {
                  app.session.apply(cutline::editor::set_clip_parameter(
                      app.session.project(), clip_id, mate, value,
                      local_playhead(app, clip_id)));
                  refresh_timeline(app);
                  invalidate_preview(app);
                  app.inspector_stale = true;
                },
            .preview =
                [&app, clip_id, mate = *mate](double value) {
                  return cutline::editor::set_clip_parameter(
                      app.session.project(), clip_id, mate, value,
                      local_playhead(app, clip_id));
                }};
      }
    }
    // Only once the other half was actually found: a pair with a missing mate
    // is a single row, and the callbacks below must not reach for it.
    const std::optional<cutline::editor::ClipParam> paired =
        partner.has_value() ? mate : std::nullopt;

    const ParamRow line{// "Position" rather than "Position X", now that both
                        // numbers are on the row.
                        .name = partner.has_value() ? std::string(pair_name(spec.param))
                                                    : spec.name,
                        // The unit, on every one of these. It was left off
                        // originally on the grounds that the names carried it,
                        // which was never true — "Scale X" does not say percent
                        // and "Position X" does not say percent of the canvas.
                        // Pairing Position made it visible: one half showed a
                        // unit and the other did not.
                        .suffix = spec.suffix,
                        .key = "motion." + spec.name,
                        .range = spec.range,
                        .value = spec.value,
                        .fallback = spec.fallback,
                        .animatable = spec.animatable,
                        .animated = spec.animated,
                        .keyed_here = spec.keyed_here,
                        .has_previous = cutline::editor::keyframe_before(keys, here).has_value(),
                        .has_next = cutline::editor::keyframe_after(keys, here).has_value(),
                        // Premiere's Uniform Scale greys Scale Width. Scale Y
                        // is the follower here: X is the one the lock is set
                        // from, and Y is what it writes.
                        .partner = std::move(partner),
                        .governed = app.aspect_locked &&
                                    spec.param == cutline::editor::ClipParam::ScaleY,
                        .interp = spec.interp};

    // The edit itself, without applying it, so the commit and the live preview
    // cannot drift apart about what a value means.
    //
    // With the lock on, the other axis follows. Applied here rather than in the
    // binding because the lock is a property of the controls, and an edit made
    // from a script or a monitor drag should not have a checkbox somewhere else
    // deciding what it meant.
    const auto edited = [&app, clip_id, param = spec.param](double value) {
      cutline::core::Project next = cutline::editor::set_clip_parameter(
          app.session.project(), clip_id, param, value, local_playhead(app, clip_id));
      if (app.aspect_locked) {
        const auto tied = other_scale_axis(param);
        if (tied.has_value()) {
          next = cutline::editor::set_clip_parameter(std::move(next), clip_id, *tied, value,
                                                     local_playhead(app, clip_id));
        }
      }
      return next;
    };

    build_param_row(
        app, line,
        {.commit =
             [&app, edited](double value) {
               app.session.apply(edited(value));
               refresh_timeline(app);
               invalidate_preview(app);
               // Marked, not rebuilt: this lambda belongs to the control that a
               // rebuild would destroy. It has to be rebuilt though — the model
               // may have clamped the value, and a speed change alters the
               // clip's length, which moves both fade controls' bounds.
               app.inspector_stale = true;
             },
         .preview = edited,
         // Both axes, on a paired row. One stopwatch that animated only X
         // would leave Position half-animated, which the lane view would then
         // honestly show as one lane where the row promised two.
         .animate =
             [&app, clip_id, param = spec.param, paired](bool animated) {
               const double here = local_playhead(app, clip_id);
               cutline::core::Project next = cutline::editor::set_clip_parameter_animated(
                   app.session.project(), clip_id, param, animated, here);
               if (paired.has_value()) {
                 next = cutline::editor::set_clip_parameter_animated(
                     std::move(next), clip_id, *paired, animated, here);
               }
               app.session.apply(std::move(next));
               refresh_timeline(app);
               invalidate_preview(app);
               app.inspector_stale = true;
             },
         .keyframe =
             [&app, clip_id, param = spec.param, paired] {
               const double here = local_playhead(app, clip_id);
               cutline::core::Project next = cutline::editor::toggle_clip_parameter_keyframe(
                   app.session.project(), clip_id, param, here);
               if (paired.has_value()) {
                 next = cutline::editor::toggle_clip_parameter_keyframe(
                     std::move(next), clip_id, *paired, here);
               }
               app.session.apply(std::move(next));
               refresh_timeline(app);
               invalidate_preview(app);
               app.inspector_stale = true;
             },
         .interp =
             [&app, clip_id, param = spec.param, paired](cutline::core::Interp mode) {
               cutline::core::Project next = cutline::editor::set_clip_parameter_interp(
                   app.session.project(), clip_id, param, mode);
               if (paired.has_value()) {
                 next = cutline::editor::set_clip_parameter_interp(
                     std::move(next), clip_id, *paired, mode);
               }
               app.session.apply(std::move(next));
               invalidate_preview(app);
               app.inspector_stale = true;
             },
         .step = [&app, clip_id, ref](int direction) {
           step_to_keyframe(app, clip_id, ref, direction);
         }});

    // Reverse belongs beside Speed and nowhere else: they are one operation in
    // the model — `set_clip_speed` takes both — and a clip played backwards at
    // half rate is one retime rather than two.
    if (spec.param == cutline::editor::ClipParam::Speed && clip != nullptr) {
      auto& backwards = app.inspector->emplace<Checkbox>("Reverse", clip->reverse);
      backwards.set_on_change([&app, clip_id](bool on) {
        const cutline::core::Clip* now =
            cutline::core::find_clip(app.session.project(), clip_id);
        if (now == nullptr) return;
        app.session.apply(
            cutline::core::set_clip_speed(app.session.project(), clip_id, now->speed, on));
        refresh_timeline(app);
        invalidate_preview(app);
        app.inspector_stale = true;
      });
    }
  }

  // Premiere's Composite, under the transform it composites. Only for a clip
  // with a picture: a blend mode on a waveform means nothing, and the
  // compositor never asks about one.
  if (visual) {
    const std::span<const cutline::core::BlendMode> modes = cutline::editor::blend_modes();
    std::vector<std::string> names;
    names.reserve(modes.size());
    std::size_t current = 0;
    for (std::size_t i = 0; i < modes.size(); ++i) {
      names.emplace_back(cutline::editor::blend_name(modes[i]));
      if (clip != nullptr && clip->blend == modes[i]) current = i;
    }

    auto& line = app.inspector->emplace<Box>(Axis::Horizontal);
    line.emplace<Label>("Blend").set_small(true);
    auto& choice = line.emplace<Dropdown>(std::move(names), current);
    choice.set_on_change([&app, clip_id, modes](std::size_t index) {
      if (index >= modes.size()) return;
      app.session.apply(
          cutline::core::set_clip_blend(app.session.project(), clip_id, modes[index]));
      invalidate_preview(app);
      app.inspector_stale = true;
    });
  }

  // Below Motion and above the effects: a transition belongs to the cut rather
  // than to the clip, and grouping it with the effect stack would suggest it
  // stacks with them.
  if (visual) build_transition_controls(app, clip_id);

  build_effect_clipboard_row(app, clip_id);

  // One stack or the other, never both: a clip is video or audio, and offering
  // a blur on a waveform would be a control with nothing behind it.
  if (visual) {
    build_effect_controls(app, clip_id);
  } else {
    build_audio_effect_controls(app, clip_id);
  }

  // Last, because it is about everything above it: the transform's animation
  // and the effect stack's, on one axis.
  build_keyframe_lanes(app, clip_id);

  app.inspector->emplace<Spacer>();

  app.main.host->request_layout();
  mark_dirty(app);
}

}  // namespace cutline::tool
