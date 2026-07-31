/// A window to actually look at the interface in.
///
/// Everything under `src/ui` is tested without a window on purpose, and that
/// catches a great deal — but not whether the result is something a person
/// would want to use. This puts the real widget tree on screen, in each of the
/// built-in themes, so the claim that they differ in *chrome* rather than in
/// colour can be checked by looking at it.
///
/// The chrome is drawn by Skia onto a Direct3D 12 swapchain. It used to go into
/// a CPU raster surface and be blitted with GDI, on the argument that interface
/// drawing is a few hundred rounded rectangles a frame and the GPU would be no
/// measurable gain. That argument was made without measuring and it was wrong:
/// `--benchmark` puts an Aero frame at 1080p at well over half a second on the
/// CPU and under fifty milliseconds on the GPU, because every glass surface
/// wants its own blurred layer and filling pixels is precisely what a GPU is
/// for. Video frames still have their own path.
///
/// Not the editor. A harness for the parts of it that exist.

#include "cutline/core/edit.hpp"
#include "cutline/core/effects.hpp"
#include "cutline/core/model.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/time.hpp"
#include "cutline/editor/browser_binding.hpp"
#include "cutline/editor/commands.hpp"
#include "cutline/editor/document.hpp"
#include "cutline/editor/effects_binding.hpp"
#include "cutline/editor/generators.hpp"
#include "cutline/editor/import.hpp"
#include "cutline/editor/inspector.hpp"
#include "cutline/editor/session.hpp"
#include "cutline/editor/timeline_binding.hpp"
#include "cutline/editor/titles.hpp"
#include "cutline/editor/transitions.hpp"
#include "cutline/editor/workspace.hpp"
#include "cutline/ui/browser.hpp"
#include "cutline/ui/color_picker.hpp"
#include "cutline/ui/controls.hpp"
#include "cutline/ui/dock.hpp"
#include "cutline/ui/dock_view.hpp"
#include "cutline/ui/monitor.hpp"
#include "cutline/ui/skia_painter.hpp"
#include "cutline/ui/skia_window.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/timeline.hpp"
#include "cutline/ui/widget.hpp"
#include "cutline/ui/widgets.hpp"

#if CUTLINE_HAVE_PREVIEW
#include "cutline/app/preview.hpp"
#include "cutline/engine/exporter.hpp"
#include "cutline/engine/player.hpp"
#endif

#include <windows.h>
// Both of these need windows.h first: one for the message parameters it
// defines, the other for the types in its own signatures.
#include <commdlg.h>
#include <dwmapi.h>
// For `timeBeginPeriod`: the default 15.6 ms scheduler tick is most of a frame
// at 60 Hz, and the playback loop cannot pace itself with one that coarse.
#include <timeapi.h>
#include <windowsx.h>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cutline::ui::Axis;
using cutline::ui::Box;
using cutline::ui::Button;
using cutline::ui::CaptionButton;
using cutline::ui::Checkbox;
using cutline::ui::Color;
using cutline::ui::ColorSwatch;
using cutline::ui::DockLayout;
using cutline::ui::Dropdown;
using cutline::ui::Edges;
using cutline::ui::IconButton;
using cutline::ui::MenuList;
using cutline::ui::DockNode;
using cutline::ui::DockView;
using cutline::ui::FloatingDock;
using cutline::ui::Key;
using cutline::ui::KeyEvent;
using cutline::ui::Label;
using cutline::ui::LayoutContext;
using cutline::ui::MediaBrowser;
using cutline::ui::Modifiers;
using cutline::ui::MonitorView;
using cutline::ui::MouseButton;
using cutline::ui::MouseEvent;
using cutline::ui::Panel;
using cutline::ui::parse_color;
using cutline::ui::ProgressBar;
using cutline::ui::PanelId;
using cutline::ui::Rect;
using cutline::ui::ScrollView;
using cutline::ui::SkiaPainter;
using cutline::ui::Slider;
using cutline::ui::Spacer;
using cutline::ui::Splitter;
using cutline::ui::TextAlign;
using cutline::ui::TextField;
using cutline::ui::Theme;
using cutline::ui::to_hex;
using cutline::ui::TimelineView;
using cutline::ui::TimeScale;
using cutline::ui::TitleBar;
using cutline::ui::ValueRange;
using cutline::ui::WheelEvent;
using cutline::ui::Widget;
using cutline::ui::WidgetHost;
using cutline::ui::built_in_themes;

/// A project for the timeline to show. Built rather than loaded: the point is
/// to exercise the real model and the real edit operations without needing any
/// media on disk to decode.
[[nodiscard]] cutline::core::Project sample_project() {
  using namespace cutline::core;

  Project project;
  project.fps = 30.0;
  project.media = {
      Media{.id = "wide", .name = "wide.mp4", .duration = 120.0, .has_video = true},
      Media{.id = "close", .name = "close.mp4", .duration = 120.0, .has_video = true},
      Media{.id = "title", .name = "title.png", .duration = 10.0, .has_video = true,
            .is_image = true},
      Media{.id = "dialogue", .name = "dialogue.wav", .duration = 120.0,
            .audio_stream_count = 1},
      Media{.id = "score", .name = "score.wav", .duration = 120.0, .audio_stream_count = 1},
  };

  const auto clip = [](std::string id, std::string media, double start, double length,
                       TrackKind kind = TrackKind::Video) {
    return Clip{.id = std::move(id), .media_id = std::move(media), .kind = kind,
                .source_in = 0.0, .source_out = length, .start = start};
  };

  Track upper{.id = "v2", .kind = TrackKind::Video};
  upper.clips = {clip("t1", "title", 6.0, 5.5), clip("t2", "title", 24.0, 5.0)};

  Track lower{.id = "v1", .kind = TrackKind::Video};
  lower.clips = {clip("c1", "wide", 0.0, 6.2), clip("c2", "close", 6.2, 7.8),
                 clip("c3", "wide", 14.0, 8.5), clip("c4", "close", 22.5, 15.5)};

  Track dialogue{.id = "a1", .kind = TrackKind::Audio};
  dialogue.clips = {clip("d1", "dialogue", 0.0, 22.5, TrackKind::Audio),
                    clip("d2", "dialogue", 22.5, 15.5, TrackKind::Audio)};

  Track music{.id = "a2", .kind = TrackKind::Audio, .muted = true};
  music.clips = {clip("m1", "score", 2.0, 36.0, TrackKind::Audio)};

  project.tracks = {std::move(upper), std::move(lower), std::move(dialogue),
                    std::move(music)};
  return project;
}

/// The panels the window can show, and what their tabs say.
///
/// A panel is a name from here plus whatever `make_panel` builds for it. The
/// layout only ever moves the name around, which is what lets an arrangement be
/// saved and restored without saving any widgets.
constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kPanels{{
    {"project", "Project"},
    {"effects", "Effect Controls"},
    {"monitor", "Program Monitor"},
    {"timeline", "Timeline"},
}};

/// The panels this build has, which is what a saved arrangement is measured
/// against: one naming a panel that has since gone shows a tab nothing can
/// fill, and one written before a panel existed leaves it unreachable.
[[nodiscard]] std::vector<PanelId> known_panels() {
  std::vector<PanelId> panels;
  for (const auto& [id, title] : kPanels) panels.emplace_back(id);
  return panels;
}

[[nodiscard]] std::string panel_title(const PanelId& id) {
  using Entry = std::pair<std::string_view, std::string_view>;
  const auto found = std::ranges::find(kPanels, id, &Entry::first);
  return found == kPanels.end() ? id : std::string(found->second);
}

/// The arrangement a window with no session behind it shows. Only the headless
/// check has one of those; everything else reads it from the active workspace.
[[nodiscard]] DockLayout default_layout() {
  return cutline::editor::built_in_workspaces().front().layout;
}

/// A frame for the monitor to show, generated rather than decoded.
///
/// This preset has no media layer on purpose — the point of it is the chrome —
/// so there is nothing here to decode. Colour bars still prove the whole path
/// works: pixels through the painter, letterboxed into the panel, at the
/// sequence's own shape. What replaces this is the compositor's output, and
/// the monitor will not know the difference.
struct TestPattern {
  static constexpr int kWidth = 640;
  static constexpr int kHeight = 360;

  TestPattern() : bytes(static_cast<std::size_t>(kWidth) * kHeight * 4) {
    constexpr std::array<std::array<std::uint8_t, 3>, 7> kBars{{
        {192, 192, 192}, {192, 192, 0}, {0, 192, 192}, {0, 192, 0},
        {192, 0, 192},   {192, 0, 0},   {0, 0, 192},
    }};

    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        const auto& bar = kBars[static_cast<std::size_t>(x * 7 / kWidth)];
        // Darkened towards the bottom, so it is obvious which way up it is and
        // that the rows are not being read in the wrong order.
        const double shade = 1.0 - 0.55 * (static_cast<double>(y) / kHeight);
        const std::size_t at = (static_cast<std::size_t>(y) * kWidth + x) * 4;
        for (int channel = 0; channel < 3; ++channel) {
          bytes[at + static_cast<std::size_t>(channel)] =
              static_cast<std::uint8_t>(bar[static_cast<std::size_t>(channel)] * shade);
        }
        bytes[at + 3] = 255;
      }
    }
  }

  [[nodiscard]] cutline::ui::ImageView view() const {
    return cutline::ui::ImageView{.pixels = bytes.data(), .width = kWidth, .height = kHeight};
  }

  std::vector<std::uint8_t> bytes;
};

struct App;

/// One window class for every window there is. The main one and the torn-out
/// ones want exactly the same message handling — drawn caption, deferred
/// layout, all of it — and the only thing that differs is which dock they show.
constexpr const wchar_t* kWindowClass = L"CutlineWindow";

/// One real window: its widgets, its pixels, and where the two meet.
///
/// The main window is one of these and every torn-out panel is another. Kept as
/// a single type deliberately: it is the only place a surface is made, resized,
/// painted into and blitted, so there is one site to change rather than one per
/// window when that stops being a CPU raster surface.
struct Shell {
  App* app = nullptr;
  HWND window = nullptr;

  /// Empty for the main window; the floating dock's id otherwise. This is what
  /// ties a real window to its entry in the layout across a rearrangement.
  std::string floating_id;
  /// A window that holds no panels — the export settings. It has no dock, is
  /// not in the layout, and closing it throws nothing away.
  bool is_dialog = false;
  [[nodiscard]] bool is_main() const noexcept { return floating_id.empty() && !is_dialog; }

  std::unique_ptr<WidgetHost> host;
  DockView* dock = nullptr;
  /// Asked which parts of the caption are draggable, so the buttons in it stay
  /// clickable while the rest of it moves the window.
  TitleBar* title_bar = nullptr;
  CaptionButton* maximise = nullptr;

  /// Rebuilt whenever the window changes size. Skia's raster surface owns the
  /// pixels; the blit reads them straight out.
  std::unique_ptr<cutline::ui::SkiaWindow> surface;
  int width = 0;
  int height = 0;
  bool dirty = true;
  /// Set once a drawing failure has been reported, so it is said once rather
  /// than on every frame that follows it.
  bool warned = false;
  /// Set by `WM_SIZE`, cleared by the next render. Laying out on the message
  /// itself would need a painter to measure with, and there is no canvas at
  /// that point — the same reason input defers layout.
  bool resized = true;
};

struct App {
  /// The editor's window, and any panels dragged out of it.
  Shell main;
  std::vector<std::unique_ptr<Shell>> floats;

  /// The switcher's buttons, so the selected one can be moved without
  /// rebuilding the tree. Rebuilding would destroy the button whose click is
  /// still on the stack.
  std::vector<Button*> theme_buttons;

  /// The document being edited. Everything the timeline shows is derived from
  /// it, and everything a drag does goes back through it.
  cutline::editor::Session session{sample_project()};
  TimelineView* timeline = nullptr;
  Label* readout = nullptr;
  /// The media pool down the left, and the button that cycles its order.
  MediaBrowser* browser = nullptr;
  Button* sort_button = nullptr;
  cutline::editor::BrowserSort browser_sort = cutline::editor::BrowserSort::Pool;

  /// What a press on a clip does. Held here rather than only on the view,
  /// because the timeline panel is rebuilt whenever the arrangement or the theme
  /// changes and the tool somebody chose has to survive that.
  cutline::ui::Tool tool = cutline::ui::Tool::Selection;
  /// The palette's buttons, so the current one can be lit without rebuilding
  /// the row — which would destroy the button whose click is still running, the
  /// same trap the theme switcher has.
  std::vector<IconButton*> tool_buttons;

  /// The named arrangements, one of which is on screen. Where the panels are
  /// lives in the active one, so a rearrangement is remembered against the
  /// workspace it was made in rather than against the application.
  cutline::editor::Workspaces workspaces = cutline::editor::default_workspaces();
  /// Where the file was read from, and where it goes back to.
  std::filesystem::path workspace_file = cutline::editor::default_workspace_path();
  Button* workspace_button = nullptr;
  /// Set when the arrangement has changed and the views need rebuilding.
  ///
  /// Deferred for the same reason the inspector is, and more sharply: what
  /// asks for it is usually a tab strip that the rebuild destroys, so doing it
  /// on the spot would return into freed memory.
  bool dock_stale = false;

  /// The column the parameter controls live in, inside the inspector's scroll
  /// view. A `Box` rather than the panel, so clearing it does not take the
  /// scrolling with it.
  Box* inspector = nullptr;
  MonitorView* monitor = nullptr;
  /// Shown when there is nothing to decode — which is always under the skia
  /// preset, and until something is imported under the ui one.
  TestPattern pattern;

#if CUTLINE_HAVE_PREVIEW
  /// The one Direct3D device: the compositor renders on it and the windows
  /// draw on it.
  ///
  /// Made at startup rather than on first use, which reverses the old note
  /// here about not paying for a device until something needs one. The reason
  /// is that a window has always created one anyway — so this is not a device
  /// that would not otherwise exist, it is the *same* device instead of a
  /// second one. Two devices meant a composited frame had to come down to
  /// system memory and go back up before it could be shown.
  std::shared_ptr<cutline::gpu::Device> device;
  /// Set once when no device could be made, so it is not retried every frame.
  /// Everything still works: the windows fall back to making their own and the
  /// preview to copying frames through system memory.
  bool device_failed = false;

  std::unique_ptr<cutline::app::ProjectPreview> preview;
  bool preview_failed = false;
  /// Set when the picture no longer matches the playhead or the project.
  bool preview_stale = true;

  /// Whether the preview and the windows are on the same device, which is what
  /// decides whether a frame can be handed over as a texture or has to be
  /// copied. Only true when the shared device was made — otherwise each built
  /// its own and they have no memory in common.
  [[nodiscard]] bool shares_device() const noexcept { return device != nullptr; }

