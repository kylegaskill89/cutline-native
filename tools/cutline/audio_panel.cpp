/// The Audio Master panel and the audio dialogs.
///
/// The mixer -- a strip per track with its fader, pan, meter, effects, sends
/// and automation -- and the dialogs that work on a clip's sound: loudness,
/// channels and Essential Sound.

#include "app.hpp"

namespace cutline::tool {

/// Measure the programme, then move the master fader to a delivery target.
///
/// The measurement is the slow half and it is done when the box opens rather
/// than when OK is pressed: a number nobody has seen is not a thing anybody can
/// decide about, and "how loud is this" is most of what somebody opening this
/// wants to know even when they change nothing.
void open_loudness_dialog(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.main.host == nullptr) return;

  const auto measured =
      cutline::engine::AudioMixer::measure_loudness(app.session.project());
  if (!measured.has_value()) {
    complain(app.main.window, "Could not measure this sequence.\n\n" + measured.error());
    return;
  }

  auto dialog = std::make_unique<cutline::ui::Dialog>("Normalise Loudness");
  Box* panel = &dialog->body();

  const bool measurable = *measured > cutline::audio::kAbsoluteGateLufs;
  panel->emplace<Label>(measurable ? std::format("This sequence measures {:.1f} LUFS.", *measured)
                                   : "This sequence has no audio to measure.")
      .set_small(true);

  // The delivery numbers people are actually given, named by where they come
  // from. A field to type a number into would be more general and less useful:
  // nobody invents a loudness target, they are handed one.
  struct Target {
    std::string_view label;
    double lufs;
  };
  static constexpr std::array<Target, 4> kTargets{{
      {"-14 LUFS — streaming (Spotify, YouTube)", -14.0},
      {"-16 LUFS — podcast (Apple)", -16.0},
      {"-23 LUFS — broadcast (EBU R128)", -23.0},
      {"-24 LUFS — broadcast (ATSC A/85)", -24.0},
  }};

  std::vector<std::string> names;
  names.reserve(kTargets.size());
  for (const Target& target : kTargets) names.emplace_back(target.label);
  auto& group = panel->emplace<cutline::ui::RadioGroup>(std::move(names), 0u);

  dialog->set_accept("Normalise",
                     [&app, chosen = &group, at = *measured]() -> std::optional<std::string> {
    const std::size_t index = chosen->selected();
    if (index >= kTargets.size()) return std::nullopt;

    // The offset goes on the *master fader*, which is where a programme's level
    // belongs: it moves everything together and leaves every clip and track
    // exactly as it was set, so normalising is one number to undo rather than a
    // change spread across the whole sequence.
    const double offset = cutline::audio::loudness_offset_db(at, kTargets[index].lufs);
    if (offset == 0.0) return std::nullopt;

    const double now = app.session.project().sequence().master_gain;
    const double moved = now * std::pow(10.0, offset / 20.0);
    app.session.apply(cutline::core::set_master_gain(app.session.project(), moved));
    show_master_gain(app, app.session.project().sequence().master_gain);
    refresh_all(app);
    stop_playback(app);
    mark_dirty(app);
    return std::nullopt;
  });
  auto& ok = dialog->accept_button();
  // Nothing to normalise, so the button says so rather than doing nothing.
  ok.set_enabled(measurable);

  app.main.host->open_popup(std::move(dialog), settings_anchor());
#else
  complain(app.main.window, "This build has no media layer, so there is nothing to measure.");
#endif
}

/// Premiere's Audio Channels: which source channel feeds each output channel.
///
/// The presets are the shoot, not the arithmetic. "Channel 1 to both" is what
/// somebody means when they say the lapel was on the left; nobody thinks of it
/// as a permutation, and offering the permutation instead is how a dialogue
/// ends up correct and unusable.
void open_channel_dialog(App& app, std::span<const std::string> clips) {
  if (app.main.host == nullptr || clips.empty()) return;

  struct Choice {
    std::string_view label;
    std::vector<int> map;
  };
  const std::vector<Choice> kChoices{
      {"Stereo — as recorded", {}},
      {"Channel 1 to both (mono)", {0, 0}},
      {"Channel 2 to both (mono)", {1, 1}},
      {"Swap left and right", {1, 0}},
      {"Left only, right silent", {0, -1}},
      {"Right only, left silent", {-1, 1}},
  };

  // What the selection is already set to, so the box opens on the answer it
  // currently has rather than on the first row.
  std::size_t current = 0;
  if (const cutline::core::Clip* first =
          cutline::core::find_clip(app.session.project(), clips.front());
      first != nullptr) {
    for (std::size_t i = 0; i < kChoices.size(); ++i) {
      if (kChoices[i].map == first->channel_map) {
        current = i;
        break;
      }
    }
  }

  auto dialog = std::make_unique<cutline::ui::Dialog>("Audio Channels");
  Box* panel = &dialog->body();

  std::vector<std::string> names;
  names.reserve(kChoices.size());
  for (const Choice& choice : kChoices) names.emplace_back(choice.label);
  auto& group = panel->emplace<cutline::ui::RadioGroup>(std::move(names), current);

  dialog->set_accept("OK", [&app, chosen = &group, kChoices,
                            ids = std::vector<std::string>(clips.begin(), clips.end())]()
                               -> std::optional<std::string> {
        const std::size_t index = chosen->selected();
        if (index >= kChoices.size()) return std::nullopt;

        // One project, edited once per clip, applied once — so mapping six
        // takes is one press of undo rather than six.
        cutline::core::Project next = app.session.project();
        for (const std::string& id : ids) {
          next = cutline::core::set_clip_channel_map(std::move(next), id, kChoices[index].map);
        }
        app.session.apply(std::move(next));
        refresh_all(app);
        invalidate_preview(app);
        // The mix is built from the document, so a channel change is heard
        // only once the player is rebuilt.
        stop_playback(app);
        mark_dirty(app);
    return std::nullopt;
  });

  app.main.host->open_popup(std::move(dialog), settings_anchor());
}

