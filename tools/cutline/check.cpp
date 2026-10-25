/// The headless check and the benchmarks.
///
/// `--check` builds the whole window with no window behind it, in every
/// theme, lays it out, paints it, drives what it can and reports what is
/// empty, clipped or out of line. `--benchmark` and `--benchmark-playback`
/// time a frame and a playback turn.

#include "app.hpp"

namespace cutline::tool {

/// The first widget of a kind somewhere in a tree, for the headless check to
/// drive. The dock, to switch panels with; a swatch, to open a picker from.
template <typename T>
[[nodiscard]] T* find_widget(Widget& widget) {
  if (auto* found = dynamic_cast<T*>(&widget); found != nullptr) return found;
  for (const std::unique_ptr<Widget>& child : widget.children()) {
    if (T* found = find_widget<T>(*child); found != nullptr) return found;
  }
  return nullptr;
}

[[nodiscard]] DockView* find_dock(Widget& widget) { return find_widget<DockView>(widget); }

/// A fabricated envelope and filmstrip for every source.
///
/// Nothing in the sample project has a file behind it, so nothing is ever
/// decoded and neither the waveform nor the filmstrip would be drawn — which
/// would make them the two parts of the timeline that no check ever looks at and
/// no benchmark ever pays for. Built here rather than taken from the caches so
/// this works under the skia preset too, which is the one the nightly runs and
/// which has no decoder at all.
[[nodiscard]] cutline::editor::TimelineMedia sample_timeline_media() {
  auto envelope = std::make_shared<cutline::ui::Waveform>();
  envelope->buckets_per_second = 20.0;
  for (int i = 0; i < 20 * 120; ++i) {
    // Something with shape to it, so a flat line would be visibly wrong rather
    // than merely different.
    const double at = i / 20.0;
    const auto level = static_cast<float>(0.25 + 0.7 * std::abs(std::sin(at * 0.7)) *
                                                     std::abs(std::cos(at * 0.11)));
    envelope->minimum.push_back(-level);
    envelope->maximum.push_back(level);
  }

  // Flat colour in four shades, enough to see that tiles land where they should
  // and that the strip advances along the clip. At the size and spacing the
  // thumbnailer really makes them, so what the benchmark pays to draw a tile is
  // what the application pays.
  auto strip = std::make_shared<cutline::ui::Filmstrip>();
  for (int n = 0; n < 60; ++n) {
    const int f = n % 4;
    cutline::ui::FilmFrame frame;
    frame.t = n * 2.0;
    frame.width = 128;
    frame.height = 72;
    frame.rgba.resize(static_cast<std::size_t>(frame.width * frame.height * 4));
    for (std::size_t p = 0; p < frame.rgba.size(); p += 4) {
      frame.rgba[p] = static_cast<std::uint8_t>(40 + f * 50);
      frame.rgba[p + 1] = 90;
      frame.rgba[p + 2] = static_cast<std::uint8_t>(200 - f * 40);
      frame.rgba[p + 3] = 255;
    }
    strip->frames.push_back(std::move(frame));
  }

  return cutline::editor::TimelineMedia{
      .waveforms = [envelope](std::string_view, int) { return envelope; },
      .filmstrips = [strip](std::string_view) { return strip; }};
}

/// Times playback's turns over a real project rather than the sample.
///
/// The sample sequence has a handful of clips, and what a playback turn costs
/// the interface grows with the timeline. Refresh and paint are timed apart,
/// for the same reason layout and paint are below. On the hundred-clip montage
/// this was written for they came to 0.3 ms and 3.5 to 7 ms at 4K, which is
/// what showed that the 43 frames a second on screen were being lost somewhere
/// other than in drawing them.
[[nodiscard]] int benchmark_playback(const std::filesystem::path& path) {
  const auto loaded = cutline::editor::read_project(path);
  if (!loaded.has_value()) {
    std::println("could not open {}: {}", path.string(), loaded.error());
    return 1;
  }

  constexpr int kWidth = 3840;
  constexpr int kHeight = 2160;
  constexpr int kTurns = 120;
  const HWND hidden =
      CreateWindowExW(0, kWindowClass, L"Cutline benchmark", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

  std::println("-- playing {} at {}x{}", path.filename().string(), kWidth, kHeight);
  std::println("{:>10} {:>10} {:>10} {:>10} {:>10}", "theme", "refresh", "worst", "paint",
               "worst");
  for (const Theme& theme : built_in_themes()) {
    auto surface = cutline::ui::SkiaWindow::create(hidden, kWidth, kHeight);
    if (!surface.has_value()) {
      std::println("{:>10} no device", theme.name);
      continue;
    }

    App app;
    app.main.host = std::make_unique<WidgetHost>(build_interface(&app));
    app.timeline_media = sample_timeline_media();
    app.session.reset(loaded->project, path);
    refresh_all(app);
    WidgetHost& host = *app.main.host;

    auto* canvas = static_cast<SkCanvas*>((*surface)->begin_frame());
    if (canvas == nullptr) continue;
    const std::unique_ptr<SkiaPainter> painter = SkiaPainter::create(canvas);
    if (painter == nullptr) continue;
    const LayoutContext context{theme, *painter};
    const Rect client{0.0, 0.0, static_cast<double>(kWidth), static_cast<double>(kHeight)};
    host.resize(client, context);

    const double fps = app.session.project().sequence().fps > 0.0
                           ? app.session.project().sequence().fps
                           : 30.0;
    std::vector<double> refresh;
    std::vector<double> paint;
    for (int turn = -10; turn < kTurns; ++turn) {
      using clock = std::chrono::steady_clock;
      const auto from = clock::now();
      show_playback_frame(app, (turn + 10) / fps);
      const auto refreshed = clock::now();
      canvas->clear(SK_ColorBLACK);
      paint_surface(*painter, client, theme.style(cutline::ui::Part::Window));
      host.paint(*painter, theme);
      (*surface)->flush_and_wait();
      const auto painted = clock::now();
      // The first few are left out, for the pipelines the card compiles the
      // first time it meets them.
      if (turn < 0) continue;
      refresh.push_back(std::chrono::duration<double, std::milli>(refreshed - from).count());
      paint.push_back(std::chrono::duration<double, std::milli>(painted - refreshed).count());
    }
    (*surface)->present();

    const auto mean = [](const std::vector<double>& v) {
      return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
    };
    std::println("{:>10} {:>8.2f}ms {:>8.2f}ms {:>8.2f}ms {:>8.2f}ms", theme.name, mean(refresh),
                 std::ranges::max(refresh), mean(paint), std::ranges::max(paint));
    std::fflush(stdout);
  }
  if (hidden != nullptr) DestroyWindow(hidden);
  return 0;
}

/// Times the frame, so "it feels sluggish" becomes a number.
///
/// Written when interface drawing was Skia into a CPU raster surface blitted
/// with GDI, on the argument that a frame was cheap; measuring it at the sizes
/// people actually run at is what moved the drawing onto the GPU, and it is
/// how to know it still belongs there.
///
/// Layout and paint are timed apart because the fixes are different: a slow
/// paint argues for the GPU, a slow layout argues for doing less of it.
[[nodiscard]] int benchmark() {
  struct Size {
    const char* name;
    int width;
    int height;
  };
  constexpr std::array kSizes{
      Size{"1280x800", 1280, 800},
      Size{"1920x1080", 1920, 1080},
      Size{"2560x1440", 2560, 1440},
      Size{"3840x2160", 3840, 2160},
  };
  constexpr int kFrames = 30;

  // The same tree, the same themes, on the GPU. A hidden window, because a
  // swapchain needs one and none of this needs to be looked at.
  const HWND hidden =
      CreateWindowExW(0, kWindowClass, L"Cutline benchmark", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

  std::println("-- on the GPU");
  std::println("{:<12} {:>10} {:>12}", "size", "theme", "paint");
  for (const Theme& theme : built_in_themes()) {
    for (const Size& size : kSizes) {
      auto surface = cutline::ui::SkiaWindow::create(hidden, size.width, size.height);
      if (!surface.has_value()) {
        std::println("{:<12} {:>10} {:>12}", size.name, theme.name, "no device");
        continue;
      }

      // A real document rather than a null one. With no app the timeline holds
      // an empty model, so every number this printed was for a window with no
      // clips in it — which is not the thing anybody is worried about the cost
      // of. Now it paints the sample sequence with waveforms and filmstrips on
      // it, which is what a populated timeline actually costs.
      App app;
      app.main.host = std::make_unique<WidgetHost>(build_interface(&app));
      app.timeline_media = sample_timeline_media();
      refresh_timeline(app);
      WidgetHost& host = *app.main.host;

      const Rect client{0.0, 0.0, static_cast<double>(size.width),
                        static_cast<double>(size.height)};

      // One buffer, drawn into repeatedly and never presented. Presenting waits
      // for the display, so timing through it reports the refresh rate and
      // nothing whatever about the drawing.
      auto* canvas = static_cast<SkCanvas*>((*surface)->begin_frame());
      if (canvas == nullptr) continue;

      const std::unique_ptr<SkiaPainter> painter = SkiaPainter::create(canvas);
      if (painter == nullptr) continue;
      const LayoutContext context{theme, *painter};

      const auto draw_one = [&] {
        host.resize(client, context);
        canvas->clear(SK_ColorBLACK);
        paint_surface(*painter, client, theme.style(cutline::ui::Part::Window));
        host.paint(*painter, theme);
        // Waits for the GPU, so what is timed is the drawing finishing rather
        // than merely being asked for.
        (*surface)->flush_and_wait();
      };

      // Warmed up first: a GPU compiles the pipelines it needs the first time
      // it meets them, and counting that would say a frame costs what the
      // first one did.
      for (int warm = 0; warm < 10; ++warm) draw_one();

      double paint_ms = 0.0;
      for (int i = 0; i < kFrames; ++i) {
        const auto from = std::chrono::steady_clock::now();
        draw_one();
        const auto to = std::chrono::steady_clock::now();
        paint_ms += std::chrono::duration<double, std::milli>(to - from).count();
      }
      (*surface)->present();

      std::println("{:<12} {:>10} {:>10.2f}ms {}", size.name, theme.name, paint_ms / kFrames,
                   (*surface)->is_software() ? "(software rasteriser)" : "");
      std::fflush(stdout);
    }
  }
  if (hidden != nullptr) DestroyWindow(hidden);

  std::println("");
  std::println("-- on the CPU, which is what the headless check still uses");
  std::println("{:<12} {:>10} {:>10} {:>10}", "size", "layout", "paint", "total");

  for (const Theme& theme : built_in_themes()) {
  std::println("-- {}", theme.name);
  for (const Size& size : kSizes) {
    const sk_sp<SkSurface> surface =
        SkSurfaces::Raster(SkImageInfo::MakeN32Premul(size.width, size.height));
    if (surface == nullptr) {
      std::println("{}: no raster surface", size.name);
      return 1;
    }

    App app;
    app.main.host = std::make_unique<WidgetHost>(build_interface(&app));
    app.timeline_media = sample_timeline_media();
    refresh_timeline(app);
    WidgetHost& host = *app.main.host;

    const std::unique_ptr<SkiaPainter> painter = SkiaPainter::create(surface->getCanvas());
    const LayoutContext context{theme, *painter};
    const Rect client{0.0, 0.0, static_cast<double>(size.width),
                      static_cast<double>(size.height)};
    host.resize(client, context);

    double layout_ms = 0.0;
    double paint_ms = 0.0;
    for (int i = 0; i < kFrames; ++i) {
      using clock = std::chrono::steady_clock;

      const auto laid_out_from = clock::now();
      // What a resize costs. A drag of a divider pays this every frame.
      host.resize(client, context);
      const auto laid_out_to = clock::now();

      surface->getCanvas()->clear(SK_ColorBLACK);
      paint_surface(*painter, client, theme.style(cutline::ui::Part::Window));
      host.paint(*painter, theme);
      // Skia records lazily, so nothing is really drawn until the pixels are
      // asked for. Timing without this measures the recording and not the work.
      SkPixmap pixels;
      surface->peekPixels(&pixels);
      const auto painted_to = clock::now();

      layout_ms += std::chrono::duration<double, std::milli>(laid_out_to - laid_out_from).count();
      paint_ms += std::chrono::duration<double, std::milli>(painted_to - laid_out_to).count();
    }

    layout_ms /= kFrames;
    paint_ms /= kFrames;
    std::println("{:<12} {:>9.2f}ms {:>9.2f}ms {:>9.2f}ms", size.name, layout_ms, paint_ms,
                 layout_ms + paint_ms);
    // Flushed as it goes, so a theme that turns out to be pathologically slow
    // is named by the output rather than having to be guessed at from a hang.
    std::fflush(stdout);
  }
  }
  return 0;
}

/// Renders one frame of every theme with no window, and reports what came out.
///
/// The unit tests cover each control on its own; this covers the whole tree
/// laid out together, which is where a widget that demands more room than
/// exists or a container that hands out nothing would show up. Being able to
/// run it without a display also means a crash in the paint path is a command
/// away rather than something to notice by launching.
///
/// Given a directory, each theme's last frame is written there as a PNG. The
/// check otherwise reports counts and a fingerprint, and when a fingerprint
/// changes there is no way to see *how* — which is the one question anybody
/// actually has at that point.
[[nodiscard]] int self_check(std::string_view shots) {
  constexpr int kWidth = 1280;
  constexpr int kHeight = 800;

  int failures = 0;
  std::vector<std::string> fingerprints;

  // Made rather than demanded. Being told a directory does not exist, once per
  // theme, by libpng, is not a useful way to learn that.
  if (!shots.empty()) {
    std::error_code ignored;
    std::filesystem::create_directories(shots, ignored);
  }

  for (const Theme& theme : built_in_themes()) {
    const sk_sp<SkSurface> surface =
        SkSurfaces::Raster(SkImageInfo::MakeN32Premul(kWidth, kHeight));
    if (surface == nullptr) {
      std::println("{}: no raster surface", theme.id);
      ++failures;
      continue;
    }

    WidgetHost host(build_interface(nullptr));
    const std::unique_ptr<SkiaPainter> painter = SkiaPainter::create(surface->getCanvas());
    const LayoutContext context{theme, *painter};
    const Rect client{0.0, 0.0, kWidth, kHeight};

    host.resize(client, context);
    surface->getCanvas()->clear(SK_ColorBLACK);
    paint_surface(*painter, client, theme.style(cutline::ui::Part::Window));
    host.paint(*painter, theme);

    // Every widget has to have ended up somewhere real and inside the window.
    // A degenerate rectangle here is a layout that gave a panel nothing, which
    // on screen reads as a missing feature rather than as a bug.
    int empty = 0;
    int escaped = 0;
    int clipped = 0;
    int squeezed = 0;
    int misaligned = 0;
    int counted = 0;
    /// The window being walked. The main one, until a settings or export
    /// window is laid out at its own size -- a widget inside that has to be
    /// inside *it*, which is how the Export window's own buttons were found
    /// sitting below its bottom edge.
    Rect frame{0.0, 0.0, static_cast<double>(kWidth), static_cast<double>(kHeight)};
    /// Whether something is inside a scroll view, where being past the bottom
    /// is the point.
    const auto scrolled = [](const Widget& widget) {
      for (const Widget* up = widget.parent(); up != nullptr; up = up->parent()) {
        if (dynamic_cast<const ScrollView*>(up) != nullptr) return true;
      }
      return false;
    };

    /// Walks a subtree, carrying the nearest ancestor that clips its children.
    ///
    /// The clip is what makes a control unreachable rather than merely untidy:
    /// a row of buttons wider than the panel holding it looks fine in the widget
    /// tree and has its last button cut in half on screen. That happened — three
    /// generator buttons in the project panel's toolbar, the third of them
    /// sliced by the panel's edge — and nothing here noticed, because everything
    /// was inside the *window*.
    ///
    /// Horizontally only. A column overflowing its scroll view vertically is the
    /// entire point of a scroll view.
    const auto walk_within = [&](this const auto& self, const Widget& widget,
                                 const Widget* clipper) -> void {
      ++counted;
      // Two exceptions, and both are "no room was the right answer".
      //
      // A spacer exists to absorb whatever is left over, and when a column has
      // overflowed its panel there is none.
      //
      // A hidden widget is one something has deliberately taken out of the
      // layout — the busy indicator is hidden whenever the workers are idle,
      // which is nearly always. Counting those as faults would mean the only
      // way to have a control that comes and goes is to rebuild the panel
      // around it.
      const bool allowed_to_be_empty =
          !widget.visible() || dynamic_cast<const Spacer*>(&widget) != nullptr;
      if (widget.bounds().empty() && !allowed_to_be_empty) ++empty;
      if (widget.visible() && !widget.bounds().empty()) {
        const Rect& at = widget.bounds();
        const bool across = at.x < frame.x - 0.5 || at.right() > frame.right() + 0.5;
        const bool down = !scrolled(widget) &&
                          (at.y < frame.y - 0.5 || at.bottom() > frame.bottom() + 0.5);
        if (across || down) ++escaped;
      }

      // A widget narrower than it asked to be.
      //
      // This is the fault the two tests above cannot see, and it has now caused
      // three bugs: a label drawn on top of the number beside it, a tab's close
      // button drawn over the last letter of its title, and a row of generator
      // buttons sliced by a panel edge. In every case the widget was neither
      // empty nor outside its clip — it had simply been given less room than its
      // content needs, and drew outside itself.
      //
      // Anything that does not *grow* is checked against the width it asked
      // for. A widget with `grow` is meant to take whatever is going and has no
      // natural size to be short of; everything else stated one, and getting
      // less than it stated is the fault.
      //
      // Shrinkability is deliberately not an excuse. A label declares itself
      // shrinkable so that it gives way before a panel is forced wider, and
      // being cut off is still a thing somebody needs to be told about — it is
      // a title nobody can read.
      if (const cutline::ui::LayoutItem wanted = widget.sizing(Axis::Horizontal, context);
          wanted.grow == 0.0 && !widget.bounds().empty() &&
          widget.bounds().width + 0.5 < wanted.basis) {
        ++squeezed;
      }

      if (clipper != nullptr && !widget.bounds().empty() &&
          dynamic_cast<const ScrollView*>(clipper) == nullptr &&
          (widget.bounds().x < clipper->bounds().x - 0.5 ||
           widget.bounds().right() > clipper->bounds().right() + 0.5)) {
        ++clipped;
      }

      // Text across a row reads along one line. Labels, buttons, fields and
      // dropdowns each centre their text in their own bounds, so in a row
      // their middles have to agree -- a label left at its own height at the
      // top of a row of taller buttons sits half a line above them, and
      // nothing above this would ever say so, because nothing is clipped,
      // squeezed or outside anything.
      if (const auto* row = dynamic_cast<const Box*>(&widget);
          row != nullptr && row->axis() == Axis::Horizontal && widget.visible()) {
        std::optional<double> line;
        for (const auto& child : row->children()) {
          const Widget* item = child.get();
          if (!item->visible() || item->bounds().empty()) continue;
          const auto* field = dynamic_cast<const TextField*>(item);
          const bool text = dynamic_cast<const Label*>(item) != nullptr ||
                            dynamic_cast<const Button*>(item) != nullptr ||
                            dynamic_cast<const Dropdown*>(item) != nullptr ||
                            (field != nullptr && !field->multiline());
          if (!text) continue;
          const double middle = item->bounds().y + item->bounds().height / 2.0;
          if (!line.has_value()) {
            line = middle;
          } else if (std::abs(middle - *line) > 1.0) {
            ++misaligned;
            break;
          }
        }
      }

      const Widget* next = widget.clips_children() ? &widget : clipper;
      for (const auto& child : widget.children()) self(*child, next);
    };
    const auto walk = [&](const Widget& widget) { walk_within(widget, nullptr); };
    walk(host.root());

    // Every panel, not only the ones the default arrangement happens to show.
    // A tabbed-away panel is built the first time it is shown, so without this
    // the check never lays out the inspector at all — and the panel nobody
    // looks at is exactly where a theme breaks unnoticed.
    if (DockView* dock = find_dock(host.root()); dock != nullptr) {
      DockLayout layout = default_layout();
      for (const auto& [id, title] : kPanels) {
        cutline::ui::activate_panel(layout, id);
        dock->set_node(layout.root);
        host.resize(client, context);
        host.paint(*painter, theme);
        walk(host.root());
      }

      // And every built-in arrangement, not only the default: each gives the
      // monitors and the timeline a different share of the window, and a row
      // of controls that fits one of them can be cut in half by another.
      for (const cutline::editor::Workspace& workspace : cutline::editor::built_in_workspaces()) {
        dock->set_node(workspace.layout.root);
        host.resize(client, context);
        host.paint(*painter, theme);
        walk(host.root());
      }
      dock->set_node(layout.root);
    }

    // With a document behind it, and something selected.
    //
    // `build_interface(nullptr)` builds every panel with no session, which is
    // what the pass above checks — but an inspector with nothing selected is
    // two labels, and the panel that is actually made of controls is never laid
    // out at all. That is the gap a dead Export button and a missing shader
    // both hid in. Every effect in the catalogue goes on the clip, so each kind
    // of parameter row — slider, toggle, colour — is laid out in every theme.
    {
      App app;
      // Given a project rather than inheriting one. A new document is empty —
      // the way every editor's is — so the panels this pass exists to lay out
      // would have nothing in them and prove nothing.
      app.session.reset(check_project());
      // Marked, so the ruler draws its span. Nothing else in the check would
      // put a mark on the sequence, and the bar is painted in the one place a
      // theme has never otherwise been asked about.
      app.session.apply(cutline::core::set_in_point(app.session.project(), 1.0));
      app.session.apply(cutline::core::set_out_point(app.session.project(), 6.0));
      // And two markers, one of them coloured: a marker wearing the theme's
      // text colour and one wearing its own are drawn from different places.
      app.session.apply(cutline::core::add_marker(app.session.project(), 2.5, "cue"));
      app.session.apply(
          cutline::core::add_marker(app.session.project(), 8.0, "end", "#ffa000"));
      // A bin with something in it, so the library's own folders and the bar
      // that makes them are laid out rather than only the catalogue's.
      cutline::editor::create_bin(app.bins, "Favourites");
      cutline::editor::add_to_bin(app.bins, "Favourites", "video:blur");
      cutline::editor::create_bin(app.bins, "Nothing In Here Yet");

      app.main.host = std::make_unique<WidgetHost>(build_interface(&app));

      // Opened, or the rows the bins add are never laid out — every folder in
      // the library starts collapsed, which is right on screen and useless here.
      if (app.library != nullptr) {
        app.library->set_open(std::string(cutline::editor::kBinFolder), true);
        app.library->set_open(cutline::editor::bin_folder("Favourites"), true);
        app.library->set_open(cutline::editor::bin_folder("Nothing In Here Yet"), true);
      }

      // Shown, not merely present. Only the active panel in a group is built,
      // and in the default arrangement the inspector is a tab behind another —
      // so without this the panel under test is never constructed at all.
      if (DockView* dock = find_dock(app.main.host->root()); dock != nullptr) {
        DockLayout layout = layout_of(app);
        // The mixer first, which holds a strip per audio track — an
        // arrangement that grows with the project and so is worth laying out
        // rather than assuming. Measured where it is and then put back behind
        // the inspector: Premiere's Editing keeps both behind the source, and
        // leaving the mixer in front put the inspector out of the tree, where
        // opening its colour swatch had no window to open in.
        cutline::ui::activate_panel(layout, "audio");
        dock->set_node(layout.root);
        app.main.host->resize(client, context);
        app.main.host->paint(*painter, theme);
        walk(app.main.host->root());

        cutline::ui::activate_panel(layout, "effects");
        dock->set_node(layout.root);
      }
      app.main.host->resize(client, context);

      // The first video clip with another abutting it, so the transition
      // controls are among the ones laid out. The topmost track's clip is on
      // its own, and a clip with no join has no transition to configure.
      std::string clip_id;
      for (const auto& track : app.session.project().sequence().tracks) {
        if (track.kind != cutline::core::TrackKind::Video) continue;
        for (const auto& clip : track.clips) {
          if (clip_id.empty()) clip_id = clip.id;
          if (cutline::editor::clip_transition(app.session.project(), clip.id).joins) {
            clip_id = clip.id;
            break;
          }
        }
        if (!clip_id.empty() &&
            cutline::editor::clip_transition(app.session.project(), clip_id).joins) {
          break;
        }
      }

      if (!clip_id.empty()) {
        app.session.select_one(clip_id);
        // A transition on the join, so the timeline draws one and the panel
        // builds the controls for it. Nothing else in the check would.
        app.session.apply(cutline::editor::set_transition(
            app.session.project(), clip_id, cutline::core::TransitionKind::Dissolve, 1.0));
        for (const cutline::editor::EffectChoice& choice : cutline::editor::addable_effects()) {
          app.session.apply(
              cutline::editor::add_effect(app.session.project(), clip_id, choice.type));
        }
        // One of each animated, so the keyframe markers are laid out too. They
        // only exist once a parameter is animated, so a clip with none would
        // leave that half of every row untested.
        app.session.apply(cutline::editor::set_clip_parameter_animated(
            app.session.project(), clip_id, cutline::editor::ClipParam::Opacity, true, 0.0));
        // And a *paired* one, which is the widest row the panel ever builds: a
        // name, two numbers, a reset and the three keyframe controls. Animating
        // only Opacity left that arrangement untested, and it did not fit — the
        // property's own name was squeezed away to nothing on screen while this
        // check reported everything well.
        //
        // Anchor Point rather than Position, because it is the same shape of
        // row with the longer name on it, and the longer name is what decides
        // whether the arrangement fits.
        app.session.apply(cutline::editor::set_clip_parameter_animated(
            app.session.project(), clip_id, cutline::editor::ClipParam::AnchorX, true, 0.0));
        app.session.apply(cutline::editor::set_clip_parameter_animated(
            app.session.project(), clip_id, cutline::editor::ClipParam::AnchorY, true, 0.0));
        app.session.apply(cutline::editor::set_effect_parameter_animated(
            app.session.project(), clip_id, 0, "amount", true, 0.0));
        // And a mask on one of them, so the seven rows and the checkbox it
        // brings with it are laid out rather than only the shape chooser. A
        // free-drawn one, because it is the shape that draws the most: an
        // outline of its own and a handle on every corner.
        app.session.apply(cutline::editor::set_effect_mask(
            app.session.project(), clip_id, 0,
            cutline::editor::EffectMaskRow{.shape = cutline::core::MaskShape::Path}));
        // And one of its numbers animated, so a mask row is laid out wearing
        // the whole navigator — stopwatch, both arrows, the marker and the
        // curve picker — rather than the bare slider. It is the widest row the
        // mask has, which is what decides whether the arrangement fits.
        app.session.apply(cutline::editor::set_effect_parameter_animated(
            app.session.project(), clip_id, 0, "mask.rotation", true, 0.0));
      }

      app.timeline_media = sample_timeline_media();

      // The panel was built before any of those edits, so the timeline is still
      // holding the model it started with — the transition and the marks would
      // never be drawn without this.
      refresh_timeline(app);
      refresh_inspector(app);
      if (app.inspector == nullptr || app.inspector->children().empty()) {
        std::println("{}: the inspector built nothing for a selected clip", theme.id);
        ++failures;
      }
      app.main.host->update_layout(context);
      app.main.host->paint(*painter, theme);
      walk(app.main.host->root());

      // And an audio clip, whose panel is a different stack from a different
      // registry. Every audio effect on it, for the same reason every visual
      // one goes on the clip above.
      std::string audio_clip;
      for (const auto& track : app.session.project().sequence().tracks) {
        if (track.kind != cutline::core::TrackKind::Audio || track.clips.empty()) continue;
        audio_clip = track.clips.front().id;
        break;
      }
      if (!audio_clip.empty()) {
        app.session.select_one(audio_clip);
        for (const cutline::editor::EffectChoice& choice :
             cutline::editor::addable_audio_effects()) {
          app.session.apply(cutline::editor::add_audio_effect(app.session.project(), audio_clip,
                                                              choice.type));
        }
        // The panner animated, so its row is laid out with the keyframe
        // controls on it rather than only in its resting arrangement.
        app.session.apply(cutline::editor::set_clip_parameter_animated(
            app.session.project(), audio_clip, cutline::editor::ClipParam::Pan, true, 0.0));
        // And an audio effect parameter, which is the same row in the other
        // stack and has the registry's longest labels on it.
        for (const cutline::editor::EffectRow& row :
             cutline::editor::clip_audio_effects(app.session.project(), audio_clip)) {
          for (const cutline::editor::EffectParamRow& param : row.params) {
            app.session.apply(cutline::editor::set_audio_effect_parameter_animated(
                app.session.project(), audio_clip, row.index, param.key, true, 0.0));
          }
        }
        refresh_inspector(app);
        app.main.host->update_layout(context);
        app.main.host->paint(*painter, theme);
        walk(app.main.host->root());
      } else {
        std::println("{}: no audio clip to build an audio panel from", theme.id);
        ++failures;
      }

      // A colour matte with a gradient on it, whose panel has an angle and two
      // swatches, and an adjustment layer, whose panel says what it is for. Both
      // are generated media the editor makes rather than imports, and neither
      // panel exists anywhere else in this check.
      std::string matte_clip;
      app.session.apply(cutline::editor::add_color_matte_at(
          app.session.project(),
          cutline::editor::MatteFill{
              .color = "#204080",
              .gradient = cutline::core::MatteGradient{.color2 = "#000000", .angle = 45.0}},
          0.0, {}, &matte_clip));
      if (!matte_clip.empty()) {
        app.session.select_one(matte_clip);
        refresh_inspector(app);
        app.main.host->update_layout(context);
        app.main.host->paint(*painter, theme);
        walk(app.main.host->root());
      }

      std::string adjust_clip;
      app.session.apply(cutline::editor::add_adjustment_layer_at(app.session.project(), 0.0, {},
                                                                 &adjust_clip));
      if (!adjust_clip.empty()) {
        app.session.select_one(adjust_clip);
        refresh_inspector(app);
        app.main.host->update_layout(context);
        app.main.host->paint(*painter, theme);
        walk(app.main.host->root());
      }

      // And again for a title, whose panel has controls no other clip does —
      // the text field among them, which nothing else in the interface uses yet
      // and which would otherwise never be laid out in any theme.
      std::string title_clip;
      app.session.apply(cutline::editor::add_title_at(app.session.project(),
                                                      cutline::editor::default_title_spec(), 0.0,
                                                      {}, &title_clip));
      if (!title_clip.empty()) {
        app.session.select_one(title_clip);
        refresh_inspector(app);
        app.main.host->update_layout(context);
        app.main.host->paint(*painter, theme);
        walk(app.main.host->root());
      }

      // And the colour picker, which lives on the popup layer and is therefore
      // in no tree the walk above reaches. Nothing else would ever lay it out:
      // it only exists while it is open.
      if (ColorSwatch* swatch = find_widget<ColorSwatch>(*app.inspector); swatch != nullptr) {
        swatch->open();
        app.main.host->update_layout(context);
        app.main.host->paint(*painter, theme);
        if (Widget* picker = app.main.host->popup(); picker != nullptr) walk(*picker);
      } else {
        std::println("{}: a title's panel offered no colour to pick", theme.id);
        ++failures;
      }

      // Every menu on the bar, and every submenu in each of them. Their rows
      // carry ticks, shortcuts and arrows in columns of their own, and a theme
      // whose wider font pushed a label into its shortcut would show up
      // nowhere else. A submenu opens only when its row is pointed at, so each
      // one is pointed at in turn — measuring the bar's menus closed would
      // have measured none of the cascades.
      {
        constexpr std::array kMenus{
            std::pair{"File", &file_menu},         std::pair{"Edit", &edit_menu},
            std::pair{"Clip", &clip_menu},         std::pair{"Sequence", &sequence_menu},
            std::pair{"Markers", &markers_menu},   std::pair{"View", &view_menu},
            std::pair{"Window", &window_menu},     std::pair{"Help", &help_menu},
        };
        for (const auto& [name, build] : kMenus) {
          app.main.host->close_popup();
          app.main.host->open_popup(std::make_unique<MenuList>(build(app)), settings_anchor());
          app.main.host->update_layout(context);
          app.main.host->paint(*painter, theme);
          auto* menu = dynamic_cast<MenuList*>(app.main.host->popup());
          if (menu == nullptr) {
            std::println("{}: the {} menu did not open", theme.id, name);
            ++failures;
            continue;
          }
          walk(*menu);
          for (std::size_t row = 0; row < menu->entries().size(); ++row) {
            if (menu->entries()[row].submenu.empty() || !menu->entries()[row].enabled) continue;
            const Rect at = menu->row_rect(row);
            app.main.host->mouse_move(MouseEvent{.x = at.x + 8.0, .y = at.y + at.height * 0.5});
            app.main.host->update_layout(context);
            app.main.host->paint(*painter, theme);
            if (menu->submenu() == nullptr) {
              std::println("{}: {} > {} did not open", theme.id, name, menu->entries()[row].label);
              ++failures;
              continue;
            }
            walk(*menu->submenu());
          }
        }
        app.main.host->close_popup();
      }

      // The dialogues — and none of them had ever been laid out in any theme.
      //
      // A popup is not in the tree this walk starts from, so six boxes of
      // controls were being measured nowhere at all, several of them holding
      // the widest rows in the application. The colour picker and the window
      // menu above are popups too, and each was added here one at a time after
      // something went wrong in one of them; this is the rest of the layer,
      // added before rather than after.
      {
        const auto measure_dialog = [&](std::string_view what) {
          app.main.host->update_layout(context);
          app.main.host->paint(*painter, theme);
          if (Widget* dialog = app.main.host->popup(); dialog != nullptr) {
            walk(*dialog);
          } else {
            std::println("{}: the {} dialogue did not open", theme.id, what);
            ++failures;
          }
          app.main.host->close_popup();
          app.main.host->update_layout(context);
        };

        // On a video clip, which is what the first two are reached from.
        if (!clip_id.empty()) app.session.select_one(clip_id);
        open_speed_dialog(app, app.session.selected_group());
        measure_dialog("speed and duration");

        // Something copied and something selected, which is the state Paste
        // Attributes greys itself out of existence without.
        run_command(app, cutline::editor::Command::Copy);
        open_paste_attributes_dialog(app);
        measure_dialog("paste attributes");

        if (!app.session.project().sequence().markers.empty()) {
          open_marker_dialog(app, 0);
          measure_dialog("marker");
        }

        if (!audio_clip.empty()) {
          app.session.select_one(audio_clip);
          open_channel_dialog(app, app.session.selected_group());
          measure_dialog("audio channels");
        }

        open_loudness_dialog(app);
        measure_dialog("loudness");

        // A source with a rate to conform from, which is the only kind the
        // entry is offered on. Reported when there is none rather than quietly
        // skipped: a dialogue nothing lays out is the exact fault this whole
        // pass was added for, and a loop that finds nothing looks identical to
        // a loop that found everything.
        const auto interpretable =
            std::ranges::find_if(app.session.project().media, [](const cutline::core::Media& m) {
              return cutline::core::can_interpret(m);
            });
        if (interpretable != app.session.project().media.end()) {
          open_interpret_dialog(app, interpretable->id);
          measure_dialog("interpret footage");
        } else {
          std::println("{}: no source the interpret box could be opened on", theme.id);
          ++failures;
        }

        // Four marks that disagree, which is the only state Fit Clip exists in
        // — with three the fourth is derived and there is nothing to ask about.
        // The sequence is already marked from 1 to 6 above, so a three-second
        // source span is a conflict.
        if (!app.session.project().media.empty()) {
          const std::string source = app.session.project().media.front().id;
          app.session.set_source_media(source);
          app.session.apply(
              cutline::core::set_source_in_point(app.session.project(), source, 0.0));
          app.session.apply(
              cutline::core::set_source_out_point(app.session.project(), source, 3.0));
          // And somewhere for it to land: nothing is targeted in a fresh
          // project, and an edit with no target has no answer to *where*.
          for (const cutline::core::Track& track : app.session.project().sequence().tracks) {
            if (track.kind != cutline::core::TrackKind::Video) continue;
            app.session.apply(cutline::core::update_track(
                app.session.project(), track.id,
                cutline::core::TrackPropsPatch{.targeted = true}));
            break;
          }
          open_fit_dialog(app);
          measure_dialog("fit clip");
        }
      }

      // The settings windows, every page of both. They are real windows now and
      // nothing else here would ever lay one out — and they are rows of numbers
      // and presets, which is exactly the shape a theme with a wider font
      // squeezes. Laid out in a host of their own at the size the dialog opens
      // at, since that is the only width they ever have.
      // A chosen proxy folder, so the page is measured in its fuller state: it
      // grows a button and a much longer label there, and that is the shape
      // that can overflow. A page checked only in its emptiest arrangement is a
      // page checked where nothing goes wrong.
      app.settings.proxy_folder = "E:/Fast Scratch/Cutline Proxies";
      for (const SettingsKind kind : {SettingsKind::Application, SettingsKind::Project}) {
        const std::vector<SettingsPage> pages = pages_for(kind);
        for (std::size_t page = 0; page < pages.size(); ++page) {
          app.settings_kind = kind;
          app.settings_page = page;
          WidgetHost dialog(build_settings_interface(app, nullptr, kind));
          refresh_settings_page(app);
          frame = Rect{0.0, 0.0, kind == SettingsKind::Project ? 470.0 : 580.0,
                       kind == SettingsKind::Project ? 430.0 : 420.0};
          dialog.resize(frame, context);
          dialog.paint(*painter, theme);
          walk(dialog.root());
        }
      }
      app.settings.proxy_folder.clear();
      app.settings_body = nullptr;
      app.settings_tabs = nullptr;

      // The Keyboard Shortcuts window: as it opens, with a command waiting
      // for its key, and with Ctrl's layer of the keyboard drawn.
      app.keyboard_window = std::make_unique<Shell>();
      for (int pass = 0; pass < 3; ++pass) {
        app.keyboard_capturing = pass == 1 ? "edit.undo" : "";
        app.keyboard_modifiers = cutline::ui::Modifiers{.control = pass == 2};
        WidgetHost keys(build_keyboard_interface(app, nullptr));
        refresh_keyboard_dialog(app);
        frame = Rect{0.0, 0.0, 820.0, 660.0};
        keys.resize(frame, context);
        keys.paint(*painter, theme);
        walk(keys.root());
      }
      app.keyboard_window.reset();
      app.keyboard_view = nullptr;
      app.keyboard_list = nullptr;
      app.keyboard_custom = nullptr;
      app.keyboard_note_label = nullptr;
      app.keyboard_search_field = nullptr;
      app.keyboard_capturing.clear();
      app.keyboard_modifiers = {};
      frame = Rect{0.0, 0.0, static_cast<double>(kWidth), static_cast<double>(kHeight)};

#if CUTLINE_HAVE_PREVIEW
      // The Export window, at the size it opens at.
      {
        Shell scratch;
        scratch.app = &app;
        WidgetHost window(build_export_interface(&app, &scratch));
        frame = Rect{0.0, 0.0, static_cast<double>(kExportWindowWidth),
                     static_cast<double>(kExportWindowHeight)};
        window.resize(frame, context);
        window.paint(*painter, theme);
        walk(window.root());
        frame = Rect{0.0, 0.0, static_cast<double>(kWidth), static_cast<double>(kHeight)};
        app.export_controls = {};
      }
#endif
    }

    // And the theme has to reach the pixels. Sampling a scatter of points
    // rather than one keeps a theme that happens to share a background colour
    // from looking like a theme that never got applied.
    std::string fingerprint;
    SkPixmap pixels;
    if (surface->peekPixels(&pixels)) {
      for (int y = 20; y < kHeight; y += 97) {
        for (int x = 20; x < kWidth; x += 89) {
          fingerprint += std::format("{:08x}", pixels.getColor(x, y));
        }
      }
    }
    fingerprints.push_back(fingerprint);

    if (!shots.empty() && pixels.addr() != nullptr) {
      const std::string path = std::format("{}/{}.png", shots, theme.id);
      SkFILEWStream file(path.c_str());
      if (!SkPngEncoder::Encode(&file, pixels, {})) {
        std::println("{}: could not write {}", theme.id, path);
        ++failures;
      }
    }

    std::println(
        "{:<10} {:>3} widgets, {} empty, {} outside the window, {} clipped away, {} squeezed, "
        "{} off the line",
        theme.id, counted, empty, escaped, clipped, squeezed, misaligned);
    if (empty > 0 || escaped > 0 || clipped > 0 || squeezed > 0 || misaligned > 0) ++failures;
  }

  // The tool cursors, which are drawings rather than system handles and so can
  // come out empty without anything else noticing — a cursor that vanishes over
  // the timeline looks like the pointer has been lost rather than like a bug.
  // Counted rather than eyeballed: what matters is that the mark reached the
  // pixels and that it did not fill the whole square, which would be a solid
  // block following the pointer around.
  for (const cutline::ui::IconButton::Icon icon :
       {cutline::ui::IconButton::Icon::Razor, cutline::ui::IconButton::Icon::RateStretch,
        cutline::ui::IconButton::Icon::Slip, cutline::ui::IconButton::Icon::Slide,
        cutline::ui::IconButton::Icon::Ripple, cutline::ui::IconButton::Icon::Roll}) {
    const sk_sp<SkSurface> art = draw_cursor_art(icon);
    SkPixmap marks;
    if (art == nullptr || !art->peekPixels(&marks)) {
      std::println("a tool cursor could not be drawn at all");
      ++failures;
      continue;
    }

    int inked = 0;
    for (int y = 0; y < marks.height(); ++y) {
      for (int x = 0; x < marks.width(); ++x) {
        if (SkColorGetA(marks.getColor(x, y)) > 0) ++inked;
      }
    }
    const int total = marks.width() * marks.height();
    if (inked == 0) {
      std::println("a tool cursor drew nothing");
      ++failures;
    } else if (inked > total / 2) {
      std::println("a tool cursor covered {} of {} pixels", inked, total);
      ++failures;
    }
  }

  for (std::size_t i = 0; i < fingerprints.size(); ++i) {
    for (std::size_t j = i + 1; j < fingerprints.size(); ++j) {
      if (fingerprints[i] == fingerprints[j]) {
        std::println("{} and {} painted identically — the theme is not reaching the pixels",
                     built_in_themes()[i].id, built_in_themes()[j].id);
        ++failures;
      }
    }
  }
  return failures == 0 ? 0 : 1;
}

}  // namespace cutline::tool