  /// Playback, with the sound card keeping time.
  ///
  /// Made on the first press of Play and dropped whenever the document changes:
  /// it decoded the audio it needs when it was made, so a project edited since
  /// is not the one it is playing.
  std::unique_ptr<cutline::engine::Player> player;
  /// The project the player was built from.
  ///
  /// Compared rather than a revision counter, because the session bumps its
  /// revision for a change of selection too — and clicking a clip should not
  /// stop the sound. This costs a project copy per press of Play, which is the
  /// same copy every edit already makes.
  cutline::core::Project player_project;
  /// The session revision the project above was last checked against, so the
  /// comparison happens when something changed rather than on every frame.
  std::uint64_t player_revision = 0;
  Button* play_button = nullptr;
  /// Which frame the picture is showing, so the loop redraws once per frame
  /// rather than as fast as it can spin. The playhead moves continuously; the
  /// picture does not have to.
  long long shown_frame = -1;
  /// Said once. A machine with no output device will not grow one.
  bool player_failed = false;

  [[nodiscard]] bool playing() const noexcept { return player != nullptr && player->playing(); }

  /// An export in flight.
  ///
  /// On its own thread because it is minutes of work and the window has to stay
  /// answerable throughout — a progress bar that cannot be repainted is worse
  /// than no progress bar. Everything crossing the thread boundary is either
  /// atomic or guarded, and the rule is that the worker only ever *writes*
  /// these and the interface only ever reads them, apart from `cancel`.
  struct ExportJob {
    std::thread worker;
    std::atomic<bool> running{false};
    /// Set by the interface, read by the worker's progress callback.
    std::atomic<bool> cancel{false};
    /// 0..1, for the bar.
    std::atomic<double> fraction{0.0};
    /// Set once, just before `running` goes false. Read after that.
    std::string outcome;
    bool failed = false;
    /// Set when the worker has finished and the thread still needs joining.
    std::atomic<bool> finished{false};
  };
  ExportJob export_job;
  std::unique_ptr<Shell> export_window;

  /// What the dialog is set to, kept on the application so reopening it
  /// remembers what was chosen last time.
  struct ExportSetup {
    std::filesystem::path path;
    std::size_t codec = 0;
    std::size_t quality = 1;
    std::size_t rate = 0;
    std::size_t resolution = 0;
    std::size_t mixdown = 0;
    bool audio = true;
    /// Render only what is marked in and out. Ignored, and unpressable, when
    /// nothing is marked — there is no such thing as "the marked range" then,
    /// and a checkbox that means the whole sequence either way is a lie.
    bool marked_only = false;
  };
  ExportSetup export_setup;

  Button* export_output = nullptr;
  Button* export_start = nullptr;
  Dropdown* export_mixdown = nullptr;
  ProgressBar* export_progress = nullptr;
  Label* export_status = nullptr;

  [[nodiscard]] bool exporting() const noexcept { return export_job.running.load(); }
#endif

  std::size_t theme = 0;
  /// Set when the inspector needs rebuilding, and acted on at the next render.
  ///
  /// Deferred rather than done on the spot because the thing asking for it is
  /// usually a slider *inside* the inspector, and rebuilding would destroy
  /// that slider while its own callback was still running.
  bool inspector_stale = true;

  [[nodiscard]] const Theme& current() const { return built_in_themes()[theme]; }

  /// Every window there is. The main one is never null; the rest come and go.
  [[nodiscard]] std::vector<Shell*> shells() {
    std::vector<Shell*> all{&main};
    for (const std::unique_ptr<Shell>& shell : floats) all.push_back(shell.get());
#if CUTLINE_HAVE_PREVIEW
    // Included so it is painted and invalidated with the rest, and kept out of
    // `floats` so the layout reconciler does not treat it as a torn-out panel
    // and close it.
    if (export_window != nullptr) all.push_back(export_window.get());
#endif
    return all;
  }
};

/// Where the panels are: the active workspace's layout.
///
/// There is exactly one of these at a time and it is owned by the workspace, so
/// dragging a panel is remembered against the arrangement it was dragged in.
[[nodiscard]] DockLayout& layout_of(App& app) {
  DockLayout* showing = app.workspaces.current();
  // `settle` guarantees there is always one. This is the last resort so that a
  // programming mistake is an empty window rather than a crash.
  static DockLayout nothing;
  return showing == nullptr ? nothing : *showing;
}

/// Marks every window as needing to be drawn again.
///
/// Most changes are to the document or the theme, and those show in whichever
/// windows happen to be open rather than in one of them.
void mark_dirty(App& app) {
  for (Shell* shell : app.shells()) shell->dirty = true;
}

void set_theme(App& app, std::size_t index);
void refresh_timeline(App& app);
void refresh_browser(App& app);
void refresh_dock(App& app);
void reconcile_windows(App& app);
void refresh_float_titles(App& app);
void refresh_all(App& app);
void invalidate_preview(App& app);
void invalidate_playback(App& app);
void open_export_dialog(App& app);
void poll_export(App& app);
void settle_export(App& app);
void toggle_playback(App& app);
void import_media(App& app);
void add_title(App& app);
void add_matte(App& app);
void add_adjustment(App& app);
void complain(HWND owner, const std::string& message);

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

/// Rebuilds the inspector when moving the playhead changes what it shows.
///
/// Which is only when the selected clip animates something: an animated row
/// reads its value at the playhead and a static one does not care where it is.
/// Guarded rather than unconditional because a rebuild is dozens of widgets and
/// playback moves the playhead thirty times a second.
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
  ValueRange range;
  double value = 0.0;
  double fallback = 0.0;
  bool animatable = false;
  bool animated = false;
  bool keyed_here = false;
  cutline::core::Interp interp = cutline::core::Interp::Linear;
};

/// The animation handlers default to nothing, for a row that is not animatable.
/// A parameter with nowhere to keep a keyframe — an audio effect's — builds no
/// stopwatch, no marker and no chip, so none of them is ever called.
void build_param_row(App& app, const ParamRow& row, std::function<void(double)> on_commit,
                     std::function<void(bool)> on_animate = {},
                     std::function<void()> on_keyframe = {},
                     std::function<void(cutline::core::Interp)> on_interp = {}) {
  auto& head = app.inspector->emplace<Box>(Axis::Horizontal);

  if (row.animatable) {
    auto& watch = head.emplace<IconButton>(
        IconButton::Icon::Stopwatch,
        [animate = std::move(on_animate), animated = row.animated] { animate(!animated); });
    watch.set_selected(row.animated);
  }

  // The unit goes in the label, because the slider has no readout to put it in
  // and "Amount" alone does not say whether 40 is pixels or percent.
  auto& label =
      head.emplace<Label>(row.suffix.empty() ? row.name : row.name + " (" + row.suffix + ")");
  label.set_small(true);
  head.emplace<Spacer>();

  // Only once animation is on. Before that there is no list to add to, and a
  // marker that silently started one would mean the same as the stopwatch.
  if (row.animated) {
    // Premiere's interpolation chip, as a button that cycles rather than a
    // dropdown of three. Three is short enough to walk round, and a dropdown
    // beside a stopwatch and a diamond on a narrow row is a lot of chrome for
    // a setting with three values.
    head.emplace<Button>(std::string(cutline::editor::interp_name(row.interp)),
                         [interp = std::move(on_interp), mode = row.interp] {
                           if (interp) interp(cutline::editor::next_interp(mode));
                         });

    auto& mark = head.emplace<IconButton>(IconButton::Icon::Diamond, std::move(on_keyframe));
    mark.set_selected(row.keyed_here);
  }

  auto& slider = app.inspector->emplace<Slider>(row.range, row.value);
  slider.set_default_value(row.fallback);
  // On commit rather than on change: the control follows the pointer as it
  // goes, and the project is written once, at the end of the gesture.
  slider.set_on_commit(std::move(on_commit));
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
    app.session.apply(
        audio ? cutline::editor::add_audio_effect(app.session.project(), clip_id,
                                                  choices[index].type)
              : cutline::editor::add_effect(app.session.project(), clip_id,
                                            choices[index].type));
    if (app.main.host != nullptr) app.main.host->close_popup();
    app.inspector_stale = true;
    invalidate_preview(app);
  });

  app.main.host->open_popup(std::move(list), anchor);
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
                         std::function<cutline::core::Project()> on_remove) {
  const auto applied = [&app](cutline::core::Project next) {
    app.session.apply(std::move(next));
    app.inspector_stale = true;
    invalidate_preview(app);
  };

  auto& line = app.inspector->emplace<Box>(Axis::Horizontal);

  // The checkbox is the name as well: a disabled effect stays in the stack and
  // stays visible, which is the whole point of disabling rather than removing.
  auto& on = line.emplace<Checkbox>(row.name, row.enabled);
  on.set_on_change([&app, applied, on_toggle](bool) { applied(on_toggle()); });

  line.emplace<Spacer>();

  // Moving is by button rather than by dragging the row. Dragging is what
  // Premiere does and what this should eventually do; a button is what can be
  // reached from the keyboard and what can be tested without a pointer.
  auto& up = line.emplace<IconButton>(IconButton::Icon::ArrowUp);
  up.set_enabled(row.index > 0);
  up.set_on_click([applied, on_move] { applied(on_move(-1)); });

  auto& down = line.emplace<IconButton>(IconButton::Icon::ArrowDown);
  down.set_enabled(row.index + 1 < count);
  down.set_on_click([applied, on_move] { applied(on_move(1)); });

  line.emplace<IconButton>(IconButton::Icon::Cross,
                           [applied, on_remove] { applied(on_remove()); });
}

/// The effect stack: a header per effect, its parameters beneath it.
///
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

      const ParamRow control{.name = param.name,
                             .suffix = param.suffix,
                             .range = param.range,
                             .value = param.value,
                             .fallback = param.fallback,
                             .animatable = true,
                             .animated = param.animated,
                             .keyed_here = param.keyed_here,
                             .interp = param.interp};

      build_param_row(
          app, control,
          [&app, clip_id, index = row.index, key = param.key](double value) {
            app.session.apply(cutline::editor::set_effect_parameter(
                app.session.project(), clip_id, index, key, value,
                local_playhead(app, clip_id)));
            invalidate_preview(app);
            app.inspector_stale = true;
          },
          [&app, clip_id, index = row.index, key = param.key](bool animated) {
            app.session.apply(cutline::editor::set_effect_parameter_animated(
                app.session.project(), clip_id, index, key, animated,
                local_playhead(app, clip_id)));
            invalidate_preview(app);
            app.inspector_stale = true;
          },
          [&app, clip_id, index = row.index, key = param.key] {
            app.session.apply(cutline::editor::toggle_effect_keyframe(
                app.session.project(), clip_id, index, key, local_playhead(app, clip_id)));
            refresh_timeline(app);
            invalidate_preview(app);
            app.inspector_stale = true;
          },
          [&app, clip_id, index = row.index, key = param.key](cutline::core::Interp mode) {
            app.session.apply(cutline::editor::set_effect_parameter_interp(
                app.session.project(), clip_id, index, key, mode));
            invalidate_preview(app);
            app.inspector_stale = true;
          });
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
  }
}