/// Premiere's Essential Sound: what a clip is, and the two things that follow
/// from saying so.
///
/// A role on its own is a label. What makes it worth setting is that something
/// reads it — a preset that knows roughly what a voice needs, and ducking that
/// knows what has to get out of the way of what — so both are in this box
/// rather than scattered through menus that would each need the role explained
/// again.
///
/// Ducking is offered whatever the role, and not only under Music. It is nearly
/// always music that ducks, and "nearly" is doing real work there: a wind bed
/// under an interview wants exactly the same treatment, and a box that decided
/// otherwise would be one to argue with rather than use.
void open_essential_sound(App& app, std::span<const std::string> clips) {
  if (app.main.host == nullptr || clips.empty()) return;

  // What the selection already says, so the box opens on the answer it has.
  // The first clip's, because a mixed selection has no single answer and
  // showing the first is at least showing one of them.
  cutline::core::AudioRole current = cutline::core::AudioRole::None;
  if (const cutline::core::Clip* first =
          cutline::core::find_clip(app.session.project(), clips.front());
      first != nullptr) {
    current = first->role;
  }
  const auto at_role = std::ranges::find(cutline::core::kAudioRoles, current);
  const auto selected_role = at_role == cutline::core::kAudioRoles.end()
                                 ? 0u
                                 : static_cast<std::size_t>(
                                       at_role - cutline::core::kAudioRoles.begin());

  auto dialog = std::make_unique<cutline::ui::Dialog>("Essential Sound");
  Box* panel = &dialog->body();

  std::vector<std::string> names;
  names.reserve(cutline::core::kAudioRoles.size());
  for (const cutline::core::AudioRole role : cutline::core::kAudioRoles) {
    names.emplace_back(cutline::core::role_name(role));
  }
  auto& group = panel->emplace<cutline::ui::RadioGroup>(std::move(names), selected_role);

  auto& preset = panel->emplace<Checkbox>("Apply the preset for this role", true);

  panel->emplace<Label>("Ducking").set_bold(true);
  const auto number = [&panel](const char* label, cutline::ui::ValueRange range, double value,
                               const char* unit) -> cutline::ui::NumericField& {
    auto& row = panel->emplace<Box>(Axis::Horizontal);
    row.emplace<Label>(label).set_small(true);
    row.emplace<Spacer>();
    auto& field = row.emplace<cutline::ui::NumericField>(range, value);
    field.set_decimals(1);
    field.set_default_value(value);
    field.set_suffix(unit);
    return field;
  };

  const cutline::core::DuckSettings defaults;
  auto& amount = number("Duck by", {.minimum = -60.0, .maximum = 0.0}, defaults.amount_db, "dB");
  auto& fade = number("Fade", {.minimum = 0.0, .maximum = 5.0}, defaults.fade, "s");
  auto& position = number("Position", {.minimum = -5.0, .maximum = 5.0}, defaults.position, "s");

  const std::vector<std::string> ids(clips.begin(), clips.end());

  auto& duck = panel->emplace<Button>("Generate Keyframes");
  duck.set_on_click([&app, ids, chosen = &group, amount = &amount, fade = &fade,
                     position = &position] {
    const cutline::core::DuckSettings settings{
        .amount_db = amount->value(),
        .fade = fade->value(),
        .position = position->value(),
    };
    if (app.main.host != nullptr) app.main.host->close_popup();

    // The role is set first, in the same edit. Ducking reads roles — the
    // dialogue it has to get out of the way of is found by role — so a box
    // that ducked without setting what was chosen would duck against a
    // sequence that is not the one on screen.
    cutline::core::Project next = app.session.project();
    const std::size_t index = chosen->selected();
    if (index < cutline::core::kAudioRoles.size()) {
      for (const std::string& id : ids) {
        next = cutline::core::set_clip_role(std::move(next), id,
                                            cutline::core::kAudioRoles[index]);
      }
    }
    for (const std::string& id : ids) {
      next = cutline::core::duck_clip(std::move(next), id, settings);
    }
    app.session.apply(std::move(next));
    refresh_all(app);
    invalidate_preview(app);
    // The curve is baked into the mix when the mix is built, so it is heard
    // once the player is rebuilt.
    stop_playback(app);
    mark_dirty(app);
  });

  dialog->set_accept(
      "OK", [&app, ids, chosen = &group, preset = &preset]() -> std::optional<std::string> {
    const std::size_t index = chosen->selected();
    const bool with_preset = preset->checked();
    if (index >= cutline::core::kAudioRoles.size()) return std::nullopt;
    const cutline::core::AudioRole role = cutline::core::kAudioRoles[index];

    // One project, edited once per clip, applied once — so naming six takes is
    // one press of undo rather than six.
    cutline::core::Project next = app.session.project();
    for (const std::string& id : ids) {
      next = with_preset ? cutline::editor::apply_role_preset(std::move(next), id, role)
                         : cutline::core::set_clip_role(std::move(next), id, role);
    }
    app.session.apply(std::move(next));
    refresh_all(app);
    invalidate_preview(app);
    stop_playback(app);
    mark_dirty(app);
    return std::nullopt;
  });

  app.main.host->open_popup(std::move(dialog), settings_anchor());
}

