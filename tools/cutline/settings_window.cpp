/// The two settings windows: Preferences, which belong to the person, and
/// Project Settings, which travel with the cut.
///
/// Each is a dialog window with its categories down the side and a page that
/// is rebuilt in place when a category is chosen.

#include "app.hpp"

namespace cutline::tool {

// --------------------------------------------------------------- settings --
//
// Premiere keeps preferences and project settings in two separate windows, and
// so does this: one belongs to the person and outlives every project, the other
// travels with the cut and is saved inside it. A single Settings window would
// quietly write half its contents to one place and half to another, which is
// the one arrangement nobody could reason about.
//
// Each page fills a container the dialog owns rather than being a widget tree
// of its own, so switching category rebuilds one box instead of the window.

/// The sequence's shape. Premiere's Project Settings ▸ General.
void build_sequence_page(App& app, cutline::ui::Form& into) {
  into.emplace<Label>("Sequence size").set_heading(true);

  // A dropdown, for the same reason the frame rates are one: seven presets are
  // one choice with seven answers rather than seven things to do, and seven
  // buttons down a settings window is a wall.
  auto& presets = into.row("Preset");

  std::vector<std::string> sizes;
  // Past the end when the sequence is at a size nobody preset, which is a state
  // the typed row below can reach and the dropdown has to be able to show.
  std::size_t chosen_size = kCanvasPresets.size();
  for (std::size_t i = 0; i < kCanvasPresets.size(); ++i) {
    const CanvasPreset& preset = kCanvasPresets[i];
    sizes.push_back(std::string(preset.name) + "  " + std::to_string(preset.width) + "x" +
                    std::to_string(preset.height));
    if (preset.width == app.session.project().sequence().canvas_w &&
        preset.height == app.session.project().sequence().canvas_h) {
      chosen_size = i;
    }
  }
  if (chosen_size == kCanvasPresets.size()) sizes.push_back(canvas_label(app.session.project()));

  auto& size_choice = presets.emplace<Dropdown>(std::move(sizes), chosen_size);
  size_choice.set_on_change([&app](std::size_t index) {
    if (index >= kCanvasPresets.size()) return;  // the "custom" row it was showing
    apply_canvas(app, kCanvasPresets[index].width, kCanvasPresets[index].height);
  });

  auto& custom = into.row("Custom");
  auto& width = custom.emplace<TextField>(std::to_string(app.session.project().sequence().canvas_w));
  custom.emplace<Label>("x").set_small(true);
  auto& height = custom.emplace<TextField>(std::to_string(app.session.project().sequence().canvas_h));
  custom.emplace<Button>("Set", [&app, w = &width, h = &height] {
    // Anything that is not a number leaves the sequence alone rather than
    // becoming zero, which the clamp would turn into the smallest canvas there
    // is — a silent, baffling answer to a typing mistake.
    int wide = 0;
    int tall = 0;
    if (std::from_chars(w->text().data(), w->text().data() + w->text().size(), wide).ec !=
            std::errc{} ||
        std::from_chars(h->text().data(), h->text().data() + h->text().size(), tall).ec !=
            std::errc{}) {
      return;
    }
    apply_canvas(app, wide, tall);
  });

  into.emplace<Label>("Frame rate").set_heading(true);

  // A dropdown rather than a row of seven buttons. Seven abreast fitted while
  // this was a popup that grew to its contents, and `--check` found four of
  // them squeezed the moment it became a window with a width — and a
  // seven-way exclusive choice is what a dropdown is for anyway, which is what
  // Premiere uses here.
  auto& rates = into.row("Rate");
  std::vector<std::string> names;
  std::size_t chosen = 0;
  for (std::size_t i = 0; i < kFrameRates.size(); ++i) {
    names.push_back(fps_label(kFrameRates[i]));
    // Compared with a tolerance: 23.976 is not the number the file holds, and a
    // preset that never looks chosen is one nobody trusts they pressed.
    if (std::abs(app.session.project().sequence().fps - kFrameRates[i]) < 0.005) chosen = i;
  }
  auto& choice = rates.emplace<Dropdown>(std::move(names), chosen);
  choice.set_on_change([&app](std::size_t index) {
    if (index < kFrameRates.size()) apply_fps(app, kFrameRates[index]);
  });

  auto& typed = rates.emplace<TextField>(fps_label(app.session.project().sequence().fps));
  rates.emplace<Button>("Set", [&app, field = &typed] {
    double rate = 0.0;
    if (std::from_chars(field->text().data(), field->text().data() + field->text().size(), rate)
            .ec != std::errc{}) {
      return;
    }
    apply_fps(app, rate);
  });

  // Only where it means anything. At 25 or 30 the timecode already counts real
  // seconds, and a choice between two spellings of one thing is a control that
  // teaches somebody the wrong lesson about their own sequence.
  if (cutline::core::supports_drop_frame(app.session.project().sequence().fps)) {
    into.emplace<Label>("Timecode").set_heading(true);

    auto& counting = into.row("Count");

    auto& how = counting.emplace<Dropdown>(
        std::vector<std::string>{"Drop frame", "Non-drop frame"},
        app.session.project().sequence().drop_frame ? 0u : 1u);
    how.set_on_change([&app](std::size_t index) {
      app.session.apply(cutline::core::set_drop_frame(app.session.project(), index == 0));
      refresh_all(app);
    });

    // Short because it has to fit the roomiest theme, and because the one
    // thing anybody needs told is that no footage is harmed by this.
    into.emplace<Label>("Skips numbers, never frames.").set_small(true);
  }

  // Premiere's sequence colour management, the same three settings.
  into.emplace<Label>("Colour").set_heading(true);
  const cutline::core::Sequence& sequence = app.session.project().sequence();

  auto& space = into.row("Working space");
  auto& working = space.emplace<Dropdown>(
      std::vector<std::string>{"Rec. 709", "Rec. 2100 PQ", "Rec. 2100 HLG"},
      static_cast<std::size_t>(sequence.colour));
  working.set_on_change([&app](std::size_t index) {
    app.session.apply(cutline::core::set_working_colour(
        app.session.project(), static_cast<cutline::core::WorkingColour>(std::min<std::size_t>(index, 2))));
    refresh_all(app);
  });

  // The three Premiere offers. Anything else a project file holds is shown as
  // itself rather than snapped to the nearest.
  constexpr std::array<double, 3> kWhites{100.0, cutline::core::kDefaultGraphicsWhite, 300.0};
  std::vector<std::string> whites;
  std::size_t chosen_white = kWhites.size();
  for (std::size_t i = 0; i < kWhites.size(); ++i) {
    whites.push_back(std::format("{:.0f} nits", kWhites[i]));
    if (std::abs(sequence.graphics_white - kWhites[i]) < 0.5) chosen_white = i;
  }
  if (chosen_white == kWhites.size()) whites.push_back(std::format("{:.0f} nits", sequence.graphics_white));
  auto& white_row = into.row("HDR graphics white");
  auto& white = white_row.emplace<Dropdown>(std::move(whites), chosen_white);
  white.set_on_change([&app, kWhites](std::size_t index) {
    if (index >= kWhites.size()) return;
    app.session.apply(cutline::core::set_graphics_white(app.session.project(), kWhites[index]));
    refresh_all(app);
  });

  auto& tone = into.emplace<Checkbox>("Auto tone map media", sequence.tone_map);
  tone.set_on_change([&app](bool on) {
    app.session.apply(cutline::core::set_tone_map(app.session.project(), on));
    refresh_all(app);
  });
}

/// The theme. Premiere's Preferences ▸ Appearance.
void build_appearance_page(App& app, cutline::ui::Form& into) {
  // A dropdown rather than a button per theme. Four buttons in a column read as
  // four things to do; this is one choice with four answers, and a settings
  // window is built out of the control that says so.
  auto& row = into.row("Theme");

  std::vector<std::string> names;
  names.reserve(built_in_themes().size());
  for (const Theme& theme : built_in_themes()) names.emplace_back(theme.name);

  auto& choice = row.emplace<Dropdown>(std::move(names), app.theme);
  choice.set_on_change([&app](std::size_t index) { set_theme(app, index); });

  // The period's sounds, which were the system's rather than any one
  // application's, so they are played from the person's own Windows scheme.
  auto& sounds = into.row("Sounds").emplace<Checkbox>("Play Windows' sounds", app.settings.sounds);
  sounds.set_on_change([&app](bool on) {
    app.settings.sounds = on;
    save_settings(app);
  });
}

/// One labelled number on a settings page.
///
/// Written once because rows of the same shape copied out three times are three
/// places for the units to drift apart.
void duration_row(App& app, cutline::ui::Form& into, const char* label, double value, double smallest,
                  double largest, const char* suffix, std::function<void(double)> keep) {
  auto& row = into.row(label);

  auto& number = row.emplace<cutline::ui::NumericField>(
      cutline::ui::ValueRange{.minimum = smallest, .maximum = largest}, value);
  number.set_decimals(suffix == std::string_view("s") ? 2 : 0);
  number.set_suffix(suffix);
  // Committed rather than changed, so dragging the number does not write the
  // file once per pixel.
  number.set_on_commit([&app, keep = std::move(keep)](double set) {
    keep(set);
    save_settings(app);
  });
}

/// How long the things with no length of their own last. Premiere's
/// Preferences ▸ Timeline.
void build_timeline_page(App& app, cutline::ui::Form& into) {
  into.emplace<Label>("Default durations").set_heading(true);
  duration_row(app, into, "Still image", app.settings.still_length,
               cutline::editor::kMinStillLength, cutline::editor::kMaxStillLength, "s",
               [&app](double set) { app.settings.still_length = set; });
  duration_row(app, into, "Transition", app.settings.transition_length,
               cutline::editor::kMinTransitionLength, cutline::editor::kMaxTransitionLength, "s",
               [&app](double set) { app.settings.transition_length = set; });

  // Said rather than left to be discovered. A still already on the timeline
  // keeps the length it was given, and somebody who changes this expecting the
  // sequence to rearrange itself has to be told it will not.
  into.emplace<Label>("These apply to what you place next.").set_small(true);
}

/// The recovery copy. Premiere's Preferences ▸ Auto Save.
void build_autosave_page(App& app, cutline::ui::Form& into) {
  into.emplace<Label>("Recovery").set_heading(true);
  duration_row(app, into, "Write a copy every",
               static_cast<double>(app.settings.autosave_seconds),
               cutline::editor::kMinAutosaveSeconds, cutline::editor::kMaxAutosaveSeconds, "sec",
               [&app](double set) { app.settings.autosave_seconds = static_cast<int>(set); });
  duration_row(app, into, "Copies kept", static_cast<double>(app.settings.autosave_versions),
               cutline::editor::kMinAutosaveVersions, cutline::editor::kMaxAutosaveVersions, "",
               [&app](double set) { app.settings.autosave_versions = static_cast<int>(set); });

  // Why more than one is worth keeping, which is not obvious and is the whole
  // reason this row exists. The newest copy is written from the state the
  // application is in — so a document that has been quietly wrong for the last
  // few minutes has had that faithfully copied over the only thing there was to
  // go back to.
  into.emplace<Label>("Older copies are kept, so a bad one is not the only one.").set_small(true);
  into.emplace<Label>("Copies are written beside the application's own data.").set_small(true);
}

/// What a kind of media is called on the Labels page.
///
/// `ui::to_string` is the file format's spelling — lower case, and "color"
/// rather than "colour" because that is what the model writes. This is the one
/// somebody reads.
[[nodiscard]] std::string kind_label(cutline::ui::MediaKind kind) {
  switch (kind) {
    case cutline::ui::MediaKind::Video: return "Video";
    case cutline::ui::MediaKind::Audio: return "Audio";
    case cutline::ui::MediaKind::Image: return "Still";
    case cutline::ui::MediaKind::Title: return "Title";
    case cutline::ui::MediaKind::Color: return "Colour matte";
    case cutline::ui::MediaKind::Adjustment: return "Adjustment layer";
  }
  return "Video";
}

/// The eight labels, their names, and what each kind of media arrives wearing.
/// Premiere's Preferences ▸ Labels.
void build_labels_page(App& app, cutline::ui::Form& into) {
  const std::span<const cutline::editor::ClipLabel> palette = cutline::editor::clip_labels();
  app.settings.label_names.resize(palette.size());

  into.emplace<Label>("Label names").set_heading(true);
  for (std::size_t i = 0; i < palette.size(); ++i) {
    auto& row = into.row(std::string(palette[i].name));
    // The built-in name stands where Premiere puts a colour swatch, and says
    // the same thing: *which* label this row renames. There is no swatch
    // because there is no read-only one — `ColorSwatch` opens a picker, and a
    // picker here would be a control that lies.
    //
    // The colours themselves are deliberately not editable. A clip stores its
    // label as a hex rather than as an index into the palette, so recolouring
    // Violet would leave every clip already wearing it holding the old colour —
    // a palette and a timeline that disagree, which is worse than Premiere,
    // where labels are stored by position and follow.
    row.label().set_tooltip(std::string(palette[i].color));

    auto& field = row.emplace<TextField>(std::string(cutline::editor::label_name(app.settings, i)));
    // A fixed width, so eight fields make a column rather than a ragged edge —
    // a flexible field takes what the row's label leaves it, and the labels are
    // not the same length. Wide enough for the longest built-in name and a
    // little more, since a renamed one is usually longer than "Iris".
    field.set_columns(16);
    field.set_on_commit([&app, i, built_in = std::string(palette[i].name)](
                            const std::string& typed) {
      if (i >= app.settings.label_names.size()) return;
      // Cleared back to the built-in name rather than left blank: a label with
      // no name at all is a row of the menu nobody can ask for.
      app.settings.label_names[i] = typed == built_in ? std::string{} : typed;
      save_settings(app);
      // The whole page, because the defaults below list the labels by name and
      // would otherwise go on offering the old one. Deferred, since this is a
      // field inside the page that is about to be rebuilt.
      app.settings_page_stale = true;
      refresh_all(app);
    });
  }

  into.emplace<Label>("Label defaults").set_heading(true);
  app.settings.label_defaults.resize(cutline::ui::kMediaKindCount);

  // "None" first, because arriving unlabelled is what every kind does now and
  // getting a default off again is as ordinary as putting one on.
  std::vector<std::string> choices{"None"};
  std::vector<std::string> colours{std::string{}};
  for (std::size_t i = 0; i < palette.size(); ++i) {
    choices.emplace_back(cutline::editor::label_name(app.settings, i));
    colours.emplace_back(palette[i].color);
  }

  for (std::size_t kind = 0; kind < cutline::ui::kMediaKindCount; ++kind) {
    auto& row = into.row(kind_label(static_cast<cutline::ui::MediaKind>(kind)));

    const auto at = std::ranges::find(colours, app.settings.label_defaults[kind]);
    auto& choice = row.emplace<Dropdown>(
        choices, at == colours.end() ? 0 : static_cast<std::size_t>(at - colours.begin()));
    choice.set_on_change([&app, kind, colours](std::size_t index) {
      if (index >= colours.size()) return;
      app.settings.label_defaults[kind] = colours[index];
      save_settings(app);
    });
  }

  into.emplace<Label>("These apply to what you import next.").set_small(true);
}

/// The two layers agree about what a proxy's height defaults to. `editor` is
/// built without FFmpeg and cannot name `media::kProxyHeight`, so this is the
/// one place both spellings are visible at once.
static_assert(cutline::editor::kDefaultProxyHeight == cutline::media::kProxyHeight,
              "the settings default and the transcode default have drifted apart");

/// How big proxies are and where they go. Premiere's Ingest Settings.
void build_proxies_page(App& app, cutline::ui::Form& into) {
  into.emplace<Label>("Proxy size").set_heading(true);

  auto& size_row = into.row("Height");

  // A short list of the sizes anybody would pick rather than a free number.
  // What a machine can keep up with is a fact about the desk this is running
  // on, so it has to be adjustable — but the useful answers are four, and a
  // field inviting 437 would only make one bad session.
  constexpr std::array<int, 4> kProxyHeights{360, 540, 720, 1080};
  std::vector<std::string> sizes;
  std::size_t chosen = 1;
  for (std::size_t i = 0; i < kProxyHeights.size(); ++i) {
    sizes.push_back(std::to_string(kProxyHeights[i]) + "p");
    if (kProxyHeights[i] == app.settings.proxy_height) chosen = i;
  }
  auto& size_choice = size_row.emplace<Dropdown>(std::move(sizes), chosen);
  size_choice.set_on_change([&app](std::size_t index) {
    if (index >= kProxyHeights.size()) return;
    app.settings.proxy_height = kProxyHeights[index];
    save_settings(app);
  });

  into.emplace<Label>("Where they go").set_heading(true);

  // The path on its own row and the buttons under it. A path has no length
  // anybody can plan for, and sharing a row with two buttons is what `--check`
  // found overflowing in the roomiest theme the moment there was a real one.
  //
  // Elided from the front, because the end of a path is the part that says
  // where it is: "…/Fast Scratch/Cutline Proxies" is useful and "E:/Fast
  // Scratch/Cutli…" is not.
  constexpr std::size_t kLongestPath = 44;
  std::string shown = app.settings.proxy_folder.empty() ? std::string("Beside the footage")
                                                        : app.settings.proxy_folder;
  if (shown.size() > kLongestPath) shown = "..." + shown.substr(shown.size() - kLongestPath + 3);
  into.emplace<Label>(shown).set_small(true);

  auto& where = into.emplace<Box>(Axis::Horizontal);
  where.emplace<Button>("Choose...", [&app] {
    const auto folder = choose_folder(app.settings_window == nullptr ? app.main.window
                                                                    : app.settings_window->window);
    if (!folder.has_value()) return;
    app.settings.proxy_folder = folder->string();
    save_settings(app);
    app.settings_page_stale = true;
  });
  // Only when there is something to go back from. A button offering the state
  // the label beside it already reports is a control that cannot do anything,
  // and two of them saying the same words is worse than one.
  if (!app.settings.proxy_folder.empty()) {
    where.emplace<Button>("Use the footage's folder", [&app] {
      app.settings.proxy_folder.clear();
      save_settings(app);
      app.settings_page_stale = true;
    });
  }

  into.emplace<Label>("Proxies already made keep the size and place they were made at.")
      .set_small(true);
}

/// Which output to play through. Premiere's Preferences ▸ Audio Hardware.
void build_audio_page([[maybe_unused]] App& app, cutline::ui::Form& into) {
  into.emplace<Label>("Output device").set_heading(true);

#if CUTLINE_HAVE_PREVIEW
  // Asked for each time the page is built rather than kept: devices are
  // plugged in and out while an application runs, and a list gathered at
  // startup would be wrong by the time anybody opened this.
  const std::vector<cutline::engine::AudioOutput> outputs = cutline::engine::audio_outputs();

  // Device names have no length anybody can plan for — "Speakers (High
  // Definition Audio Device)" is an ordinary one — and a dropdown is as wide as
  // its widest entry. `--check` found five squeezed widgets in every theme
  // before this was cut back.
  constexpr std::size_t kLongestDevice = 34;
  const auto shorten = [](std::string name) {
    if (name.size() > kLongestDevice) name = name.substr(0, kLongestDevice - 3) + "...";
    return name;
  };

  std::vector<std::string> names{"System default"};
  std::vector<std::string> ids{std::string{}};
  std::size_t chosen = 0;
  for (const cutline::engine::AudioOutput& output : outputs) {
    names.push_back(shorten(output.is_default ? output.name + " (default)" : output.name));
    ids.push_back(output.id);
    if (output.id == app.settings.audio_device) chosen = ids.size() - 1;
  }

  // A chosen device that is not plugged in stays in the list, by the name it
  // had when it was picked. Dropping it would silently reset the preference to
  // the default, and plugging the interface back in would not bring it back.
  if (!app.settings.audio_device.empty() && chosen == 0) {
    names.push_back(shorten((app.settings.audio_device_name.empty()
                                 ? std::string("Chosen device")
                                 : app.settings.audio_device_name) +
                            " (not connected)"));
    ids.push_back(app.settings.audio_device);
    chosen = ids.size() - 1;
  }

  into.emplace<Label>("Play through").set_small(true);
  auto& choice = into.emplace<Dropdown>(names, chosen);
  choice.set_on_change([&app, ids, names](std::size_t index) {
    if (index >= ids.size()) return;
    app.settings.audio_device = ids[index];
    // The name goes with it, so a device that is unplugged later can still be
    // named in this list rather than shown as a line of hex.
    app.settings.audio_device_name = index == 0 ? std::string{} : names[index];
    save_settings(app);
    // The players hold a device open. Dropped rather than retargeted: WASAPI
    // has no way to move a stream, and a player rebuilt on the next press is a
    // pause nobody notices.
    app.player.reset();
    app.source_player.reset();
    app.player_failed = false;
    app.settings_page_stale = true;
  });

  if (outputs.empty()) {
    into.emplace<Label>("No output devices. Playback will be silent.").set_small(true);
  } else {
    into.emplace<Label>("Playback restarts on the next press.").set_small(true);
  }
#else
  into.emplace<Label>("This build has no audio.").set_small(true);
#endif
}

/// How far back an edit can be taken. Premiere's Preferences ▸ General.
void build_general_page(App& app, cutline::ui::Form& into) {
  into.emplace<Label>("Undo").set_heading(true);
  duration_row(app, into, "Edits kept", static_cast<double>(app.settings.undo_depth),
               cutline::editor::kMinUndoDepth, cutline::editor::kMaxUndoDepth, "",
               [&app](double set) {
                 app.settings.undo_depth = static_cast<int>(set);
                 // Applied to the open document as well as saved, so lowering it
                 // frees the memory now rather than at the next edit. A setting
                 // that appears to do nothing until something else happens is
                 // one nobody believes they changed.
                 app.session.set_undo_depth(static_cast<std::size_t>(app.settings.undo_depth));
               });

  // The trade said out loud, because it is not the one anybody expects. Undo
  // here is a stack of whole projects rather than a list of things to reverse,
  // which is what makes it correct for every operation without any of them
  // knowing about it — and what makes a long history cost real memory on a cut
  // with a lot of keyframes in it.
  into.emplace<Label>("Each step keeps a copy of the whole project.").set_small(true);
}

/// What plays around a marked range. Premiere's Preferences ▸ Playback.
void build_playback_page(App& app, cutline::ui::Form& into) {
  into.emplace<Label>("Around the marked range").set_heading(true);
  duration_row(app, into, "Preroll", app.settings.preroll, cutline::editor::kMinRoll,
               cutline::editor::kMaxRoll, "s",
               [&app](double set) { app.settings.preroll = set; });
  duration_row(app, into, "Postroll", app.settings.postroll, cutline::editor::kMinRoll,
               cutline::editor::kMaxRoll, "s",
               [&app](double set) { app.settings.postroll = set; });

  // Which gesture this changes, since it changes only one. A run-up is for
  // judging a cut in context, and the gesture that judges a cut is looping over
  // it — playing from the playhead already starts wherever you put it.
  into.emplace<Label>("Looping plays this much either side of the marks.").set_small(true);
}

/// Which adapter everything draws on. Premiere's Preferences ▸ Graphics.
void build_graphics_page(App& app, cutline::ui::Form& into) {
  into.emplace<Label>("Renderer").set_heading(true);

  auto& row = into.row("Draw with");

  auto& choice = row.emplace<Dropdown>(std::vector<std::string>{"Graphics card", "Software"},
                                       app.settings.software_renderer ? 1u : 0u);
  choice.set_on_change([&app](std::size_t index) {
    app.settings.software_renderer = index == 1;
    save_settings(app);
    // The page says what is running now and what was chosen, and those have
    // just stopped agreeing.
    app.settings_page_stale = true;
  });

#if CUTLINE_HAVE_PREVIEW
  // What is actually in use, which is not always what was chosen: the device
  // falls back to software on its own when no adapter will have it, and a
  // machine quietly running on WARP is exactly the machine somebody has opened
  // this page to ask about.
  if (app.device != nullptr) {
    // Elided, because an adapter name is a name from outside — "NVIDIA GeForce
    // RTX 4090 Laptop GPU" is an ordinary one, and a settings window has a
    // width.
    constexpr std::size_t kLongestAdapter = 40;
    std::string now = app.device->adapter_name();
    if (now.size() > kLongestAdapter) now = now.substr(0, kLongestAdapter - 3) + "...";
    into.emplace<Label>("Now using " + now).set_small(true);
  }
#endif

  // Both halves of the honest answer. The choice is read once when the device
  // is made, and everything that draws — the compositor, the swapchain, Skia's
  // context — is built on it; there is no way to move those to another adapter
  // while they are open, so a tick that appeared to take effect would be lying.
  if (app.settings.software_renderer) {
    into.emplace<Label>("Software drawing is correct and much slower.").set_small(true);
  }
  into.emplace<Label>("Takes effect when Cutline is restarted.").set_small(true);
}

/// A size in the units somebody reads. Kilobytes are too many to take in and
/// bytes are not a number at all once a cache has anything in it.
[[nodiscard]] std::string bytes_label(std::uintmax_t bytes) {
  if (bytes >= 1024ull * 1024 * 1024) {
    return std::format("{:.1f} GB", static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
  }
  if (bytes >= 1024ull * 1024) {
    return std::format("{:.0f} MB", static_cast<double>(bytes) / (1024.0 * 1024));
  }
  if (bytes == 0) return "nothing";
  return std::format("{:.0f} KB", std::max(1.0, static_cast<double>(bytes) / 1024.0));
}

/// Where what has already been read out of footage is kept. Premiere's
/// Preferences ▸ Media Cache.
void build_media_cache_page([[maybe_unused]] App& app, cutline::ui::Form& into) {
#if CUTLINE_HAVE_PREVIEW
  into.emplace<Label>("Where it goes").set_heading(true);

  const std::filesystem::path dir =
      cutline::app::media_cache_dir(app.settings.media_cache_folder);

  // The path on its own row and the buttons under it, for the reason the proxy
  // folder is laid out that way: a path has no length anybody can plan for, and
  // sharing a row with two buttons is what `--check` found overflowing in the
  // roomiest theme the moment there was a real one. Elided from the front,
  // because the end of a path is the part that says where it is.
  constexpr std::size_t kLongestPath = 44;
  std::string shown = dir.generic_string();
  if (shown.size() > kLongestPath) shown = "..." + shown.substr(shown.size() - kLongestPath + 3);
  into.emplace<Label>(shown).set_small(true);

  auto& where = into.emplace<Box>(Axis::Horizontal);
  where.emplace<Button>("Choose...", [&app] {
    const auto folder = choose_folder(app.settings_window == nullptr ? app.main.window
                                                                    : app.settings_window->window);
    if (!folder.has_value()) return;
    app.settings.media_cache_folder = folder->string();
    save_settings(app);
    apply_media_cache(app);
    app.settings_page_stale = true;
  });
  if (!app.settings.media_cache_folder.empty()) {
    where.emplace<Button>("Use the default place", [&app] {
      app.settings.media_cache_folder.clear();
      save_settings(app);
      apply_media_cache(app);
      app.settings_page_stale = true;
    });
  }

  into.emplace<Label>("What is in it").set_heading(true);

  const std::uintmax_t held = cutline::app::media_cache_bytes(dir);
  into.emplace<Label>(std::format("Holding {}.", bytes_label(held))).set_small(true);

  // Only when there is something to delete. A button that does nothing is a
  // control that teaches somebody not to trust the page — and the *row* only
  // when the button is in it, which `--check` is the reason for knowing: a row
  // holding nothing but a spacer has nothing to take its size from, and lands
  // nowhere at all.
  if (held > 0) {
    auto& tidy = into.emplace<Box>(Axis::Horizontal);
    tidy.emplace<Button>("Delete", [&app, dir] {
      (void)cutline::app::clear_media_cache(dir);
      app.settings_page_stale = true;
    });
    tidy.emplace<Spacer>();
  }

  // Said plainly, because "delete" beside anything to do with media is a word
  // that ought to frighten somebody until they know what it means here.
  into.emplace<Label>("Waveforms and filmstrips only. Nothing here is your footage,")
      .set_small(true);
  into.emplace<Label>("and anything deleted is worked out again when it is next needed.")
      .set_small(true);
#else
  into.emplace<Label>("This build has no media.").set_small(true);
#endif
}

/// The pages a settings window offers, in the order they are listed.
[[nodiscard]] std::vector<SettingsPage> pages_for(SettingsKind kind) {
  if (kind == SettingsKind::Project) {
    // One for now. Premiere's Project Settings also has Scratch Disks and
    // Ingest Settings, and neither has anything to say here yet — the list down
    // the side appears on its own once a second page does.
    return {SettingsPage{"General", build_sequence_page}};
  }
  return {SettingsPage{"General", build_general_page},
          SettingsPage{"Appearance", build_appearance_page},
          SettingsPage{"Audio Hardware", build_audio_page},
          SettingsPage{"Playback", build_playback_page},
          SettingsPage{"Graphics", build_graphics_page},
          SettingsPage{"Labels", build_labels_page},
          SettingsPage{"Timeline", build_timeline_page},
          SettingsPage{"Proxies", build_proxies_page},
          SettingsPage{"Media Cache", build_media_cache_page},
          SettingsPage{"Auto Save", build_autosave_page}};
}

/// Fills the pane with whichever category is chosen, and lights that one.
void refresh_settings_page(App& app) {
  if (app.settings_body == nullptr) return;

  // `clear_children` drops the host's own references on the way, which is what
  // makes this safe from inside a category button's click: the button that
  // asked is not in the pane, but a control the pointer was resting on might
  // be.
  app.settings_body->clear_children();

  const std::vector<SettingsPage> pages = pages_for(app.settings_kind);
  if (app.settings_page < pages.size() && pages[app.settings_page].build != nullptr) {
    pages[app.settings_page].build(app, *app.settings_body);
  }
  if (app.settings_tabs != nullptr) app.settings_tabs->set_current(app.settings_page);

  // Back to the top: a page arrived at half way down because the last one was
  // long is a page that looks like it starts in the middle of itself.
  if (auto* scroller = dynamic_cast<ScrollView*>(app.settings_body->parent())) {
    scroller->scroll_to(0.0);
  }
  if (app.settings_window != nullptr && app.settings_window->host != nullptr) {
    app.settings_window->host->request_layout();
    app.settings_window->dirty = true;
  }
}

void close_settings_dialog(App& app);

/// The settings window: a list of categories down the side, a pane beside it.
[[nodiscard]] std::unique_ptr<Widget> build_settings_interface(App& app, Shell* shell,
                                                               SettingsKind kind) {
  const std::vector<SettingsPage> pages = pages_for(kind);
  const std::string title =
      kind == SettingsKind::Project ? "Project Settings" : "Preferences";

  auto root = std::make_unique<Box>(Axis::Vertical);
  root->set_spacing(0.0);

  auto& caption = root->emplace<TitleBar>(title);
  if (shell != nullptr) shell->title_bar = &caption;
  caption.emplace<CaptionButton>(CaptionButton::Kind::Close,
                                 [&app] { close_settings_dialog(app); });

  // A dialog with nothing to cancel: every setting takes effect as it changes,
  // which is what the live preview behind it is for. One Close, which Enter
  // and Escape both press.
  auto& dialog = root->emplace<cutline::ui::Dialog>("");
  dialog.set_accept("Close", [] { return std::optional<std::string>{}; });
  dialog.set_cancel("");
  dialog.set_on_close([&app] { close_settings_dialog(app); });

  auto& split = dialog.body().emplace<Box>(Axis::Horizontal);
  split.set_spacing(Space::Group);
  // Takes the height under the title bar rather than only what its contents
  // need. Without this the category list stops wherever its last row does and
  // the rest of the window is empty background, which reads as unfinished
  // rather than as compact.
  split.set_fills_cross(true);

  app.settings_tabs = nullptr;
  // Only when there is a choice to make. A list of one is a heading pretending
  // to be a control, and Project Settings has one page until Scratch Disks
  // gives it a second.
  if (pages.size() > 1) {
    // A list rather than a column of buttons, which is what Premiere's is and
    // what it should always have been: the categories are one exclusive choice
    // among several, and that is the shape `MenuList` already draws — flat
    // rows, the chosen one lit, nothing pretending each name is pressable.
    std::vector<std::string> names;
    names.reserve(pages.size());
    for (const SettingsPage& page : pages) names.push_back(page.name);

    auto& tabs = split.emplace<MenuList>(std::move(names));
    tabs.set_fills_height(true);
    tabs.set_current(app.settings_page);
    tabs.set_on_choose([&app](std::size_t index) {
      if (app.settings_page == index) return;
      app.settings_page = index;
      refresh_settings_page(app);
    });
    app.settings_tabs = &tabs;
  }

  // Scrolled, because the pages are not the same height and never will be:
  // Labels is sixteen rows and Auto Save is two. A window sized to the tallest
  // would be mostly empty on every other page, and one sized to the shortest
  // would hide half of Labels with nothing saying so.
  auto& scroller = split.emplace<ScrollView>(Axis::Vertical);
  auto body = std::make_unique<cutline::ui::Form>();
  // Across the window rather than at the width of its widest row. A page that
  // only asked for its own width left a column of empty background down the
  // right of the window, with the page's scrollbar standing in the middle.
  body->set_fills_cross(true);
  app.settings_body = body.get();
  scroller.set_content(std::move(body));
  return root;
}

/// Opens one, or brings the open one forward.
void open_settings_dialog(App& app, SettingsKind kind) {
  if (app.settings_window != nullptr) {
    // Already open. Switching kind rather than opening a second window: two
    // modals would each be waiting for the other to be answered.
    if (app.settings_kind != kind) {
      close_settings_dialog(app);
    } else {
      SetForegroundWindow(app.settings_window->window);
      return;
    }
  }

  auto shell = std::make_unique<Shell>();
  shell->app = &app;
  shell->is_dialog = true;
  app.settings_kind = kind;
  app.settings_page = 0;
  shell->host = std::make_unique<WidgetHost>(build_settings_interface(app, shell.get(), kind));
  give_sounds(app, *shell->host);

  // Centred on the main window, which is where a dialog belongs and where the
  // eye already is.
  RECT owner{};
  GetWindowRect(app.main.window, &owner);
  // Wide enough for the widest page, which `--check` measures rather than
  // anybody guessing: at 460 it found the pane short of the sentence under the
  // durations by forty pixels, in every theme.
  // Or as big as it was last left.
  const auto [units_wide, units_tall] =
      dialog_size(app, kind == SettingsKind::Project ? "Project Settings" : "Preferences",
                  kind == SettingsKind::Project ? 470 : 580,
                  kind == SettingsKind::Project ? 430 : 420);
  // On the editor's display, which is where it opens.
  shell->scale = app.main.scale;
  const int width = to_pixels(units_wide, shell->scale);
  const int height = to_pixels(units_tall, shell->scale);
  const int x = owner.left + ((owner.right - owner.left) - width) / 2;
  const int y = owner.top + ((owner.bottom - owner.top) - height) / 2;

  const std::wstring title =
      kind == SettingsKind::Project ? L"Project Settings" : L"Preferences";
  const HWND window =
      CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, title.c_str(), WS_POPUP | WS_THICKFRAME, x,
                      y, width, height, app.main.window, nullptr, GetModuleHandleW(nullptr),
                      nullptr);
  shell->size_key = kind == SettingsKind::Project ? "Project Settings" : "Preferences";
  if (window == nullptr) return;

  shell->window = window;
  SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(shell.get()));