/// The audio effect stack, for an audio clip.
///
/// Shorter than the visual one and not because it was cut down: the audio
/// registry has no colours and `AudioClipEffect` has no keyframes, so there is
/// no swatch and no stopwatch to build. What is here is the same header and the
/// same parameter rows, from the same structs.
void build_audio_effect_controls(App& app, const std::string& clip_id) {
  auto& header = app.inspector->emplace<Box>(Axis::Horizontal);
  header.emplace<Label>("Audio Effects").set_bold(true);
  header.emplace<Spacer>();

  auto& add = header.emplace<Button>("Add Effect");
  add.set_on_click([&app, clip_id, control = &add] {
    open_effect_menu(app, clip_id, control->bounds(), true);
  });

  const std::vector<cutline::editor::EffectRow> rows =
      cutline::editor::clip_audio_effects(app.session.project(), clip_id);
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
        });

    if (row.unknown) {
      app.inspector->emplace<Label>("Not available in this version").set_small(true);
      continue;
    }

    for (const cutline::editor::EffectParamRow& param : row.params) {
      const ParamRow control{.name = param.name,
                             .suffix = param.suffix,
                             .range = param.range,
                             .value = param.value,
                             .fallback = param.fallback,
                             // No stopwatch: the model has nowhere to put an
                             // audio effect keyframe, and offering one would be
                             // a button that could not do what it said.
                             .animatable = false};

      build_param_row(app, control,
                      [&app, clip_id, index = row.index, key = param.key](double value) {
                        app.session.apply(cutline::core::set_audio_effect_param(
                            app.session.project(), clip_id, index, key, value));
                        // No `invalidate_preview`: this changes the sound and
                        // not one pixel. The player notices the project has
                        // moved on by itself, on the next frame.
                        app.inspector_stale = true;
                      });
    }
  }
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
                                      app.session.project(), clip_id, *kind)
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

  const auto write = [&app, clip_id](cutline::editor::MatteFill changed, bool rebuild_panel) {
    const cutline::core::Clip* clip =
        cutline::core::find_clip(app.session.project(), clip_id);
    if (clip == nullptr) return;
    app.session.apply(cutline::editor::set_matte_fill(app.session.project(), clip->media_id,
                                                      std::move(changed)));
    refresh_browser(app);
    refresh_timeline(app);
    invalidate_preview(app);
    // Off for the swatches: rebuilding destroys the swatch, and a destroyed
    // swatch closes the picker hanging open above it.
    if (rebuild_panel) app.inspector_stale = true;
  };

  auto& colour_row = app.inspector->emplace<Box>(Axis::Horizontal);
  colour_row.emplace<Label>("Colour").set_small(true);
  auto& colour = colour_row.emplace<ColorSwatch>(parse_color(fill.color));
  colour.set_on_commit([write, fill](const Color& picked) {
    cutline::editor::MatteFill changed = fill;
    changed.color = to_hex(picked);
    write(std::move(changed), false);
  });

  auto& ramp = app.inspector->emplace<Box>(Axis::Horizontal);
  auto& gradient = ramp.emplace<Checkbox>("Gradient", fill.gradient.has_value());
  gradient.set_on_change([write, fill](bool on) {
    cutline::editor::MatteFill changed = fill;
    if (on) {
      // Black by default, which is the one second colour that reads as a ramp
      // whatever the first one is.
      changed.gradient = cutline::core::MatteGradient{.color2 = "#000000", .angle = 0.0};
    } else {
      changed.gradient.reset();
    }
    // This one does rebuild: turning it on adds two controls below.
    write(std::move(changed), true);
  });
  ramp.emplace<Spacer>();

  if (!fill.gradient.has_value()) return;

  auto& second_row = app.inspector->emplace<Box>(Axis::Horizontal);
  second_row.emplace<Label>("To").set_small(true);
  auto& second = second_row.emplace<ColorSwatch>(parse_color(fill.gradient->color2));
  second.set_on_commit([write, fill](const Color& picked) {
    cutline::editor::MatteFill changed = fill;
    changed.gradient->color2 = to_hex(picked);
    write(std::move(changed), false);
  });

  app.inspector->emplace<Label>("Angle (deg)").set_small(true);
  auto& angle = app.inspector->emplace<Slider>(ValueRange{.minimum = 0.0, .maximum = 360.0},
                                               fill.gradient->angle);
  angle.set_on_commit([write, fill](double value) {
    cutline::editor::MatteFill changed = fill;
    changed.gradient->angle = value;
    write(std::move(changed), false);
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

  // Premiere's own heading for the built-in transform, and the honest one for
  // an audio clip, which has no geometry to move.
  inspector_heading(app, visual ? "Motion" : "Audio");

  for (const cutline::editor::ParamSpec& spec :
       cutline::editor::clip_parameters(app.session.project(), clip_id,
                                        local_playhead(app, clip_id))) {
    const ParamRow line{.name = spec.name,
                        // The transform's units are already in its names —
                        // "Position X" in percent of the canvas — and repeating
                        // them would read as noise.
                        .range = spec.range,
                        .value = spec.value,
                        .fallback = spec.fallback,
                        .animatable = spec.animatable,
                        .animated = spec.animated,
                        .keyed_here = spec.keyed_here,
                        .interp = spec.interp};

    build_param_row(
        app, line,
        [&app, clip_id, param = spec.param](double value) {
          app.session.apply(cutline::editor::set_clip_parameter(
              app.session.project(), clip_id, param, value, local_playhead(app, clip_id)));
          refresh_timeline(app);
          invalidate_preview(app);
          // Marked, not rebuilt: this lambda belongs to the slider that a
          // rebuild would destroy. It has to be rebuilt though — the model may
          // have clamped the value, and a speed change alters the clip's
          // length, which moves the bounds of both fade controls.
          app.inspector_stale = true;
        },
        [&app, clip_id, param = spec.param](bool animated) {
          app.session.apply(cutline::editor::set_clip_parameter_animated(
              app.session.project(), clip_id, param, animated, local_playhead(app, clip_id)));
          refresh_timeline(app);
          invalidate_preview(app);
          app.inspector_stale = true;
        },
        [&app, clip_id, param = spec.param] {
          app.session.apply(cutline::editor::toggle_clip_parameter_keyframe(
              app.session.project(), clip_id, param, local_playhead(app, clip_id)));
          refresh_timeline(app);
          invalidate_preview(app);
          app.inspector_stale = true;
        },
        [&app, clip_id, param = spec.param](cutline::core::Interp mode) {
          app.session.apply(cutline::editor::set_clip_parameter_interp(app.session.project(),
                                                                       clip_id, param, mode));
          invalidate_preview(app);
          app.inspector_stale = true;
        });
  }

  // Below Motion and above the effects: a transition belongs to the cut rather
  // than to the clip, and grouping it with the effect stack would suggest it
  // stacks with them.
  if (visual) build_transition_controls(app, clip_id);

  // One stack or the other, never both: a clip is video or audio, and offering
  // a blur on a waveform would be a control with nothing behind it.
  if (visual) {
    build_effect_controls(app, clip_id);
  } else {
    build_audio_effect_controls(app, clip_id);
  }

  app.inspector->emplace<Spacer>();

  app.main.host->request_layout();
  mark_dirty(app);
}

/// Rebuilds what the timeline draws from the session.
///
/// Everything goes through here rather than being patched in place, which is
/// what keeps the view from drifting out of step with the document — an undo
/// changes the project in ways no incremental update could follow.
void refresh_timeline(App& app) {
  if (app.timeline == nullptr) return;
  app.timeline->set_model(
      cutline::editor::timeline_model(app.session.project(), app.session.selection()));
  app.timeline->set_playhead(app.session.playhead());
  mark_dirty(app);
}

/// What the sort button says, and what pressing it moves to next.
[[nodiscard]] std::string_view sort_name(cutline::editor::BrowserSort sort) noexcept {
  using cutline::editor::BrowserSort;
  switch (sort) {
    case BrowserSort::Pool: return "Pool";
    case BrowserSort::Name: return "Name";
    case BrowserSort::Kind: return "Kind";
    case BrowserSort::Duration: return "Length";
    case BrowserSort::Uses: return "Used";
  }
  return "Pool";
}

/// Rebuilds the media pool from the session.
///
/// Through the binding every time rather than being patched, for the same
/// reason the timeline is: an undo can put media back that no incremental
/// update would know to restore.
void refresh_browser(App& app) {
  if (app.browser == nullptr) return;

  cutline::editor::BrowserOptions options;
  options.sort = app.browser_sort;
#if CUTLINE_HAVE_PREVIEW
  // Whatever the renderer could not open. It only knows once it has tried, so
  // a file that has moved shows as offline after the first render rather than
  // the moment the project opens.
  if (app.preview != nullptr) options.offline = app.preview->missing_media();
#endif

  app.browser->set_items(cutline::editor::browser_items(app.session.project(), options));
  if (app.sort_button != nullptr) {
    app.sort_button->set_text("Sort: " + std::string(sort_name(app.browser_sort)));
  }
  mark_dirty(app);
}

/// Puts a pool entry on the timeline.
///
/// `where` is where it was dropped, or nothing for a double-click — which
/// lands at the playhead on the topmost video track, the way every editor's
/// "insert" does.
void place_from_pool(App& app, std::size_t index,
                     std::optional<cutline::ui::DropPoint> where = std::nullopt) {
  if (app.browser == nullptr || index >= app.browser->items().size()) return;
  const std::string media_id = app.browser->items()[index].id;

  double at = app.session.playhead();
  std::string track_id;

  if (where.has_value() && app.timeline != nullptr) {
    at = cutline::core::snap_to_frame(where->time, app.session.project().fps);

    // Only a video track can be named here: `place_media` puts the audio
    // streams on lanes of its own choosing, and handing it an audio track as
    // the video one would put a picture where no picture can go.
    const cutline::ui::TimelineModel& model = app.timeline->model();
    if (where->track < model.tracks.size() && !model.tracks[where->track].audio) {
      track_id = model.tracks[where->track].id;
    }
  }

  app.session.apply(
      cutline::core::place_media(app.session.project(), media_id, at, track_id));
  refresh_all(app);
}

/// Takes an entry out of the pool, and every clip that used it with it.
void remove_from_pool(App& app) {
  if (app.browser == nullptr) return;
  const cutline::ui::MediaItem* chosen = app.browser->selected();
  if (chosen == nullptr) return;

  // Asked rather than assumed. Removing media the sequence is built from is
  // undoable, but it is still not something to do on a mis-click.
  if (chosen->uses > 0) {
    const std::string message = chosen->name + " is used by " + std::to_string(chosen->uses) +
                                (chosen->uses == 1 ? " clip." : " clips.") +
                                "\n\nRemoving it takes them out of the sequence too.";
    if (MessageBoxA(app.main.window, message.c_str(), "Cutline", MB_OKCANCEL | MB_ICONWARNING) !=
        IDOK) {
      return;
    }
  }

  app.session.apply(cutline::editor::remove_media(app.session.project(), chosen->id));
  refresh_all(app);
}

/// Renders the frame under the playhead into the monitor.
///
/// Deferred and coalesced like everything else: scrubbing produces a mouse
/// move per pixel, and a read-back stalls the GPU, so rendering on each one
/// would make the drag itself the slow part.
void refresh_preview(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.monitor == nullptr || app.preview_failed) return;

  // Nothing to show until something has been imported. Rendering an empty
  // project would replace the colour bars with a black rectangle and look
  // like a fault.
  bool has_media = false;
  for (const cutline::core::Track& track : app.session.project().tracks) {
    if (!track.clips.empty()) has_media = true;
  }
  if (!has_media) return;

  const cutline::core::Project& project = app.session.project();
  if (app.preview == nullptr) {
    // The same device the windows draw on, when there is one. That is what
    // lets the finished frame be handed over rather than copied.
    auto made =
        cutline::app::ProjectPreview::create(project.canvas_w, project.canvas_h, app.device);
    if (!made.has_value()) {
      // Once, and then never again: a machine with no usable device will not
      // acquire one, and complaining on every scrub would be unbearable.
      app.preview_failed = true;
      complain(app.main.window, "Preview is unavailable.\n\n" + made.error());
      return;
    }
    app.preview = std::move(*made);
  }

  // Two ways to the same picture. Sharing a device means the frame is already
  // in memory the window can draw from, so it is handed over as a texture;
  // without one the two have nothing in common and it has to go down to the
  // CPU and back up again.
  if (app.shares_device()) {
    const auto frame = app.preview->texture_at(project, app.session.playhead());
    if (!frame.has_value()) {
      app.preview_failed = true;
      complain(app.main.window, "Could not render the preview.\n\n" + frame.error());
      return;
    }
    app.monitor->set_texture(*frame);
  } else {
    const auto frame = app.preview->frame_at(project, app.session.playhead());
    if (!frame.has_value()) {
      app.preview_failed = true;
      complain(app.main.window, "Could not render the preview.\n\n" + frame.error());
      return;
    }
    app.monitor->set_frame(*frame);
  }

  app.monitor->set_canvas_aspect(static_cast<double>(project.canvas_w) / project.canvas_h);
  mark_dirty(app);
#else
  (void)app;
#endif
}

/// Marks the picture as no longer matching the playhead or the project.
void invalidate_preview(App& app) {
#if CUTLINE_HAVE_PREVIEW
  app.preview_stale = true;
  mark_dirty(app);
#else
  (void)app;
#endif
}

// ---------------------------------------------------------------- playback --

/// Stops playback and puts the transport back the way it looks when idle.
///
/// Separate from pausing because it is also what happens at the end of the
/// timeline and when the document changes underneath a playing project.
void stop_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.player == nullptr) return;
  if (app.player->playing()) {
    app.player->pause();
    // Windows' scheduler tick goes back to its lazy default. Asking for a
    // millisecond costs power across the whole machine, so it is held only
    // while something is actually being paced by it.
    timeEndPeriod(1);
  }
  if (app.play_button != nullptr) app.play_button->set_text("Play");
  app.shown_frame = -1;
  mark_dirty(app);
#else
  (void)app;
#endif
}

/// Forgets the player, because what it decoded no longer matches the document.
void invalidate_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.player == nullptr) return;
  stop_playback(app);
  app.player.reset();
#else
  (void)app;
#endif
}

void toggle_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.playing()) {
    stop_playback(app);
    return;
  }
  if (app.player_failed) return;

  // A player that decoded a different project is no use. Dropped here rather
  // than at every edit, so there is one place that decides and it is the place
  // that knows what the player was made from.
  if (app.player != nullptr && app.player_project != app.session.project()) {
    app.player.reset();
  }

  if (app.player == nullptr) {
    auto made = cutline::engine::Player::create(app.session.project());
    if (!made.has_value()) {
      // Once and no more: a machine with no output device will not acquire one,
      // and a dialog on every press of Play would be worse than the silence.
      app.player_failed = true;
      complain(app.main.window, "Playback is unavailable.\n\n" + made.error());
      return;
    }
    app.player = std::move(*made);
    app.player_project = app.session.project();
  }
  app.player_revision = app.session.revision();

  // From wherever the playhead is, not from wherever playback last stopped —
  // the user may have scrubbed since.
  app.player->seek(app.session.playhead());
  app.player->play();
  // Without this, `Sleep(1)` in the playback loop really sleeps about 15 ms,
  // which is most of a frame at 60 Hz.
  timeBeginPeriod(1);

  if (app.play_button != nullptr) app.play_button->set_text("Pause");
  app.shown_frame = -1;
  mark_dirty(app);
#else
  (void)app;
#endif
}

/// One turn of playback: follows the sound card and redraws when the frame it
/// is pointing at changes.
///
/// The playhead is *read*, never advanced here. The sound card decides where
/// time is, because it is the one that cannot be nudged — a repeated or dropped
/// picture frame goes unnoticed, while a gap of the same length in audio is a
/// click. So the picture is what adapts.
void advance_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (!app.playing()) return;

  if (!app.player->error().empty()) {
    const std::string message = app.player->error();
    stop_playback(app);
    complain(app.main.window, "Playback stopped.\n\n" + message);
    return;
  }

  // An edit while the sound is running leaves the player playing audio the
  // document no longer describes. Stopping is the honest answer: it decoded
  // what it needs up front, so there is nothing to update in place.
  //
  // Guarded by the revision so the project comparison happens when something
  // changed rather than thirty times a second. The revision also moves for a
  // change of selection, which is why the projects are then compared rather
  // than trusted — clicking a clip should not stop playback.
  if (app.session.revision() != app.player_revision) {
    app.player_revision = app.session.revision();
    if (app.player_project != app.session.project()) {
      invalidate_playback(app);
      return;
    }
  }

  const double at = app.player->position();
  if (app.player->finished()) {
    app.session.set_playhead(at);
    refresh_timeline(app);
    invalidate_preview(app);
    stop_playback(app);
    return;
  }

  // Once per frame of the sequence, not once per turn of the loop. Rendering
  // faster than the project's frame rate would draw the same picture twice and
  // charge a decode for it.
  const double fps = app.session.project().fps > 0.0 ? app.session.project().fps : 30.0;
  const auto frame = static_cast<long long>(at * fps);
  if (frame == app.shown_frame) {
    // Nothing due yet. Sleeping rather than spinning, because the audio thread
    // wants a core more than this loop does.
    Sleep(1);
    return;
  }
  app.shown_frame = frame;

  app.session.set_playhead(at);
  if (app.readout != nullptr) {
    app.readout->set_text(cutline::core::seconds_to_timecode(at, app.session.project().fps));
  }
  follow_playhead(app);
  refresh_timeline(app);
  invalidate_preview(app);
#else
  (void)app;
#endif
}

/// Puts the document's name where it can be seen — in the caption the theme
/// draws, and in the window text the taskbar reads.
void refresh_title(App& app) {
  const std::string title = app.session.document_title() + " - Cutline";
  if (app.main.title_bar != nullptr) app.main.title_bar->set_title(title);
  if (app.main.window != nullptr) SetWindowTextA(app.main.window, title.c_str());
  mark_dirty(app);
}

/// Everything a view needs told after the document has been replaced.
void refresh_all(App& app) {
  refresh_timeline(app);
  refresh_browser(app);
  refresh_title(app);
  invalidate_preview(app);
  app.inspector_stale = true;
  if (app.monitor != nullptr) {
    const cutline::core::Project& project = app.session.project();
    app.monitor->set_canvas_aspect(static_cast<double>(project.canvas_w) / project.canvas_h);
  }
  if (app.main.host != nullptr) app.main.host->request_layout();
}

/// Brings a file into the project and drops it at the playhead.
void import_media(App& app) {
#if CUTLINE_HAVE_PREVIEW
  std::array<wchar_t, MAX_PATH> buffer{};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = app.main.window;
  dialog.lpstrFilter =
      L"Media\0*.mp4;*.mov;*.mkv;*.avi;*.webm;*.wav;*.mp3;*.flac;*.png;*.jpg\0"
      L"All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (GetOpenFileNameW(&dialog) == FALSE) return;

  const std::filesystem::path path{buffer.data()};
  const auto source = cutline::app::probe_source(path.string());
  if (!source.has_value()) {
    complain(app.main.window, "Could not read that file.\n\n" + source.error());
    return;
  }

  // At the playhead, which is where an editor expects a drop to land.
  app.session.apply(cutline::editor::import_and_place(app.session.project(), *source,
                                                      app.session.playhead()));
  refresh_all(app);
#else
  complain(app.main.window, "This build has no media layer, so there is nothing to import with.");
#endif
}