/// The effect stack on a mixer strip, as a box that can be opened from it.
///
/// `track_id` empty means the master, which has one stack and no id to name it
/// by. The two are one function because everything about editing a stack is the
/// same either way — only which project operation is called differs, and
/// writing it twice is how the two would come to disagree about what a disabled
/// entry looks like.
///
/// A popup rather than rows in the strip. Premiere puts inserts in a disclosure
/// at the head of each strip, and a strip is a narrow column: five effects with
/// three parameters each is taller than the panel and would push the fader —
/// the one control the panel exists for — off the bottom.
void open_strip_effects(App& app, const std::string& track_id) {
  if (app.main.host == nullptr) return;
  const bool master = track_id.empty();

  const auto stack_of = [&app, track_id, master]() -> std::vector<cutline::core::AudioClipEffect> {
    if (master) return app.session.project().sequence().master_effects;
    const auto found = std::ranges::find(app.session.project().sequence().tracks, track_id,
                                         &cutline::core::Track::id);
    return found == app.session.project().sequence().tracks.end()
               ? std::vector<cutline::core::AudioClipEffect>{}
               : found->audio_effects;
  };

  auto dialog = std::make_unique<cutline::ui::Dialog>(master ? "Master Effects" : "Track Effects");
  Box* panel = &dialog->body();

  auto& add = dialog->add_button("Add Effect...", {});
  add.set_on_click([&app, track_id, master, control = &add] {
    std::vector<std::string> names;
    for (const cutline::audio::AudioEffectDef& def : cutline::audio::audio_effect_defs()) {
      names.emplace_back(def.name);
    }
    auto list = std::make_unique<MenuList>(std::move(names));
    list->set_on_choose([&app, track_id, master](std::size_t index) {
      const std::span<const cutline::audio::AudioEffectDef> defs =
          cutline::audio::audio_effect_defs();
      if (index >= defs.size()) return;
      // Every parameter written out at its default, for the reason the clip
      // stack does it: the panel reads what is stored, and an effect added
      // with an empty map would show its controls at the registry's values
      // while the file said nothing.
      std::map<std::string, double> params;
      for (const cutline::audio::AudioEffectParamDef& param : defs[index].params) {
        params.emplace(std::string(param.key), param.fallback);
      }
      const std::string type{defs[index].id};
      app.session.apply(master ? cutline::core::add_master_audio_effect(app.session.project(),
                                                                       type, std::move(params))
                               : cutline::core::add_track_audio_effect(
                                     app.session.project(), track_id, type, std::move(params)));
      if (app.main.host != nullptr) app.main.host->close_popup();
      refresh_all(app);
      open_strip_effects(app, track_id);
    });
    if (app.main.host != nullptr) app.main.host->open_popup(std::move(list), control->bounds());
  });

  const std::vector<cutline::core::AudioClipEffect> stack = stack_of();
  if (stack.empty()) {
    panel->emplace<Label>("No effects").set_small(true);
  }

  for (std::size_t i = 0; i < stack.size(); ++i) {
    const cutline::core::AudioClipEffect& effect = stack[i];
    const cutline::audio::AudioEffectDef* def = cutline::audio::audio_effect_def(effect.type);

    auto& row = panel->emplace<Box>(Axis::Horizontal);
    auto& on = row.emplace<Checkbox>(def == nullptr ? effect.type : std::string(def->name),
                                     effect.enabled);
    on.set_on_change([&app, track_id, master, i](bool) {
      app.session.apply(master
                            ? cutline::core::toggle_master_audio_effect(app.session.project(), i)
                            : cutline::core::toggle_track_audio_effect(app.session.project(),
                                                                       track_id, i));
      refresh_all(app);
      open_strip_effects(app, track_id);
    });
    row.emplace<Spacer>();
    row.emplace<Button>("Remove", [&app, track_id, master, i] {
      app.session.apply(master
                            ? cutline::core::remove_master_audio_effect(app.session.project(), i)
                            : cutline::core::remove_track_audio_effect(app.session.project(),
                                                                       track_id, i));
      if (app.main.host != nullptr) app.main.host->close_popup();
      refresh_all(app);
      open_strip_effects(app, track_id);
    });

    if (def == nullptr) {
      panel->emplace<Label>("Not available in this version").set_small(true);
      continue;
    }

    for (const cutline::audio::AudioEffectParamDef& param : def->params) {
      auto& line = panel->emplace<Box>(Axis::Horizontal);
      line.emplace<Label>(std::string(param.label)).set_small(true);
      line.emplace<Spacer>();

      const auto held = effect.params.find(std::string(param.key));
      auto& field = line.emplace<cutline::ui::NumericField>(
          cutline::ui::ValueRange{.minimum = param.minimum, .maximum = param.maximum},
          held == effect.params.end() ? param.fallback : held->second);
      field.set_decimals(param.step < 1.0 ? 2 : 1);
      field.set_default_value(param.fallback);
      if (!param.unit.empty()) field.set_suffix(std::string(param.unit));
      // On release only. A track stack is baked into the mix when the mix is
      // built, so there is no way to hear a value the document does not hold —
      // unlike a fader, which the mixer takes live.
      field.set_on_commit([&app, track_id, master, i, key = std::string(param.key)](double v) {
        app.session.apply(
            master ? cutline::core::set_master_audio_effect_param(app.session.project(), i, key, v)
                   : cutline::core::set_track_audio_effect_param(app.session.project(), track_id,
                                                                 i, key, v));
        refresh_all(app);
      });
    }
  }

  // Every change is made as it is asked for, so there is nothing to cancel.
  dialog->set_accept("Close", [] { return std::optional<std::string>{}; });
  dialog->set_cancel("");

  app.main.host->open_popup(std::move(dialog), settings_anchor());
}