  const MARGINS shadow{0, 0, 1, 0};
  DwmExtendFrameIntoClientArea(window, &shadow);
  SetWindowPos(window, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

  app.settings_window = std::move(shell);
  refresh_settings_page(app);

  // Modal, the way Premiere's Preferences is. Disabling the owner is what makes
  // it one on Windows, and every path out of here goes through
  // `close_settings_dialog` so the owner cannot be left disabled — an editor
  // that ignores every click is a far worse fault than a dialog left open.
  EnableWindow(app.main.window, FALSE);
  ShowWindow(window, SW_SHOW);
  SetForegroundWindow(window);
}

void close_settings_dialog(App& app) {
  if (app.settings_window == nullptr) return;

  app.settings_body = nullptr;
  app.settings_tabs = nullptr;

  const HWND window = app.settings_window->window;
  remember_dialog_size(app, *app.settings_window);
  app.settings_window.reset();
  // The owner comes back *before* the dialog goes, so the system has an enabled
  // window to give the foreground to. Re-enabling afterwards leaves the focus
  // on some other application, which reads as the editor disappearing.
  EnableWindow(app.main.window, TRUE);
  SetForegroundWindow(app.main.window);
  if (window != nullptr) DestroyWindow(window);
  mark_dirty(app);
}

}  // namespace cutline::tool