/// Makes a title, drops it at the playhead, and selects it.
///
/// Selected because the next thing anyone wants is to type: the inspector shows
/// a title's own text and styling for whatever is selected, so creating one and
/// leaving the selection elsewhere would mean making a card and then having to
/// go and find it.
void add_title(App& app) {
  std::string clip_id;
  app.session.apply(cutline::editor::add_title_at(app.session.project(),
                                                  cutline::editor::default_title_spec(),
                                                  app.session.playhead(), {}, &clip_id));
  if (!clip_id.empty()) app.session.select_one(clip_id);
  refresh_all(app);
}

void add_matte(App& app) {
  std::string clip_id;
  app.session.apply(cutline::editor::add_color_matte_at(
      app.session.project(), cutline::editor::MatteFill{}, app.session.playhead(), {},
      &clip_id));
  if (!clip_id.empty()) app.session.select_one(clip_id);
  refresh_all(app);
}

void add_adjustment(App& app) {
  std::string clip_id;
  app.session.apply(cutline::editor::add_adjustment_layer_at(app.session.project(),
                                                             app.session.playhead(), {},
                                                             &clip_id));
  if (!clip_id.empty()) app.session.select_one(clip_id);
  refresh_all(app);
}

/// The system's open or save dialog.
[[nodiscard]] std::optional<std::filesystem::path> choose_file(HWND owner, bool saving) {
  std::array<wchar_t, MAX_PATH> buffer{};

  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  // Doubly terminated, and embedded nulls separate the pairs, which is why
  // this is a raw literal rather than anything that counts its own length.
  dialog.lpstrFilter = L"Cutline project\0*.cutline\0All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrDefExt = L"cutline";
  dialog.Flags = saving ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
                        : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);

  const BOOL chosen = saving ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
  if (chosen == FALSE) return std::nullopt;
  return std::filesystem::path(buffer.data());
}

/// Where an export should go. Its own dialog rather than a parameter on the
/// one above, because the filters and the default extension are the whole of
/// what that function does and threading them through would leave neither
/// caller readable.
[[nodiscard]] std::optional<std::filesystem::path> choose_movie_file(HWND owner) {
  std::array<wchar_t, MAX_PATH> buffer{};

  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = L"MP4 video\0*.mp4\0All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrDefExt = L"mp4";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

  if (GetSaveFileNameW(&dialog) == FALSE) return std::nullopt;
  return std::filesystem::path(buffer.data());
}

void complain(HWND owner, const std::string& message) {
  MessageBoxA(owner, message.c_str(), "Cutline", MB_OK | MB_ICONWARNING);
}

void open_project(App& app) {
  const auto path = choose_file(app.main.window, false);
  if (!path.has_value()) return;

  const auto loaded = cutline::editor::read_project(*path);
  if (!loaded.has_value()) {
    complain(app.main.window, "Could not open that project.\n\n" + loaded.error());
    return;
  }

  app.session.reset(loaded->project, *path);
  refresh_all(app);

  // Warnings are not failures: a project whose footage has moved still opens,
  // and saying so is more use than refusing it.
  if (!loaded->warnings.empty()) {
    std::string message = "The project opened with warnings:\n";
    for (const std::string& warning : loaded->warnings) message += "\n" + warning;
    complain(app.main.window, message);
  }
}

/// Returns whether it was written, so a cancelled dialog is not mistaken for
/// a successful save.
bool save_project(App& app, bool ask_where) {
  std::filesystem::path path = app.session.path();
  if (ask_where || path.empty()) {
    const auto chosen = choose_file(app.main.window, true);
    if (!chosen.has_value()) return false;
    path = cutline::editor::with_project_extension(*chosen);
  }

  const auto written = cutline::editor::write_project(path, app.session.project());
  if (!written.has_value()) {
    complain(app.main.window, "Could not save the project.\n\n" + written.error());
    return false;
  }

  app.session.mark_saved(path);
  refresh_title(app);
  return true;
}

void new_project(App& app) {
  app.session.reset(sample_project());
  refresh_all(app);
}

/// A key bound to an editing command.
struct Binding {
  Key key;
  bool control = false;
  bool shift = false;
  cutline::editor::Command command;
  /// Rarely set, and last so the common bindings stay readable. What it is for
  /// is the destructive twin of a harmless key: M drops a marker and
  /// Ctrl+Alt+M throws every one of them away, which is Premiere's arrangement
  /// and a good one — the awkward chord is the point.
  bool alt = false;
};

/// Taken before the widget tree sees them. Nothing should be able to swallow
/// undo, and a control-modified key is never something a control wants.
constexpr std::array kApplicationKeys{
    Binding{Key::Z, true, false, cutline::editor::Command::Undo},
    Binding{Key::Z, true, true, cutline::editor::Command::Redo},
    Binding{Key::Y, true, false, cutline::editor::Command::Redo},
    Binding{Key::A, true, false, cutline::editor::Command::SelectAll},
    Binding{Key::K, true, false, cutline::editor::Command::Split},
};

/// Offered only after the widget tree has declined them.
///
/// These are unmodified keys that a control may legitimately want — the arrows
/// belong to a focused slider before they belong to the playhead, and taking
/// them first would make the inspector unusable from the keyboard.
constexpr std::array kTransportKeys{
    // Premiere's I and O, and its Ctrl+Shift+X to throw both away.
    Binding{Key::I, false, false, cutline::editor::Command::MarkIn},
    Binding{Key::O, false, false, cutline::editor::Command::MarkOut},
    Binding{Key::X, true, true, cutline::editor::Command::ClearMarks},
    // And Premiere's markers: M drops one, shift walks to the next, control
    // and shift together walk back.
    Binding{Key::M, false, false, cutline::editor::Command::AddMarker},
    Binding{Key::M, false, true, cutline::editor::Command::NextMarker},
    Binding{Key::M, true, true, cutline::editor::Command::PreviousMarker},
    Binding{Key::M, true, false, cutline::editor::Command::ClearMarkers, true},
    Binding{Key::Left, false, false, cutline::editor::Command::PreviousFrame},
    Binding{Key::Right, false, false, cutline::editor::Command::NextFrame},
    Binding{Key::Home, false, false, cutline::editor::Command::GoToStart},
    Binding{Key::End, false, false, cutline::editor::Command::GoToEnd},
    Binding{Key::Comma, false, false, cutline::editor::Command::NudgeLeft},
    Binding{Key::Period, false, false, cutline::editor::Command::NudgeRight},
    Binding{Key::Delete, false, false, cutline::editor::Command::Delete},
    Binding{Key::Delete, false, true, cutline::editor::Command::RippleDelete},
    Binding{Key::Escape, false, false, cutline::editor::Command::SelectNone},
};

/// The tool palette, in the order it is drawn and offered.
///
/// The keys are Premiere's, which is the point of choosing them: anybody who
/// has used one of these reaches for V and C without thinking, and a shortcut
/// somebody has to learn is one they will not use.
struct ToolEntry {
  cutline::ui::Tool tool;
  IconButton::Icon icon;
  Key key;
};

constexpr std::array kTools{
    ToolEntry{cutline::ui::Tool::Selection, IconButton::Icon::Pointer, Key::V},
    ToolEntry{cutline::ui::Tool::Razor, IconButton::Icon::Razor, Key::C},
    ToolEntry{cutline::ui::Tool::RateStretch, IconButton::Icon::RateStretch, Key::R},
    ToolEntry{cutline::ui::Tool::Slip, IconButton::Icon::Slip, Key::Y},
    ToolEntry{cutline::ui::Tool::Slide, IconButton::Icon::Slide, Key::U},
};

/// Picks a tool, from the palette or from a key.
///
/// The buttons are lit here rather than rebuilt, so the one that was just
/// clicked is still alive when its handler returns.
void choose_tool(App& app, cutline::ui::Tool tool) {
  app.tool = tool;
  if (app.timeline != nullptr) app.timeline->set_tool(tool);
  for (std::size_t i = 0; i < app.tool_buttons.size() && i < kTools.size(); ++i) {
    app.tool_buttons[i]->set_selected(kTools[i].tool == tool);
  }
  mark_dirty(app);
}

/// Runs a command and brings the interface back into line with it.
///
/// One place, so a key and a button that name the same command cannot end up
/// refreshing different things. The command decides whether it applies —
/// nothing here needs to know when a razor has anything to cut.
void run_command(App& app, cutline::editor::Command command) {
  if (cutline::editor::run(app.session, command)) {
    refresh_timeline(app);
    refresh_title(app);
    invalidate_preview(app);
    app.inspector_stale = true;

    // The playhead may have moved: every transport command moves it, and so do
    // the jumps to the next and previous marker. Here rather than in each of
    // them — which is also why the timecode used to sit still while the arrow
    // keys walked the playhead along.
    if (app.readout != nullptr) {
      app.readout->set_text(cutline::core::seconds_to_timecode(
          app.session.playhead(), app.session.project().fps));
    }
    follow_playhead(app);
  }
  mark_dirty(app);
}

/// Runs whichever command the key is bound to, if any.
bool run_binding(App& app, std::span<const Binding> bindings, Key key,
                 const Modifiers& modifiers) {
  for (const Binding& binding : bindings) {
    if (binding.key != key) continue;
    if (binding.control != modifiers.control || binding.shift != modifiers.shift ||
        binding.alt != modifiers.alt) {
      continue;
    }
    run_command(app, binding.command);
    return true;
  }
  return false;
}

// ------------------------------------------------------------------ panels --
//
// Each panel is built once, by name, and then moved around by the dock rather
// than rebuilt. `app` may be null, so the headless check can build every panel
// with no window and no session behind it.

[[nodiscard]] std::unique_ptr<Widget> make_project_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  auto& tools = panel->emplace<Box>(Axis::Horizontal);
  tools.emplace<Button>("Import", [app] {
    if (app != nullptr) import_media(*app);
  });
  tools.emplace<Button>("Remove", [app] {
    if (app != nullptr) remove_from_pool(*app);
  });
  // One button and a menu rather than three buttons. Three would not fit — the
  // first attempt put New Title, Matte and Adjustment in this row and the last
  // of them was cut off by the edge of the panel — and a fourth generator would
  // not fit either.
  auto& generate = tools.emplace<Button>("New");
  generate.set_on_click([app, control = &generate] {
    if (app == nullptr || app->main.host == nullptr) return;

    auto list = std::make_unique<MenuList>(
        std::vector<std::string>{"Title", "Colour Matte", "Adjustment Layer"});
    list->set_on_choose([app](std::size_t index) {
      if (app->main.host != nullptr) app->main.host->close_popup();
      switch (index) {
        case 0: add_title(*app); break;
        case 1: add_matte(*app); break;
        case 2: add_adjustment(*app); break;
        default: break;
      }
    });
    app->main.host->open_popup(std::move(list), control->bounds());
  });
  tools.emplace<Spacer>();

  auto& sort_choice = tools.emplace<Button>("Sort: Pool", [app] {
    if (app == nullptr) return;
    using cutline::editor::BrowserSort;
    // Cycled rather than offered in a menu, because there is no menu yet and a
    // button that does one useful thing beats a control that does nothing.
    constexpr std::array kOrder{BrowserSort::Pool, BrowserSort::Name, BrowserSort::Kind,
                                BrowserSort::Duration, BrowserSort::Uses};
    const auto found = std::ranges::find(kOrder, app->browser_sort);
    const auto next =
        (found == kOrder.end() || found + 1 == kOrder.end()) ? kOrder.begin() : found + 1;
    app->browser_sort = *next;
    refresh_browser(*app);
    app->main.host->request_layout();
  });

  auto& pool = panel->emplace<MediaBrowser>();

  // Double-click or Enter drops it at the playhead; dragging it out puts it
  // where it was released. Both go through the same placement, so a clip
  // arrives the same way however it was asked for.
  pool.set_on_activate([app](std::size_t index) {
    if (app != nullptr) place_from_pool(*app, index);
  });
  pool.set_on_drop([app](std::size_t index, double x, double y) {
    if (app == nullptr) return;
    const auto where = app->timeline == nullptr ? std::nullopt : app->timeline->drop_at(x, y);
    // A drag that ended anywhere but over a track is one that was thought
    // better of, not one that meant the playhead.
    if (where.has_value()) place_from_pool(*app, index, where);
  });

  if (app != nullptr) {
    app->browser = &pool;
    app->sort_button = &sort_choice;
    refresh_browser(*app);
  } else {
    // The headless check builds these with no session behind them. Filling the
    // pool anyway is what makes it lay out and paint real rows in every theme,
    // which is the only thing that would catch a row too tall for the one
    // theme whose lists are roomier.
    pool.set_items(cutline::editor::browser_items(sample_project()));
  }
  return panel;
}

[[nodiscard]] std::unique_ptr<Widget> make_monitor_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  auto& picture = panel->emplace<MonitorView>();
  if (app != nullptr) {
    app->monitor = &picture;
    picture.set_frame(app->pattern.view());
  }

  auto& transport = panel->emplace<Box>(Axis::Horizontal);
  // Both toggle: marking where a mark already is takes it away, which is how
  // one is removed without a third button that exists only to undo the other
  // two. `run` is what decides that, so the button and the I key cannot drift.
  transport.emplace<Button>("Mark In", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::MarkIn);
  });
  transport.emplace<Button>("Mark Out", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::MarkOut);
  });
  transport.emplace<Spacer>();
  auto& play = transport.emplace<Button>("Play", [app] {
    if (app != nullptr) toggle_playback(*app);
  });
  if (app != nullptr) {
    app->play_button = &play;
    // The panel may have been rebuilt mid-playback — a rearrangement, a theme
    // change — and a button that says Play while the sound is running is worse
    // than one that does nothing.
    if (app->playing()) play.set_text("Pause");
  }
  transport.emplace<Button>("Export", [app] {
    if (app != nullptr) open_export_dialog(*app);
  });
  return panel;
}