/// The pass being recorded for a track, made if there is not one yet.
App::AutomationPass& automation_pass(App& app, const std::string& track_id) {
  const auto found = std::ranges::find(app.automation, track_id, &App::AutomationPass::track_id);
  if (found != app.automation.end()) return *found;
  app.automation.push_back(App::AutomationPass{.track_id = track_id});
  return app.automation.back();
}

/// Whether a track is writing automation at this moment.
///
/// The three writing modes differ only here, which is the whole of what
/// separates them: Write records the length of the pass whether or not anybody
/// is touching the fader, Latch starts at the first touch and carries on to the
/// end, and Touch records only while the fader is actually held.
[[nodiscard]] bool pass_is_writing(cutline::core::AutomationMode mode,
                                   const App::AutomationPass& pass) noexcept {
  switch (mode) {
    case cutline::core::AutomationMode::Write: return true;
    case cutline::core::AutomationMode::Latch: return pass.latched;
    case cutline::core::AutomationMode::Touch: return pass.holding;
    case cutline::core::AutomationMode::Off:
    case cutline::core::AutomationMode::Read: return false;
  }
  return false;
}

/// Takes one point from every writing fader, at the playhead.
///
/// Called once per displayed frame while the sequence runs. A pass is a shape
/// somebody drew with their hand, and the frame rate is the resolution that
/// hand has.
void record_automation(App& app) {
  if (app.automation.empty()) return;
  const double now = app.session.playhead();

  for (App::AutomationPass& pass : app.automation) {
    const auto track = std::ranges::find(app.session.project().sequence().tracks, pass.track_id,
                                         &cutline::core::Track::id);
    if (track == app.session.project().sequence().tracks.end()) continue;
    if (!pass_is_writing(track->automation, pass)) continue;

    // Never backwards. A pass is walked forwards, and a seek during one would
    // otherwise leave two points at times that cross.
    if (!pass.written.empty() && now <= pass.written.back().t) continue;
    pass.written.push_back(cutline::core::Keyframe{.t = now, .v = pass.gain});
  }

  App::AutomationPass& master = app.master_automation;
  if (pass_is_writing(app.session.project().sequence().master_automation, master) &&
      (master.written.empty() || now > master.written.back().t)) {
    master.written.push_back(cutline::core::Keyframe{.t = now, .v = master.gain});
  }
}

/// Lays every recorded pass into the document, as one edit.
///
/// Called when the sequence stops. One edit for all of them together, because
/// riding two faders through the same passage is one thing somebody did.
void commit_automation(App& app) {
  bool any = false;
  cutline::core::Project next = app.session.project();
  for (App::AutomationPass& pass : app.automation) {
    if (pass.written.size() >= 2) {
      next = cutline::core::write_track_gain_pass(std::move(next), pass.track_id, pass.written);
      any = true;
    }
    pass.written.clear();
    pass.latched = false;
  }

  App::AutomationPass& master = app.master_automation;
  if (master.written.size() >= 2) {
    next = cutline::core::write_master_gain_pass(std::move(next), master.written);
    any = true;
  }
  master.written.clear();
  master.latched = false;

  if (!any) return;

  app.session.apply(std::move(next));
  refresh_all(app);
}

/// The buses a track could be routed into, as names, with "Master" first.
///
/// Built from `can_route`, so a bus that would close a loop is not offered
/// rather than being offered and refused. The track's own current output is
/// always in the list even when it would not be legal to choose again — a
/// dropdown that could not show what it is set to would be a dropdown showing
/// the wrong thing.
struct BusChoices {
  std::vector<std::string> names{"Master"};
  std::vector<std::string> ids{std::string{}};
  std::size_t selected = 0;
};

[[nodiscard]] BusChoices bus_choices(const cutline::core::Project& project,
                                     const cutline::core::Track& track) {
  BusChoices out;
  for (std::size_t i = 0; i < project.sequence().tracks.size(); ++i) {
    const cutline::core::Track& bus = project.sequence().tracks[i];
    if (!bus.submix) continue;
    if (bus.id != track.output && !cutline::core::can_route(project, track.id, bus.id)) continue;
    out.names.push_back(bus.label.empty()
                            ? cutline::editor::default_track_label(project, i)
                            : bus.label);
    out.ids.push_back(bus.id);
    if (bus.id == track.output) out.selected = out.ids.size() - 1;
  }
  return out;
}