[[nodiscard]] std::unique_ptr<Widget> make_timeline_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  auto& tools = panel->emplace<Box>(Axis::Horizontal);
  tools.emplace<Button>("Undo", [app] {
    if (app == nullptr || !app->session.undo()) return;
    refresh_timeline(*app);
    refresh_browser(*app);
    app->inspector_stale = true;
  });
  tools.emplace<Button>("Redo", [app] {
    if (app == nullptr || !app->session.redo()) return;
    refresh_timeline(*app);
    refresh_browser(*app);
    app->inspector_stale = true;
  });
  // The tool palette, between the history buttons and the timecode. Premiere
  // floats it over the timeline; a row in the toolbar is the same thing without
  // a second window to keep track of, and it cannot be lost behind anything.
  auto& palette = tools.emplace<Box>(Axis::Horizontal);
  palette.set_spacing(2.0);
  if (app != nullptr) app->tool_buttons.clear();
  for (const ToolEntry& entry : kTools) {
    auto& button = palette.emplace<IconButton>(entry.icon, [app, tool = entry.tool] {
      if (app != nullptr) choose_tool(*app, tool);
    });
    button.set_name(std::string("tool.") + std::string(cutline::ui::to_string(entry.tool)));
    if (app != nullptr) {
      button.set_selected(app->tool == entry.tool);
      app->tool_buttons.push_back(&button);
    }
  }

  tools.emplace<Spacer>();
  auto& readout = tools.emplace<Label>("00:00:00:00");
  readout.set_align(cutline::ui::TextAlign::Right);
  if (app != nullptr) app->readout = &readout;

  auto& tracks = panel->emplace<TimelineView>();
  tracks.set_scale(TimeScale{.pixels_per_second = 60.0});
  if (app != nullptr) {
    app->timeline = &tracks;
    // The panel is new; the tool is not. A rearrangement or a theme change must
    // not quietly put the selection tool back.
    tracks.set_tool(app->tool);
  }

  tracks.set_on_scrub([app](double at) {
    if (app == nullptr) return;
    app->session.set_playhead(at);
    if (app->readout != nullptr) {
      app->readout->set_text(
          cutline::core::seconds_to_timecode(app->session.playhead(), app->session.project().fps));
    }
    follow_playhead(*app);
#if CUTLINE_HAVE_PREVIEW
    // Dragging the playhead while it is playing takes the sound with it.
    // Without this the audio carries on from where it was and the picture is
    // somewhere else, which is worse than either.
    if (app->playing()) {
      app->player->seek(at);
      app->shown_frame = -1;
    }
#endif
    invalidate_preview(*app);
  });

  tracks.set_on_select([app](std::optional<cutline::ui::BlockRef> ref) {
    if (app == nullptr || app->timeline == nullptr) return;
    if (!ref.has_value()) {
      app->session.clear_selection();
      app->inspector_stale = true;
      return;
    }
    const auto id = cutline::editor::block_clip_id(app->timeline->model(), *ref);
    if (id.has_value()) app->session.select_one(*id);
    app->inspector_stale = true;
  });

  // The drag goes back through the model's own operations, so a move that a
  // neighbour would not allow is refused there rather than being allowed here
  // and looking different once the view is rebuilt.
  tracks.set_on_track_toggle([app](cutline::ui::TrackControlRef which) {
    if (app == nullptr || app->timeline == nullptr) return;
    if (which.track >= app->timeline->model().tracks.size()) return;

    app->session.apply(cutline::editor::toggle_track_switch(
        app->session.project(), app->timeline->model().tracks[which.track].id, which.control));
    // A mute silences the track; a solo silences every other one. Either way
    // the headers no longer say what they should, so the whole view is rebuilt
    // rather than the one switch that was pressed.
    refresh_timeline(*app);
    // Hiding a video track changes the picture. Muting does not, and asking for
    // a frame that has not changed costs a comparison.
    invalidate_preview(*app);
    // The sound was decoded from a project that is not this one any more.
    stop_playback(*app);
  });

  tracks.set_on_edit([app](const cutline::ui::TimelineEdit& edit) {
    if (app == nullptr || app->timeline == nullptr) return;
    const auto id = cutline::editor::block_clip_id(app->timeline->model(), edit.block);
    if (!id.has_value()) return;

    app->session.apply(
        cutline::editor::apply_timeline_edit(app->session.project(), *id, edit));
    // Rebuilt whether or not the edit applied: when it did not, the view is
    // showing where the pointer went rather than where the clip is allowed to
    // be, and it has to snap back.
    refresh_timeline(*app);
    // A cut leaves two clips where the panel was showing one, and a rate
    // stretch changes the speed the panel reads out. Neither is visible from
    // the block alone, so the inspector cannot be trusted to still be right.
    app->inspector_stale = true;
    invalidate_preview(*app);
  });

  if (app != nullptr) refresh_timeline(*app);
  return panel;
}

[[nodiscard]] std::unique_ptr<Widget> make_effects_panel(App* app) {
  // Only somewhere to put the controls: they are rebuilt from whatever is
  // selected, and a clip has more parameters than fit in a panel.
  auto panel = std::make_unique<Panel>();
  auto& scroll = panel->emplace<ScrollView>(Axis::Vertical);
  auto& rows = scroll.set_content(std::make_unique<Box>(Axis::Vertical));

  if (app != nullptr) {
    app->inspector = static_cast<Box*>(&rows);
    // Filled the first time the tab is shown, which may be long after startup.
    app->inspector_stale = true;
  }
  return panel;
}

[[nodiscard]] std::unique_ptr<Widget> make_panel(App* app, const PanelId& id) {
  if (id == "project") return make_project_panel(app);
  if (id == "monitor") return make_monitor_panel(app);
  if (id == "timeline") return make_timeline_panel(app);
  if (id == "effects") return make_effects_panel(app);

  // A layout naming a panel this build does not have. Saying so beats an empty
  // rectangle, which looks like a panel that failed to draw.
  auto missing = std::make_unique<Panel>();
  missing->emplace<Label>("There is no panel called " + id).set_small(true);
  return missing;
}

/// Where a drop at a point on the screen would land: which window, and where in
/// its dock.
///
/// Floating windows are asked before the main one, because they are above it.
/// Nothing means the point is over no dock at all — the desktop, another
/// application, or a window's own caption — which is what makes a new window.
[[nodiscard]] std::optional<std::pair<Shell*, cutline::ui::DropTarget>> drop_across(
    App& app, POINT screen) {
  std::vector<Shell*> order;
  for (const std::unique_ptr<Shell>& shell : app.floats) order.push_back(shell.get());
  order.push_back(&app.main);

  for (Shell* shell : order) {
    if (shell->window == nullptr || shell->dock == nullptr) continue;

    POINT local = screen;
    ScreenToClient(shell->window, &local);

    RECT client{};
    GetClientRect(shell->window, &client);
    if (local.x < 0 || local.y < 0 || local.x >= client.right || local.y >= client.bottom) {
      continue;
    }

    // Over this window, so it answers — whether or not the answer is a drop.
    // A point over a caption must not fall through to the window behind it.
    const auto target =
        shell->dock->drop_target(static_cast<double>(local.x), static_cast<double>(local.y));
    if (target.has_value()) return std::pair{shell, *target};
    return std::nullopt;
  }
  return std::nullopt;
}

/// Shows a drag in whichever window the pointer is over, and nowhere else.
void show_drag_across(App& app, const PanelId& panel, POINT screen) {
  const auto landed = drop_across(app, screen);

  for (Shell* shell : app.shells()) {
    if (shell->dock == nullptr) continue;

    if (landed.has_value() && landed->first == shell && !panel.empty()) {
      POINT local = screen;
      ScreenToClient(shell->window, &local);
      shell->dock->set_drag(panel, static_cast<double>(local.x), static_cast<double>(local.y));
    } else {
      shell->dock->set_drag(std::nullopt, 0.0, 0.0);
    }
    shell->dirty = true;
    if (shell->window != nullptr) InvalidateRect(shell->window, nullptr, FALSE);
  }
}

/// Docks every panel of a torn-out window back into the main one.
///
/// What closing a floating window does. Throwing the panels away would be the
/// one action in the application with no way back, because there is no menu to
/// reopen a panel from yet.
void return_panels_home(App& app, const std::string& window_id) {
  const auto found =
      std::ranges::find(layout_of(app).floating, window_id, &cutline::ui::FloatingDock::id);
  if (found == layout_of(app).floating.end()) return;

  for (const PanelId& panel : cutline::ui::panels_in(found->root)) {
    const std::vector<PanelId> home = cutline::ui::panels_in(layout_of(app).root);
    if (home.empty()) {
      // Nothing to dock against: it becomes the main window's whole layout.
      cutline::ui::close_panel(layout_of(app), panel);
      cutline::ui::open_panel(layout_of(app), panel);
      continue;
    }
    cutline::ui::dock_panel(layout_of(app), panel, home.front(), cutline::ui::DockSide::Centre);
  }
  app.dock_stale = true;
}

/// Tears out whichever panel the pointer is over, without a drag.
///
/// The gesture is the drag; this is the same thing on a key. Worth having in
/// its own right — a drag across two windows is not a reachable action for
/// everyone — and it is also the only way any of this can be exercised without
/// a hand on a mouse.
void float_panel_under_cursor(App& app, Shell& shell) {
  if (shell.dock == nullptr || shell.window == nullptr) return;

  POINT screen{};
  GetCursorPos(&screen);
  POINT local = screen;
  ScreenToClient(shell.window, &local);

  for (const cutline::ui::DockGroup* group : shell.dock->groups()) {
    if (!group->bounds().contains(static_cast<double>(local.x), static_cast<double>(local.y))) {
      continue;
    }
    const PanelId panel = group->active_panel();
    if (panel.empty()) return;

    const cutline::ui::Rect where{static_cast<double>(screen.x) - 130.0,
                                  static_cast<double>(screen.y) - 12.0, 520.0, 380.0};
    if (cutline::ui::float_panel(layout_of(app), panel, where)) app.dock_stale = true;
    return;
  }
}

/// Everything a dock reports, wired to the layout.
///
/// The same for the main window and for a torn-out one: which window a panel is
/// in is a fact about the layout, so the handlers are identical and only the
/// tree they were reported from differs.
void wire_dock(App* app, Shell* shell, DockView& dock) {
  dock.set_titles(panel_title);
  dock.set_content_factory(
      [app](const PanelId& id) -> std::unique_ptr<Widget> { return make_panel(app, id); });

  // Every one of these marks rather than rebuilds. What asked is a tab strip
  // inside the tree the rebuild replaces.
  dock.set_on_activate([app](PanelId panel) {
    if (app == nullptr) return;
    if (cutline::ui::activate_panel(layout_of(*app), panel)) app->dock_stale = true;
  });
  dock.set_on_close([app](PanelId panel) {
    if (app == nullptr) return;
    if (cutline::ui::close_panel(layout_of(*app), panel)) app->dock_stale = true;
  });
  dock.set_on_dock([app](PanelId panel, cutline::ui::DropTarget target) {
    if (app == nullptr) return;
    const bool moved =
        target.at_edge ? cutline::ui::dock_panel_at_edge(layout_of(*app), panel, target.side)
                       : cutline::ui::dock_panel(layout_of(*app), panel, target.onto, target.side);
    if (moved) app->dock_stale = true;
  });

  dock.set_on_drag([app, shell](const PanelId& panel, double x, double y) {
    if (app == nullptr || shell == nullptr || shell->window == nullptr) return;
    POINT screen{static_cast<LONG>(x), static_cast<LONG>(y)};
    ClientToScreen(shell->window, &screen);
    show_drag_across(*app, panel, screen);
  });

  // Dropped somewhere this window's own dock did not want it. It may still be
  // over another window — a panel dragged out of a torn-out window and back
  // into the editor comes through here — and only if it is over nothing at all
  // does it become a window of its own.
  dock.set_on_tear_out([app, shell](PanelId panel, double x, double y) {
    if (app == nullptr || shell == nullptr || shell->window == nullptr) return;

    POINT screen{static_cast<LONG>(x), static_cast<LONG>(y)};
    ClientToScreen(shell->window, &screen);
    show_drag_across(*app, PanelId{}, screen);

    if (const auto landed = drop_across(*app, screen); landed.has_value()) {
      const cutline::ui::DropTarget& target = landed->second;
      const bool moved =
          target.at_edge ? cutline::ui::dock_panel_at_edge(layout_of(*app), panel, target.side)
                         : cutline::ui::dock_panel(layout_of(*app), panel, target.onto, target.side);
      if (moved) app->dock_stale = true;
      return;
    }

    // Placed so the caption lands near the cursor rather than the far corner,
    // because the tab being dragged was already under it.
    constexpr double kTornWidth = 520.0;
    constexpr double kTornHeight = 380.0;
    const cutline::ui::Rect where{static_cast<double>(screen.x) - kTornWidth * 0.25,
                                  static_cast<double>(screen.y) - 12.0, kTornWidth, kTornHeight};
    if (cutline::ui::float_panel(layout_of(*app), panel, where)) app->dock_stale = true;
  });

  // A divider is read back rather than rebuilt: nothing about the arrangement
  // changed, only its proportions, and a rebuild would be work for nothing.
  dock.set_on_resize([app, shell] {
    if (app == nullptr || shell == nullptr || shell->dock == nullptr) return;
    if (shell->is_main()) {
      shell->dock->read_fractions_into(layout_of(*app).root);
      return;
    }
    const auto found = std::ranges::find(layout_of(*app).floating, shell->floating_id,
                                         &cutline::ui::FloatingDock::id);
    if (found != layout_of(*app).floating.end()) shell->dock->read_fractions_into(found->root);
  });
}

/// Moves to another named arrangement.
///
/// Nothing is copied: the layout being left *is* the workspace's, so whatever
/// it was dragged into stays with it and is there on the way back.
void set_workspace(App& app, std::string_view name) {
  if (!cutline::editor::activate_workspace(app.workspaces, name)) return;
  if (app.workspace_button != nullptr) {
    app.workspace_button->set_text("Workspace: " + app.workspaces.active);
  }
  app.dock_stale = true;
}

/// Cycles to the next arrangement. A menu is what this wants to be; a button
/// that does one useful thing is what it can be until there is one.
void next_workspace(App& app) {
  const auto& all = app.workspaces.named;
  if (all.size() < 2) return;

  const auto found = std::ranges::find(all, app.workspaces.active,
                                       &cutline::editor::Workspace::name);
  const auto next = (found == all.end() || found + 1 == all.end()) ? all.begin() : found + 1;
  set_workspace(app, next->name);
}

/// Puts the arrangement back to how it was defined, and says so when there is
/// nothing to go back to.
void reset_workspace(App& app) {
  if (cutline::editor::reset_workspace(app.workspaces, app.workspaces.active)) {
    app.dock_stale = true;
    return;
  }
  complain(app.main.window,
           app.workspaces.active + " is already as it was defined, or is one you made "
                                   "yourself and so has nothing to go back to.");
}

/// Writes the arrangements out. Quiet on success, and quiet on failure too
/// except at the point it matters — losing a layout is not worth a dialog in
/// the middle of an edit.
void save_workspaces(App& app, bool complain_on_failure) {
  const auto written = cutline::editor::write_workspaces(app.workspace_file, app.workspaces);
  if (!written.has_value() && complain_on_failure) {
    complain(app.main.window, "Could not save the workspace.\n\n" + written.error());
  }
}

/// Rebuilds the dock from the layout.
///
/// Panel contents are carried across by the view itself, so this costs a walk
/// of the tree rather than building a browser and a timeline again.
void refresh_dock(App& app) {
  // Windows first: a rearrangement may have emptied one or made another, and
  // rebuilding a view that is about to be destroyed is wasted work.
  reconcile_windows(app);

  for (Shell* shell : app.shells()) {
    if (shell->dock == nullptr) continue;

    const DockNode* node = &layout_of(app).root;
    if (!shell->is_main()) {
      const auto found =
          std::ranges::find(layout_of(app).floating, shell->floating_id, &FloatingDock::id);
      if (found == layout_of(app).floating.end()) continue;
      node = &found->root;
    }

    shell->dock->set_node(*node);
    if (shell->host != nullptr) shell->host->request_layout();
    shell->dirty = true;
    if (shell->window != nullptr) InvalidateRect(shell->window, nullptr, FALSE);
  }
  refresh_float_titles(app);
}