/// A track's sends, as a box opened from its strip.
///
/// A popup for the reason the effect stack is one: a strip is a narrow column,
/// and three sends with a level and a tap point each would push the fader — the
/// control the panel exists for — off the bottom. Premiere keeps sends in a
/// disclosure at the head of the strip for the same reason.
void open_strip_sends(App& app, const std::string& track_id) {
  if (app.main.host == nullptr) return;

  const auto track_now = [&app, track_id]() -> const cutline::core::Track* {
    const auto found = std::ranges::find(app.session.project().sequence().tracks, track_id,
                                         &cutline::core::Track::id);
    return found == app.session.project().sequence().tracks.end() ? nullptr : &*found;
  };
  const cutline::core::Track* track = track_now();
  if (track == nullptr) return;

  auto dialog = std::make_unique<cutline::ui::Dialog>("Sends");
  Box* panel = &dialog->body();

  // Every bus this track could feed, each as a row that is either a send or an
  // offer of one. A list of what exists rather than an "Add" button and a
  // second menu: there are only ever a handful of buses, and showing them all
  // is both shorter to build and shorter to read.
  bool any = false;
  for (std::size_t i = 0; i < app.session.project().sequence().tracks.size(); ++i) {
    const cutline::core::Track& bus = app.session.project().sequence().tracks[i];
    if (!bus.submix) continue;
    if (!cutline::core::can_route(app.session.project(), track_id, bus.id)) continue;
    any = true;

    const auto existing = std::ranges::find(track->sends, bus.id, &cutline::core::Send::to);
    const bool sending = existing != track->sends.end();
    const std::string bus_id = bus.id;

    auto& row = panel->emplace<Box>(Axis::Horizontal);
    auto& on = row.emplace<Checkbox>(
        bus.label.empty() ? cutline::editor::default_track_label(app.session.project(), i)
                          : bus.label,
        sending);
    on.set_on_change([&app, track_id, bus_id](bool now) {
      // A new send starts well below unity. A reverb fed at full is a reverb
      // nobody wanted, and the first thing anybody would do is pull it down.
      constexpr double kNewSendLevel = 0.25;
      app.session.apply(now ? cutline::core::set_send(app.session.project(), track_id, bus_id,
                                                      kNewSendLevel)
                            : cutline::core::remove_send(app.session.project(), track_id,
                                                         bus_id));
      if (app.main.host != nullptr) app.main.host->close_popup();
      refresh_all(app);
      stop_playback(app);
      mark_dirty(app);
      open_strip_sends(app, track_id);
    });

    if (!sending) continue;

    row.emplace<Spacer>();
    auto& level = row.emplace<cutline::ui::NumericField>(
        cutline::ui::ValueRange{.minimum = cutline::ui::kGainFloorDb,
                                .maximum = cutline::ui::gain_to_fader_db(cutline::core::kMaxGain)},
        cutline::ui::gain_to_fader_db(existing->level));
    level.set_decimals(1);
    level.set_default_value(0.0);
    level.set_suffix("dB");
    level.set_on_commit([&app, track_id, bus_id, pre = existing->pre_fader](double db) {
      app.session.apply(cutline::core::set_send(
          app.session.project(), track_id, bus_id,
          cutline::ui::fader_db_to_gain(db, cutline::core::kMaxGain), pre));
      refresh_all(app);
      stop_playback(app);
      mark_dirty(app);
    });

    auto& tap = panel->emplace<Checkbox>("Pre-fader", existing->pre_fader);
    tap.set_on_change([&app, track_id, bus_id, level = existing->level](bool pre) {
      app.session.apply(
          cutline::core::set_send(app.session.project(), track_id, bus_id, level, pre));
      if (app.main.host != nullptr) app.main.host->close_popup();
      refresh_all(app);
      stop_playback(app);
      mark_dirty(app);
      open_strip_sends(app, track_id);
    });
  }

  if (!any) {
    // The honest empty state. A send needs somewhere to go, and the answer to
    // an empty box is a submix rather than a longer list.
    panel->emplace<Label>("No submixes to send to").set_small(true);
    panel->emplace<Button>("Add Submix Track", [&app, track_id] {
      if (app.main.host != nullptr) app.main.host->close_popup();
      app.session.apply(cutline::core::add_submix_track(app.session.project()));
      refresh_all(app);
      stop_playback(app);
      mark_dirty(app);
      open_strip_sends(app, track_id);
    });
  }

  dialog->set_accept("Close", [&app]() -> std::optional<std::string> {
    return std::nullopt;
  });

  dialog->set_cancel("");

  app.main.host->open_popup(std::move(dialog), settings_anchor());
}

/// One audio track's strip in the mixer, in Premiere's order down the column:
/// the panner, the mute and solo pair, the fader with its meter beside it, the
/// level as a number, and the track's name at the foot.
///
/// A mix has two levels for a reason. A clip's gain is what that take needed;
/// a track's is what the whole stem needs against the others, and riding one to
/// fix the other is how a mix stops being reversible.
///
/// This used to be two numeric fields labelled Volume and Balance, stacked in
/// one column, on the argument that a docked panel may be too narrow for a row
/// of strips. That was a description of a layout problem rather than a reason
/// to build different controls — Premiere has the same problem and scrolls
/// horizontally, which is what the panel does now. A fader is not a number in
/// a different costume: a mix is set by ear, with a hand on a long throw and an
/// eye on a meter beside it, and a field can only be typed into or scrubbed.
void build_track_strip(App& app, Box& strips, const cutline::core::Track& track,
                       std::size_t index) {
  const std::string track_id = track.id;
  auto& strip = strips.emplace<Box>(Axis::Vertical);

  auto& pan = strip.emplace<cutline::ui::PanKnob>(track.pan * 100.0);
  pan.set_on_commit([&app, track_id](double value) {
    app.session.apply(
        cutline::core::set_track_pan(app.session.project(), track_id, value / 100.0));
    app.inspector_stale = true;
  });

  // What the fader does with its curve. Premiere's five, in Premiere's order.
  static constexpr std::array<cutline::core::AutomationMode, 5> kModes{
      cutline::core::AutomationMode::Off, cutline::core::AutomationMode::Read,
      cutline::core::AutomationMode::Write, cutline::core::AutomationMode::Latch,
      cutline::core::AutomationMode::Touch};
  const auto at_mode = std::ranges::find(kModes, track.automation);
  auto& modes = strip.emplace<Dropdown>(
      std::vector<std::string>{"Off", "Read", "Write", "Latch", "Touch"},
      at_mode == kModes.end() ? 1u : static_cast<std::size_t>(at_mode - kModes.begin()));
  modes.set_on_change([&app, track_id](std::size_t index) {
    if (index >= kModes.size()) return;
    app.session.apply(
        cutline::core::set_track_automation(app.session.project(), track_id, kModes[index]));
    refresh_all(app);
  });

  // Mute and solo are also on the track head in the timeline, and that is not
  // two controls for one flag — it is one flag reachable from both of the
  // places you are when you want it. Premiere has both for the same reason:
  // mixing and editing are two activities, and leaving one to reach the other
  // is what a mixer exists to avoid.
  //
  // No record-arm beside them. R arms a track for *automation*, and nothing
  // here records fader moves yet — a button that arms nothing is worse than no
  // button.
  auto& switches = strip.emplace<Box>(Axis::Horizontal);
  // Premiere's insert slots, as one button rather than five rows: a strip is a
  // narrow column and five effects with three parameters each would push the
  // fader off the bottom of it. Lit when the track has a stack, so a strip says
  // whether anything is on it without being opened.
  auto& fx = switches.emplace<Button>("fx", [&app, track_id] {
    open_strip_effects(app, track_id);
  });
  fx.set_selected(!track.audio_effects.empty());
  // Premiere's sends, beside the inserts and lit the same way: a strip says
  // whether anything is going out of it without being opened.
  auto& sends = switches.emplace<Button>("snd", [&app, track_id] {
    open_strip_sends(app, track_id);
  });
  sends.set_selected(!track.sends.empty());
  auto& mute = switches.emplace<Button>("M", [&app, track_id] {
    const auto now = std::ranges::find(app.session.project().sequence().tracks, track_id,
                                       &cutline::core::Track::id);
    if (now == app.session.project().sequence().tracks.end()) return;
    app.session.apply(cutline::core::update_track(
        app.session.project(), track_id, cutline::core::TrackPropsPatch{.muted = !now->muted}));
    refresh_all(app);
  });
  mute.set_selected(track.muted);
  auto& solo = switches.emplace<Button>("S", [&app, track_id] {
    const auto now = std::ranges::find(app.session.project().sequence().tracks, track_id,
                                       &cutline::core::Track::id);
    if (now == app.session.project().sequence().tracks.end()) return;
    app.session.apply(cutline::core::update_track(
        app.session.project(), track_id, cutline::core::TrackPropsPatch{.solo = !now->solo}));
    refresh_all(app);
  });
  solo.set_selected(track.solo);

  // The throw and its meter side by side, which is the arrangement the whole
  // panel exists for: the level you are setting, against the level you are
  // getting.
  auto& throw_row = strip.emplace<Box>(Axis::Horizontal);
  auto& fader = throw_row.emplace<cutline::ui::Fader>(
      cutline::ui::ValueRange{.minimum = cutline::ui::kGainFloorDb,
                              .maximum = cutline::ui::gain_to_fader_db(cutline::core::kMaxGain)},
      cutline::ui::gain_to_fader_db(track.gain));
  fader.set_default_value(0.0);

  auto& bars = throw_row.emplace<cutline::ui::MeterView>();
  // No numbers on a strip's meter. The fader beside it already prints a scale,
  // and two sets of decibels a few pixels apart is one set too many to read —
  // which is also how Premiere draws it.
  bars.set_shows_scale(false);
  app.track_meters.push_back(App::TrackMeter{.index = index, .view = &bars});

  auto& reading = strip.emplace<Label>();
  reading.set_small(true);
  reading.set_text(std::format("{:.1f}", cutline::ui::gain_to_fader_db(track.gain)));

  // Straight to the mix on every pixel of the drag, and to the number beside
  // it. A track fader is balanced against the other tracks, by ear, while they
  // are all playing — one that only took effect on release could not be used
  // that way. The document waits for the button to come up, so one gesture is
  // still one undo entry, and applying it rebuilds the mix with the new gain
  // baked in, which is where the live trim goes back to one.
  // The pass exists from the moment the strip does, holding the fader's
  // position, so a Write pass has something to record before anybody touches
  // anything — which is exactly what Write means.
  automation_pass(app, track_id).gain = track.gain;

  fader.set_on_change([&app, index, track_id, out = &reading](double db) {
    out->set_text(std::format("{:.1f}", db));
    const double gain = cutline::ui::fader_db_to_gain(db, cutline::core::kMaxGain);

    App::AutomationPass& pass = automation_pass(app, track_id);
    pass.gain = gain;
    // A move *is* the touch. Latch and Touch both key off the hand being on
    // the fader, and the change handler is the only thing that knows.
    pass.holding = true;
    pass.latched = true;

#if CUTLINE_HAVE_PREVIEW
    if (app.player != nullptr) app.player->set_track_gain(static_cast<int>(index), gain);
#endif
    mark_dirty(app);
  });
  fader.set_on_commit([&app, track_id](double db) {
    // The hand is off. Touch stops writing here and goes back to the curve;
    // Latch keeps going, which is the whole difference between them.
    automation_pass(app, track_id).holding = false;

    // A track that is recording a pass does not also take the fader as its
    // constant — the pass is what is being set, and writing the constant too
    // would leave the curve and the number underneath it disagreeing.
    const auto track = std::ranges::find(app.session.project().sequence().tracks, track_id,
                                         &cutline::core::Track::id);
    if (track != app.session.project().sequence().tracks.end() &&
        pass_is_writing(track->automation, automation_pass(app, track_id))) {
      return;
    }

    app.session.apply(cutline::core::set_track_gain(
        app.session.project(), track_id,
        cutline::ui::fader_db_to_gain(db, cutline::core::kMaxGain)));
    app.inspector_stale = true;
  });

  // Where this strip goes, under the fader and above the name — which is where
  // Premiere puts a track's output assignment, and the order the signal runs
  // in: through the fader, then out to whatever is next.
  const BusChoices buses = bus_choices(app.session.project(), track);
  auto& output = strip.emplace<Dropdown>(buses.names, buses.selected);
  output.set_on_change([&app, track_id, ids = buses.ids](std::size_t choice) {
    if (choice >= ids.size()) return;
    app.session.apply(
        cutline::core::set_track_output(app.session.project(), track_id, ids[choice]));
    refresh_all(app);
    // The routing is built into the mix when the mix is built, so a new route
    // is heard once the player is rebuilt.
    stop_playback(app);
    mark_dirty(app);
  });

  // The same name the timeline shows, at the foot where Premiere puts it. The
  // raw id is an internal string, and reading "a1" in one panel and "A1" in
  // another is two names for one track.
  //
  // A submix carries its own label instead. Its lane number is the one thing
  // about it that says nothing: "A4" for a dialogue bus is a name that has to
  // be looked up every time it is read.
  strip
      .emplace<Label>(track.submix && !track.label.empty()
                          ? track.label
                          : cutline::editor::default_track_label(app.session.project(), index))
      .set_bold(true);
}