/// The window's own widget tree: a caption, a theme switcher, and the dock.
///
/// `app` may be null, so the headless check can build the same tree with no
/// window behind it.
[[nodiscard]] std::unique_ptr<Widget> build_interface(App* app) {
  auto shell = std::make_unique<Box>(Axis::Vertical);
  shell->set_spacing(0.0);

  // The window's own caption. The system one cannot be themed, so it is turned
  // off in `WM_NCCALCSIZE` and this is drawn in its place.
  auto& caption = shell->emplace<TitleBar>("Cutline");
  if (app != nullptr) app->main.title_bar = &caption;

  caption.emplace<CaptionButton>(CaptionButton::Kind::Minimise, [app] {
    if (app != nullptr && app->main.window != nullptr) ShowWindow(app->main.window, SW_MINIMIZE);
  });
  auto& maximise = caption.emplace<CaptionButton>(CaptionButton::Kind::Maximise, [app] {
    if (app == nullptr || app->main.window == nullptr) return;
    ShowWindow(app->main.window, IsZoomed(app->main.window) ? SW_RESTORE : SW_MAXIMIZE);
  });
  caption.emplace<CaptionButton>(CaptionButton::Kind::Close, [app] {
    if (app != nullptr && app->main.window != nullptr) PostMessageW(app->main.window, WM_CLOSE, 0, 0);
  });
  if (app != nullptr) app->main.maximise = &maximise;

  // The switcher lives in the window rather than on a keyboard shortcut. A
  // harness whose only control is an undocumented keystroke is one nobody can
  // use, including whoever wrote it.
  auto& bar = shell->emplace<Box>(Axis::Horizontal);
  bar.set_padding(cutline::ui::Edges::all(4.0));
  bar.emplace<Label>("Theme");
  for (std::size_t i = 0; i < built_in_themes().size(); ++i) {
    auto& choice = bar.emplace<Button>(built_in_themes()[i].name, [app, i] {
      if (app != nullptr) set_theme(*app, i);
    });
    choice.set_selected(app != nullptr && i == app->theme);
    if (app != nullptr) app->theme_buttons.push_back(&choice);
  }
  bar.emplace<Spacer>();

  auto& workspace_choice = bar.emplace<Button>("Workspace: Editing", [app] {
    if (app != nullptr) next_workspace(*app);
  });
  if (app != nullptr) {
    app->workspace_button = &workspace_choice;
    workspace_choice.set_text("Workspace: " + app->workspaces.active);
  }
  bar.emplace<Button>("Reset", [app] {
    if (app != nullptr) reset_workspace(*app);
  });

  // Everything under the toolbar is the dock. Which panel is where is data now
  // rather than structure: this builds a view over a layout, and dragging a tab
  // is what changes the layout.
  auto& dock = shell->emplace<DockView>();
  if (app != nullptr) app->main.dock = &dock;

  wire_dock(app, app == nullptr ? nullptr : &app->main, dock);
  dock.set_node(app == nullptr ? default_layout().root : layout_of(*app).root);
  return shell;
}

// ------------------------------------------------------------------ windows --

/// The tree a torn-out window gets: a caption it can be dragged by, and its
/// dock. No theme switcher and no menu — it is one or two panels, not a second
/// copy of the editor.
// ------------------------------------------------------------------ export --

#if CUTLINE_HAVE_PREVIEW

/// The formats offered, and what each means to the encoder.
constexpr std::array kExportCodecs{"H.264", "HEVC (H.265)"};

/// Named qualities rather than a bare number. The encoder wants a CRF, which
/// is a scale most people have no reason to know and which runs the wrong way
/// round — lower is better. Naming them is what the reference did too.
struct QualityChoice {
  const char* name;
  int quality;
};
constexpr std::array kExportQualities{
    QualityChoice{"Best (large file)", 16},
    QualityChoice{"High", 18},
    QualityChoice{"Medium", 21},
    QualityChoice{"Low (small file)", 25},
};

/// Zero means the project's own rate, which is the honest default: re-timing a
/// sequence on the way out is a decision, not a preference.
struct RateChoice {
  const char* name;
  double fps;
};
constexpr std::array kExportRates{
    RateChoice{"Same as sequence", 0.0}, RateChoice{"24 fps", 24.0},
    RateChoice{"25 fps", 25.0},          RateChoice{"30 fps", 30.0},
    RateChoice{"50 fps", 50.0},          RateChoice{"60 fps", 60.0},
};

/// Output sizes, as a fraction of the sequence's own.
///
/// A fraction rather than a fixed pair of numbers, because the aspect ratio is
/// the sequence's business: offering "1280 x 720" to a vertical sequence would
/// have to either letterbox it or stretch it, and neither is what choosing a
/// smaller size means. What each fraction works out to in pixels is put in the
/// label, so nothing has to be taken on trust.
struct ResolutionChoice {
  const char* name;
  double scale;
};
constexpr std::array kExportResolutions{
    ResolutionChoice{"Same as sequence", 1.0}, ResolutionChoice{"3/4", 0.75},
    ResolutionChoice{"2/3", 2.0 / 3.0},        ResolutionChoice{"1/2", 0.5},
    ResolutionChoice{"1/3", 1.0 / 3.0},        ResolutionChoice{"1/4", 0.25},
};

/// How the mix is folded down. Mono sums the sources rather than dropping a
/// side; the resampler does it as each one is decoded.
struct MixdownChoice {
  const char* name;
  int channels;
};
constexpr std::array kExportMixdowns{
    MixdownChoice{"Stereo", 2},
    MixdownChoice{"Mono", 1},
};

/// The output size a resolution choice works out to for this project.
///
/// Even in both directions: H.264 and HEVC encode in even-sized blocks, and an
/// odd size would be rounded down by the encoder anyway — better to round here,
/// where it can be shown, than to say one size and write another.
[[nodiscard]] std::pair<int, int> export_size(const App& app, std::size_t choice) {
  const cutline::core::Project& project = app.session.project();
  const double scale = choice < kExportResolutions.size() ? kExportResolutions[choice].scale : 1.0;
  const auto even = [](double value) {
    return std::max(2, static_cast<int>(std::llround(value)) & ~1);
  };
  return {even(project.canvas_w * scale), even(project.canvas_h * scale)};
}

void close_export_dialog(App& app);

/// Where the export writes to by default: beside the project, named after it.
[[nodiscard]] std::filesystem::path default_export_path(const App& app) {
  const std::filesystem::path& project = app.session.path();
  if (project.empty()) return {};
  return std::filesystem::path(project).replace_extension(".mp4");
}

/// What the button under "Output Name" says. The full path is too long for the
/// dialog and the interesting part is the end of it.
[[nodiscard]] std::string output_label(const App& app) {
  if (app.export_setup.path.empty()) return "Choose a file...";
  return app.export_setup.path.filename().string();
}

void refresh_export_dialog(App& app) {
  if (app.export_window == nullptr) return;

  const bool busy = app.exporting();
  if (app.export_output != nullptr) app.export_output->set_text(output_label(app));
  if (app.export_mixdown != nullptr) app.export_mixdown->set_enabled(app.export_setup.audio);
  if (app.export_start != nullptr) {
    app.export_start->set_text(busy ? "Cancel" : "Export");
    // Nothing to export to is not an error worth a dialog; it is a button that
    // should not be pressable yet.
    app.export_start->set_enabled(busy || !app.export_setup.path.empty());
  }
  if (app.export_progress != nullptr) {
    app.export_progress->set_fraction(app.export_job.fraction.load());
  }
  if (app.export_status != nullptr && !busy && !app.export_job.outcome.empty()) {
    app.export_status->set_text(app.export_job.outcome);
  }
  mark_dirty(app);
}

/// Runs the export on its own thread.
///
/// Its own `gpu::Device` as well, deliberately. The compositor keeps one
/// command list and one allocator, so two threads compositing on the same
/// device would be a race on both. The window keeps drawing on the shared
/// device throughout; the export gets its own and they never meet.
void start_export(App& app) {
  if (app.exporting() || app.export_setup.path.empty()) return;

  const cutline::core::Project project = app.session.project();
  cutline::engine::ExportSettings settings;
  settings.path = app.export_setup.path.string();
  settings.codec = app.export_setup.codec == 1 ? cutline::media::VideoCodec::Hevc
                                               : cutline::media::VideoCodec::H264;
  settings.quality = kExportQualities[app.export_setup.quality].quality;
  settings.fps = kExportRates[app.export_setup.rate].fps;
  const auto [width, height] = export_size(app, app.export_setup.resolution);
  settings.width = width;
  settings.height = height;
  settings.audio = app.export_setup.audio;
  settings.audio_channels = kExportMixdowns[app.export_setup.mixdown].channels;
  if (app.export_setup.marked_only && cutline::core::has_marks(project)) {
    const cutline::core::MarkedSpan span = cutline::core::marked_span(project);
    settings.start = span.start;
    settings.duration = span.duration;
  }

  App::ExportJob& job = app.export_job;
  job.cancel = false;
  job.fraction = 0.0;
  job.finished = false;
  job.failed = false;
  job.outcome.clear();
  job.running = true;

  if (app.export_status != nullptr) app.export_status->set_text("Exporting...");

  job.worker = std::thread([&app, &job, project, settings] {
    auto device = cutline::gpu::Device::create();
    if (!device.has_value()) {
      job.outcome = "No usable graphics device: " + device.error();
      job.failed = true;
      job.finished = true;
      job.running = false;
      return;
    }

    const auto result = cutline::engine::export_project(
        *device, project, settings, [&job](const cutline::engine::ExportProgress& progress) {
          if (progress.total > 0) {
            job.fraction = static_cast<double>(progress.frame) /
                           static_cast<double>(progress.total);
          }
          // Returning false is how the exporter is told to stop, which leaves a
          // truncated file rather than a complete one.
          return !job.cancel.load();
        });

    if (!result.has_value()) {
      job.outcome = result.error();
      job.failed = true;
    } else if (job.cancel.load()) {
      job.outcome = "Cancelled.";
      job.failed = true;
    } else {
      job.outcome = std::format("Done — {} frames in {:.1f}s ({:.1f} fps) via {}",
                                result->frames, result->elapsed_seconds,
                                result->frames_per_second(), result->encoder);
    }
    job.finished = true;
    job.running = false;
  });

  refresh_export_dialog(app);
}

/// Asks the export to stop. It notices at the next frame it finishes.
void cancel_export(App& app) {
  if (!app.exporting()) return;
  app.export_job.cancel = true;
  if (app.export_status != nullptr) app.export_status->set_text("Cancelling...");
  mark_dirty(app);
}

/// Joins a finished worker and shows what it did. Called once a frame while a
/// window is open, which is also what keeps the progress bar moving.
void poll_export(App& app) {
  App::ExportJob& job = app.export_job;
  if (job.running.load()) {
    refresh_export_dialog(app);
    return;
  }
  if (!job.finished.load()) return;

  job.finished = false;
  if (job.worker.joinable()) job.worker.join();
  job.fraction = job.failed ? 0.0 : 1.0;
  refresh_export_dialog(app);
}

/// Waits for an export to finish, because the process is going away and the
/// thread reads a project this is about to destroy.
void settle_export(App& app) {
  if (app.export_job.running.load()) app.export_job.cancel = true;
  if (app.export_job.worker.joinable()) app.export_job.worker.join();
}

[[nodiscard]] std::unique_ptr<Widget> build_export_interface(App* app, Shell* shell) {
  auto root = std::make_unique<Box>(Axis::Vertical);
  root->set_spacing(0.0);

  auto& caption = root->emplace<TitleBar>("Export Settings");
  shell->title_bar = &caption;
  caption.emplace<CaptionButton>(CaptionButton::Kind::Close, [app] {
    if (app != nullptr) close_export_dialog(*app);
  });

  auto& body = root->emplace<Box>(Axis::Vertical);
  body.set_padding(Edges{16.0, 16.0, 16.0, 16.0});
  body.set_spacing(10.0);

  // One row per setting, label on the left, control on the right — which is
  // how every settings dialog on the platform reads, Premiere's included.
  const auto row = [&body](std::string label) -> Box& {
    auto& line = body.emplace<Box>(Axis::Horizontal);
    line.set_spacing(10.0);
    auto& text = line.emplace<Label>(std::move(label));
    text.set_align(TextAlign::Left);
    return line;
  };

  {
    auto& line = row("Format");
    auto& choice = line.emplace<Dropdown>(
        std::vector<std::string>{kExportCodecs.begin(), kExportCodecs.end()},
        app != nullptr ? app->export_setup.codec : 0);
    choice.set_on_change([app](std::size_t index) {
      if (app != nullptr) app->export_setup.codec = index;
    });
  }
  {
    auto& line = row("Quality");
    std::vector<std::string> names;
    for (const QualityChoice& quality : kExportQualities) names.emplace_back(quality.name);
    auto& choice =
        line.emplace<Dropdown>(std::move(names), app != nullptr ? app->export_setup.quality : 1);
    choice.set_on_change([app](std::size_t index) {
      if (app != nullptr) app->export_setup.quality = index;
    });
  }
  {
    auto& line = row("Resolution");
    std::vector<std::string> names;
    for (std::size_t i = 0; i < kExportResolutions.size(); ++i) {
      if (app == nullptr) {
        names.emplace_back(kExportResolutions[i].name);
        continue;
      }
      const auto [width, height] = export_size(*app, i);
      names.push_back(std::format("{} ({} x {})", kExportResolutions[i].name, width, height));
    }
    auto& choice = line.emplace<Dropdown>(std::move(names),
                                          app != nullptr ? app->export_setup.resolution : 0);
    choice.set_on_change([app](std::size_t index) {
      if (app != nullptr) app->export_setup.resolution = index;
    });
  }
  {
    auto& line = row("Frame rate");
    std::vector<std::string> names;
    for (const RateChoice& rate : kExportRates) names.emplace_back(rate.name);
    auto& choice =
        line.emplace<Dropdown>(std::move(names), app != nullptr ? app->export_setup.rate : 0);
    choice.set_on_change([app](std::size_t index) {
      if (app != nullptr) app->export_setup.rate = index;
    });
  }
  {
    auto& line = row("Output name");
    auto& output = line.emplace<Button>(app != nullptr ? output_label(*app) : "Choose a file...",
                                        [app] {
                                          if (app == nullptr) return;
                                          const auto chosen = choose_movie_file(
                                              app->export_window != nullptr
                                                  ? app->export_window->window
                                                  : app->main.window);
                                          if (!chosen.has_value()) return;
                                          app->export_setup.path = *chosen;
                                          refresh_export_dialog(*app);
                                        });
    if (app != nullptr) app->export_output = &output;
  }

  {
    auto& line = row("Audio");
    std::vector<std::string> names;
    for (const MixdownChoice& mix : kExportMixdowns) names.emplace_back(mix.name);
    auto& choice =
        line.emplace<Dropdown>(std::move(names), app != nullptr ? app->export_setup.mixdown : 0);
    choice.set_on_change([app](std::size_t index) {
      if (app != nullptr) app->export_setup.mixdown = index;
    });
    if (app != nullptr) app->export_mixdown = &choice;
  }

  auto& sound = body.emplace<Checkbox>("Export audio", app == nullptr || app->export_setup.audio);
  sound.set_on_change([app](bool on) {
    if (app == nullptr) return;
    app->export_setup.audio = on;
    // A mixdown for a file with no audio in it is a choice with nothing behind
    // it, so it greys out rather than sitting there looking answerable.
    refresh_export_dialog(*app);
  });

  // Labelled with the span it means. "In to out" says nothing about how long
  // the file will be, and this is the last screen before a render that may take
  // minutes.
  const bool marked = app != nullptr && cutline::core::has_marks(app->session.project());
  std::string range = "Only the marked range";
  if (marked) {
    const cutline::core::MarkedSpan span = cutline::core::marked_span(app->session.project());
    const double fps = app->session.project().fps;
    range += std::format(" ({} to {})", cutline::core::seconds_to_timecode(span.start, fps),
                         cutline::core::seconds_to_timecode(span.start + span.duration, fps));
  }
  auto& only = body.emplace<Checkbox>(std::move(range),
                                      app != nullptr && app->export_setup.marked_only && marked);
  only.set_enabled(marked);
  only.set_on_change([app](bool on) {
    if (app != nullptr) app->export_setup.marked_only = on;
  });

  body.emplace<Spacer>();

  auto& progress = body.emplace<ProgressBar>();
  if (app != nullptr) app->export_progress = &progress;

  auto& status = body.emplace<Label>("");
  status.set_small(true);
  if (app != nullptr) app->export_status = &status;

  auto& buttons = body.emplace<Box>(Axis::Horizontal);
  buttons.set_spacing(8.0);
  buttons.emplace<Spacer>();
  buttons.emplace<Button>("Close", [app] {
    if (app != nullptr) close_export_dialog(*app);
  });
  auto& go = buttons.emplace<Button>("Export", [app] {
    if (app == nullptr) return;
    if (app->exporting()) {
      cancel_export(*app);
    } else {
      start_export(*app);
    }
  });
  if (app != nullptr) app->export_start = &go;

  return root;
}

void open_export_dialog(App& app) {
  if (app.export_window != nullptr) {
    SetForegroundWindow(app.export_window->window);
    return;
  }
  if (app.export_setup.path.empty()) app.export_setup.path = default_export_path(app);

  auto shell = std::make_unique<Shell>();
  shell->app = &app;
  shell->is_dialog = true;
  shell->host = std::make_unique<WidgetHost>(build_export_interface(&app, shell.get()));

  // Centred on the main window, which is where a dialog belongs and where the
  // eye already is.
  RECT owner{};
  GetWindowRect(app.main.window, &owner);
  constexpr int kWidth = 460;
  constexpr int kHeight = 460;
  const int x = owner.left + ((owner.right - owner.left) - kWidth) / 2;
  const int y = owner.top + ((owner.bottom - owner.top) - kHeight) / 2;

  const HWND window =
      CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"Export Settings",
                      WS_POPUP | WS_THICKFRAME, x, y, kWidth, kHeight, app.main.window, nullptr,
                      GetModuleHandleW(nullptr), nullptr);
  if (window == nullptr) return;

  shell->window = window;
  SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(shell.get()));

  const MARGINS shadow{0, 0, 1, 0};
  DwmExtendFrameIntoClientArea(window, &shadow);
  SetWindowPos(window, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

  app.export_window = std::move(shell);
  ShowWindow(window, SW_SHOW);
  SetForegroundWindow(window);
  refresh_export_dialog(app);
}

void close_export_dialog(App& app) {
  if (app.export_window == nullptr) return;
  // An export in flight keeps going. It writes to a file, not to the dialog,
  // and stopping it because a window was closed would throw away minutes of
  // work for no reason anybody asked for.
  app.export_output = nullptr;
  app.export_progress = nullptr;
  app.export_status = nullptr;
  app.export_start = nullptr;
  app.export_mixdown = nullptr;

  const HWND window = app.export_window->window;
  app.export_window.reset();
  if (window != nullptr) DestroyWindow(window);
  mark_dirty(app);
}

#else
void open_export_dialog(App&) {}
void poll_export(App&) {}
void settle_export(App&) {}
#endif

[[nodiscard]] std::unique_ptr<Widget> build_floating_interface(App* app, Shell* shell) {
  auto root = std::make_unique<Box>(Axis::Vertical);
  root->set_spacing(0.0);

  auto& caption = root->emplace<TitleBar>("Cutline");
  shell->title_bar = &caption;

  caption.emplace<CaptionButton>(CaptionButton::Kind::Close, [app, shell] {
    // The panels go home rather than being thrown away. There is no menu to
    // reopen one from yet, so closing a window would otherwise be the one
    // action in the application that cannot be undone by any means.
    if (app != nullptr) return_panels_home(*app, shell->floating_id);
  });

  auto& dock = root->emplace<DockView>();
  shell->dock = &dock;
  wire_dock(app, shell, dock);
  return root;
}

/// Opens a real window for a floating dock.
void open_float_window(App& app, const cutline::ui::FloatingDock& floating) {
  auto shell = std::make_unique<Shell>();
  shell->app = &app;
  shell->floating_id = floating.id;
  shell->host = std::make_unique<WidgetHost>(build_floating_interface(&app, shell.get()));
  shell->dock->set_node(floating.root);

  // `WS_POPUP | WS_THICKFRAME` is a window with no caption that can still be
  // resized by its edges, which is what a drawn caption needs. Owned by the
  // main window so it stays above it and goes away with it.
  const HWND window = CreateWindowExW(
      WS_EX_TOOLWINDOW, kWindowClass, L"Cutline", WS_POPUP | WS_THICKFRAME,
      static_cast<int>(floating.bounds.x), static_cast<int>(floating.bounds.y),
      static_cast<int>(floating.bounds.width), static_cast<int>(floating.bounds.height),
      app.main.window, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (window == nullptr) return;

  shell->window = window;
  SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(shell.get()));

  const MARGINS shadow{0, 0, 1, 0};
  DwmExtendFrameIntoClientArea(window, &shadow);
  SetWindowPos(window, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

  // Shown without taking the keyboard: the panel was dragged out, not asked
  // to become the thing being typed into.
  ShowWindow(window, SW_SHOWNOACTIVATE);
  app.floats.push_back(std::move(shell));
}

void reconcile_windows(App& app) {
  // Windows whose dock has gone, because its last panel was docked elsewhere.
  for (auto it = app.floats.begin(); it != app.floats.end();) {
    const bool alive =
        std::ranges::find(layout_of(app).floating, (*it)->floating_id,
                          &cutline::ui::FloatingDock::id) != layout_of(app).floating.end();
    if (alive) {
      ++it;
      continue;
    }

    const HWND doomed = (*it)->window;
    // Unhooked before it is destroyed. `DestroyWindow` sends `WM_DESTROY`
    // straight back here, and the shell it would look up is about to be freed.
    if (doomed != nullptr) SetWindowLongPtrW(doomed, GWLP_USERDATA, 0);
    it = app.floats.erase(it);
    if (doomed != nullptr) DestroyWindow(doomed);
  }

  // Docks with no window yet, because something was just torn out.
  for (const cutline::ui::FloatingDock& floating : layout_of(app).floating) {
    const bool built = std::ranges::any_of(app.floats, [&](const std::unique_ptr<Shell>& shell) {
      return shell->floating_id == floating.id;
    });
    if (!built) open_float_window(app, floating);
  }
}

/// A torn-out window is named after whatever it is showing.
void refresh_float_titles(App& app) {
  for (const std::unique_ptr<Shell>& shell : app.floats) {
    if (shell->title_bar == nullptr || shell->dock == nullptr) continue;

    std::string title = "Cutline";
    if (const auto showing = shell->dock->node().active_panel(); showing.has_value()) {
      title = panel_title(*showing);
    } else if (const std::vector<PanelId> inside = cutline::ui::panels_in(shell->dock->node());
               !inside.empty()) {
      title = panel_title(inside.front());
    }

    shell->title_bar->set_title(title);
    if (shell->window != nullptr) SetWindowTextA(shell->window, title.c_str());
  }
}

/// Win32 virtual keys to the ones the interface knows about. Only the keys
/// that are actually bound; the rest arrive as `Key::None` and go nowhere.
[[nodiscard]] Key key_from_win32(WPARAM vk) {
  if (vk >= 'A' && vk <= 'Z') return static_cast<Key>(vk);
  if (vk >= '0' && vk <= '9') return static_cast<Key>(vk);
  switch (vk) {
    case VK_SPACE: return Key::Space;
    case VK_ESCAPE: return Key::Escape;
    case VK_TAB: return Key::Tab;
    case VK_RETURN: return Key::Enter;
    case VK_BACK: return Key::Backspace;
    case VK_DELETE: return Key::Delete;
    case VK_LEFT: return Key::Left;
    case VK_RIGHT: return Key::Right;
    case VK_UP: return Key::Up;
    case VK_DOWN: return Key::Down;
    case VK_HOME: return Key::Home;
    case VK_END: return Key::End;
    case VK_PRIOR: return Key::PageUp;
    case VK_NEXT: return Key::PageDown;
    default: return Key::None;
  }
}

[[nodiscard]] Modifiers modifiers_now() {
  return Modifiers{
      .shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0,
      .control = (GetKeyState(VK_CONTROL) & 0x8000) != 0,
      .alt = (GetKeyState(VK_MENU) & 0x8000) != 0,
  };
}

[[nodiscard]] MouseEvent mouse_from(LPARAM lparam, MouseButton button, int clicks = 1) {
  return MouseEvent{
      .x = static_cast<double>(GET_X_LPARAM(lparam)),
      .y = static_cast<double>(GET_Y_LPARAM(lparam)),
      .button = button,
      .modifiers = modifiers_now(),
      .click_count = clicks,
  };
}

/// The device every window here draws on, made once and kept.
///
/// Empty when there is no compositor in this build, or when no device could be
/// made — in both cases the window falls back to creating its own, which is
/// what it did before there was anything to share.
[[nodiscard]] cutline::ui::AdoptedDevice shared_device(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.device == nullptr && !app.device_failed) {
    auto made = cutline::gpu::Device::create();
    if (made.has_value()) {
      app.device = std::move(*made);
    } else {
      // Not worth interrupting anybody over: everything still works, just with
      // a copy in the middle. The preview says so if it cannot start at all.
      app.device_failed = true;
    }
  }
  if (app.device == nullptr) return {};
  return cutline::ui::AdoptedDevice{.adapter = app.device->native_adapter(),
                                    .device = app.device->native_device(),
                                    .queue = app.device->native_queue()};
#else
  (void)app;
  return {};
#endif
}

/// Makes or resizes the window's swapchain.
///
/// A failure is said once and then the window simply does not draw. There is
/// nothing sensible to fall back to: the raster path this replaced was slower
/// by an order of magnitude, and the themes with a blur behind their chrome
/// were not usable on it at all.
void resize_surface(Shell& shell, int width, int height) {
  if (width <= 0 || height <= 0) return;
  if (shell.surface != nullptr && width == shell.width && height == shell.height) return;

  shell.width = width;
  shell.height = height;

  if (shell.surface != nullptr) {
    if (const auto resized = shell.surface->resize(width, height); !resized.has_value()) {
      shell.surface.reset();
      if (!shell.warned) {
        shell.warned = true;
        complain(shell.window, "The window stopped drawing.\n\n" + resized.error());
      }
    }
    return;
  }

  auto made = cutline::ui::SkiaWindow::create(shell.window, width, height,
                                              shared_device(*shell.app));
  if (!made.has_value()) {
    if (!shell.warned) {
      shell.warned = true;
      complain(shell.window, "Could not draw on this display.\n\n" + made.error());
    }
    return;
  }
  shell.surface = std::move(*made);
}

/// The work that belongs to the document rather than to any one window.
///
/// Done once a frame, from whichever window is drawing first. Each of these
/// rebuilds part of the interface, so none of them can happen where they are
/// asked for — the thing asking is usually inside what the rebuild replaces.
void settle(App& app) {
  // The arrangement first, because rebuilding it is what creates a panel that
  // has never been shown before — and that panel then wants filling in this
  // same frame rather than staying blank until something else changes.
  if (app.dock_stale) {
    app.dock_stale = false;
    refresh_dock(app);
  }
  if (app.inspector_stale) {
    app.inspector_stale = false;
    refresh_inspector(app);
  }
#if CUTLINE_HAVE_PREVIEW
  // Once per frame at most, however many mouse moves a scrub produced. A
  // read-back stalls the GPU, so rendering on each one would make the drag
  // itself the slow part.
  if (app.preview_stale) {
    app.preview_stale = false;
    refresh_preview(app);
  }
#endif
}

/// Draws one window's tree and puts the pixels on it.
void render(Shell& shell) {
  if (shell.app == nullptr || shell.host == nullptr || shell.surface == nullptr) return;
  App& app = *shell.app;

  // The swapchain's next buffer, once the GPU has finished with it. Null means
  // the swapchain is not usable, and the window goes unpainted rather than
  // being drawn somewhere nothing will look.
  auto* canvas = static_cast<SkCanvas*>(shell.surface->begin_frame());
  if (canvas == nullptr) return;

  const Theme& theme = app.current();
  // The window's texture cache goes with the painter: a wrapper for somebody
  // else's GPU frame has to outlive the paint that drew it, and the painter
  // does not.
  const std::unique_ptr<SkiaPainter> painter =
      SkiaPainter::create(canvas, shell.surface->textures());
  if (painter == nullptr) {
    shell.surface->present();
    return;
  }

  const Rect client{0.0, 0.0, static_cast<double>(shell.width),
                    static_cast<double>(shell.height)};
  const LayoutContext context{theme, *painter};

  settle(app);

  // The one place a theme and a text measurer are both in hand, which is
  // exactly why layout is deferred to here rather than done where the size
  // changed or the drag happened.
  if (shell.resized) {
    shell.host->resize(client, context);
    shell.resized = false;
  } else {
    shell.host->update_layout(context);
  }

  // Through the painter rather than a canvas clear, so a themed background
  // that is a gradient stays one.
  canvas->clear(SK_ColorBLACK);
  paint_surface(*painter, client, theme.style(cutline::ui::Part::Window));
  shell.host->paint(*painter, theme);

  shell.surface->present();
}