/// Fills the mixer with a strip per audio track and the master at the end.
///
/// Separate from making the panel so the strips can be rebuilt in place. They
/// have to be: adding a track, routing one into a submix or putting the first
/// effect on a stack all change what a strip says, and until this was split the
/// mixer only ever described the project as it stood when the dock was built —
/// a submix added from the timeline appeared there and nowhere else.
void fill_audio_panel(App* app, Panel& panel) {
  // Side by side and scrolling, which is Premiere's arrangement and the answer
  // to the problem that made an earlier version stack them: a docked panel is
  // as likely to be a narrow column as a whole region, and a row of strips
  // needs a width it cannot count on. Premiere has exactly the same problem and
  // lets the strips run off the right-hand edge. Stacking them instead solved
  // the layout and lost the mixer.
  auto& scroll = panel.emplace<ScrollView>(Axis::Horizontal);
  auto& strips = static_cast<Box&>(scroll.set_content(std::make_unique<Box>(Axis::Horizontal)));

  // The old strips are about to be destroyed, so the pointers to their meters
  // are too. Cleared before they are rebuilt rather than after, since a panel
  // built twice would otherwise leave the first set dangling in the list.
  if (app != nullptr) app->track_meters.clear();

  // Tracks first and the master at the right-hand end, where a mixer puts it:
  // signal flows left to right and the master is where it all arrives.
  if (app != nullptr) {
    const std::vector<cutline::core::Track>& tracks = app->session.project().sequence().tracks;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      if (tracks[i].kind != cutline::core::TrackKind::Audio) continue;
      build_track_strip(*app, strips, tracks[i], i);
    }
  }

  auto& column = strips.emplace<Box>(Axis::Vertical);

  // Gaps where a track strip carries its panner and its switches, so every
  // fader in the panel starts at the same height. Found by looking at it: the
  // master's throw sat an inch above the others and read as a different kind of
  // control rather than the same one at the end of the row. A master has
  // nothing to pan and nothing to solo, so the space stays empty — which is
  // what Premiere leaves there too.
  // A gap where a track strip carries its panner — a master has nothing to pan
  // — then its own automation mode, which Premiere's master has like every
  // other strip, then a gap where the mute and solo pair go, which a master
  // also has nothing to do with.
  const double control = 24.0;
  column.emplace<Spacer>(Axis::Vertical, control * 1.4);

  static constexpr std::array<cutline::core::AutomationMode, 5> kMasterModes{
      cutline::core::AutomationMode::Off, cutline::core::AutomationMode::Read,
      cutline::core::AutomationMode::Write, cutline::core::AutomationMode::Latch,
      cutline::core::AutomationMode::Touch};
  const cutline::core::AutomationMode mode =
      app == nullptr ? cutline::core::AutomationMode::Read
                     : app->session.project().sequence().master_automation;
  const auto at_mode = std::ranges::find(kMasterModes, mode);
  auto& master_modes = column.emplace<Dropdown>(
      std::vector<std::string>{"Off", "Read", "Write", "Latch", "Touch"},
      at_mode == kMasterModes.end() ? 1u
                                    : static_cast<std::size_t>(at_mode - kMasterModes.begin()));
  if (app != nullptr) {
    master_modes.set_on_change([app](std::size_t index) {
      if (index >= kMasterModes.size()) return;
      app->session.apply(
          cutline::core::set_master_automation(app->session.project(), kMasterModes[index]));
      refresh_all(*app);
    });
  }

  // Where a track strip carries fx, mute and solo. The master has a stack of
  // its own but nothing to mute or solo, so it gets the one button.
  auto& master_switches = column.emplace<Box>(Axis::Horizontal);
  auto& master_fx = master_switches.emplace<Button>("fx");
  if (app != nullptr) {
    master_fx.set_on_click([app] { open_strip_effects(*app, {}); });
    master_fx.set_selected(!app->session.project().sequence().master_effects.empty());
  }

  const double gain = app == nullptr ? 1.0 : app->session.project().sequence().master_gain;
  auto& master_row = column.emplace<Box>(Axis::Horizontal);
  auto& fader = master_row.emplace<cutline::ui::Fader>(
      cutline::ui::ValueRange{
          .minimum = cutline::ui::kGainFloorDb,
          .maximum = cutline::ui::gain_to_fader_db(cutline::core::kMaxMasterGain)},
      cutline::ui::gain_to_fader_db(gain));
  // Unity, which is where a volume control resets to and where a project that
  // has never been touched sits.
  fader.set_default_value(0.0);

  // Beside the fader, not under it — the same arrangement every track strip
  // has, and the reason a mixer is shaped this way at all. Stacked, the master
  // read as a fader and a separate meter that happened to be near each other.
  auto& bars = master_row.emplace<cutline::ui::MeterView>();
  if (app != nullptr) app->meter = &bars;

  auto& reading = column.emplace<Label>();
  reading.set_small(true);

  // A gap where a track strip carries its output assignment. The master has
  // nowhere to be routed — it is where everything arrives — and leaving the
  // space keeps every name in the row on one line.
  column.emplace<Spacer>(Axis::Vertical, control);

  // The name at the foot, where every other strip carries its own.
  column.emplace<Label>("Master").set_bold(true);

  if (app != nullptr) {
    app->master_fader = &fader;
    app->master_reading = &reading;
    show_master_gain(*app, gain);

    // On every pixel of the drag, straight to the mixer: a master fader that
    // only took effect on release could not be set by ear, which is the only
    // way anybody sets one. The model is left alone until the button comes up,
    // so one gesture is still one undo entry.
    app->master_automation.gain = gain;
    fader.set_on_change([app](double db) {
      const double moved = cutline::ui::fader_db_to_gain(db, cutline::core::kMaxMasterGain);
      app->master_automation.gain = moved;
      // A move is the touch, which is what Latch and Touch key off.
      app->master_automation.holding = true;
      app->master_automation.latched = true;
      if (app->player != nullptr) app->player->set_master_gain(moved);
      show_master_gain(*app, moved);
    });
    fader.set_on_commit([app](double db) {
      app->master_automation.holding = false;
      // A fader recording a pass does not also write the constant underneath
      // it: the pass is what is being set, and both would disagree.
      if (pass_is_writing(app->session.project().sequence().master_automation, app->master_automation)) {
        return;
      }
      app->session.apply(cutline::core::set_master_gain(
          app->session.project(),
          cutline::ui::fader_db_to_gain(db, cutline::core::kMaxMasterGain)));
      show_master_gain(*app, app->session.project().sequence().master_gain);
    });
  }
}

/// The master fader and the meter it is set against.
///
/// The two belong together: a fader is moved by ear and by eye, and the eye
/// part is the meter. Splitting them across panels would mean setting a level
/// while watching something that might be behind another tab.
[[nodiscard]] std::unique_ptr<Widget> make_audio_panel(App* app) {
  auto panel = std::make_unique<Panel>();
  if (app != nullptr) app->mixer = panel.get();
  fill_audio_panel(app, *panel);
  return panel;
}

/// Rebuilds the strips against the project as it stands.
///
/// `clear_children` first, which tells the host to drop hover, focus and
/// capture: a dropdown that was open when the route changed must not be freed
/// underneath its own callback. Same reason the inspector does it.
void refresh_audio_panel(App& app) {
  if (app.mixer == nullptr || app.main.host == nullptr) return;
  app.meter = nullptr;
  app.master_fader = nullptr;
  app.master_reading = nullptr;
  app.track_meters.clear();
  app.mixer->clear_children();
  fill_audio_panel(&app, *app.mixer);
  app.main.host->request_layout();
  mark_dirty(app);
}

}  // namespace cutline::tool