void set_theme(App& app, std::size_t index) {
  if (index >= built_in_themes().size() || index == app.theme) return;
  app.theme = index;

  for (std::size_t i = 0; i < app.theme_buttons.size(); ++i) {
    app.theme_buttons[i]->set_selected(i == index);
  }

  // Metrics change with the theme, so everything has to be measured again —
  // in every window, not only the one whose button was pressed.
  for (Shell* shell : app.shells()) {
    if (shell->host != nullptr) shell->host->request_layout();
    shell->dirty = true;
    if (shell->window != nullptr) InvalidateRect(shell->window, nullptr, FALSE);
  }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* shell = reinterpret_cast<Shell*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (shell == nullptr || shell->app == nullptr || shell->host == nullptr) {
    return DefWindowProcW(window, message, wparam, lparam);
  }
  App* app = shell->app;

  switch (message) {
    case WM_NCCALCSIZE: {
      // The whole point: with no non-client area, there is no system caption
      // and no system border, and the interface owns every pixel.
      if (wparam == FALSE) break;
      auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);

      if (IsZoomed(window)) {
        // A maximised window's rectangle deliberately overhangs the monitor by
        // the frame thickness. Without putting that back, the top of the
        // interface — the caption itself — sits off the screen.
        const int frame = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
        params->rgrc[0].left += frame;
        params->rgrc[0].right -= frame;
        params->rgrc[0].top += frame;
        params->rgrc[0].bottom -= frame;
      }
      return 0;
    }

    case WM_NCHITTEST: {
      POINT at{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window, &at);

      RECT client{};
      GetClientRect(window, &client);

      // The resize edges have to be answered by hand now that there is no
      // system frame to do it. A maximised window has none.
      if (!IsZoomed(window)) {
        const LONG grab = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
        const bool left = at.x < grab;
        const bool right = at.x >= client.right - grab;
        const bool top = at.y < grab;
        const bool bottom = at.y >= client.bottom - grab;

        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
      }

      // The widget tree decides what drags the window: the caption itself
      // does, and anything sitting on it — a button — does not. Asking the
      // tree rather than comparing against a height is what keeps the two from
      // drifting apart.
      if (shell->title_bar != nullptr &&
          shell->host->root().at(static_cast<double>(at.x), static_cast<double>(at.y)) ==
              shell->title_bar) {
        return HTCAPTION;
      }
      return HTCLIENT;
    }

    case WM_SIZE:
      resize_surface(*shell, LOWORD(lparam), HIWORD(lparam));
      shell->resized = true;
      shell->dirty = true;
      // Maximising swaps which glyph the middle caption button shows.
      if (shell->maximise != nullptr) {
        shell->maximise->set_kind(IsZoomed(window) ? CaptionButton::Kind::Restore
                                                 : CaptionButton::Kind::Maximise);
      }
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT paint;
      BeginPaint(window, &paint);
      render(*shell);
      EndPaint(window, &paint);
      shell->dirty = false;
      shell->host->clear_paint();
      return 0;
    }

    case WM_MOUSEMOVE: {
      // Asked for once per entry, so WM_MOUSELEAVE arrives and hover clears
      // when the pointer goes somewhere else entirely.
      TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0};
      TrackMouseEvent(&track);
      shell->host->mouse_move(mouse_from(lparam, MouseButton::Left));
      // Not unconditionally: the pointer crossing a panel that does not care
      // leaves the picture exactly as it was, and a full repaint costs
      // milliseconds. `--benchmark` is where that number comes from.
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;
    }
    case WM_MOUSELEAVE:
      shell->host->mouse_exit();
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;

    case WM_LBUTTONDOWN:
      SetCapture(window);
      shell->host->mouse_down(mouse_from(lparam, MouseButton::Left));
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;
    case WM_LBUTTONDBLCLK:
      shell->host->mouse_down(mouse_from(lparam, MouseButton::Left, 2));
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;
    case WM_LBUTTONUP:
      ReleaseCapture();
      shell->host->mouse_up(mouse_from(lparam, MouseButton::Left));
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;

    case WM_MOUSEWHEEL: {
      // Wheel messages carry screen coordinates, unlike every other mouse
      // message, so they have to be brought back into the client area.
      POINT at{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window, &at);
      const double notches =
          -static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
      shell->host->wheel(WheelEvent{.x = static_cast<double>(at.x),
                                  .y = static_cast<double>(at.y),
                                  .delta_y = notches,
                                  .modifiers = modifiers_now()});
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;
    }

    // A typed character. Nothing delivered these until there was something to
    // type into, which is why a field could be focused, show a caret, and take
    // nothing but the arrow keys.
    case WM_CHAR: {
      const auto unit = static_cast<char16_t>(wparam);

      // UTF-16 arrives a code unit at a time, so anything outside the basic
      // plane comes as a surrogate pair across two messages. Holding the high
      // half is what turns those back into one code point.
      static char16_t pending_high = 0;
      char32_t codepoint = unit;
      if (unit >= 0xD800 && unit <= 0xDBFF) {
        pending_high = unit;
        return 0;
      }
      if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (pending_high == 0) return 0;  // a low half on its own is nothing
        codepoint = 0x10000 + ((static_cast<char32_t>(pending_high - 0xD800) << 10) |
                               static_cast<char32_t>(unit - 0xDC00));
        pending_high = 0;
      } else {
        pending_high = 0;
      }

      if (shell->host->text(codepoint)) mark_dirty(*app);
      return 0;
    }

    case WM_KEYDOWN: {
      // Everything a field could mean belongs to the field. A digit switches
      // theme and a space plays the timeline — but not while someone is typing,
      // where they are a digit and a space.
      const Widget* typing = shell->host->focused();
      const bool editing = typing != nullptr && typing->wants_text();

      if (!editing && wparam >= '1' && wparam <= '9') {
        set_theme(*app, static_cast<std::size_t>(wparam - '1'));
        return 0;
      }

      // Application shortcuts, taken before the widget tree sees them: undo is
      // not something any one control should be able to swallow.
      const Modifiers held = modifiers_now();

      // The tools, on bare letters. Before the tree for the same reason the
      // theme digits are: no control wants a V, and a tool that only works when
      // nothing happens to be focused is a tool nobody trusts.
      if (!editing && held.none()) {
        const Key pressed = key_from_win32(wparam);
        const auto tool = std::ranges::find(kTools, pressed, &ToolEntry::key);
        if (tool != kTools.end()) {
          choose_tool(*app, tool->tool);
          return 0;
        }
      }
      if (held.control && wparam == 'O') {
        open_project(*app);
        return 0;
      }
      if (held.control && wparam == 'S') {
        save_project(*app, held.shift);
        return 0;
      }
      if (held.control && wparam == 'N') {
        new_project(*app);
        return 0;
      }
      if (held.control && wparam == 'I') {
        import_media(*app);
        return 0;
      }
      if (held.control && held.shift && wparam == 'F') {
        float_panel_under_cursor(*app, *shell);
        return 0;
      }
      // Space, before the tree sees it, because a focused button would
      // otherwise take it as a press. Every editor in the world plays with it.
      if (!editing && wparam == VK_SPACE && !held.control && !held.alt) {
        toggle_playback(*app);
        return 0;
      }
      const KeyEvent event{.key = key_from_win32(wparam),
                           .modifiers = held,
                           .repeat = (lparam & (1 << 30)) != 0};

      // While typing, the tree goes first. Ctrl+A means every clip normally and
      // every character inside a field, and a shortcut that reaches past the
      // keyboard focus to select the timeline instead is simply wrong there.
      // Undo still works: a field does not handle Ctrl+Z, so it falls through.
      if (editing && shell->host->key_down(event)) {
        mark_dirty(*app);
        return 0;
      }

      // Before the tree: nothing should be able to swallow undo.
      if (run_binding(*app, kApplicationKeys, event.key, held)) return 0;

      if (event.key == Key::Tab) {
        shell->host->focus_next(event.modifiers.shift);
        mark_dirty(*app);
        return 0;
      }

      // After the tree, and only if it did not want the key. A focused slider
      // owns the arrows before the playhead does.
      if (!shell->host->key_down(event)) {
        run_binding(*app, kTransportKeys, event.key, held);
      }
      mark_dirty(*app);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;  // every pixel is painted, so erasing only causes a flash

    case WM_CLOSE:
#if CUTLINE_HAVE_PREVIEW
      // A dialog holds nothing but itself, so it simply goes.
      if (shell->is_dialog) {
        close_export_dialog(*app);
        return 0;
      }
#endif
      // Closing a torn-out window sends its panels home rather than destroying
      // it here. The window goes when the layout stops having a dock for it,
      // which is what keeps the two from ever disagreeing about what exists.
      if (!shell->is_main()) {
        return_panels_home(*app, shell->floating_id);
        return 0;
      }
      break;

    case WM_DESTROY:
      // Only the editor's own window ends the session. A floating one closing
      // is a rearrangement.
      if (shell->is_main()) PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

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

/// Times the frame, so "it feels sluggish" becomes a number.
///
/// Interface drawing here is Skia into a CPU raster surface, blitted with GDI.
/// That is a deliberate choice rather than a placeholder, but it is only the
/// right choice while a frame is cheap, and the way to know is to measure it at
/// the sizes people actually run at rather than to assume.
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

      WidgetHost host(build_interface(nullptr));
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

    WidgetHost host(build_interface(nullptr));
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
[[nodiscard]] int self_check(std::string_view shots = {}) {
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
    int counted = 0;

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
      // A spacer is the exception: it exists to absorb whatever room is left
      // over, and when a column has overflowed its panel there is none. Zero is
      // the right answer there, not a layout that went wrong.
      if (widget.bounds().empty() && dynamic_cast<const Spacer*>(&widget) == nullptr) ++empty;
      if (widget.bounds().x < -0.5 || widget.bounds().right() > kWidth + 0.5) ++escaped;

      if (clipper != nullptr && !widget.bounds().empty() &&
          dynamic_cast<const ScrollView*>(clipper) == nullptr &&
          (widget.bounds().x < clipper->bounds().x - 0.5 ||
           widget.bounds().right() > clipper->bounds().right() + 0.5)) {
        ++clipped;
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
      app.main.host = std::make_unique<WidgetHost>(build_interface(&app));

      // Shown, not merely present. Only the active panel in a group is built,
      // and in the default arrangement the inspector is a tab behind another —
      // so without this the panel under test is never constructed at all.
      if (DockView* dock = find_dock(app.main.host->root()); dock != nullptr) {
        DockLayout layout = layout_of(app);
        cutline::ui::activate_panel(layout, "effects");
        dock->set_node(layout.root);
      }
      app.main.host->resize(client, context);

      // The first video clip with another abutting it, so the transition
      // controls are among the ones laid out. The topmost track's clip is on
      // its own, and a clip with no join has no transition to configure.
      std::string clip_id;
      for (const auto& track : app.session.project().tracks) {
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
        app.session.apply(cutline::editor::set_effect_parameter_animated(
            app.session.project(), clip_id, 0, "amount", true, 0.0));
      }

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
      for (const auto& track : app.session.project().tracks) {
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

    std::println("{:<10} {:>3} widgets, {} empty, {} outside the window, {} clipped away",
                 theme.id, counted, empty, escaped, clipped);
    if (empty > 0 || escaped > 0 || clipped > 0) ++failures;
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

}  // namespace

int main(int argc, char** argv) {
  // `--check [dir]`: the second argument, when there is one, is where each
  // theme's frame is written so it can be looked at.
  if (argc > 1 && std::string_view(argv[1]) == "--check") {
    return self_check(argc > 2 ? argv[2] : "");
  }
  // Both of these make windows now -- the benchmark a hidden one for its
  // swapchain -- so the class has to exist first.
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(WNDCLASSEXW);
  window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  RegisterClassExW(&window_class);

  if (argc > 1 && std::string_view(argv[1]) == "--benchmark") return benchmark();

  App app;
  app.main.app = &app;

  // Read before the tree is built, so the window opens in the arrangement it
  // was last left in rather than flashing the default and then moving.
  if (auto read = cutline::editor::read_workspaces(app.workspace_file); read.has_value()) {
    app.workspaces = std::move(*read);
  }
  // Reconciled against the panels this build has, whatever the file said.
  cutline::editor::settle(app.workspaces, known_panels());

  app.main.host = std::make_unique<WidgetHost>(build_interface(&app));

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const HWND window =
      CreateWindowExW(0, kWindowClass, L"Cutline", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                      CW_USEDEFAULT, 1280, 800, nullptr, nullptr, instance, nullptr);
  if (window == nullptr) {
    std::println("could not create a window");
    return 1;
  }

  app.main.window = window;
  // The shell rather than the app: a floating window's proc needs to know
  // which window it is, and every one of them finds its app through this.
  SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app.main));
  refresh_title(app);

  // One pixel of frame handed back to the compositor, which is enough to keep
  // the drop shadow and the rounded corners the system draws around a window.
  // Without it a borderless window looks like a rectangle pasted on the screen.
  const MARGINS shadow{0, 0, 1, 0};
  DwmExtendFrameIntoClientArea(window, &shadow);

  // Forces the frame to be recalculated now that WM_NCCALCSIZE will answer
  // differently, rather than at whatever moment the window is first moved.
  SetWindowPos(window, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

  ShowWindow(window, SW_SHOW);
  SetForegroundWindow(window);

  // A saved arrangement can have had panels torn out of it. Those windows are
  // made here rather than at the first rearrangement, or they would not come
  // back until something else moved.
  reconcile_windows(app);
  refresh_float_titles(app);

  // Two loops in one, and which is running is the difference between an
  // interface and a video player.
  //
  // Sitting still, this blocks in `GetMessage` and does nothing at all until
  // something happens: an interface that repaints at sixty hertz while nothing
  // moves is heat for nothing. Playing, it cannot block — the playhead moves
  // whether or not the mouse does — so it drains what has arrived and then asks
  // playback whether a new frame is due.
  MSG message{};
  bool running = true;
  while (running) {
    // Playing or exporting, the loop cannot block: the playhead moves and the
    // progress bar fills whether or not anything is being clicked.
    //
    // `finished` is in here as well as `exporting`, and it matters. The worker
    // clears `running` and sets `finished` as its last two acts, so a loop
    // watching only the first would go back to blocking with a thread still to
    // join and a bar stopped short of the end — which worked, whenever the
    // timing happened to fall the right way.
    if (app.playing() || app.exporting() || app.export_job.finished.load()) {
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
          running = false;
          break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      if (running) {
        advance_playback(app);
        poll_export(app);
        // Nothing is due this instant when only the export is running, and the
        // encoder wants the core far more than this loop does.
        if (!app.playing()) Sleep(8);
      }
    } else if (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    } else {
      running = false;
    }

    // Asked of every window, because most changes are to the document and show
    // in whichever ones happen to be open.
    for (Shell* shell : app.shells()) {
      if (shell->dirty && shell->window != nullptr) {
        InvalidateRect(shell->window, nullptr, FALSE);
      }
    }
  }

  // Before the windows go, so neither the audio thread nor the export thread is
  // still reading things that are being torn down.
  invalidate_playback(app);
  settle_export(app);

  // Where the panels were left, so the next session opens the same way. On the
  // way out rather than on every drag: a rearrangement is a dozen small moves
  // and writing a file for each of them would be silly.
  save_workspaces(app, false);

  // The floating windows are owned by the main one and are already gone, but
  // their shells still hold host pointers that a stray message would follow.
  app.floats.clear();
  return 0;
}
