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
#include "cutline/core/version.hpp"
#include "cutline/core/effects.hpp"
#include "cutline/core/model.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/time.hpp"
#include "cutline/editor/autosave.hpp"
#include "cutline/editor/browser_binding.hpp"
#include "cutline/editor/commands.hpp"
#include "cutline/editor/document.hpp"
#include "cutline/editor/effects_binding.hpp"
#include "cutline/editor/generators.hpp"
#include "cutline/editor/import.hpp"
#include "cutline/editor/inspector.hpp"
#include "cutline/editor/keyframes.hpp"
#include "cutline/editor/bins.hpp"
#include "cutline/editor/presets.hpp"
#include "cutline/editor/monitor_binding.hpp"
#include "cutline/editor/session.hpp"
#include "cutline/editor/timeline_binding.hpp"
#include "cutline/editor/titles.hpp"
#include "cutline/editor/transitions.hpp"
#include "cutline/editor/workspace.hpp"
#include "cutline/render/preview.hpp"
#include "cutline/ui/browser.hpp"
#include "cutline/ui/color_picker.hpp"
#include "cutline/ui/controls.hpp"
#include "cutline/ui/dock.hpp"
#include "cutline/ui/dock_view.hpp"
#include "cutline/ui/effects_browser.hpp"
#include "cutline/ui/keyframe_view.hpp"
#include "cutline/ui/meter_view.hpp"
#include "cutline/ui/monitor.hpp"
#include "cutline/ui/scopes_view.hpp"
#include "cutline/ui/scrub_bar.hpp"
#include "cutline/ui/waveform_view.hpp"
#include "cutline/ui/skia_painter.hpp"
#include "cutline/ui/skia_window.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/timeline.hpp"
#include "cutline/ui/widget.hpp"
#include "cutline/ui/widgets.hpp"

#if CUTLINE_HAVE_PREVIEW
#include "cutline/app/preview.hpp"
#include "cutline/app/updater.hpp"
#include "cutline/app/proxies.hpp"
#include "cutline/app/thumbnails.hpp"
#include "cutline/app/waveforms.hpp"
#include "cutline/engine/exporter.hpp"
#include "cutline/engine/player.hpp"
#endif

#include <windows.h>
// Both of these need windows.h first: one for the message parameters it
// defines, the other for the types in its own signatures.
#include <commdlg.h>
#include <dwmapi.h>
// For `ShellExecuteW`, which is what starts the downloaded installer.
#include <shellapi.h>
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
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <thread>
#include <optional>
#include <print>
#include <set>
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
using cutline::ui::ScrubBar;
using cutline::ui::SkiaPainter;
using cutline::ui::Slider;
using cutline::ui::Spacer;
using cutline::ui::Splitter;
using cutline::ui::TextAlign;
using cutline::ui::TextField;
using cutline::ui::WaveformView;
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

/// What a new document is: an empty sequence with somewhere to put things.
///
/// One video track and one audio track rather than none. A timeline with no
/// tracks has nowhere to drop the first import, and "add a track before you can
/// do anything" is a step no editor asks for. More are a menu away.
[[nodiscard]] cutline::core::Project new_project_model() {
  cutline::core::Project project;
  project.fps = 30.0;
  project.tracks = {
      cutline::core::Track{.id = "v1", .kind = cutline::core::TrackKind::Video},
      cutline::core::Track{.id = "a1", .kind = cutline::core::TrackKind::Audio},
  };
  return project;
}

/// A project with something in it, for the headless check only.
///
/// `--check` lays out and paints every panel in every theme, and a panel with
/// nothing in it proves nothing: an empty pool has no rows to be too tall, and
/// an empty timeline has no clip to be clipped by the edge of its track. This
/// is the only caller — the application itself starts empty, the way every
/// other editor does.
[[nodiscard]] cutline::core::Project check_project() {
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
constexpr std::array<std::pair<std::string_view, std::string_view>, 8> kPanels{{
    {"project", "Project"},
    {"effects", "Effect Controls"},
    // Premiere's own two names, which are confusingly similar on purpose: this
    // one holds the things you can apply and "Effect Controls" holds what the
    // one you applied is doing. "Effects Library" was clearer and half a tab
    // too wide — three titles and three close buttons did not fit the panel,
    // and the buttons ended up drawn over the last letter of each label.
    {"library", "Effects"},
    {"monitor", "Program Monitor"},
    // The other half of Premiere's two-monitor workflow: what is about to be
    // used, on its own time, beside what the sequence currently is. An editor
    // with only the program monitor makes you do on the timeline what should
    // have been decided before anything was placed.
    {"source", "Source Monitor"},
    {"timeline", "Timeline"},
    {"audio", "Audio Master"},
    // A panel rather than the overlay on the monitor the spec describes. The
    // overlay was the right answer for an application with one fixed layout;
    // this one has dockable panels that tear out into windows of their own, so
    // a scope can sit beside the picture, or on another screen, or be closed —
    // none of which an overlay offers. It costs the picture no room either way.
    {"scopes", "Scopes"},
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

/// The icon resource, matching `cutline.rc`. Numbered rather than named
/// because Windows picks an executable's representative icon by the lowest
/// resource *number*, and a name would leave that to whatever the linker did.
constexpr WORD kIconResource = 1;

/// Posted by a media worker when something the timeline draws is ready.
///
/// A message rather than a flag the loop polls, because the loop blocks on its
/// queue whenever nothing is playing — a waveform would otherwise appear at the
/// next mouse move rather than when it was decoded. One message for both caches
/// rather than one each: what it triggers is a rebuild, and a rebuild takes
/// whatever has arrived.
constexpr UINT kMediaReady = WM_APP + 1;

/// The timer that offers to write a recovery copy.
///
/// A Win32 timer rather than a check in the frame loop, because the loop blocks
/// on its message queue whenever nothing is playing — and an editor left alone
/// with unsaved work is exactly the case a recovery copy is for. It fires
/// oftener than a copy is due; `autosave_due` decides.
constexpr UINT_PTR kAutosaveTimer = 1;

/// The tooltip's delay, and its timer.
///
/// Half a second, which is Windows' own: long enough that a pointer crossing a
/// toolbar on its way somewhere leaves no trail of boxes behind it, short
/// enough that resting on a control to ask what it is feels answered rather
/// than waited for.
constexpr UINT_PTR kTooltipTimer = 2;
constexpr UINT kTooltipDelayMs = 500;
constexpr UINT kAutosaveTickMs = 5000;

/// Posted by the updater's worker when its state changes. The same reason
/// `kMediaReady` exists: the loop blocks on its queue, and an answer that
/// arrived while nothing was moving would be shown at the next mouse move.
constexpr UINT kUpdateChanged = WM_APP + 2;

/// Posted by the proxy builder when it starts one, finishes one, or fails.
///
/// Its own message rather than sharing `kMediaReady`, because what it triggers
/// is not a rebuild: a finished proxy changes the *project*, and folding that
/// into the media rebuild would mean a filmstrip arriving could apply one.
constexpr UINT kProxyChanged = WM_APP + 3;

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

  /// The document being edited. Everything the timeline shows is derived from
  /// it, and everything a drag does goes back through it.
  cutline::editor::Session session{new_project_model()};
  TimelineView* timeline = nullptr;

  /// Where the timeline's envelopes and filmstrips come from.
  ///
  /// Functions rather than the caches themselves, and that is what keeps
  /// `refresh_timeline` free of `#if`s: under a build with no media layer they
  /// are simply unset and clips are drawn plain, and the self-check points them
  /// at fabricated ones so every theme paints both.
  cutline::editor::TimelineMedia timeline_media;
  /// The playhead's timecode, and a field rather than a label: Premiere's is
  /// typed into to go somewhere, which is the fastest way there is to reach an
  /// exact time and the only way to reach one that is off screen.
  TextField* readout = nullptr;
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

  /// The two whose usefulness depends on what is selected, so they can be
  /// greyed out. Held rather than rebuilt for the same reason as the palette,
  /// and asked of `can_run` rather than of the selection directly, so the
  /// button and the command cannot disagree about when an edit is possible.
  Button* link_button = nullptr;
  Button* unlink_button = nullptr;

  /// Whether an edge being dragged jumps to the things near it. A property of
  /// the view rather than the document — undoing a snap setting is not a thing
  /// anybody wants — but held here, because the view is rebuilt by a
  /// rearrangement and a setting that resets when a panel moves is one nobody
  /// can rely on.
  bool snapping = true;
  Button* snap_button = nullptr;

  /// Whether playback returns to the start of the marked range instead of
  /// stopping at the end of it. A view setting like snapping: it says how to
  /// watch the sequence, not what the sequence is.
  bool looping = false;
  Button* loop_button = nullptr;

  /// Whether scaling keeps a layer's shape. Premiere's "Uniform Scale".
  ///
  /// A setting rather than a field on the clip, because it describes how the
  /// controls behave and not what the picture is: a clip scaled to 80 by 80 is
  /// the same clip whether or not the lock was on while it was set. Putting it
  /// in the model would mean serialising it, undoing it, and answering what a
  /// locked clip with unequal scales means.
  bool aspect_locked = false;

  /// The lane view, while the inspector is showing one.
  ///
  /// Held because the context menu has to read the selection at the moment it
  /// is chosen from, not at the moment it was opened — and because the panel is
  /// rebuilt from nothing on every edit, so this is null far more often than it
  /// is not. Cleared by `refresh_inspector` before anything is built.
  cutline::ui::KeyframeView* keyframes = nullptr;

  /// The effects library, while a window is showing one.
  cutline::ui::EffectsBrowser* library = nullptr;
  /// The library's bin-name field, held so making a bin can empty it.
  cutline::ui::TextField* library_name = nullptr;
  /// The inspector's "which clip is this" line, held so an edit that does not
  /// rebuild the panel can still keep it honest.
  Label* clip_heading = nullptr;

  /// Keyframes copied out of a lane, waiting to go back somewhere.
  ///
  /// Kept apart from the effect clipboard rather than folded into it: they hold
  /// different things and are filled by different gestures, and one clipboard
  /// serving both would mean copying an effect stack silently threw away a
  /// curve somebody was about to paste.
  cutline::editor::KeyframeClipboard keyframe_clipboard;

  /// The effect card a reorder drag would land on, while one is in flight, and
  /// the header rows themselves.
  ///
  /// The rows are held because the highlight has to change *during* a drag, and
  /// the panel is not rebuilt then — rebuilding it would destroy the row the
  /// pointer is holding.
  std::optional<std::size_t> effect_drop_target;
  std::vector<cutline::ui::GrabRow*> effect_rows;

  /// Which lanes are showing their graph, by property name. Here for the same
  /// reason `expanded_params` is: the panel is rebuilt on every edit.
  std::set<std::string> expanded_lanes;

  /// Which keyframes are picked out, by the lane's *name* and the keyframe's
  /// *time* rather than by index.
  ///
  /// On `App` for the same reason the open graphs are: the panel is rebuilt
  /// from nothing on every edit, and a selection left in the widget is a
  /// selection lost the moment anything is changed. That was survivable while
  /// the only thing a selection did was wait for a menu; it stopped being
  /// survivable when handles arrived, because dragging one is an edit and you
  /// cannot drag a handle you can no longer see.
  ///
  /// By name and time because both move: adding an effect renumbers every lane
  /// below it, and removing a keyframe renumbers the ones after it.
  std::set<std::pair<std::string, double>> keyframe_selection;

  /// Which effect each shape on the monitor belongs to, in the order the
  /// monitor was handed them. The widget draws shapes and has never heard of an
  /// effect stack, so the mapping back lives here.
  std::vector<std::size_t> mask_effects;

  /// Named effect stacks, read once at startup and written whenever one is
  /// saved or removed. Application data rather than project data: a look you
  /// have built up belongs to you, not to whichever project happened to be open
  /// when you made it.
  cutline::editor::Presets presets;

  /// What is typed in the preset name field, kept here because the panel it
  /// lives in is rebuilt from nothing on every edit.
  std::string preset_name;

  /// User bins: folders gathered by hand, holding ids rather than copies. Saved
  /// beside the presets and for the same reason — which effects you keep to hand
  /// is a fact about you rather than about a project.
  cutline::editor::Bins bins;

  /// What is typed in the library's name field. It names a new bin, and it is
  /// what "Rename Bin" renames one to — there is no modal text prompt in this
  /// application, and a bin name is not the right place to introduce one.
  std::string bin_name;

  /// Which parameters are showing their slider, by the key `ParamRow` carries.
  ///
  /// Premiere's arrangement: the number is always there and the slider is
  /// behind a disclosure triangle, because the exact value is wanted far more
  /// often than the coarse gesture. Kept here rather than on the row because
  /// the inspector is rebuilt from scratch on every edit — state left in a
  /// widget would close every triangle each time a value changed.
  std::set<std::string> expanded_params;

  /// Shuttle speed, as a multiple of real time. Zero is not shuttling.
  ///
  /// A rate of exactly 1 is ordinary playback, with sound, driven by the audio
  /// clock. Anything else is a silent shuttle: the sound card can only play at
  /// its own rate, and pitching a mix up to run at 4x — or backwards — is not
  /// what J and L are for. What they are for is finding a moment by eye, and
  /// the picture is what does that.
  double shuttle = 0.0;
  /// When the shuttle last moved the playhead, so it advances by elapsed time
  /// rather than by however often the loop happened to turn.
  std::chrono::steady_clock::time_point shuttled_at{};
  /// Where the shuttle has got to, in seconds, before the playhead quantises it.
  ///
  /// Held separately because `Session::set_playhead` snaps to the frame grid:
  /// reading it back and adding a couple of milliseconds rounds to the frame it
  /// was already on, so a shuttle that used the playhead as its own position
  /// could never leave the frame it started on. It moved at 4x and not at all
  /// at 1x, which looked like the rate being ignored rather than like rounding.
  double shuttle_at = 0.0;

  [[nodiscard]] bool shuttling() const noexcept { return shuttle != 0.0; }

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

  /// The scopes, what they are showing, and the tabs that choose it.
  ///
  /// The readings are held here rather than only in the widget because the
  /// panel is rebuilt by a rearrangement or a theme change, and measuring again
  /// would mean rendering a frame again — which is the expensive half.
  /// The last stack copied, held across selections and documents. Not part of
  /// the project: a clipboard is a property of the session in front of you, and
  /// saving one would put a copy of somebody's effects in every file that had
  /// ever had Copy pressed in it.
  cutline::editor::EffectClipboard effect_clipboard;

  /// What has been written to the recovery copy so far.
  cutline::editor::AutosaveState autosave;

  cutline::ui::ScopesView* scopes = nullptr;
  cutline::ui::ScopeKind scope_kind = cutline::ui::ScopeKind::Histogram;
  std::shared_ptr<const cutline::ui::ScopeReadings> scope_readings;
  Button* scope_button = nullptr;

  /// The master fader and the meter beside it.
  ///
  /// The meter is polled from the frame loop rather than pushed to, because the
  /// levels are produced on the player's render thread every few milliseconds
  /// and the panel is repainted at frame rate. Waking the interface per audio
  /// block to draw a bar that is about to be drawn again would be a lot of
  /// messages for nothing.
  cutline::ui::MeterView* meter = nullptr;
  Slider* master_fader = nullptr;
  Label* master_reading = nullptr;

  /// True between the press and the release on a transform handle, which is
  /// what stops the refresh below writing the model's box over the one the
  /// pointer is describing.
  bool dragging_handle = false;

  /// What the preview renders instead of the document, while a gesture is in
  /// flight.
  ///
  /// One gesture is one undo entry, so a drag must not write to the session
  /// until it finishes — but the whole point of dragging on the *picture* is
  /// watching the picture. Every other dragged control resolves this by
  /// previewing inside the view: the timeline moves its own blocks, a slider
  /// moves its own thumb. A monitor cannot, because what it would have to
  /// preview is a rendered frame. So the edit is made against a copy, shown,
  /// and thrown away; the release makes it again against the session, once.
  std::optional<cutline::core::Project> live_project;

  /// Whether a control is mid-gesture and `live_project` is what it is saying.
  ///
  /// The same argument as `dragging_handle` one field up, for the controls
  /// rather than the monitor: scrubbing a number should move the *picture*,
  /// which is the only way to judge what a value is doing, but it must leave
  /// one undo entry rather than one per pixel. So each change renders against a
  /// copy and only the release is applied.
  ///
  /// Cleared in `settle` when nothing holds the pointer, rather than by the
  /// control that set it. A scrub that ends back where it began reports no
  /// change at all — deliberately, so an undo stack does not collect entries
  /// for gestures that did nothing — and a flag cleared only by a commit would
  /// then stay set for ever, freezing the preview on a project the document no
  /// longer has.
  bool live_gesture = false;

  /// The project the preview should render: the gesture's if there is one.
  [[nodiscard]] const cutline::core::Project& showing() const noexcept {
    return live_project.has_value() ? *live_project : session.project();
  }

  /// How much of the sequence's own resolution the preview renders at.
  ///
  /// Compositing costs per pixel, so a half is a quarter of the work and a
  /// quarter a sixteenth. It changes nothing about the document and nothing
  /// about an export — a snapshot renders at full size too, since it is a frame
  /// of the sequence rather than a picture of the monitor.
  double preview_scale = 1.0;
  Dropdown* preview_scale_choice = nullptr;

  /// The update check. Shows the running version and asks about newer ones.
  cutline::app::Updater updater;
  Button* version_button = nullptr;

  /// Says when the workers are still reading a source.
  ///
  /// The interface does not stop while they do — measured with
  /// `SendMessageTimeout`, and the message loop does not miss a beat — but a
  /// ten-minute 4K capture with four audio streams costs around a hundred
  /// seconds of processor time across every core it can reach, and takes some
  /// seconds of wall clock before the waveforms and the filmstrip appear. With
  /// nothing saying so, a busy machine and an empty timeline lane are
  /// indistinguishable from an editor that has hung.
  Label* busy_label = nullptr;

  // ----------------------------------------------------- the source monitor --
  //
  // One source, shown on its own time, so which part of it to use can be
  // decided before anything is placed. What it shows is `Session::source_media`
  // — whatever the pool has selected — and the marks it sets live on the media,
  // where insert, overwrite and a drag from the pool already read them.

  MonitorView* source_monitor = nullptr;
  /// Shown instead of the picture when the source has none. A sound file in a
  /// monitor that renders video is a black rectangle, which looks exactly like
  /// a file that failed to decode.
  WaveformView* source_waveform = nullptr;
  ScrubBar* source_scrub = nullptr;
  /// The sources looked at lately, most recent first, and the control that
  /// chooses between them.
  ///
  /// Premiere's source monitor names what it is showing and drops down the
  /// others, which is the difference between a monitor and a monitor you can
  /// work in: comparing two takes means going back and forth, and going through
  /// the pool each time makes that a chore rather than a glance.
  Dropdown* source_choice = nullptr;
  std::vector<std::string> source_recent;
  /// The panel's root, so "is the source monitor the one being worked in" can
  /// be answered by asking where the keyboard is rather than by keeping a flag
  /// that something forgets to clear.
  Widget* source_panel = nullptr;

  /// Its own renderer, and so its own decoders.
  ///
  /// Sharing the sequence's would put both monitors on one decoder per file,
  /// and they are almost never looking at the same moment of it — every glance
  /// at one would seek the other's decoder away and cost a group of pictures to
  /// get back. Two decoders on one file is the cheaper of the two by a wide
  /// margin, and this is the case they exist for.
  std::unique_ptr<cutline::app::ProjectPreview> source_preview;
  /// Which media the renderer was built for, so it is rebuilt when that changes
  /// rather than showing the last one at the new one's size.
  std::string source_built_for;
  double source_playhead = 0.0;
  bool source_failed = false;

  /// Playing the source, with its sound.
  ///
  /// The same `engine::Player` the sequence uses, handed the one-clip project
  /// the source monitor is already drawn from — a source monitor being a
  /// sequence of one clip is what makes that work, and it is why playing one
  /// gets audio without a second audio path being written.
  ///
  /// One at a time, because there is one sound card: starting this stops the
  /// sequence, and starting the sequence stops this. Premiere does the same.
  std::unique_ptr<cutline::engine::Player> source_player;
  std::string source_player_for;
  Button* source_play_button = nullptr;

  [[nodiscard]] bool source_playing() const noexcept {
    return source_player != nullptr && source_player->playing();
  }

  /// Set when the picture has changed and the measurements no longer describe
  /// it. Deferred like the preview is, so scrubbing across ten frames measures
  /// the one that is finally shown rather than all ten.
  bool scopes_stale = true;
  /// Shown by builds that cannot decode anything at all — the skia preset,
  /// where there is no media layer and the point of the monitor is the chrome
  /// around it.
  ///
  /// Deliberately *not* shown by a build that renders. An empty sequence there
  /// shows an empty frame: colour bars would read as something somebody put on
  /// the timeline rather than as "there is nothing here yet".
  TestPattern pattern;

#if CUTLINE_HAVE_PREVIEW
  /// The audio envelopes and the filmstrips the timeline draws, both filled in
  /// on workers of their own.
  ///
  /// Above the device deliberately: they need no GPU and no preview, only a
  /// decoder, and they are the pieces of media work that happen whether or not
  /// anything is being rendered.
  cutline::app::WaveformCache waveforms;
  cutline::app::ThumbnailCache filmstrips;

  /// The worker that makes proxies. Beside the other two and unlike them in one
  /// respect: what it produces outlives the session, so its results go into the
  /// project rather than into a cache.
  cutline::app::ProxyBuilder proxies;

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
void refresh_busy(App& app);
void refresh_drop_ghost(App& app);
void relink_pool_entry(App& app);
void make_proxies(App& app);
void collect_proxies(App& app);
void toggle_use_proxies(App& app);
void stop_making_proxies(App& app);
void open_from_command_line(App& app, const std::filesystem::path& path);
void refresh_source(App& app);
void refresh_source_list(App& app);
void show_source(App& app, const std::string& media_id);
[[nodiscard]] const cutline::core::Media* source_media_of(const App& app);
void step_source(App& app, double frames);
void mark_source(App& app, bool out_point);
[[nodiscard]] bool source_has_focus(const App& app);
void toggle_source_playback(App& app);
void stop_source_playback(App& app);
void stop_playback(App& app);
void advance_source_playback(App& app);
[[nodiscard]] cutline::core::Project source_project(const cutline::core::Media& media);
void refresh_browser(App& app);
void refresh_dock(App& app);
void reconcile_windows(App& app);
void refresh_float_titles(App& app);
void refresh_all(App& app);
void invalidate_preview(App& app);
void invalidate_scopes(App& app);
void refresh_scopes(App& app);
void choose_scope(App& app, cutline::ui::ScopeKind kind);
void show_master_gain(App& app, double gain);
void refresh_meter(App& app);
void refresh_handles(App& app);
void refresh_title(App& app);
void show_snapping(App& app);
void toggle_snapping(App& app);
void choose_preview_scale(App& app, double scale);
void invalidate_playback(App& app);
void open_export_dialog(App& app);
void poll_export(App& app);
void settle_export(App& app);
void toggle_playback(App& app);
void toggle_looping(App& app);
void set_shuttle(App& app, double rate);
void advance_shuttle(App& app);
void import_media(App& app);
void add_title(App& app);
void add_matte(App& app);
void add_adjustment(App& app);
void complain(HWND owner, const std::string& message);
[[nodiscard]] std::optional<std::filesystem::path> choose_image_file(HWND owner);
void take_snapshot(App& app);
void check_for_updates(App& app);
void settle_update(App& app);
[[nodiscard]] bool confirm_discard(App& app);
bool save_project(App& app, bool ask_where);

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
      cutline::core::seconds_to_timecode(app.session.playhead(), app.session.project().fps));
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

/// The menu a right-click on an effect card offers.
void open_effect_row_menu(App& app, const std::function<void()>& copy, double x, double y) {
  if (app.main.host == nullptr) return;

  auto list = std::make_unique<MenuList>(std::vector<std::string>{"Copy Effect"});
  list->set_on_choose([&app, copy](std::size_t index) {
    if (app.main.host != nullptr) app.main.host->close_popup();
    if (index == 0) copy();
  });
  app.main.host->open_popup(std::move(list), Rect{x, y, 0.0, 0.0});
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
  if (on_copy) {
    line.set_on_context_menu([&app, copy = std::move(on_copy)](double x, double y) {
      open_effect_row_menu(app, copy, x, y);
    });
  }

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
    // Both stacks, because the button is next to neither of them and "clear
    // the effects" on a clip means all of them. Only one is ever non-empty in
    // practice — a clip is video or audio.
    cutline::core::Project next =
        cutline::core::clear_clip_effects(app.session.project(), clip_id);
    next = cutline::core::clear_audio_effects(std::move(next), clip_id);
    app.session.apply(std::move(next));
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

/// Rebuilds what the timeline draws from the session.
///
/// Everything goes through here rather than being patched in place, which is
/// what keeps the view from drifting out of step with the document — an undo
/// changes the project in ways no incremental update could follow.
/// Asks for any envelope or filmstrip the project needs and does not have.
///
/// Here rather than in the importer because that is not the only way a source
/// arrives: an undo, a paste, and a project opened from disk all bring media
/// that never went past it. Requests already known or already queued cost
/// nothing, which is what makes calling this on every rebuild reasonable.
void request_media([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  const cutline::core::Project& project = app.session.project();
  for (const cutline::core::Track& track : project.tracks) {
    const bool audio = track.kind == cutline::core::TrackKind::Audio;
    for (const cutline::core::Clip& clip : track.clips) {
      const auto media =
          std::ranges::find(project.media, clip.media_id, &cutline::core::Media::id);
      if (media == project.media.end()) continue;

      if (audio) {
        app.waveforms.request(media->id, media->path, clip.audio_stream);
        continue;
      }
      // A still has one frame that never changes and a generated source has no
      // file, so neither is a filmstrip — asking would be an error reported
      // once a frame for as long as the clip is on screen.
      if (!media->has_video || cutline::core::is_generated_media(*media) || media->is_image) {
        continue;
      }

      // Only the stretch of the source that is actually on screen, translated
      // from timeline time through the clip's own trim and speed. A ten-minute
      // capture at an ordinary zoom shows about thirteen seconds of itself, and
      // asking for the whole file to draw those was 48 seeks across a 4K
      // capture — the hundred seconds of processor time that made dropping a
      // clip feel like a crash.
      if (app.timeline == nullptr) continue;
      const cutline::ui::TimeScale& view = app.timeline->scale();
      const double left = view.start;
      const double right = left + view.visible_duration(app.timeline->time_area().width);

      const double ends = cutline::core::clip_end(clip);
      if (ends <= left || clip.start >= right) continue;  // not on screen at all

      // Clamped to the clip, then carried into the source. `source_in` is where
      // the clip starts in the file, and a clip playing at speed covers more of
      // the source than it occupies on the timeline.
      const double speed = cutline::core::clip_speed(clip);
      const double from_edge = std::max(0.0, left - clip.start);
      const double to_edge = std::min(ends, right) - clip.start;

      // A margin either side, so scrolling gently does not ask for a new
      // stretch on every frame of the movement.
      const double margin = (to_edge - from_edge) * 0.5;
      const double from = clip.source_in + std::max(0.0, from_edge - margin) * speed;
      const double to = clip.source_in + (to_edge + margin) * speed;
      app.filmstrips.request(media->id, media->path, from, std::min(to, media->duration));
    }
  }
#endif
}

/// Greys out the commands that have nothing to act on.
///
/// Through `can_run` rather than by asking the selection here, so a button is
/// enabled exactly when pressing it would do something — the two cannot drift,
/// because there is only one answer.
void refresh_command_buttons(App& app) {
  if (app.link_button != nullptr) {
    app.link_button->set_enabled(cutline::editor::can_run(app.session,
                                                          cutline::editor::Command::LinkClips));
  }
  if (app.unlink_button != nullptr) {
    app.unlink_button->set_enabled(
        cutline::editor::can_run(app.session, cutline::editor::Command::UnlinkClips));
  }
}

/// Says what the workers are still reading, or nothing when they are idle.
///
/// Called from the timeline refresh, which is where the sources get asked for,
/// and again when one arrives — the two ends of the work. Polling it on a timer
/// instead would mean the label appeared a beat after the machine got busy,
/// which is the beat that matters.
void refresh_busy(App& app) {
  if (app.busy_label == nullptr) return;
#if CUTLINE_HAVE_PREVIEW
  const std::size_t left = app.waveforms.pending() + app.filmstrips.pending();
  // Counted rather than a bare "working": four audio streams and a filmstrip is
  // five things, and a number that comes down is the difference between waiting
  // and wondering.
  std::string says = left == 0 ? std::string{} : std::format("Reading media ({})", left);

  // Proxies take precedence over the reading, because they take minutes where
  // the reading takes seconds — and a line that flickered between the two would
  // say less than either. Named and given a percentage for the same reason the
  // rest is counted: something that will run for five minutes has to show it is
  // moving, or it is indistinguishable from something stuck.
  if (const auto making = app.proxies.progress()) {
    // The name is elided rather than let run. This label sits in the corner of
    // the project panel with the version badge at the other end, and a camera
    // file called A001_C003_0410XX_001.R3D would push it off the edge — which
    // `--check` cannot catch, because nothing is being transcoded when it runs.
    constexpr std::size_t kLongestName = 20;
    std::string name = making->name;
    if (name.size() > kLongestName) name = name.substr(0, kLongestName - 3) + "...";

    says = std::format("Proxy: {} {}%", name, static_cast<int>(making->done * 100.0));
    if (making->queued > 0) says += std::format(" (+{})", making->queued);
  }
#else
  std::string says;
#endif
  if (says == app.busy_label->text()) return;
  // Hidden rather than merely blank when there is nothing to say. `--check`
  // counts a widget that takes up room and draws nothing as a fault, and it is
  // right to: an idle editor should have an empty corner, not an empty label
  // holding a space open for one.
  app.busy_label->set_visible(!says.empty());
  app.busy_label->set_text(std::move(says));
  mark_dirty(app);
}

void refresh_timeline(App& app) {
  refresh_command_buttons(app);
  if (app.timeline == nullptr) return;

  request_media(app);
  refresh_busy(app);
  app.timeline->set_model(cutline::editor::timeline_model(
      app.session.project(), app.session.selection(), app.timeline_media));
  app.timeline->set_playhead(app.session.playhead());
  refresh_handles(app);
  mark_dirty(app);
}

/// Puts the handles on whatever is selected, or takes them away.
///
/// Driven from the same refresh as the timeline, because that is where a change
/// of selection or of playhead lands, and both move the box: a keyframed
/// transform is a different rectangle at a different moment.
///
/// The one exception is while a handle is being dragged. The view is showing
/// the box the pointer is describing and the model is a frame behind it, so
/// writing the model's answer back mid-gesture would fight the drag — which
/// looked, on screen, like the layer sticking every few pixels.
///
/// Reads `showing()` rather than the document, so that scrubbing Position in
/// the inspector moves the box in the monitor. The two are the same picture
/// described twice, and a box that stayed put while the number went by would be
/// the more convincing of the two — it is drawn over the frame itself.
void refresh_handles(App& app) {
  if (app.monitor == nullptr) return;
  if (app.dragging_handle) return;

  const cutline::core::Project& document = app.showing();
  const auto selection = app.session.selection();
  app.monitor->set_transform(
      selection.empty()
          ? std::nullopt
          : cutline::editor::monitor_box(document, selection.front(), app.session.playhead()));

  // The masks on whatever is selected, drawn where they actually fall on the
  // frame. Which effect each belongs to is remembered here, because the widget
  // knows only that it has some shapes to draw.
  app.mask_effects.clear();
  std::vector<cutline::ui::MaskOverlay> shapes;
  if (!selection.empty()) {
    for (const cutline::editor::MaskOverlayRef& mask : cutline::editor::mask_overlays(
             document, selection.front(), app.session.playhead())) {
      app.mask_effects.push_back(mask.effect);
      shapes.push_back(mask.overlay);
    }
  }
  app.monitor->set_masks(std::move(shapes));
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
/// Puts a named media on the timeline, wherever the drop landed.
///
/// Shared by the pool and the source monitor, which are two ways of saying the
/// same thing about the same media — and a second copy of this arithmetic would
/// be a second chance for the two to disagree about where a clip goes.
void place_media_from(App& app, const std::string& media_id,
                      std::optional<cutline::ui::DropPoint> where = std::nullopt) {
  if (media_id.empty()) return;

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

  // Dragged from the pool, so it lands as the source is marked — the same span
  // insert and overwrite would place, since it is the same decision.
  const auto range = cutline::core::source_range(app.session.project(), media_id);
  app.session.apply(
      cutline::core::place_media(app.session.project(), media_id, at, track_id, range));
  refresh_all(app);
}

/// The same, named by its row in the pool.
void place_from_pool(App& app, std::size_t index,
                     std::optional<cutline::ui::DropPoint> where = std::nullopt) {
  if (app.browser == nullptr || index >= app.browser->items().size()) return;
  place_media_from(app, app.browser->items()[index].id, where);
}

/// Shows where a row dragged out of the pool would land, while it is in the air.
///
/// The same arithmetic `place_from_pool` does on release — snapped to the frame
/// grid, aimed at the track under the pointer, the length the source's marks
/// give it — so what is drawn is a promise the drop keeps rather than an
/// approximation of it.
void refresh_drop_ghost(App& app) {
  if (app.timeline == nullptr) return;

  // Worked out first and applied once, so the repaint can be asked for exactly
  // when the answer changed. A ghost that redrew on every mouse move would cost
  // a full repaint per pixel of a gesture that mostly stands still.
  std::optional<cutline::ui::DropGhost> ghost;

  // Two ways a clip arrives from elsewhere: dragged out of the pool, or out of
  // the source monitor's picture. Both promise the same thing, so both are
  // answered here rather than by two nearly identical pieces of code.
  std::string dragged;
  double at_x = 0.0;
  double at_y = 0.0;
  if (app.browser != nullptr) {
    if (const std::optional<std::size_t> row = app.browser->dragging();
        row.has_value() && *row < app.browser->items().size()) {
      dragged = app.browser->items()[*row].id;
      at_x = app.browser->drag_x();
      at_y = app.browser->drag_y();
    }
  }
  if (dragged.empty() && app.source_monitor != nullptr && app.source_monitor->dragging_out()) {
    dragged = app.session.source_media();
    at_x = app.source_monitor->drag_x();
    at_y = app.source_monitor->drag_y();
  }

  if (!dragged.empty()) {
    // Nothing over the tracks means nothing to promise — over the headers, the
    // ruler, or off the panel entirely.
    if (const auto where = app.timeline->drop_at(at_x, at_y); where.has_value()) {
      const cutline::core::Project& project = app.session.project();
      const auto media = std::ranges::find(project.media, dragged, &cutline::core::Media::id);
      if (media != project.media.end()) {
        ghost = cutline::ui::DropGhost{
            .track = where->track,
            .start = cutline::core::snap_to_frame(where->time, project.fps),
            .duration = cutline::core::placed_length(
                *media, cutline::core::source_range(project, dragged)),
            .label = media->name};
      }
    }
  }

  if (app.timeline->drop_ghost() == ghost) return;
  app.timeline->set_drop_ghost(std::move(ghost));
  mark_dirty(app);
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

/// Points a pool entry at a file somewhere else.
///
/// What repairs a project whose footage has moved — a drive letter that
/// changed, a folder that was tidied. Clips name media by id, so repointing the
/// one entry repairs every clip that used it, however many there are.
void relink_pool_entry([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.browser == nullptr) return;
  const cutline::ui::MediaItem* chosen = app.browser->selected();
  // Said rather than ignored. A menu item that does nothing at all is
  // indistinguishable from one that is broken, and this is reached exactly when
  // something is already wrong and patience is short.
  if (chosen == nullptr) {
    complain(app.main.window,
             "Choose the media to relink in the project panel first.");
    return;
  }

  const cutline::core::Project& project = app.session.project();
  const auto media =
      std::ranges::find(project.media, chosen->id, &cutline::core::Media::id);
  if (media == project.media.end()) return;
  // Generated media have no file to point anywhere.
  if (media->path.empty() || cutline::core::is_generated_media(*media)) {
    complain(app.main.window, media->name + " is made rather than read, so there is no file "
                                            "to relink it to.");
    return;
  }

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
  // Named in the title, because a project with several files missing is exactly
  // when this is used and "Open" would not say which one is being answered.
  const std::wstring title = L"Relink " + std::filesystem::path(media->name).wstring();
  dialog.lpstrTitle = title.c_str();
  if (GetOpenFileNameW(&dialog) == FALSE) return;

  const std::filesystem::path path{buffer.data()};
  const auto source = cutline::app::probe_source(path.string());
  if (!source.has_value()) {
    complain(app.main.window, "Could not read that file.\n\n" + source.error());
    return;
  }

  app.session.apply(cutline::editor::relink_media(app.session.project(), chosen->id, *source));
  // The renderer is holding a decoder for the old path, and the browser is
  // holding the old file's filmstrip and envelope. All three would go on
  // describing a file that is no longer what this entry means.
  if (app.preview != nullptr) app.preview->release_sources();
  app.filmstrips.clear();
  app.waveforms.clear();
  refresh_all(app);
  invalidate_preview(app);
#endif
}

/// Queues proxies for the sources that have not got one.
///
/// Everything in the pool rather than whatever is selected, which is the one
/// place this deliberately parts company with Premiere's per-clip menu. A proxy
/// is wanted when a machine cannot keep up with the footage, and that is a fact
/// about the whole cut — being asked to select the right files first, and to
/// remember which ones were done, is work the application can do itself.
void make_proxies([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  int asked = 0;
  for (const cutline::core::Media& media : app.session.project().media) {
    // Generated media has no file to transcode, and a source that already has a
    // proxy is not made again — rebuilding one is what taking it away first is
    // for.
    if (media.path.empty() || cutline::core::is_generated_media(media)) continue;
    if (!media.proxy_path.empty()) continue;
    if (!media.has_video) continue;  // audio is read from the original either way

    app.proxies.request(media.id, media.name, media.path,
                        cutline::media::default_proxy_path(media.path));
    ++asked;
  }

  // Said rather than silent. The work is on a worker and the only sign of it is
  // a line in the corner, so a press that queued nothing has to say why or it
  // reads as one that did nothing at all.
  if (asked == 0) {
    const std::size_t have = cutline::core::proxy_count(app.session.project());
    complain(app.main.window,
             have == 0 ? "There is nothing in this project to make a proxy of."
                       : "Every source in this project already has a proxy.");
    return;
  }
  refresh_busy(app);
#endif
}

/// Attaches the proxies that have finished, and reports the ones that did not.
///
/// Called from the frame loop, because this is where a worker's answer becomes
/// part of the project — and the project may only be touched from the thread
/// that owns it.
void collect_proxies([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  const auto finished = app.proxies.take_finished();
  if (!finished.empty()) {
    cutline::core::Project project = app.session.project();
    for (const cutline::app::FinishedProxy& made : finished) {
      project = cutline::core::set_proxy_path(std::move(project), made.media_id, made.path);
    }
    // Applied as one edit however many arrived, so a batch of proxies is one
    // step to undo rather than forty.
    app.session.apply(std::move(project));
    // Only when the project is already reading from proxies: attaching one to a
    // source the renderer has open would otherwise leave it on the original
    // until something else happened to reopen it.
    if (app.session.project().use_proxies && app.preview != nullptr) {
      app.preview->release_sources();
      invalidate_preview(app);
    }
    refresh_all(app);
  }

  const auto failures = app.proxies.take_failures();
  if (!failures.empty()) {
    // One dialog for however many arrived together, because several failing at
    // once is usually one cause — a drive gone, a folder that cannot be written
    // to — and a modal per file during a long batch would be unusable. Said at
    // all, though: a proxy that never appears is indistinguishable from one
    // still being made, and that difference is a session spent waiting.
    std::string message = "Could not make proxies for:";
    for (const cutline::app::FailedProxy& failed : failures) {
      message += "\n\n" + failed.name + "\n" + failed.message;
    }
    complain(app.main.window, message);
  }
#endif
}

/// Abandons the queue and the transcode in progress.
void stop_making_proxies([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.proxies.pending() == 0) {
    complain(app.main.window, "No proxies are being made.");
    return;
  }
  // What has already finished is kept. Those files are written and attached,
  // and throwing them away because the rest was stopped would mean transcoding
  // them again.
  app.proxies.cancel_all();
  refresh_busy(app);
#endif
}

/// Turns reading from proxies on or off.
void toggle_use_proxies([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  const cutline::core::Project& project = app.session.project();
  const bool wanted = !project.use_proxies;
  if (wanted && cutline::core::proxy_count(project) == 0) {
    complain(app.main.window,
             "Nothing in this project has a proxy yet. Make some first, from the Project menu.");
    return;
  }

  app.session.apply(cutline::core::set_use_proxies(project, wanted));
  // The renderer is holding decoders open on the other file. Dropping them is
  // what makes the switch take effect on the next frame rather than the next
  // time something happened to reopen a source.
  if (app.preview != nullptr) app.preview->release_sources();
  refresh_all(app);
  invalidate_preview(app);
#endif
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

  // The gesture's project while one is in flight, so a transform being dragged
  // on the picture shows in the picture without being written down. Then at
  // whatever resolution the preview is set to, which is a copy — but only when
  // it is set to something, and the copy costs a fraction of the render it
  // saves.
  const cutline::core::Project& document = app.showing();
  const cutline::core::Project reduced =
      app.preview_scale == 1.0 ? cutline::core::Project{}
                               : cutline::render::scaled_canvas(document, app.preview_scale);
  const cutline::core::Project& project = app.preview_scale == 1.0 ? document : reduced;
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

/// The source monitor's sequence: one video track holding one clip, the whole
/// of the media, at the media's own shape and rate.
///
/// A source monitor *is* a sequence of one clip, so it can be shown by handing
/// the ordinary renderer a project built on the spot. Nothing new decodes,
/// nothing new composites, and what the source monitor shows is by construction
/// what the sequence would show — which is the property that makes it worth
/// trusting when deciding what to place.
[[nodiscard]] cutline::core::Project source_project(const cutline::core::Media& media) {
  cutline::core::Project project;
  project.canvas_w = std::max(1, media.width.value_or(1280));
  project.canvas_h = std::max(1, media.height.value_or(720));
  project.fps = media.fps.value_or(30.0) > 0.0 ? *media.fps : 30.0;
  project.media = {media};

  cutline::core::Track track;
  track.id = "v1";
  track.kind = cutline::core::TrackKind::Video;

  cutline::core::Clip clip;
  clip.id = "c1";
  clip.media_id = media.id;
  clip.kind = cutline::core::TrackKind::Video;
  clip.source_in = 0.0;
  // The whole of it, marks and all. The marks say what will be *placed*; the
  // monitor still shows the entire source, because a mark you cannot see past
  // is one you cannot move.
  clip.source_out = std::max(media.duration, 1.0 / project.fps);
  clip.start = 0.0;
  track.clips.push_back(std::move(clip));

  project.tracks.push_back(std::move(track));
  return project;
}

/// The media the source monitor is showing, or null when there is none.
[[nodiscard]] const cutline::core::Media* source_media_of(const App& app) {
  const std::string& id = app.session.source_media();
  if (id.empty()) return nullptr;
  const cutline::core::Project& project = app.session.project();
  const auto found = std::ranges::find(project.media, id, &cutline::core::Media::id);
  return found == project.media.end() ? nullptr : &*found;
}

/// Whether the keyboard is somewhere inside the source monitor.
///
/// Premiere routes the transport and the marking keys to whichever monitor is
/// focused, and this is that question asked of the widget tree rather than of a
/// flag: a flag has to be cleared by everything that could take the focus away,
/// and the one that forgets is the one that leaves `I` marking the wrong thing.
[[nodiscard]] bool source_has_focus(const App& app) {
  if (app.source_panel == nullptr || app.main.host == nullptr) return false;
  for (const Widget* at = app.main.host->focused(); at != nullptr; at = at->parent()) {
    if (at == app.source_panel) return true;
  }
  return false;
}

/// Moves the source monitor's playhead, in frames of the source itself.
void step_source(App& app, double frames) {
  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr) return;

  const double fps = media->fps.value_or(0.0) > 0.0 ? *media->fps : 30.0;
  const double at = app.source_playhead + frames / fps;
  app.source_playhead =
      std::clamp(cutline::core::snap_to_frame(at, fps), 0.0, std::max(0.0, media->duration));
  refresh_source(app);
}

/// Marks the source at its own playhead, and takes the mark away when it is
/// already there — the same rule the sequence's own `I` and `O` follow.
void mark_source(App& app, bool out_point) {
  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr) return;

  const std::optional<double>& already = out_point ? media->out_point : media->in_point;
  const double fps = media->fps.value_or(0.0) > 0.0 ? *media->fps : 30.0;
  const bool here =
      already.has_value() && std::abs(*already - app.source_playhead) < 0.5 / fps;

  const std::optional<double> to = here ? std::nullopt
                                        : std::optional<double>(app.source_playhead);
  app.session.apply(out_point
                        ? cutline::core::set_source_out_point(app.session.project(),
                                                              media->id, to)
                        : cutline::core::set_source_in_point(app.session.project(),
                                                             media->id, to));
  refresh_all(app);
}

/// Stops the source playing, and says so on the button.
void stop_source_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.source_player != nullptr) app.source_player->pause();
  if (app.source_play_button != nullptr) app.source_play_button->set_text("Play");
  mark_dirty(app);
#else
  (void)app;
#endif
}

/// Plays the source, or stops it.
void toggle_source_playback([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.source_playing()) {
    stop_source_playback(app);
    return;
  }

  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr || media->path.empty()) return;
  if (app.player_failed) return;

  // One sound card, so one thing playing. Stopping the sequence rather than
  // refusing: pressing play on a source is a clear statement about which of the
  // two is being watched.
  if (app.playing()) stop_playback(app);
  if (app.shuttling()) set_shuttle(app, 0.0);

  if (app.source_player != nullptr && app.source_player_for != media->id) {
    app.source_player.reset();
  }
  if (app.source_player == nullptr) {
    auto made = cutline::engine::Player::create(source_project(*media));
    if (!made.has_value()) {
      app.player_failed = true;
      complain(app.main.window, "Playback is unavailable.\n\n" + made.error());
      return;
    }
    app.source_player = std::move(*made);
    app.source_player_for = media->id;
  }

  app.source_player->seek(app.source_playhead);
  app.source_player->play();
  // The same reason the sequence does it: without this `Sleep(1)` in the loop
  // really sleeps about 15 ms, which is most of a frame at 60 Hz.
  timeBeginPeriod(1);
  if (app.source_play_button != nullptr) app.source_play_button->set_text("Stop");
  mark_dirty(app);
#endif
}

/// One turn of source playback. The sound card says where time is, exactly as
/// it does for the sequence, and the picture follows.
void advance_source_playback([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (!app.source_playing()) return;

  if (!app.source_player->error().empty()) {
    const std::string message = app.source_player->error();
    stop_source_playback(app);
    complain(app.main.window, "Playback stopped.\n\n" + message);
    return;
  }

  const double at = app.source_player->position();
  if (app.source_player->finished()) {
    app.source_playhead = at;
    stop_source_playback(app);
    refresh_source(app);
    return;
  }

  app.source_playhead = at;
  refresh_source(app);
#endif
}

/// Shows a source in the source monitor, remembering it as recently looked at.
///
/// The one way in, so the pool's selection, the dropdown and anything later all
/// leave the same state behind: the list in the order last looked at, and the
/// playhead back at the start when the source itself changed.
void show_source(App& app, const std::string& media_id) {
  if (media_id != app.session.source_media()) app.source_playhead = 0.0;
  app.session.set_source_media(media_id);

  // Newest first, and only ever *added* to the front — something already in the
  // list stays where it is.
  //
  // Reordering on every look was the first attempt and it fought the dropdown
  // it feeds: choosing the second entry moved that entry to the top, so the
  // index the control had just acted on named something else, and the list
  // settled showing the name of the source you had switched away from. A list
  // whose items move while you are choosing from them is the wrong shape for a
  // control you choose from.
  if (!media_id.empty() && std::ranges::find(app.source_recent, media_id) ==
                               app.source_recent.end()) {
    app.source_recent.insert(app.source_recent.begin(), media_id);
    // Long enough to go back and forth between a handful of takes, short enough
    // that the list is still something to glance at rather than read.
    constexpr std::size_t kRecentSources = 10;
    if (app.source_recent.size() > kRecentSources) app.source_recent.resize(kRecentSources);
  }
  refresh_source(app);
  mark_dirty(app);
}

/// Fills the dropdown from what has been looked at, dropping anything the
/// project no longer has — media can be removed, and a list naming something
/// gone would offer a source that cannot be shown.
void refresh_source_list(App& app) {
  if (app.source_choice == nullptr) return;

  const cutline::core::Project& project = app.session.project();
  std::erase_if(app.source_recent, [&](const std::string& id) {
    return std::ranges::find(project.media, id, &cutline::core::Media::id) == project.media.end();
  });

  std::vector<std::string> names;
  names.reserve(app.source_recent.size());
  for (const std::string& id : app.source_recent) {
    const auto found = std::ranges::find(project.media, id, &cutline::core::Media::id);
    names.push_back(found == project.media.end() ? id : found->name);
  }
  if (names.empty()) names.emplace_back("No source");

  if (names != app.source_choice->options()) app.source_choice->set_options(std::move(names));

  const auto at = std::ranges::find(app.source_recent, app.session.source_media());
  app.source_choice->set_selected(at == app.source_recent.end()
                                      ? 0
                                      : static_cast<std::size_t>(at - app.source_recent.begin()));
}

/// Renders the source monitor, and keeps its scrub bar in step.
void refresh_source([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.source_monitor == nullptr) return;

  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr || cutline::core::is_generated_media(*media) || media->path.empty()) {
    // Nothing chosen, or something with no file behind it. The panel keeps its
    // placeholder rather than showing the last source, which would be a picture
    // of something nobody is pointing at.
    app.source_monitor->clear_frame();
    if (app.source_waveform != nullptr) app.source_waveform->set_visible(false);
    if (app.source_scrub != nullptr) {
      app.source_scrub->set_duration(0.0);
      app.source_scrub->set_marks(std::nullopt, std::nullopt);
    }
    refresh_source_list(app);
    app.source_built_for.clear();
    return;
  }

  refresh_source_list(app);
  if (app.source_scrub != nullptr) {
    app.source_scrub->set_duration(media->duration);
    app.source_scrub->set_marks(media->in_point, media->out_point);
    app.source_scrub->set_playhead(app.source_playhead);
  }

  // A source with no picture is shown as its shape instead. Which of the two
  // widgets is in the layout at all is decided here, so the panel never holds
  // an empty rectangle where the other one would be.
  const bool has_picture = media->has_video;
  if (app.source_waveform != nullptr) {
    app.source_waveform->set_visible(!has_picture);
    if (!has_picture) {
      app.waveforms.request(media->id, media->path, 0);
      app.source_waveform->set_waveform(app.waveforms.find(media->id, 0));
      app.source_waveform->set_duration(media->duration);
      app.source_waveform->set_playhead(app.source_playhead);
    }
  }
  app.source_monitor->set_visible(has_picture);
  if (!has_picture) {
    app.source_monitor->clear_frame();
    mark_dirty(app);
    return;
  }

  if (app.source_failed) return;

  const cutline::core::Project project = source_project(*media);
  // Rebuilt when the source changes, because the renderer is made at one canvas
  // size and sources are not all the same shape.
  if (app.source_preview == nullptr || app.source_built_for != media->id) {
    auto made =
        cutline::app::ProjectPreview::create(project.canvas_w, project.canvas_h, app.device);
    if (!made.has_value()) {
      app.source_failed = true;
      complain(app.main.window, "The source monitor is unavailable.\n\n" + made.error());
      return;
    }
    app.source_preview = std::move(*made);
    app.source_built_for = media->id;
  }

  const double at = std::clamp(app.source_playhead, 0.0, std::max(0.0, media->duration));
  if (app.shares_device()) {
    const auto frame = app.source_preview->texture_at(project, at);
    if (!frame.has_value()) {
      app.source_failed = true;
      complain(app.main.window, "Could not render the source.\n\n" + frame.error());
      return;
    }
    app.source_monitor->set_texture(*frame);
  } else {
    const auto frame = app.source_preview->frame_at(project, at);
    if (!frame.has_value()) {
      app.source_failed = true;
      complain(app.main.window, "Could not render the source.\n\n" + frame.error());
      return;
    }
    app.source_monitor->set_frame(*frame);
  }

  app.source_monitor->set_canvas_aspect(static_cast<double>(project.canvas_w) /
                                        project.canvas_h);
  mark_dirty(app);
#endif
}

/// Writes the frame at the playhead to a PNG.
///
/// Rendered again rather than read back off the monitor: what is on screen is
/// letterboxed into whatever the panel happens to be, and a snapshot is a frame
/// of the *sequence* at its own size. This is the same call export makes per
/// frame, so the file is what the exported movie would contain.
void take_snapshot([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.preview == nullptr) {
    complain(app.main.window, "There is no preview to take a snapshot of yet.");
    return;
  }

  const auto frame = app.preview->frame_at(app.session.project(), app.session.playhead());
  if (!frame.has_value()) {
    complain(app.main.window, "Could not render the frame.\n\n" + frame.error());
    return;
  }
  if (frame->empty()) {
    complain(app.main.window, "There is nothing at the playhead to save.");
    return;
  }

  // Asked for after the frame is in hand. A dialog that opens and then reports
  // that there was nothing to save wastes the answer it just collected.
  const auto path = choose_image_file(app.main.window);
  if (!path.has_value()) return;

  // Through Skia, which this already links for the window's surface. The
  // alternative was the uncompressed writer `render_frame` carries for its own
  // use, and a 4K snapshot stored raw is thirty megabytes.
  const SkImageInfo info = SkImageInfo::Make(frame->width, frame->height,
                                             kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  const SkPixmap pixels(info, frame->pixels, static_cast<std::size_t>(frame->row_bytes()));

  SkDynamicMemoryWStream out;
  if (!SkPngEncoder::Encode(&out, pixels, SkPngEncoder::Options{})) {
    complain(app.main.window, "Could not encode the snapshot.");
    return;
  }

  const sk_sp<SkData> encoded = out.detachAsData();
  std::ofstream file(*path, std::ios::binary);
  if (!file) {
    complain(app.main.window, "Could not write " + path->string());
    return;
  }
  file.write(static_cast<const char*>(encoded->data()),
             static_cast<std::streamsize>(encoded->size()));
  if (!file) complain(app.main.window, "Could not write " + path->string());
#else
  complain(app.main.window, "This build has no preview to take a snapshot of.");
#endif
}

/// How wide a frame is measured at.
///
/// A scope is a statistical picture, and a couple of hundred columns tell the
/// same story as four thousand for a fortieth of the work — which matters when
/// it happens every time the playhead moves. It is also about the width the
/// panel is drawn at, so the waveform is not being asked to describe detail
/// finer than a pixel of graph.
constexpr int kScopeWidth = 320;

/// The sequence sizes offered, from the specification's list.
///
/// Named by what they are for rather than only by their numbers, because
/// "1080x1920" is a size and "Vertical" is a decision — and the decision is
/// what somebody is making when they open this.
struct CanvasPreset {
  std::string_view name;
  int width;
  int height;
};

constexpr std::array kCanvasPresets{
    CanvasPreset{"1080p", 1920, 1080},        CanvasPreset{"1440p", 2560, 1440},
    CanvasPreset{"4K UHD", 3840, 2160},       CanvasPreset{"Ultrawide 1440", 3440, 1440},
    CanvasPreset{"Ultrawide 1080", 2560, 1080}, CanvasPreset{"Vertical", 1080, 1920},
    CanvasPreset{"Square", 1080, 1080},
};

/// A sequence size in words: the preset's name when it is one of them, and the
/// plain numbers when it is not. A sequence somebody typed a size into should
/// show that size rather than the word "Custom", which says nothing.
[[nodiscard]] std::string canvas_label(const cutline::core::Project& project) {
  for (const CanvasPreset& preset : kCanvasPresets) {
    if (preset.width == project.canvas_w && preset.height == project.canvas_h) {
      return std::string(preset.name);
    }
  }
  return std::to_string(project.canvas_w) + "x" + std::to_string(project.canvas_h);
}

/// The resolutions the preview offers, and what each is called.
///
/// Three, doubling: full, half, quarter. A quarter is a sixteenth of the work,
/// which is as far as it is worth going — below that the picture stops saying
/// anything about focus or grain, and the compositing is no longer what the
/// scrub is waiting on.
constexpr std::array<std::pair<double, std::string_view>, 3> kPreviewScales{{
    {1.0, "Full"},
    {0.5, "1/2"},
    {0.25, "1/4"},
}};

/// Resizes the sequence and brings everything that depends on its size along.
///
/// The preview's renderer is sized from the project on every frame, so it
/// follows without being told; what does not is the monitor's letterbox while
/// there is no picture yet, and the export dialog's "same as sequence", which
/// is showing a number that has just changed.
void apply_canvas(App& app, int width, int height) {
  if (!app.session.apply(cutline::core::set_canvas(app.session.project(), width, height))) {
    return;
  }
  if (app.monitor != nullptr) {
    const cutline::core::Project& project = app.session.project();
    app.monitor->set_canvas_aspect(static_cast<double>(project.canvas_w) / project.canvas_h);
  }
  refresh_title(app);
  invalidate_preview(app);
  app.inspector_stale = true;
}

/// Sets the sequence's frame rate and tells what depends on it.
void apply_fps(App& app, double fps) {
  if (!app.session.apply(cutline::core::set_fps(app.session.project(), fps))) return;
  refresh_timeline(app);
  refresh_title(app);
  invalidate_preview(app);
}

/// The rates worth offering, which are the ones cameras and screens produce.
constexpr std::array<double, 7> kFrameRates{23.976, 24.0, 25.0, 29.97, 30.0, 50.0, 60.0};

/// A frame rate written the way a rate is written: 23.976 keeps its decimals
/// and 30 does not get three zeroes it never had.
[[nodiscard]] std::string fps_label(double fps) {
  const std::string text = std::format("{:.3f}", fps);
  const std::size_t last = text.find_last_not_of('0');
  const std::size_t cut = text[last] == '.' ? last : last + 1;
  return text.substr(0, cut);
}

/// The project's settings: the size of the sequence and its frame rate.
///
/// A popup rather than a dialog window because that is all it needs to be —
/// `open_popup` takes any widget, and a second real window would bring its own
/// message loop, its own close path and its own way of being left open behind
/// the editor.
[[nodiscard]] std::unique_ptr<Widget> build_project_settings_popup(App& app) {
  auto panel = std::make_unique<Panel>();
  // The current size in the heading, because the presets below only show which
  // one is chosen and a sequence at a typed size matches none of them.
  panel->emplace<Label>("Sequence size — " + canvas_label(app.session.project()))
      .set_bold(true);

  for (const CanvasPreset& preset : kCanvasPresets) {
    auto& row = panel->emplace<Button>(
        std::string(preset.name) + "  " + std::to_string(preset.width) + "x" +
            std::to_string(preset.height),
        [&app, preset] {
          if (app.main.host != nullptr) app.main.host->close_popup();
          apply_canvas(app, preset.width, preset.height);
        });
    row.set_selected(preset.width == app.session.project().canvas_w &&
                     preset.height == app.session.project().canvas_h);
  }

  auto& custom = panel->emplace<Box>(Axis::Horizontal);
  auto& width = custom.emplace<TextField>(std::to_string(app.session.project().canvas_w));
  custom.emplace<Label>("x").set_small(true);
  auto& height = custom.emplace<TextField>(std::to_string(app.session.project().canvas_h));
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
    if (app.main.host != nullptr) app.main.host->close_popup();
    apply_canvas(app, wide, tall);
  });

  panel->emplace<Label>("Frame rate").set_bold(true);
  auto& rates = panel->emplace<Box>(Axis::Horizontal);
  for (const double rate : kFrameRates) {
    auto& choice = rates.emplace<Button>(fps_label(rate), [&app, rate] {
      if (app.main.host != nullptr) app.main.host->close_popup();
      apply_fps(app, rate);
    });
    // Compared with a tolerance: 23.976 is not the number the file holds, and
    // a preset that never looks chosen is one nobody trusts they pressed.
    choice.set_selected(std::abs(app.session.project().fps - rate) < 0.005);
  }

  auto& own = panel->emplace<Box>(Axis::Horizontal);
  auto& typed = own.emplace<TextField>(fps_label(app.session.project().fps));
  own.emplace<Button>("Set", [&app, field = &typed] {
    double rate = 0.0;
    if (std::from_chars(field->text().data(), field->text().data() + field->text().size(),
                        rate)
            .ec != std::errc{}) {
      return;
    }
    if (app.main.host != nullptr) app.main.host->close_popup();
    apply_fps(app, rate);
  });

  return panel;
}

/// Where a settings popup opens: under the menu bar, at its left edge.
///
/// Not at the pointer, and not over the bar it came from — a panel that covers
/// the menu it was opened from hides the thing you would click to close it.
[[nodiscard]] constexpr Rect settings_anchor() noexcept { return Rect{8.0, 72.0, 0.0, 0.0}; }

/// The application's own settings, which belong to the person rather than to
/// the project: the theme, and whatever else is a preference rather than a
/// property of what is being edited.
[[nodiscard]] std::unique_ptr<Widget> build_application_settings_popup(App& app) {
  auto panel = std::make_unique<Panel>();
  panel->emplace<Label>("Theme").set_bold(true);

  for (std::size_t i = 0; i < built_in_themes().size(); ++i) {
    auto& choice = panel->emplace<Button>(built_in_themes()[i].name, [&app, i] {
      // Closed on the way, so nothing here outlives the popup. The theme
      // buttons used to be kept in a list on `App` and set from `set_theme`;
      // in a popup that list would be a set of pointers into a widget tree
      // that has already been destroyed.
      if (app.main.host != nullptr) app.main.host->close_popup();
      set_theme(app, i);
    });
    choice.set_selected(i == app.theme);
  }
  return panel;
}

void choose_preview_scale(App& app, double scale) {
  if (app.preview_scale == scale) return;
  app.preview_scale = scale;
  // Kept in step for the callers that are not the control itself — the
  // dropdown has already moved when it is the one asking, and setting it back
  // to where it is costs nothing.
  if (app.preview_scale_choice != nullptr) {
    using Entry = std::pair<double, std::string_view>;
    const auto at = std::ranges::find(kPreviewScales, scale, &Entry::first);
    if (at != kPreviewScales.end()) {
      app.preview_scale_choice->set_selected(
          static_cast<std::size_t>(at - kPreviewScales.begin()));
    }
  }
  invalidate_preview(app);
}

/// The reading under the master fader.
///
/// Silence rather than "-36 dB" at the bottom stop: that is what the scale's
/// floor means everywhere else in the application, and a number there would
/// suggest the mix was merely quiet.
void show_master_gain(App& app, double gain) {
  if (app.master_reading == nullptr) return;
  const double db = cutline::ui::gain_to_fader_db(gain);
  app.master_reading->set_text(db <= cutline::ui::kGainFloorDb
                                   ? std::string("Silent")
                                   : std::format("{:+.1f} dB", db));
}

/// Copies the player's levels into the meter, once a frame.
///
/// Polled rather than pushed: levels are produced every few milliseconds on the
/// render thread and the meter is drawn at frame rate, so a message per block
/// would be several wake-ups for each thing drawn. A meter is also the one
/// display where a missed update is invisible — the next one is milliseconds
/// away and the ballistics carry across it.
void refresh_meter([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.meter == nullptr) return;
  // A player that has not been made yet reads as silence, which is true: there
  // is nothing playing.
  app.meter->set_levels(app.player == nullptr ? cutline::audio::MeterReading{}
                                              : app.player->levels());
#endif
}

/// Measures the frame at the playhead, if the scopes are on show and stale.
///
/// Through `frame_at` rather than off the monitor's texture: a scope measures
/// the *frame*, and what the monitor holds may be a texture on the graphics
/// card that would have to come down anyway. Downscaled by sampling rather than
/// averaging — a scope counts pixels that exist, and an averaged pixel is one
/// the frame does not contain.
void refresh_scopes([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.scopes == nullptr || !app.scopes_stale) return;
  app.scopes_stale = false;

  if (app.preview == nullptr) return;
  const auto frame = app.preview->frame_at(app.session.project(), app.session.playhead());
  if (!frame.has_value() || frame->empty()) return;

  const int step = std::max(1, frame->width / kScopeWidth);
  const int width = std::max(1, frame->width / step);
  const int height = std::max(1, frame->height / step);

  // Not `small`: <rpcndr.h>, which arrives with windows.h, defines that as
  // `char`, and the error it produces names neither.
  std::vector<std::uint8_t> sampled(static_cast<std::size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    const std::uint8_t* row =
        frame->pixels + static_cast<std::ptrdiff_t>(y) * step * frame->row_bytes();
    for (int x = 0; x < width; ++x) {
      const std::uint8_t* pixel = row + static_cast<std::ptrdiff_t>(x) * step * 4;
      const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 4;
      sampled[at] = pixel[0];
      sampled[at + 1] = pixel[1];
      sampled[at + 2] = pixel[2];
      sampled[at + 3] = pixel[3];
    }
  }

  const cutline::render::ScopeImage image{
      .pixels = sampled.data(), .width = width, .height = height};

  auto readings = std::make_shared<cutline::ui::ScopeReadings>();
  readings->histogram = cutline::render::compute_histogram(image);
  readings->waveform =
      cutline::render::compute_waveform(image, cutline::render::ScopeChannel::Luma);
  readings->parade = cutline::render::compute_parade(image);
  readings->vectorscope = cutline::render::compute_vectorscope(image);
  readings->measured = true;

  app.scope_readings = std::move(readings);
  app.scopes->set_readings(app.scope_readings);
  mark_dirty(app);
#endif
}

/// Says the measurements no longer describe what is on screen.
void invalidate_scopes(App& app) {
  app.scopes_stale = true;
  mark_dirty(app);
}

/// Shows one of the four, and lights its tab.
void choose_scope(App& app, cutline::ui::ScopeKind kind) {
  app.scope_kind = kind;
  if (app.scopes != nullptr) app.scopes->set_kind(kind);

  // Relabelled here rather than by rebuilding the row, which would destroy the
  // button whose click is still running — the same trap the theme switcher has.
  if (app.scope_button != nullptr) {
    app.scope_button->set_text(std::string(cutline::ui::to_string(kind)));
  }
  mark_dirty(app);
}

/// Marks the picture as no longer matching the playhead or the project.
void invalidate_preview(App& app) {
#if CUTLINE_HAVE_PREVIEW
  // The gesture's project outlives the gesture only by accident, and an
  // accident here is invisible: the preview goes on rendering a project the
  // document no longer has, and looks merely out of date. Anything asking for
  // a fresh picture while no handle is being dragged is asking for the
  // document's.
  if (!app.dragging_handle && !app.live_gesture) app.live_project.reset();

  app.preview_stale = true;
  // The scopes measure the picture, so anything that changes the picture
  // changes them. Here rather than at every call site, because every one of
  // those already says "the picture is out of date" and would otherwise have to
  // remember to say it twice.
  app.scopes_stale = true;
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
  // Once more with the sound stopped, or the bars stay where the last thing
  // playing left them and the meter goes on claiming there is audio.
  refresh_meter(app);
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

void toggle_looping(App& app) {
  app.looping = !app.looping;
  if (app.loop_button != nullptr) app.loop_button->set_selected(app.looping);
  mark_dirty(app);
}

/// The speeds J and L step through, in each direction.
///
/// Premiere's: press again and it goes up. Four is as fast as a picture is
/// still worth looking at — past that the frames a decoder can produce are so
/// far apart that scrubbing the ruler is quicker.
constexpr std::array kShuttleRates{1.0, 2.0, 4.0};

/// The next speed in `direction` from whatever is running now.
///
/// Changing direction starts again at 1 rather than counting down through the
/// speeds: pressing J while running forwards means "go back", not "go forwards
/// a bit less".
[[nodiscard]] double next_shuttle(double current, double direction) noexcept {
  if (current == 0.0 || (current > 0.0) != (direction > 0.0)) return direction;
  const double speed = std::abs(current);
  for (const double step : kShuttleRates) {
    if (step > speed + 1e-9) return direction * step;
  }
  return current;
}

void set_shuttle(App& app, double rate) {
#if CUTLINE_HAVE_PREVIEW
  // Ordinary playback is a shuttle of exactly one: it is the only rate the
  // sound card can play, so it is the only one that gets sound. Everything
  // else moves the picture and stays silent — which is what J and L are for,
  // since what they are used for is finding a moment by eye.
  if (rate == 1.0) {
    app.shuttle = 0.0;
    if (!app.playing()) toggle_playback(app);
    return;
  }

  if (app.playing()) stop_playback(app);
  app.shuttle = rate;
  app.shuttled_at = std::chrono::steady_clock::now();
  app.shuttle_at = app.session.playhead();
  // So the first turn draws rather than deciding it is on the frame already
  // showing and sleeping instead.
  app.shown_frame = -1;
  if (app.play_button != nullptr) {
    app.play_button->set_text(app.shuttling() ? "Stop" : "Play");
  }
  mark_dirty(app);
#else
  (void)app;
  (void)rate;
#endif
}

/// One turn of a shuttle: moves the playhead by however much time has really
/// passed, times the rate.
///
/// By elapsed time rather than a fixed step per turn, because the loop's turn
/// rate depends on how long a frame took to decode — and a shuttle that ran
/// slower on heavy footage would be a shuttle nobody could aim with.
void advance_shuttle(App& app) {
  if (!app.shuttling()) return;

  const auto now = std::chrono::steady_clock::now();
  // Capped, and the cap is what stops one slow turn making the next one worse.
  //
  // A shuttle that advances by however long the last turn took will, after a
  // hitch of a quarter of a second, jump fifteen frames of a 60fps source — and
  // fifteen frames is half the run of decoded frames the renderer keeps, so the
  // next stretch has half as many turns to read ahead in, and stalls sooner.
  // Driven, that showed as hitches arriving four seconds apart and then bunching
  // up: 10832, 11396, 11881, 12941, 13493 milliseconds, closing in.
  //
  // Nothing is out of sync as a result. J and L are a *silent* shuttle — the
  // sound card only plays at its own rate — so there is no clock this has to
  // agree with, and playing a moment slow after a hitch is invisible where the
  // hitch itself is not.
  constexpr double kMaxShuttleStep = 0.1;
  const double elapsed =
      std::min(std::chrono::duration<double>(now - app.shuttled_at).count(), kMaxShuttleStep);
  app.shuttled_at = now;

  const double duration = cutline::core::timeline_duration(app.session.project());
  app.shuttle_at += elapsed * app.shuttle;

  // Stopping at the ends rather than wrapping, unless looping is on — the same
  // rule playback follows, and the same answer to "what does the end mean".
  const bool past_the_end = app.shuttle_at <= 0.0 ||
                            (duration > 0.0 && app.shuttle_at >= duration);
  if (past_the_end && app.looping) {
    app.shuttle_at = app.shuttle > 0.0 ? 0.0 : std::max(0.0, duration);
  }
  app.shuttle_at = std::clamp(app.shuttle_at, 0.0, std::max(0.0, duration));
  app.session.set_playhead(app.shuttle_at);
  if (past_the_end && !app.looping) set_shuttle(app, 0.0);

  // Once per frame of the sequence, the same rule playback follows: the
  // playhead moves continuously and the picture does not have to. Without this
  // a shuttle rebuilt the timeline and asked for a render as fast as the loop
  // could turn, which is a busy core for pictures nobody sees.
  const double fps = app.session.project().fps > 0.0 ? app.session.project().fps : 30.0;
  const auto frame = static_cast<long long>(app.session.playhead() * fps);
  if (frame == app.shown_frame) {
    Sleep(1);
    return;
  }
  app.shown_frame = frame;

  show_playhead(app);
  follow_playhead(app);
  refresh_timeline(app);
  scroll_to_playhead(app);
  invalidate_preview(app);
}

void toggle_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  // A shuttle is playback of a sort, so the space bar stops it.
  if (app.shuttling()) {
    set_shuttle(app, 0.0);
    return;
  }
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

    // The master fader is the one edit playback survives, and it has to: it is
    // set by ear against what is playing, and stopping the sound the moment the
    // button came up would mean it could only ever be set against silence. The
    // mixer takes it live, and nothing that was decoded depends on it — so the
    // change is handed over and then discounted from the comparison below.
    const double master = app.session.project().master_gain;
    if (master != app.player_project.master_gain) {
      app.player->set_master_gain(master);
      app.player_project.master_gain = master;
    }

    if (app.player_project != app.session.project()) {
      invalidate_playback(app);
      return;
    }
  }

  // Before the frame check below: a meter is the one thing here that has
  // something new to say between two pictures, and it costs a copy of a dozen
  // doubles to say it.
  refresh_meter(app);
  mark_dirty(app);

  const double at = app.player->position();
  if (app.player->finished()) {
    // Round again from the start of whatever is marked, rather than stopping.
    // The marked range and not the whole sequence, because marking a passage
    // and looping it is one gesture: it is how a cut gets watched twenty times
    // while a level is set against it.
    if (app.looping) {
      const double from = cutline::core::marked_span(app.session.project()).start;
      app.session.set_playhead(from);
      app.player->seek(from);
      app.player->play();
      app.shown_frame = -1;
      refresh_timeline(app);
      invalidate_preview(app);
      return;
    }
    app.session.set_playhead(at);
    refresh_timeline(app);
    invalidate_preview(app);
    stop_playback(app);
    return;
  }

  // The end of the marked range is the loop's end. The player only knows where
  // the *sequence* stops, so this is the one place that can notice.
  if (app.looping) {
    const cutline::core::MarkedSpan span =
        cutline::core::marked_span(app.session.project());
    const double end = span.start + span.duration;
    if (span.duration > 0.0 && at >= end) {
      app.session.set_playhead(span.start);
      app.player->seek(span.start);
      app.shown_frame = -1;
      refresh_timeline(app);
      invalidate_preview(app);
      return;
    }
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
  show_playhead(app);
  follow_playhead(app);
  refresh_timeline(app);
  // After the refresh, which is what puts the new time on the view.
  scroll_to_playhead(app);
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
  // The master fader belongs to the document, so a loaded project moves it.
  if (app.master_fader != nullptr) {
    app.master_fader->set_value(
        cutline::ui::gain_to_fader_db(app.session.project().master_gain));
    show_master_gain(app, app.session.project().master_gain);
  }
  if (app.monitor != nullptr) {
    const cutline::core::Project& project = app.session.project();
    app.monitor->set_canvas_aspect(static_cast<double>(project.canvas_w) / project.canvas_h);
  }
  // After the browser, which is what may have changed which source is chosen.
  refresh_source(app);
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

  // Into the pool and nowhere else. Importing is filing something away, not
  // editing with it — and a file that lands on the timeline the moment it is
  // read is one that has to be undone before the sequence can be looked at.
  // Placing it is a gesture of its own: a drag onto a track, or a drop.
  std::string id;
  app.session.apply(cutline::editor::import_media(app.session.project(), *source, &id));
  refresh_all(app);

  // Selected in the pool, so it is obvious where it went — a browser that has
  // silently grown by one is how somebody imports the same file twice.
  if (app.browser != nullptr && !id.empty()) {
    const auto& items = app.browser->items();
    const auto found = std::ranges::find(items, id, &cutline::ui::MediaItem::id);
    if (found != items.end()) {
      app.browser->select(static_cast<std::size_t>(found - items.begin()));
    }
    // And into the source monitor, because selecting a row from here does not
    // go through the browser's own callback — it is set, not chosen. Without
    // this the pool showed the new file highlighted and the source monitor
    // said "No source", which is two panels disagreeing about what is selected.
    show_source(app, id);
  }
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

/// Where a snapshot should go. A third dialog for the same reason the second
/// exists: the filter and the default extension are the whole of what these
/// functions do.
[[nodiscard]] std::optional<std::filesystem::path> choose_image_file(HWND owner) {
  std::array<wchar_t, MAX_PATH> buffer{};

  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = L"PNG image\0*.png\0All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrDefExt = L"png";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

  if (GetSaveFileNameW(&dialog) == FALSE) return std::nullopt;
  return std::filesystem::path(buffer.data());
}

void complain(HWND owner, const std::string& message) {
  MessageBoxA(owner, message.c_str(), "Cutline", MB_OK | MB_ICONWARNING);
}

/// Writes a recovery copy if one is due. Called from the autosave timer.
///
/// Silent about failure, and deliberately: this is a background courtesy, and a
/// message box interrupting an edit to say that a file the user never asked for
/// could not be written would be worse than the thing it is warning about. It
/// simply tries again at the next tick.
void poll_autosave(App& app) {
  const std::uint64_t revision = app.session.revision();
  if (!cutline::editor::autosave_due(app.autosave, app.session.modified(), revision,
                                     std::chrono::steady_clock::now())) {
    return;
  }

  if (cutline::editor::write_autosave(app.session.path(), app.session.project())) {
    app.autosave = {.written_at = std::chrono::steady_clock::now(),
                    .written_revision = revision,
                    .ever_written = true};
  }
}

/// Throws away the recovery copy and forgets there was one.
///
/// After a save, and on the way out. What is left in the recovery directory is
/// then exactly what was never recovered from, which is what makes finding one
/// at startup mean something.
void clear_autosave(App& app, const std::filesystem::path& document) {
  cutline::editor::discard_autosave(document);
  app.autosave = {};
}

/// Offers back a recovery copy of `document`, if there is one newer than the
/// document itself. Returns whether it was taken.
///
/// Asked rather than restored silently. A recovery copy is a guess about what
/// somebody wanted, and one that opens a different project from the one they
/// double-clicked without saying so is a guess made too confidently.
[[nodiscard]] bool offer_recovery(App& app, const std::filesystem::path& document) {
  const auto found = cutline::editor::find_recovery(document);
  if (!found.has_value()) return false;

  const std::string what =
      document.empty() ? std::string("an unsaved project")
                       : document.filename().string();
  const std::string message =
      "Cutline has unsaved changes to " + what +
      " from a session that did not close normally.\n\nRecover them?";
  if (MessageBoxA(app.main.window, message.c_str(), "Cutline",
                  MB_YESNO | MB_ICONQUESTION) != IDYES) {
    // Refused once is refused: keeping it would offer the same changes again
    // every time the project was opened.
    clear_autosave(app, document);
    return false;
  }

  const auto loaded = cutline::editor::read_project(found->path);
  if (!loaded.has_value()) {
    complain(app.main.window, "Could not read the recovered project.\n\n" + loaded.error());
    return false;
  }

  // Opened against the *document's* path, so saving writes where the project
  // lives rather than into the recovery directory. It stays modified, because
  // it is: what is on disk is still the older version.
  app.session.reset(loaded->project, document);
  app.session.mark_unsaved();
  refresh_all(app);
  return true;
}

/// Asks whether there is a newer version.
///
/// Only ever from the button. Nothing checks on its own: an editor that phones
/// home the moment it opens is one that has decided on the user's behalf that
/// it may, and this one has no telemetry and no reason to start.
void check_for_updates(App& app) {
  using State = cutline::app::Updater::State;
  switch (app.updater.state()) {
    case State::Available:
      // Already found; pressing again gets on with it.
      app.updater.download();
      break;
    case State::Ready:
      settle_update(app);
      break;
    case State::Checking:
    case State::Downloading:
      break;
    default:
      app.updater.check(cutline::editor::Version{cutline::core::kVersionMajor,
                                                 cutline::core::kVersionMinor,
                                                 cutline::core::kVersionPatch});
      if (app.version_button != nullptr) app.version_button->set_text("Checking...");
      mark_dirty(app);
      break;
  }
}

/// Acts on whatever the updater has just decided.
///
/// Every branch that offers something asks first. An editor that downloaded and
/// ran an installer because somebody pressed a button labelled with a version
/// number would be doing rather more than it said it would.
void settle_update(App& app) {
  using State = cutline::app::Updater::State;
  const std::string running = std::string("v") + std::string(cutline::core::kVersion);

  switch (app.updater.state()) {
    case State::UpToDate:
      if (app.version_button != nullptr) app.version_button->set_text(running);
      MessageBoxA(app.main.window, ("Cutline " + running + " is the latest version.").c_str(),
                  "Cutline", MB_OK | MB_ICONINFORMATION);
      break;

    case State::Available: {
      const cutline::editor::Release found = app.updater.found();
      const std::string message =
          "Cutline " + found.version.to_string() + " is available.\n\n" +
          (found.notes.empty() ? std::string("") : found.notes + "\n\n") + "Download it now?";
      if (app.version_button != nullptr) {
        app.version_button->set_text("Update to " + found.version.to_string());
      }
      if (MessageBoxA(app.main.window, message.c_str(), "Cutline",
                      MB_YESNO | MB_ICONQUESTION) == IDYES) {
        app.updater.download();
      }
      break;
    }

    case State::Ready: {
      // The installer replaces the running program, so the editor has to go
      // first — and anything unsaved has to be dealt with before it does.
      if (!confirm_discard(app)) break;
      if (MessageBoxA(app.main.window,
                      "The update is ready.\n\nCutline will close while it installs.", "Cutline",
                      MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
        break;
      }

      const std::wstring path = app.updater.installer().wstring();
      const auto started = reinterpret_cast<INT_PTR>(
          ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
      // Anything at or below 32 is a failure code rather than an instance
      // handle, which is this API's way of saying so.
      if (started <= 32) {
        complain(app.main.window, "The installer would not start. It is at\n\n" +
                                      app.updater.installer().string());
        break;
      }
      PostMessageW(app.main.window, WM_CLOSE, 0, 0);
      break;
    }

    case State::Failed:
      if (app.version_button != nullptr) app.version_button->set_text(running);
      complain(app.main.window, "Could not check for updates.\n\n" + app.updater.error());
      break;

    case State::Downloading:
      if (app.version_button != nullptr) {
        app.version_button->set_text(
            std::format("{:.0f}%", app.updater.progress() * 100.0));
      }
      break;

    case State::Idle:
      break;
  }
  mark_dirty(app);
}

/// Opens a named project, having already established that it is wanted.
///
/// Split from the menu command so the same path serves a file named on the
/// command line — which is what "Open with", a shortcut with an argument, and a
/// project dragged onto the executable all amount to. Without it the
/// application read `--check` and `--benchmark` and silently ignored everything
/// else, so every one of those opened an empty editor.
void open_project_at(App& app, const std::filesystem::path& path) {
  if (offer_recovery(app, path)) return;

  const auto loaded = cutline::editor::read_project(path);
  if (!loaded.has_value()) {
    complain(app.main.window, "Could not open that project.\n\n" + loaded.error());
    return;
  }

  app.session.reset(loaded->project, path);
  refresh_all(app);

  // Warnings are not failures: a project whose footage has moved still opens,
  // and saying so is more use than refusing it.
  if (!loaded->warnings.empty()) {
    std::string message = "The project opened with warnings:\n";
    for (const std::string& warning : loaded->warnings) message += "\n" + warning;
    complain(app.main.window, message);
  }
}

void open_project(App& app) {
  // Before the file dialog, so somebody who decides to save first is not asked
  // to pick a file twice.
  if (!confirm_discard(app)) return;

  const auto path = choose_file(app.main.window, false);
  if (!path.has_value()) return;

  open_project_at(app, *path);
}

/// Acts on a file named on the command line.
///
/// A project opens; anything the importer recognises goes into the pool, which
/// is what "Open with" on a video should reasonably do and is the same place
/// importing puts it. Anything else is said out loud rather than ignored — a
/// mistyped name that opens an empty editor looks exactly like a crash on
/// startup.
void open_from_command_line(App& app, const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    complain(app.main.window, "There is no file at\n\n" + path.string());
    return;
  }

  if (cutline::editor::has_project_extension(path)) {
    open_project_at(app, path);
    return;
  }

#if CUTLINE_HAVE_PREVIEW
  if (cutline::editor::looks_like_media(path.string())) {
    const auto source = cutline::app::probe_source(path.string());
    if (!source.has_value()) {
      complain(app.main.window, "Could not read that file.\n\n" + source.error());
      return;
    }
    std::string id;
    app.session.apply(cutline::editor::import_media(app.session.project(), *source, &id));
    refresh_all(app);
    if (app.browser != nullptr && !id.empty()) app.browser->select_id(id);
    show_source(app, id);
    return;
  }
#endif

  complain(app.main.window,
           path.filename().string() + " is not a project or a media file.");
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

  // The old document's recovery copy goes before the new path is recorded, or
  // "Save As" would leave one behind under the name it used to have.
  clear_autosave(app, app.session.path());
  app.session.mark_saved(path);
  clear_autosave(app, path);
  refresh_title(app);
  return true;
}

/// Asks what to do about unsaved work, and reports whether to carry on.
///
/// Three answers, not two. "Are you sure?" with only Yes and No makes
/// cancelling and discarding the same button, and one of those throws away an
/// afternoon — so Save writes and continues, Discard continues, and Cancel does
/// not. A save that is cancelled at the file dialog cancels the whole thing,
/// which is the only reading of it that cannot lose anything.
[[nodiscard]] bool confirm_discard(App& app) {
  if (!app.session.modified()) return true;

  // The file's own name, not `document_title` — that one carries the asterisk
  // that *means* unsaved, and "unsaved changes to Untitled *" says it twice and
  // reads like a filename with a typo in it.
  const std::string name = app.session.path().empty()
                               ? std::string("this project")
                               : app.session.path().filename().string();
  const int answer = MessageBoxA(
      app.main.window,
      ("There are unsaved changes to " + name + ".\n\nSave them before closing?").c_str(),
      "Cutline", MB_YESNOCANCEL | MB_ICONWARNING);

  if (answer == IDCANCEL) return false;
  if (answer == IDYES) return save_project(app, false);
  return true;
}

void new_project(App& app) {
  if (!confirm_discard(app)) return;
  clear_autosave(app, app.session.path());
  app.session.reset(new_project_model());
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
    // Premiere's comma and full stop, which are insert and overwrite there and
    // were nudge here. Nudge moves to alt and the arrows, which is where
    // Premiere keeps it — the two keys next to each other on the bottom row are
    // worth more to the edit that needs a hand on the keyboard.
    Binding{Key::Comma, false, false, cutline::editor::Command::Insert},
    Binding{Key::Period, false, false, cutline::editor::Command::Overwrite},
    Binding{Key::Left, false, false, cutline::editor::Command::NudgeLeft, true},
    Binding{Key::Right, false, false, cutline::editor::Command::NudgeRight, true},
    // Premiere's Q and W: trim the edit point before the playhead up to it, and
    // the one after it back. Both ripple, which is what makes them the fastest
    // way to tighten a cut — the sequence closes over what went.
    Binding{Key::Q, false, false, cutline::editor::Command::TrimPreviousToPlayhead},
    Binding{Key::W, false, false, cutline::editor::Command::TrimNextToPlayhead},
    Binding{Key::Delete, false, false, cutline::editor::Command::Delete},
    Binding{Key::Delete, false, true, cutline::editor::Command::RippleDelete},
    Binding{Key::Escape, false, false, cutline::editor::Command::SelectNone},
    // Premiere's Ctrl+L, and shift for the other direction. Not one key that
    // toggles: a selection with some clips linked and some not has no honest
    // answer to which way it is going, and guessing would quietly unlink what
    // somebody meant to gather up.
    Binding{Key::L, true, false, cutline::editor::Command::LinkClips},
    Binding{Key::L, true, true, cutline::editor::Command::UnlinkClips},
    // The clipboard. Here rather than among the application keys on purpose:
    // the keyframe lanes have their own Ctrl+C and Ctrl+V for the keyframes in
    // them, and taking these first would mean the panel you are working in
    // could never have a clipboard of its own.
    Binding{Key::C, true, false, cutline::editor::Command::Copy},
    Binding{Key::X, true, false, cutline::editor::Command::Cut},
    Binding{Key::V, true, false, cutline::editor::Command::Paste},
    Binding{Key::V, true, true, cutline::editor::Command::PasteInsert},
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
  /// What it says when the pointer rests on it. The name and the key, because
  /// an icon button has no words at all and a shortcut nobody can discover is
  /// one nobody uses.
  const char* hint;
};

constexpr std::array kTools{
    ToolEntry{cutline::ui::Tool::Selection, IconButton::Icon::Pointer, Key::V,
              "Selection (V)"},
    // Premiere's B and N, in Premiere's order: the two edge tools sit together
    // between the pointer and the razor, because they are variations on the
    // trim the pointer already does.
    ToolEntry{cutline::ui::Tool::Ripple, IconButton::Icon::Ripple, Key::B,
              "Ripple Edit (B)"},
    ToolEntry{cutline::ui::Tool::Roll, IconButton::Icon::Roll, Key::N, "Rolling Edit (N)"},
    ToolEntry{cutline::ui::Tool::Razor, IconButton::Icon::Razor, Key::C, "Razor (C)"},
    ToolEntry{cutline::ui::Tool::RateStretch, IconButton::Icon::RateStretch, Key::R,
              "Rate Stretch (R)"},
    ToolEntry{cutline::ui::Tool::Slip, IconButton::Icon::Slip, Key::Y, "Slip (Y)"},
    ToolEntry{cutline::ui::Tool::Slide, IconButton::Icon::Slide, Key::U, "Slide (U)"},
};

/// Pushes the snap setting into the view and lights the button for it.
///
/// The setting lives on the application rather than on the view, because the
/// view is destroyed and rebuilt by a rearrangement or a theme change — and
/// snapping turning itself back on when a panel moved would be a setting
/// nobody could rely on.
void show_snapping(App& app) {
  if (app.timeline != nullptr) app.timeline->set_snapping(app.snapping);
  if (app.snap_button != nullptr) app.snap_button->set_selected(app.snapping);
  mark_dirty(app);
}

void toggle_snapping(App& app) {
  app.snapping = !app.snapping;
  show_snapping(app);
}

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
    refresh_browser(app);
    refresh_title(app);
    invalidate_preview(app);
    app.inspector_stale = true;

    // The playhead may have moved: every transport command moves it, and so do
    // the jumps to the next and previous marker. Here rather than in each of
    // them — which is also why the timecode used to sit still while the arrow
    // keys walked the playhead along.
    show_playhead(app);
    follow_playhead(app);
  }
  mark_dirty(app);
}

/// Moves the playhead somewhere and brings everything that follows it along.
///
/// What the scrub handler does, in a function, because the timecode field
/// needs the same thing and a second copy of it is how the two come to disagree
/// about whether a jump takes the sound with it.
void go_to_time(App& app, double at) {
  app.session.set_playhead(at);
  show_playhead(app);
  follow_playhead(app);
  refresh_timeline(app);
#if CUTLINE_HAVE_PREVIEW
  // A jump while it is playing takes the sound with it. Without this the audio
  // carries on from where it was and the picture is somewhere else, which is
  // worse than either.
  if (app.playing()) {
    app.player->seek(app.session.playhead());
    app.shown_frame = -1;
  }
#endif
  invalidate_preview(app);
  mark_dirty(app);
}

/// The transport and marking keys, when the source monitor is the one in hand.
///
/// Reports whether it took the key, so the sequence's own bindings see only
/// what this did not want. The set is deliberately the same as the sequence's —
/// these are the keys everybody's fingers already know, and which of the two
/// monitors they act on is the whole question a focused panel answers.
bool handle_source_key(App& app, Key key, const Modifiers& modifiers) {
  if (!source_has_focus(app)) return false;
  if (source_media_of(app) == nullptr) return false;
  if (modifiers.control || modifiers.alt) return false;

  const cutline::core::Media* media = source_media_of(app);
  switch (key) {
    case Key::I:
      if (modifiers.shift) return false;
      mark_source(app, false);
      return true;
    case Key::O:
      if (modifiers.shift) return false;
      mark_source(app, true);
      return true;
    case Key::Left:
      // Shift for a bigger step, as the sequence's nudge does.
      step_source(app, modifiers.shift ? -5.0 : -1.0);
      return true;
    case Key::Right:
      step_source(app, modifiers.shift ? 5.0 : 1.0);
      return true;
    case Key::Home:
      app.source_playhead = 0.0;
      refresh_source(app);
      return true;
    case Key::End:
      app.source_playhead = std::max(0.0, media->duration);
      refresh_source(app);
      return true;
    case Key::Space:
      if (modifiers.shift) return false;
      toggle_source_playback(app);
      return true;
    case Key::K:
      stop_source_playback(app);
      return true;
    default:
      return false;
  }
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
  }).set_tooltip("Bring media into the project (Ctrl+I)");
  tools.emplace<Button>("Remove", [app] {
    if (app != nullptr) remove_from_pool(*app);
  }).set_tooltip("Take the selected media out, and every clip of it");
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
  // What an insert or an overwrite would place. Premiere's source monitor holds
  // this; there is none here yet, so the pool's selection is the source, which
  // is also what a double-click already places.
  pool.set_on_select([app](std::optional<std::size_t> index) {
    if (app == nullptr || app->browser == nullptr) return;
    const std::vector<cutline::ui::MediaItem>& items = app->browser->items();
    show_source(*app, index.has_value() && *index < items.size() ? items[*index].id
                                                                : std::string{});
  });
  pool.set_on_drop([app](std::size_t index, double x, double y) {
    if (app == nullptr) return;
    // The promise is kept or it is withdrawn; either way it stops being shown.
    if (app->timeline != nullptr) app->timeline->set_drop_ghost(std::nullopt);
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
    pool.set_items(cutline::editor::browser_items(check_project()));
  }

  // Under the pool rather than in the toolbar above it. It was in the toolbar
  // first, and `--check` reported two controls cut in half by the edge of the
  // panel — the same trap a third generator button fell into. This row has room
  // and this is a thing you look at rather than reach for.
  //
  // The version doubles as the button: a label saying what is running and a
  // separate control asking about newer would be two where one will do, and
  // putting the number on the button is what makes "there is a newer one" mean
  // anything.
  auto& footer = panel->emplace<Box>(Axis::Horizontal);
  auto& version = footer.emplace<Button>(
      std::string("v") + std::string(cutline::core::kVersion), [app] {
        if (app != nullptr) check_for_updates(*app);
      });
  if (app != nullptr) app->version_button = &version;
  footer.emplace<Spacer>();

  // At the far end of the row the version sits on, which is where Premiere puts
  // its progress too. Small and quiet: it is reassurance that something is
  // happening, not a thing to watch.
  auto& busy = footer.emplace<Label>(std::string{});
  busy.set_small(true);
  busy.set_visible(false);
  if (app != nullptr) app->busy_label = &busy;

  return panel;
}

[[nodiscard]] std::unique_ptr<Widget> make_monitor_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  auto& picture = panel->emplace<MonitorView>();
  if (app != nullptr) {
    app->monitor = &picture;
#if !CUTLINE_HAVE_PREVIEW
    // Only where nothing can be decoded at all. In a build that renders, an
    // empty sequence shows an empty frame — colour bars there read as content
    // somebody put on the timeline rather than as "there is nothing here yet".
    picture.set_frame(app->pattern.view());
#endif
    // Pushed rather than defaulted, for the same reason snapping is: the panel
    // is rebuilt by a rearrangement and the setting is not.
    picture.set_aspect_locked(app->aspect_locked);

    // Live on every pixel so the preview follows the drag, and once on release
    // so the whole gesture is one entry in the undo stack — the same split the
    // timeline and every slider use.
    // A mask dragged on the picture. The same bargain as everything else
    // dragged here: rendered against a copy while the button is down, written
    // once on release, so the whole gesture is one undo entry.
    picture.set_on_mask_change([app](std::size_t index, const cutline::ui::MaskOverlay& shape) {
      if (index >= app->mask_effects.size()) return;
      const auto selection = app->session.selection();
      if (selection.empty()) return;

      app->live_gesture = true;
      app->live_project = cutline::editor::apply_mask_overlay(
          app->session.project(), selection.front(), app->mask_effects[index], shape,
          app->session.playhead());
      invalidate_preview(*app);
    });
    picture.set_on_mask_commit([app](std::size_t index, const cutline::ui::MaskOverlay& shape) {
      if (index >= app->mask_effects.size()) return;
      const auto selection = app->session.selection();
      if (selection.empty()) return;

      app->session.apply(cutline::editor::apply_mask_overlay(
          app->session.project(), selection.front(), app->mask_effects[index], shape,
          app->session.playhead()));
      app->live_gesture = false;
      invalidate_preview(*app);
      app->inspector_stale = true;
    });

    picture.set_on_transform_change([app](const cutline::ui::MonitorBox& box) {
      const auto selection = app->session.selection();
      if (selection.empty()) return;
      app->dragging_handle = true;
      // Against the session's project every time rather than against the last
      // preview: the box the view reports is where the layer should be, not how
      // far it has come, and compounding them would send it off across the
      // canvas at several times the speed of the pointer.
      app->live_project = cutline::editor::apply_monitor_box(
          app->session.project(), selection.front(), box, app->session.playhead());
      invalidate_preview(*app);
    });
    picture.set_on_transform_commit([app](const cutline::ui::MonitorBox& box) {
      const auto selection = app->session.selection();
      app->dragging_handle = false;
      app->live_project.reset();
      if (selection.empty()) return;

      app->session.apply(cutline::editor::apply_monitor_box(
          app->session.project(), selection.front(), box, app->session.playhead()));
      refresh_timeline(*app);
      invalidate_preview(*app);
      // The inspector's Motion rows are showing the same numbers, and a
      // transform dragged on the picture that left them stale would be two
      // controls disagreeing about one value. On the commit only — rebuilding
      // it per mouse move would destroy and remake dozens of widgets a second.
      app->inspector_stale = true;
    });
    refresh_handles(*app);
  }

  auto& transport = panel->emplace<Box>(Axis::Horizontal);
  // Both toggle: marking where a mark already is takes it away, which is how
  // one is removed without a third button that exists only to undo the other
  // two. `run` is what decides that, so the button and the I key cannot drift.
  transport.emplace<Button>("Mark In", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::MarkIn);
  }).set_tooltip("Mark in at the playhead, or clear it (I)");
  transport.emplace<Button>("Mark Out", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::MarkOut);
  }).set_tooltip("Mark out at the playhead, or clear it (O)");
  // The frame at the playhead, written out as a PNG. Under the monitor rather
  // than in the project panel, which the spec suggests: that row is already
  // full, and `--check` said so by finding two controls cut in half by the edge
  // of it. This is also where the frame being saved is on show.
  transport.emplace<Button>("Snapshot", [app] {
    if (app != nullptr) take_snapshot(*app);
  }).set_tooltip("Write the frame at the playhead out as a PNG");

  // The sequence's size used to be a button here. It is a property of the
  // project, and it lives in Project Settings now — the monitor showing it was
  // a second way in from before there was a menu bar to hold the first.

  // A dropdown rather than a button that cycles. Cycling is fine for two
  // states; with three it hides two of them behind the one showing, so choosing
  // a quality means clicking until the right one comes round and there is no
  // way to see what the choices are without doing it.
  {
    std::vector<std::string> names;
    std::size_t chosen = 0;
    for (const auto& [scale, name] : kPreviewScales) {
      if (app != nullptr && scale == app->preview_scale) chosen = names.size();
      names.emplace_back(name);
    }
    auto& quality = transport.emplace<Dropdown>(std::move(names), chosen);
    quality.set_tooltip("How much of the picture to render while scrubbing");
    quality.set_on_change([app](std::size_t index) {
      if (app == nullptr || index >= kPreviewScales.size()) return;
      choose_preview_scale(*app, kPreviewScales[index].first);
    });
    if (app != nullptr) app->preview_scale_choice = &quality;
  }

  transport.emplace<Spacer>();
  auto& loop = transport.emplace<Button>("Loop", [app] {
    if (app != nullptr) toggle_looping(*app);
  });
  loop.set_tooltip("Play the marked span over and over");
  if (app != nullptr) {
    app->loop_button = &loop;
    loop.set_selected(app->looping);
  }

  auto& play = transport.emplace<Button>("Play", [app] {
    if (app != nullptr) toggle_playback(*app);
  });
  play.set_tooltip("Play or pause (Space)");
  if (app != nullptr) {
    app->play_button = &play;
    // The panel may have been rebuilt mid-playback — a rearrangement, a theme
    // change — and a button that says Play while the sound is running is worse
    // than one that does nothing.
    if (app->playing()) play.set_text("Pause");
  }
  transport.emplace<Button>("Export", [app] {
    if (app != nullptr) open_export_dialog(*app);
  }).set_tooltip("Render the sequence to a file");
  return panel;
}

/// The colours a clip can be labelled with, as a menu.
///
/// Its own popup rather than eight more rows on the clip menu, which is long
/// enough already. Ticked where the selection already carries one, and "None"
/// first because getting a label off is as ordinary as putting one on.
void open_label_menu(App& app, std::span<const std::string> clips, double x, double y) {
  if (app.main.host == nullptr || clips.empty()) return;

  // What they are now. A mixed selection ticks nothing, which is honest: there
  // is no one answer to what colour these are.
  std::string current;
  bool agreed = true;
  for (const std::string& id : clips) {
    const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), id);
    if (clip == nullptr) continue;
    if (current.empty() && agreed) {
      current = clip->label_color;
    } else if (clip->label_color != current) {
      agreed = false;
    }
  }

  std::vector<std::string> labels{"None"};
  std::vector<bool> ticks{agreed && current.empty()};
  std::vector<std::string> colors{std::string{}};
  for (const cutline::editor::ClipLabel& label : cutline::editor::clip_labels()) {
    labels.emplace_back(label.name);
    colors.emplace_back(label.color);
    ticks.push_back(agreed && current == label.color);
  }

  auto list = std::make_unique<MenuList>(std::move(labels));
  list->set_checked(std::move(ticks));
  list->set_on_choose([&app, colors, chosen = std::vector<std::string>(clips.begin(),
                                                                      clips.end())](
                          std::size_t index) {
    if (app.main.host != nullptr) app.main.host->close_popup();
    if (index >= colors.size()) return;
    app.session.apply(
        cutline::core::set_clips_label(app.session.project(), chosen, colors[index]));
    refresh_timeline(app);
    mark_dirty(app);
  });
  app.main.host->open_popup(std::move(list), Rect{x, y, 0.0, 0.0});
}

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

  const double fps = app.session.project().fps;
  const double span = cutline::core::source_span(*anchor);
  const double speed = cutline::core::clip_speed(*anchor);

  auto panel = std::make_unique<Panel>();
  panel->emplace<Label>("Speed / Duration").set_bold(true);

  auto& speed_row = panel->emplace<Box>(Axis::Horizontal);
  speed_row.emplace<Label>("Speed");
  auto& speed_field = speed_row.emplace<TextField>(std::format("{:.2f}", speed * 100.0));
  speed_field.set_columns(7);
  speed_row.emplace<Label>("%").set_small(true);

  auto& length_row = panel->emplace<Box>(Axis::Horizontal);
  length_row.emplace<Label>("Duration");
  auto& length_field = length_row.emplace<TextField>(
      cutline::core::seconds_to_timecode(cutline::core::clip_duration(*anchor), fps));
  length_field.set_columns(11);

  auto& reverse = panel->emplace<Checkbox>("Reverse speed", anchor->reverse);
  auto& ripple = panel->emplace<Checkbox>("Ripple edit, shifting trailing clips", false);

  // Each field rewrites the other when its edit ends. `span` is fixed by the
  // trim, so it is the constant the two are computed through.
  speed_field.set_on_finish([field = &speed_field, other = &length_field, span, fps] {
    double percent = 0.0;
    if (std::from_chars(field->text().data(), field->text().data() + field->text().size(),
                        percent)
            .ec != std::errc{} ||
        percent <= 0.0) {
      return;
    }
    const double rate = std::clamp(percent / 100.0, cutline::core::kMinSpeed,
                                   cutline::core::kMaxSpeed);
    other->set_text(cutline::core::seconds_to_timecode(span / rate, fps));
  });
  length_field.set_on_finish([field = &length_field, other = &speed_field, span, fps] {
    const std::optional<double> wanted = cutline::core::timecode_to_seconds(field->text(), fps);
    if (!wanted.has_value() || *wanted <= 0.0) return;
    const double rate =
        std::clamp(span / *wanted, cutline::core::kMinSpeed, cutline::core::kMaxSpeed);
    other->set_text(std::format("{:.2f}", rate * 100.0));
  });

  auto& buttons = panel->emplace<Box>(Axis::Horizontal);
  buttons.emplace<Button>(
      "OK", [&app, chosen = std::vector<std::string>(clips.begin(), clips.end()),
             field = &speed_field, rev = &reverse, rip = &ripple] {
        double percent = 0.0;
        if (std::from_chars(field->text().data(), field->text().data() + field->text().size(),
                            percent)
                .ec != std::errc{} ||
            percent <= 0.0) {
          // A typing mistake leaves the clips alone rather than retiming them
          // to nothing, which is what a zero would clamp to.
          if (app.main.host != nullptr) app.main.host->close_popup();
          return;
        }
        if (app.main.host != nullptr) app.main.host->close_popup();
        app.session.apply(cutline::core::set_clips_speed(app.session.project(), chosen,
                                                         percent / 100.0, rev->checked(),
                                                         rip->checked()));
        refresh_timeline(app);
        invalidate_preview(app);
        app.inspector_stale = true;
        mark_dirty(app);
      });
  buttons.emplace<Button>("Cancel", [&app] {
    if (app.main.host != nullptr) app.main.host->close_popup();
  });

  app.main.host->open_popup(std::move(panel), settings_anchor());
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
  const std::vector<cutline::core::Marker>& markers = app.session.project().markers;
  if (index >= markers.size()) return;
  const cutline::core::Marker marker = markers[index];
  const double fps = app.session.project().fps;

  auto panel = std::make_unique<Panel>();
  panel->emplace<Label>("Marker at " + cutline::core::seconds_to_timecode(marker.time, fps))
      .set_bold(true);

  auto& name_row = panel->emplace<Box>(Axis::Horizontal);
  name_row.emplace<Label>("Name");
  auto& name = name_row.emplace<TextField>(marker.label);
  name.set_columns(18);

  auto& note_row = panel->emplace<Box>(Axis::Horizontal);
  note_row.emplace<Label>("Comment");
  auto& comment = note_row.emplace<TextField>(marker.comment);
  comment.set_columns(28);

  auto& span_row = panel->emplace<Box>(Axis::Horizontal);
  span_row.emplace<Label>("Duration");
  // A timecode rather than seconds, like every other length in the interface.
  // Zero reads as a point marker, which is what it writes back as too.
  auto& span = span_row.emplace<TextField>(
      cutline::core::seconds_to_timecode(marker.duration, fps));
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

  auto& buttons = panel->emplace<Box>(Axis::Horizontal);
  buttons.emplace<Button>("OK", [&app, id = marker.id, fps, chosen, n = &name, c = &comment,
                                 d = &span] {
    // A duration that does not parse leaves the one it had rather than
    // becoming zero, which would silently turn a span back into a point.
    const std::optional<double> length = cutline::core::timecode_to_seconds(d->text(), fps);
    const cutline::core::Marker* was = nullptr;
    for (const cutline::core::Marker& m : app.session.project().markers) {
      if (m.id == id) was = &m;
    }
    const double duration = length.value_or(was != nullptr ? was->duration : 0.0);

    if (app.main.host != nullptr) app.main.host->close_popup();
    app.session.apply(cutline::core::set_marker(app.session.project(), id, n->text(),
                                                c->text(), *chosen, duration));
    refresh_timeline(app);
    mark_dirty(app);
  });
  buttons.emplace<Button>("Delete", [&app, id = marker.id] {
    if (app.main.host != nullptr) app.main.host->close_popup();
    app.session.apply(cutline::core::remove_marker(app.session.project(), id));
    refresh_timeline(app);
    mark_dirty(app);
  });
  buttons.emplace<Button>("Cancel", [&app] {
    if (app.main.host != nullptr) app.main.host->close_popup();
  });

  // Beside the marker rather than under the menu bar. A dialogue about a thing
  // on the ruler that opens at the far corner of the window makes you look away
  // from what you are editing and then find your way back — and the marker is
  // usually the very thing you want to see while typing what it means.
  const Rect tab = app.timeline->marker_rect(index);
  app.main.host->open_popup(std::move(panel),
                            tab.empty() ? settings_anchor() : Rect{tab.x, tab.bottom(), 0.0, 0.0});
}

/// Renaming a track: a popup with a field in it, hung under the header.
///
/// A popup rather than a field the timeline holds: the view draws its headers
/// and builds no widgets in them, and giving it one for this alone would mean
/// it owning focus, a caret and a commit rule it has no other use for.
void rename_track(App& app, std::size_t track) {
  if (app.timeline == nullptr || app.main.host == nullptr) return;
  const auto& rows = app.timeline->model().tracks;
  if (track >= rows.size()) return;

  const std::string id = rows[track].id;
  auto popup = std::make_unique<Panel>();
  auto& row = popup->emplace<Box>(Axis::Horizontal);
  auto& field = row.emplace<TextField>(rows[track].name);

  const auto commit = [&app, id, control = &field] {
    if (app.main.host != nullptr) app.main.host->close_popup();
    app.session.apply(
        cutline::core::set_track_label(app.session.project(), id, control->text()));
    refresh_timeline(app);
  };
  // Both, because a field is finished either by pressing the button or by
  // pressing return, and somebody who typed a name expects both to work.
  field.set_on_commit([commit](const std::string&) { commit(); });
  row.emplace<Button>("Rename", commit);

  app.main.host->open_popup(std::move(popup), app.timeline->header_rect(track));
  app.main.host->set_focus(&field);
}

/// A track's own menu, from a right-click on its header.
///
/// Premiere has one and this had none: a right-click on a header opened the
/// clip menu, which offered Cut and Copy over a place where there is no clip.
///
/// Every row that is a *state* is ticked rather than named for what it would
/// become, so the menu reads as what is true rather than as a list of verbs —
/// which is also the only way sync lock, the one switch with no letter in the
/// header, is reachable at all.
void open_track_menu(App& app, std::size_t track, double x, double y) {
  if (app.main.host == nullptr || app.timeline == nullptr) return;
  const std::vector<cutline::ui::TimelineTrack>& rows = app.timeline->model().tracks;
  if (track >= rows.size()) return;

  const std::string id = rows[track].id;
  const cutline::core::Track* held =
      std::ranges::find(app.session.project().tracks, id, &cutline::core::Track::id) !=
              app.session.project().tracks.end()
          ? &*std::ranges::find(app.session.project().tracks, id, &cutline::core::Track::id)
          : nullptr;
  if (held == nullptr) return;

  const bool audio = held->kind == cutline::core::TrackKind::Audio;

  /// One row: what it says, whether it is ticked, and what taking it does.
  struct Row {
    std::string label;
    bool ticked = false;
    std::function<void()> act;
  };

  const auto patched = [&app, id](const cutline::core::TrackPropsPatch& patch) {
    app.session.apply(cutline::core::update_track(app.session.project(), id, patch));
    refresh_timeline(app);
    invalidate_preview(app);
    stop_playback(app);
    mark_dirty(app);
  };

  std::vector<Row> rows_out;
  rows_out.push_back(Row{.label = "Rename...", .act = [&app, track] {
                           if (app.timeline != nullptr) rename_track(app, track);
                         }});
  rows_out.push_back(Row{.label = "Target",
                         .ticked = held->targeted,
                         .act = [patched, was = held->targeted] {
                           patched({.targeted = !was});
                         }});
  rows_out.push_back(Row{.label = "Sync Lock",
                         .ticked = held->sync_locked,
                         .act = [patched, was = held->sync_locked] {
                           patched({.sync_locked = !was});
                         }});
  rows_out.push_back(Row{.label = "Lock",
                         .ticked = held->locked,
                         .act = [patched, was = held->locked] { patched({.locked = !was}); }});

  if (audio) {
    rows_out.push_back(Row{.label = "Mute",
                           .ticked = held->muted,
                           .act = [patched, was = held->muted] { patched({.muted = !was}); }});
    rows_out.push_back(Row{.label = "Solo",
                           .ticked = held->solo,
                           .act = [patched, was = held->solo] { patched({.solo = !was}); }});
  } else {
    rows_out.push_back(Row{.label = "Hide",
                           .ticked = held->hidden,
                           .act = [patched, was = held->hidden] { patched({.hidden = !was}); }});
  }

  rows_out.push_back(Row{.label = "Reset Height", .act = [&app, id] {
                           app.session.apply(cutline::core::set_track_height(
                               app.session.project(), id, std::nullopt));
                           refresh_timeline(app);
                           mark_dirty(app);
                         }});
  rows_out.push_back(Row{.label = "Add Video Track", .act = [&app] {
                           run_command(app, cutline::editor::Command::AddVideoTrack);
                         }});
  rows_out.push_back(Row{.label = "Add Audio Track", .act = [&app] {
                           run_command(app, cutline::editor::Command::AddAudioTrack);
                         }});
  rows_out.push_back(Row{.label = "Delete Track", .act = [&app, id] {
                           app.session.apply(
                               cutline::core::remove_track(app.session.project(), id));
                           refresh_timeline(app);
                           invalidate_preview(app);
                           stop_playback(app);
                           mark_dirty(app);
                         }});

  std::vector<std::string> labels;
  std::vector<bool> ticks;
  std::vector<std::function<void()>> acts;
  for (Row& row : rows_out) {
    labels.push_back(std::move(row.label));
    ticks.push_back(row.ticked);
    acts.push_back(std::move(row.act));
  }

  auto list = std::make_unique<MenuList>(std::move(labels));
  list->set_checked(std::move(ticks));
  list->set_on_choose([&app, acts](std::size_t index) {
    if (app.main.host != nullptr) app.main.host->close_popup();
    if (index < acts.size() && acts[index]) acts[index]();
  });
  app.main.host->open_popup(std::move(list), Rect{x, y, 0.0, 0.0});
}

[[nodiscard]] std::unique_ptr<Widget> make_timeline_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  auto& tools = panel->emplace<Box>(Axis::Horizontal);
  // Through `run_command` rather than calling the session directly, so the
  // button and Ctrl+Z do the same thing. They did not: these two refreshed the
  // timeline and the pool and forgot the preview, so undoing a transform with
  // the button left the picture showing the edit that had just been undone,
  // while the same undo from the keyboard was fine. Anything with two ways to
  // reach it needs one place that does it.
  tools.emplace<Button>("Undo", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::Undo);
  }).set_tooltip("Undo (Ctrl+Z)");
  tools.emplace<Button>("Redo", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::Redo);
  }).set_tooltip("Redo (Ctrl+Y)");
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
    button.set_tooltip(entry.hint);
    if (app != nullptr) {
      button.set_selected(app->tool == entry.tool);
      app->tool_buttons.push_back(&button);
    }
  }

  // Linking, and the track operations. Both act through the command table
  // rather than calling the model here, so the button and the shortcut cannot
  // come to mean different things — and `can_run` is what greys them out, which
  // is the same answer a menu would ask for.
  auto& link = tools.emplace<Button>("Link", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::LinkClips);
  });
  link.set_tooltip("Link the selected clips (Ctrl+L)");
  auto& unlink = tools.emplace<Button>("Unlink", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::UnlinkClips);
  });
  unlink.set_tooltip("Unlink them (Ctrl+Shift+L)");
  if (app != nullptr) {
    app->link_button = &link;
    app->unlink_button = &unlink;
  }

  // A menu rather than three more buttons, for the reason the project panel's
  // New menu already found out: the row runs out of width before the commands
  // run out.
  auto& track_menu = tools.emplace<Button>("Track");
  track_menu.set_tooltip("Add or remove a track");
  track_menu.set_on_click([app, control = &track_menu] {
    if (app == nullptr || app->main.host == nullptr) return;

    auto list = std::make_unique<MenuList>(
        std::vector<std::string>{"Add Video Track", "Add Audio Track", "Remove Track"});
    list->set_on_choose([app](std::size_t index) {
      if (app->main.host != nullptr) app->main.host->close_popup();
      using cutline::editor::Command;
      switch (index) {
        case 0: run_command(*app, Command::AddVideoTrack); break;
        case 1: run_command(*app, Command::AddAudioTrack); break;
        case 2: run_command(*app, Command::RemoveTrack); break;
        default: break;
      }
    });
    app->main.host->open_popup(std::move(list), control->bounds());
  });

  // Snapping, and the zoom that fits the whole sequence. Both are properties of
  // the *view* rather than of the document, which is why neither goes through
  // the command table: undoing a zoom is not a thing anybody wants.
  auto& snap = tools.emplace<Button>("Snap", [app] {
    if (app != nullptr) toggle_snapping(*app);
  });
  snap.set_tooltip("Snap edges together (S)");
  if (app != nullptr) app->snap_button = &snap;

  tools.emplace<Button>("Fit", [app] {
    if (app == nullptr || app->timeline == nullptr) return;
    app->timeline->zoom_to_fit();
    mark_dirty(*app);
  }).set_tooltip("Zoom to the whole sequence (\\)");

  tools.emplace<Spacer>();
  // Eleven columns: "00:00:00:00" is what a timecode is and it is never any
  // other length, so the field asks for exactly that and leaves the rest of the
  // row to the buttons.
  auto& readout = tools.emplace<TextField>("00:00:00:00");
  readout.set_columns(11);
  readout.set_tooltip("Where the playhead is. Type a time to go there");
  readout.set_on_commit([app](const std::string& typed) {
    if (app == nullptr) return;
    const std::optional<double> at =
        cutline::core::timecode_to_seconds(typed, app->session.project().fps);
    // Nothing that parses leaves the playhead where it is rather than sending
    // it to zero, which is what a mistyped character would otherwise do.
    if (!at.has_value()) {
      show_playhead(*app);
      return;
    }
    go_to_time(*app, *at);
    // The keyboard goes back to the editor. A field that keeps it after Enter
    // swallows every single-letter shortcut there is — pressing N for the roll
    // tool types an N into the timecode instead, which is a confusing way to
    // find out where the focus went.
    if (app->main.host != nullptr) app->main.host->set_focus(nullptr);
  });
  // However the edit ends, the field goes back to saying where the playhead
  // actually is — including after an escape, and after a half-typed time the
  // keyboard simply left.
  readout.set_on_finish([app] {
    if (app != nullptr) show_playhead(*app);
  });
  if (app != nullptr) app->readout = &readout;

  auto& tracks = panel->emplace<TimelineView>();
  tracks.set_scale(TimeScale{.pixels_per_second = 60.0});
  if (app != nullptr) {
    app->timeline = &tracks;
    // The panel is new; the tool and the snap setting are not. A rearrangement
    // or a theme change must not quietly put the selection tool back, or turn
    // snapping on again after somebody deliberately turned it off.
    tracks.set_tool(app->tool);
    show_snapping(*app);
  }

  tracks.set_on_scrub([app](double at) {
    if (app == nullptr) return;
    app->session.set_playhead(at);
    show_playhead(*app);
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

  tracks.set_on_select([app](std::span<const cutline::ui::BlockRef> refs) {
    if (app == nullptr || app->timeline == nullptr) return;
    if (refs.empty()) {
      app->session.clear_selection();
      app->inspector_stale = true;
      refresh_timeline(*app);
      return;
    }

    // Each block, expanded to the whole linked group it belongs to. Clicking a
    // video clip and seeing its audio stay unhighlighted says the two are
    // separate when the point of linking them is that they are not — and every
    // edit was already going to reach both, so showing one selected was the
    // interface disagreeing with what it was about to do.
    std::vector<std::string> ids;
    for (const cutline::ui::BlockRef& ref : refs) {
      const auto id = cutline::editor::block_clip_id(app->timeline->model(), ref);
      if (!id.has_value()) continue;
      for (std::string& member :
           cutline::core::group_members(app->session.project(), *id)) {
        // A sweep across a linked pair reaches both blocks and each names the
        // whole group, so without this the same clip arrives twice.
        if (std::ranges::find(ids, member) == ids.end()) ids.push_back(std::move(member));
      }
    }

    app->session.select(std::move(ids));
    // The view highlighted what was swept; the session may say more, because a
    // group reaches clips the rectangle never touched. Rebuilt rather than
    // patched, for the usual reason: which blocks are in a group is the model's
    // answer, not the view's.
    refresh_timeline(*app);
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

  // The right-click. Premiere's clip menu, which is where most of what can be
  // done to a clip is actually reached from — the toolbar holds the handful of
  // things that are worth a permanent button and nothing else fits there.
  //
  // Nothing is offered that would do nothing: every entry answers `can_run`
  // first, so the menu over a clip and the menu over empty track differ by
  // themselves rather than by a switch here deciding which menu to build. The
  // one exception is Enable, which is a state rather than an action and so is
  // ticked rather than hidden.
  tracks.set_on_context_menu([app](double x, double y) {
    if (app == nullptr || app->main.host == nullptr || app->timeline == nullptr) return;
    using cutline::editor::Command;

    // A right-click on a header is about the *track*, not about a clip. It used
    // to open the clip menu, which offered Cut and Copy over a place where
    // there is no clip to cut.
    if (const auto header = app->timeline->header_at(x, y); header.has_value()) {
      open_track_menu(*app, *header, x, y);
      return;
    }

    struct Entry {
      const char* label;
      Command command;
    };
    constexpr std::array kEntries{
        Entry{"Cut", Command::Cut},
        Entry{"Copy", Command::Copy},
        Entry{"Paste", Command::Paste},
        Entry{"Paste Insert", Command::PasteInsert},
        Entry{"Clear", Command::Delete},
        Entry{"Ripple Delete", Command::RippleDelete},
        Entry{"Split at Playhead", Command::Split},
        Entry{"Link", Command::LinkClips},
        Entry{"Unlink", Command::UnlinkClips},
        Entry{"Add Video Track", Command::AddVideoTrack},
        Entry{"Add Audio Track", Command::AddAudioTrack},
        Entry{"Remove Track", Command::RemoveTrack},
    };

    std::vector<std::string> labels;
    std::vector<Command> commands;
    for (const Entry& entry : kEntries) {
      if (!cutline::editor::can_run(app->session, entry.command)) continue;
      labels.emplace_back(entry.label);
      commands.push_back(entry.command);
    }

    // The framing pair and Enable, which are about the clips rather than about
    // the sequence and so are not commands. Offered only when something is
    // selected, since all three act on it.
    const std::vector<std::string> selected = app->session.selected_group();
    std::vector<bool> ticks(labels.size(), false);
    if (!selected.empty()) {
      labels.emplace_back("Label...");
      ticks.push_back(false);
      labels.emplace_back("Speed / Duration...");
      ticks.push_back(false);
      // Ticked, because a hold is a state a clip is in rather than something
      // to do to it — and the row that turns it off has to be the same row.
      labels.emplace_back("Frame Hold");
      ticks.push_back(std::ranges::any_of(selected, [&](const std::string& id) {
        const cutline::core::Clip* clip = cutline::core::find_clip(app->session.project(), id);
        return clip != nullptr && clip->hold.has_value();
      }));
      labels.emplace_back("Fit to Frame");
      ticks.push_back(false);
      labels.emplace_back("Fill Frame");
      ticks.push_back(false);
    }
    if (!selected.empty()) {
      const bool enabled = std::ranges::all_of(selected, [&](const std::string& id) {
        const cutline::core::Clip* clip = cutline::core::find_clip(app->session.project(), id);
        return clip != nullptr && !clip->disabled;
      });
      labels.emplace_back("Enable");
      ticks.push_back(enabled);
    }
    if (labels.empty()) return;

    // Last, and past the end of the commands, which is how it is recognised
    // without a sentinel command standing in for something that is not one.
    auto list = std::make_unique<MenuList>(std::move(labels));
    // Only when there is a tick to draw, so every other menu keeps its own
    // indent rather than gaining a gutter it never uses.
    if (!selected.empty()) list->set_checked(std::move(ticks));

    list->set_on_choose([app, commands, selected, x, y](std::size_t index) {
      if (app->main.host != nullptr) app->main.host->close_popup();
      if (index < commands.size()) {
        run_command(*app, commands[index]);
        return;
      }
      if (selected.empty()) return;

      // The trailing rows, in the order they were added: the label menu, the
      // speed box, the frame hold, the two framing rows, then Enable.
      if (index == commands.size()) {
        open_label_menu(*app, selected, x, y);
        return;
      }
      if (index == commands.size() + 1) {
        open_speed_dialog(*app, selected);
        return;
      }
      if (index == commands.size() + 2) {
        // At the playhead when it is over the clip, which is Premiere's default
        // and the only choice that means anything: the frame you are looking at
        // is the one you want held. Each clip clamps it into its own trim, so a
        // selection spread across the sequence still freezes on a frame it owns.
        const bool held = std::ranges::any_of(selected, [&](const std::string& id) {
          const cutline::core::Clip* clip =
              cutline::core::find_clip(app->session.project(), id);
          return clip != nullptr && clip->hold.has_value();
        });
        app->session.apply(cutline::core::set_clips_hold(
            app->session.project(), selected,
            held ? std::nullopt : std::optional<double>{app->session.playhead()}));
        refresh_timeline(*app);
        invalidate_preview(*app);
        app->inspector_stale = true;
        mark_dirty(*app);
        return;
      }
      if (index == commands.size() + 3 || index == commands.size() + 4) {
        const auto fit = index == commands.size() + 3 ? cutline::editor::FrameFit::Fit
                                                      : cutline::editor::FrameFit::Fill;
        app->session.apply(cutline::editor::scale_to_frame(
            app->session.project(), selected, fit, app->session.playhead()));
        refresh_timeline(*app);
        invalidate_preview(*app);
        app->inspector_stale = true;
        mark_dirty(*app);
        return;
      }
      {
        const bool enabled = std::ranges::all_of(selected, [&](const std::string& id) {
          const cutline::core::Clip* clip = cutline::core::find_clip(app->session.project(), id);
          return clip != nullptr && !clip->disabled;
        });
        app->session.apply(
            cutline::core::set_clips_enabled(app->session.project(), selected, !enabled));
        refresh_timeline(*app);
        invalidate_preview(*app);
        app->inspector_stale = true;
        stop_playback(*app);
        mark_dirty(*app);
      }
    });

    // At the pointer: there is no widget under a right-click to hang a menu
    // from, only a place.
    app->main.host->open_popup(std::move(list), Rect{x, y, 0.0, 0.0});
  });

  // A lane dragged taller or shorter. The view has already resized itself so
  // the drag could be aimed; this writes it down, once, at the end.
  tracks.set_on_track_resize([app](std::size_t track, std::optional<double> height) {
    if (app == nullptr || app->timeline == nullptr) return;
    const auto& rows = app->timeline->model().tracks;
    if (track >= rows.size()) return;

    app->session.apply(
        cutline::core::set_track_height(app->session.project(), rows[track].id, height));
    // Not rebuilt: the view is already showing the height that was dragged to,
    // and rebuilding would take the same numbers back out of the project it
    // has just put them into.
    mark_dirty(*app);
  });

  // Renaming, from a double-click on a header. A popup with a field in it,
  // rather than a field the timeline holds: the view draws its headers and
  // builds no widgets in them, and giving it one for this alone would mean it
  // owning focus, a caret and a commit rule it has no other use for.
  tracks.set_on_marker_activate([app](std::size_t marker) {
    if (app != nullptr) open_marker_dialog(*app, marker);
  });

  tracks.set_on_track_rename([app](std::size_t track) {
    if (app != nullptr) rename_track(*app, track);
  });

  tracks.set_on_edit([app](const cutline::ui::TimelineEdit& edit) {
    if (app == nullptr || app->timeline == nullptr) return;
    const auto id = cutline::editor::block_clip_id(app->timeline->model(), edit.block);
    if (!id.has_value()) return;

    // The selection goes with it, so a move carries every clip that is
    // highlighted rather than only the one under the pointer. `selected_group`
    // rather than the raw selection, so linked partners travel too.
    const std::vector<std::string> carried = app->session.selected_group();
    std::vector<std::string> made;
    app->session.apply(cutline::editor::apply_timeline_edit(app->session.project(), *id, edit,
                                                            carried, &made));
    // An alt-drag leaves the originals where they were and the copies under the
    // pointer. Selecting the copies is what makes the nudge or the second drag
    // that usually follows act on what was just put down rather than on what it
    // came from.
    if (!made.empty()) app->session.select(made);
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

  if (app != nullptr) {
    refresh_timeline(*app);
  } else {
    // The headless check builds this with no session behind it, and an empty
    // timeline paints no clips at all — so a block too tall for its track in
    // one theme, or a label running past a block's edge, would never be seen.
    // The same reasoning as the pool's fallback above.
    tracks.set_model(cutline::editor::timeline_model(check_project(), {}, {}));
  }
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

/// The clip under a point, if dropping `id` there would do anything.
///
/// Works in the *main window's* coordinates. A drag that began in the library
/// keeps the pointer captured, so the moves arrive at the browser however far
/// away the cursor is — and the timeline it has to hit is a different panel
/// entirely. Nothing below this knows about both, which is why the answer is
/// worked out here.
///
/// Nothing when the pointer is not over a clip, or over one the entry could not
/// be applied to: a video effect over a waveform is a drop that has to decline
/// rather than land somewhere approximate.
/// Whether an entry could be applied to a clip, presets included.
///
/// `library_entry_fits` answers for the catalogue and has never seen a preset,
/// so this asks the saved set for those — and a preset carrying nothing the
/// clip could take is refused, exactly as an audio effect over a picture is.
[[nodiscard]] bool drop_fits(const App& app, const std::string& clip_id,
                             const std::string& id) {
  const std::string_view name = cutline::editor::preset_name_of(id);
  if (name.empty()) {
    return cutline::editor::library_entry_fits(app.session.project(), clip_id, id);
  }

  const cutline::editor::EffectPreset* saved =
      cutline::editor::find_preset(app.presets, name);
  if (saved == nullptr) return false;
  return cutline::editor::apply_preset(app.session.project(), clip_id, *saved) !=
         app.session.project();
}

/// What a drop on the program monitor would land on: whatever the monitor is
/// showing at the playhead.
///
/// Premiere's rule, and it is the one that makes the gesture worth having —
/// what you are looking at is what you are dropping onto, and there is nothing
/// to aim at because the picture is the target. The topmost video clip under
/// the playhead, because that is the one on screen.
///
/// Nothing when the pointer is not over the monitor, when nothing is under the
/// playhead, or when the entry could not be applied to what is: a transition
/// dropped on the picture has no join to sit on and declines rather than
/// landing somewhere approximate.
[[nodiscard]] std::optional<std::string> monitor_drop_target(const App& app,
                                                             const std::string& id, double x,
                                                             double y) {
  if (app.monitor == nullptr || id.empty()) return std::nullopt;
  if (!app.monitor->bounds().contains(x, y)) return std::nullopt;

  const cutline::core::Project& project = app.session.project();
  const double t = app.session.playhead();

  std::optional<std::string> found;
  for (const cutline::core::Track& track : project.tracks) {
    if (track.kind != cutline::core::TrackKind::Video || track.hidden) continue;
    for (const cutline::core::Clip& clip : track.clips) {
      if (clip.disabled) continue;
      const double end = clip.start + cutline::core::clip_duration(clip);
      if (t < clip.start || t >= end) continue;
      // Later tracks are drawn over earlier ones, so the last match is the one
      // on screen.
      found = clip.id;
    }
  }

  if (!found.has_value()) return std::nullopt;
  if (!drop_fits(app, *found, id)) return std::nullopt;
  return found;
}

[[nodiscard]] std::optional<std::string> library_drop_target(const App& app,
                                                             const std::string& id, double x,
                                                             double y) {
  // The monitor first: it sits above the timeline and a drag crossing one on
  // the way to the other should land on whichever it is actually over.
  if (const std::optional<std::string> shown = monitor_drop_target(app, id, x, y);
      shown.has_value()) {
    return shown;
  }

  if (app.timeline == nullptr || id.empty()) return std::nullopt;

  const std::optional<cutline::ui::BlockRef> block = app.timeline->block_at(x, y);
  if (!block.has_value()) return std::nullopt;

  const cutline::ui::TimelineModel& model = app.timeline->model();
  if (block->track >= model.tracks.size()) return std::nullopt;
  const auto& blocks = model.tracks[block->track].blocks;
  if (block->block >= blocks.size()) return std::nullopt;

  const std::string& clip_id = blocks[block->block].id;
  if (!drop_fits(app, clip_id, id)) return std::nullopt;
  return clip_id;
}

/// The bin a point in the library would drop into, declared here because the
/// outline below has to know and is written above it.
[[nodiscard]] std::string library_bin_at(const App& app, double x, double y);

/// Outlines the clip a drop would land on, or clears the outline.
///
/// An empty `id` means the drag is over, which is how a drop that lands on
/// nothing still tidies up after itself.
void show_library_drop(App& app, const std::string& id, double x, double y) {
  if (app.timeline == nullptr) return;

  // Back inside the library, over a bin: the drop gathers rather than applies,
  // so the bin is outlined and nothing on the timeline is.
  const std::string bin = id.empty() ? std::string{} : library_bin_at(app, x, y);
  if (app.library != nullptr) {
    app.library->set_drop_folder(bin.empty() ? std::string{}
                                             : cutline::editor::bin_folder(bin));
  }
  if (!bin.empty()) {
    app.timeline->set_drop_target(std::nullopt);
    if (app.monitor != nullptr) app.monitor->set_drop_lit(false);
    return;
  }

  // Over the monitor, the whole picture is the target, so the timeline's
  // outline comes off and the monitor lights instead. Two outlines at once
  // would say the drop was going to two places.
  const bool over_monitor = !id.empty() && monitor_drop_target(app, id, x, y).has_value();
  if (app.monitor != nullptr) app.monitor->set_drop_lit(over_monitor);
  if (over_monitor) {
    app.timeline->set_drop_target(std::nullopt);
    return;
  }

  if (id.empty()) {
    app.timeline->set_drop_target(std::nullopt);
    return;
  }

  // Through `library_drop_target` rather than through `block_at` directly, so
  // the outline appears on exactly the clips a drop would be accepted by. An
  // outline on a clip that then refused the drop would be a lie.
  const std::optional<std::string> clip_id = library_drop_target(app, id, x, y);
  if (!clip_id.has_value()) {
    app.timeline->set_drop_target(std::nullopt);
    return;
  }
  app.timeline->set_drop_target(app.timeline->block_at(x, y));
}

/// The bin a point in the library would drop into, or empty.
///
/// A point over a bin's heading or over anything already gathered in it counts,
/// which is what makes the whole folder a target rather than one row of it.
[[nodiscard]] std::string library_bin_at(const App& app, double x, double y) {
  if (app.library == nullptr || !app.library->bounds().contains(x, y)) return {};
  return std::string(cutline::editor::bin_of_folder(app.library->folder_at(y)));
}

/// A name nobody has used yet, for a bin made without one typed.
///
/// Premiere makes "Custom Bin 01" and lets you rename it; the same idea, because
/// a button that refuses to do anything until a field is filled is a button that
/// looks broken.
[[nodiscard]] std::string unused_bin_name(const App& app) {
  for (int n = 1;; ++n) {
    std::string name = "Bin " + std::to_string(n);
    if (cutline::editor::find_bin(app.bins, name) == nullptr) return name;
  }
}

/// Makes a bin, opens it, and leaves the name field empty for the next one.
void new_bin(App& app) {
  const std::string name = app.bin_name.empty() ? unused_bin_name(app) : app.bin_name;
  if (!cutline::editor::create_bin(app.bins, name)) {
    complain(app.main.window, "There is already a bin called " + name + ".");
    return;
  }

  app.bin_name.clear();
  if (app.library != nullptr) {
    // Opened, and its parent with it. A folder made where nothing can see it is
    // one somebody makes twice.
    app.library->set_open(std::string(cutline::editor::kBinFolder), true);
    app.library->set_open(cutline::editor::bin_folder(name), true);
  }
  keep_bins(app);
  if (app.library_name != nullptr) app.library_name->set_text({});
}

/// The menu a right-click in the library opens.
///
/// What it offers depends on what is under the pointer, which is the whole
/// reason it is a menu rather than a row of buttons: a bin's operations only
/// mean anything while a bin is in front of you.
void open_library_menu(App& app, double x, double y) {
  if (app.main.host == nullptr || app.library == nullptr) return;

  const std::string folder = app.library->folder_at(y);
  const std::string bin(cutline::editor::bin_of_folder(folder));
  const std::string id = app.library->selected();
  // An entry only counts as "in this bin" when the pointer is on it rather than
  // on the heading, or removing would take whatever happened to be selected.
  const std::size_t row = app.library->row_at(y);
  const bool on_entry = !bin.empty() && row < app.library->rows().size() &&
                        !app.library->rows()[row].id.empty();

  struct Item {
    std::string label;
    std::function<void()> act;
  };
  std::vector<Item> items;

  items.push_back(Item{"New Bin", [&app] { new_bin(app); }});
  if (!bin.empty()) {
    if (!app.bin_name.empty()) {
      // Renamed to what is typed in the field. There is no modal text prompt in
      // this application, and a bin name is not the place to introduce one.
      items.push_back(Item{"Rename Bin to \"" + app.bin_name + "\"", [&app, bin] {
                             if (!cutline::editor::rename_bin(app.bins, bin, app.bin_name)) {
                               complain(app.main.window,
                                        "There is already a bin called " + app.bin_name + ".");
                               return;
                             }
                             // The tree remembers what is open by path, so a
                             // renamed bin would close itself — carry it over,
                             // or renaming one you are working in hides it.
                             if (app.library != nullptr) {
                               const std::string was = cutline::editor::bin_folder(bin);
                               const bool open = app.library->is_open(was);
                               app.library->set_open(was, false);
                               app.library->set_open(
                                   cutline::editor::bin_folder(app.bin_name), open);
                             }
                             app.bin_name.clear();
                             if (app.library_name != nullptr) app.library_name->set_text({});
                             keep_bins(app);
                           }});
    }
    // Quoted, because "Delete Bin Bin 1" is what naming it without them reads
    // as the first time somebody makes a bin and does not name it.
    items.push_back(Item{"Delete Bin \"" + bin + "\"", [&app, bin] {
                           cutline::editor::remove_bin(app.bins, bin);
                           keep_bins(app);
                         }});
    if (on_entry) {
      items.push_back(Item{"Remove from Bin", [&app, bin, id] {
                             cutline::editor::remove_from_bin(app.bins, bin, id);
                             keep_bins(app);
                           }});
    }
  }

  std::vector<std::string> labels;
  labels.reserve(items.size());
  for (const Item& item : items) labels.push_back(item.label);

  auto list = std::make_unique<MenuList>(std::move(labels));
  list->set_on_choose([&app, acts = std::move(items)](std::size_t index) {
    if (app.main.host != nullptr) app.main.host->close_popup();
    if (index < acts.size()) acts[index].act();
  });
  app.main.host->open_popup(std::move(list), Rect{x, y, 0.0, 0.0});
}

/// The effects library: everything that can be applied, searchable.
///
/// Premiere has a panel for this, and it is the right shape. A menu was the
/// right answer while there were eleven effects and no panel machinery; it
/// stopped being the right answer as soon as transitions wanted to live beside
/// them, and it was already why the video and audio stacks needed separate
/// buttons opening separate menus.
[[nodiscard]] std::unique_ptr<Widget> make_library_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  // The search field above the tree, which is where every search field in every
  // application of this kind is.
  auto& search = panel->emplace<TextField>();
  search.set_placeholder("Search effects");

  auto& browser = panel->emplace<cutline::ui::EffectsBrowser>();

  browser.set_items([app] {
    if (app != nullptr) return library_entries(*app);

    std::vector<cutline::ui::EffectEntry> entries;
    for (const cutline::editor::LibraryEntry& entry : cutline::editor::effect_library()) {
      entries.push_back(cutline::ui::EffectEntry{
          .id = entry.id, .name = entry.name, .folder = entry.folder});
    }
    return entries;
  }());

  // As you type rather than on Enter. A library is read by narrowing it, and
  // having to commit each guess makes finding something a slower job than
  // scrolling would have been.
  search.set_on_change([control = &browser](const std::string& text) {
    control->set_filter(text);
  });

  // The bin bar along the bottom, which is where Premiere keeps its New Custom
  // Bin button. The field names the bin; empty, one is named for you.
  auto& bins = panel->emplace<Box>(Axis::Horizontal);
  auto& bin_name = bins.emplace<TextField>();
  bin_name.set_placeholder("Bin name");
  auto& new_bin_button = bins.emplace<Button>("New Bin", [app] {
    if (app != nullptr) new_bin(*app);
  });
  if (app == nullptr) new_bin_button.set_enabled(false);

  if (app != nullptr) {
    app->library = &browser;
    app->library_name = &bin_name;
    refresh_library(*app);

    // Only as it is typed. Committing would also fire when the keyboard left
    // the field, and a bin made by clicking away from a half-typed name is a
    // bin nobody asked for — the button is the gesture.
    bin_name.set_on_change([app](const std::string& text) { app->bin_name = text; });

    browser.set_on_context_menu([app](double x, double y) { open_library_menu(*app, x, y); });

    // Every clip named, against one project, applied once.
    //
    // Applying per clip would put one entry in the undo stack for each, so
    // undoing "blur these six" would be six presses. And each one is still
    // refused on its own terms: an audio effect on a picture, or a transition
    // where nothing abuts the clip, declines rather than appearing to work — so
    // a mixed selection takes the effect on the clips it suits and leaves the
    // rest alone, which is what Premiere does too.
    const auto apply_to = [app](std::span<const std::string> clip_ids, const std::string& id) {
      // A preset is applied from the saved set rather than by the binding: the
      // binding knows what an effect *type* means and has never seen a preset,
      // which is what keeps a settings file out of a pure layer.
      const std::string_view preset = cutline::editor::preset_name_of(id);
      const cutline::editor::EffectPreset* saved =
          preset.empty() ? nullptr : cutline::editor::find_preset(app->presets, preset);

      cutline::core::Project next = app->session.project();
      for (const std::string& clip_id : clip_ids) {
        next = saved != nullptr
                   ? cutline::editor::apply_preset(std::move(next), clip_id, *saved)
                   : cutline::editor::apply_library_entry(std::move(next), clip_id, id);
      }
      app->session.apply(std::move(next));
      refresh_timeline(*app);
      invalidate_preview(*app);
      app->inspector_stale = true;
    };

    browser.set_on_choose([app, apply_to](const std::string& id) {
      // The whole selection, not the first of it. Selecting six clips and
      // double-clicking an effect meant five of them were quietly skipped.
      const auto selection = app->session.selection();
      if (selection.empty()) return;
      const std::vector<std::string> clip_ids(selection.begin(), selection.end());
      apply_to(clip_ids, id);
    });

    // Dragged onto a clip, which is how Premiere applies one and the only way
    // that does not need the right thing selected first.
    browser.set_on_drag([app](const std::string& id, double x, double y) {
      show_library_drop(*app, id, x, y);
    });
    browser.set_on_drop([app, apply_to](const std::string& id, double x, double y) {
      // Into a bin, when the drop landed back inside the library. Gathering
      // rather than applying: the same drag, and where it ends says which.
      const std::string bin = library_bin_at(*app, x, y);
      show_library_drop(*app, {}, 0.0, 0.0);
      if (!bin.empty()) {
        if (cutline::editor::add_to_bin(app->bins, bin, id)) {
          // Opened, so what has just been gathered can be seen. Dropping into a
          // shut folder and being shown nothing looks exactly like a drop that
          // was refused.
          app->library->set_open(cutline::editor::bin_folder(bin), true);
          keep_bins(*app);
        }
        return;
      }

      const std::optional<std::string> clip_id = library_drop_target(*app, id, x, y);
      if (!clip_id.has_value()) return;

      // Dropped onto one of several selected clips, it takes all of them —
      // Premiere's rule, and the one that makes dragging onto a selection worth
      // doing. Onto a clip outside the selection it takes only that clip, which
      // is what aiming at it meant.
      const auto selection = app->session.selection();
      const bool inside = std::ranges::find(selection, *clip_id) != selection.end();
      if (inside) {
        apply_to(std::vector<std::string>(selection.begin(), selection.end()), id);
      } else {
        apply_to(std::vector{*clip_id}, id);
      }
    });
  }
  return panel;
}

/// The master fader and the meter it is set against.
///
/// The two belong together: a fader is moved by ear and by eye, and the eye
/// part is the meter. Splitting them across panels would mean setting a level
/// while watching something that might be behind another tab.
/// One audio track's strip in the mixer: a fader and a panner.
///
/// A mix has two levels for a reason. A clip's gain is what that take needed;
/// a track's is what the whole stem needs against the others, and riding one to
/// fix the other is how a mix stops being reversible.
///
/// Numbers rather than a vertical fader per track. The panel is as likely to be
/// a narrow column as a whole region, and a row of strips needs a width it
/// cannot count on — the master fader learned that the hard way and is stacked
/// for the same reason.
void build_track_strip(App& app, Box& column, const cutline::core::Track& track,
                       std::size_t index) {
  const std::string track_id = track.id;

  // No mute or solo here. They are on the track head in the timeline, which is
  // where Premiere keeps them too and where they are while you are editing —
  // a second pair somewhere else is two controls for one flag.
  // The same name the timeline shows. The raw id is an internal string and
  // reading "a1" in one panel and "A1" in another is two names for one track.
  column.emplace<Label>(cutline::editor::default_track_label(app.session.project(), index))
      .set_bold(true);

  auto& fader = column.emplace<Box>(Axis::Horizontal);
  fader.emplace<Label>("Volume").set_small(true);
  fader.emplace<Spacer>();

  auto& volume = fader.emplace<cutline::ui::NumericField>(
      cutline::ui::ValueRange{.minimum = cutline::ui::kGainFloorDb,
                              .maximum = cutline::ui::gain_to_fader_db(
                                  cutline::core::kMaxGain)},
      cutline::ui::gain_to_fader_db(track.gain));
  volume.set_decimals(1);
  volume.set_suffix("dB");
  volume.set_default_value(0.0);
  volume.set_on_commit([&app, track_id](double db) {
    app.session.apply(cutline::core::set_track_gain(
        app.session.project(), track_id,
        cutline::ui::fader_db_to_gain(db, cutline::core::kMaxGain)));
    app.inspector_stale = true;
  });

  auto& panner = column.emplace<Box>(Axis::Horizontal);
  panner.emplace<Label>("Balance").set_small(true);
  panner.emplace<Spacer>();

  auto& pan = panner.emplace<cutline::ui::NumericField>(
      cutline::ui::ValueRange{.minimum = -100.0, .maximum = 100.0}, track.pan * 100.0);
  pan.set_decimals(1);
  pan.set_default_value(0.0);
  pan.set_on_commit([&app, track_id](double value) {
    app.session.apply(
        cutline::core::set_track_pan(app.session.project(), track_id, value / 100.0));
    app.inspector_stale = true;
  });
}

[[nodiscard]] std::unique_ptr<Widget> make_audio_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  // Stacked rather than side by side. The panel is as likely to be docked in a
  // narrow column as given a whole region, and a fader beside a meter needs a
  // width neither of them has on their own — the first attempt pushed the
  // fader clean off the edge of the panel it was in.
  auto& column = panel->emplace<Box>(Axis::Vertical);
  column.emplace<Label>("Master").set_bold(true);

  const double gain = app == nullptr ? 1.0 : app->session.project().master_gain;
  auto& fader = column.emplace<Slider>(
      ValueRange{.minimum = cutline::ui::kGainFloorDb,
                 .maximum = cutline::ui::gain_to_fader_db(cutline::core::kMaxMasterGain)},
      cutline::ui::gain_to_fader_db(gain));
  // Unity, which is where a volume control resets to and where a project that
  // has never been touched sits.
  fader.set_default_value(0.0);

  auto& reading = column.emplace<Label>();
  reading.set_small(true);

  auto& bars = column.emplace<cutline::ui::MeterView>();
  if (app != nullptr) app->meter = &bars;

  // A strip per audio track, under the master. Premiere calls this the audio
  // mixer and puts the master at the right-hand end of it; stacked, the master
  // going first is what the same arrangement reads as.
  if (app != nullptr) {
    const std::vector<cutline::core::Track>& tracks = app->session.project().tracks;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      if (tracks[i].kind != cutline::core::TrackKind::Audio) continue;
      build_track_strip(*app, column, tracks[i], i);
    }
  }

  if (app != nullptr) {
    app->master_fader = &fader;
    app->master_reading = &reading;
    show_master_gain(*app, gain);

    // On every pixel of the drag, straight to the mixer: a master fader that
    // only took effect on release could not be set by ear, which is the only
    // way anybody sets one. The model is left alone until the button comes up,
    // so one gesture is still one undo entry.
    fader.set_on_change([app](double db) {
      const double moved = cutline::ui::fader_db_to_gain(db, cutline::core::kMaxMasterGain);
      if (app->player != nullptr) app->player->set_master_gain(moved);
      show_master_gain(*app, moved);
    });
    fader.set_on_commit([app](double db) {
      app->session.apply(cutline::core::set_master_gain(
          app->session.project(),
          cutline::ui::fader_db_to_gain(db, cutline::core::kMaxMasterGain)));
      show_master_gain(*app, app->session.project().master_gain);
    });
  }

  return panel;
}

/// The scopes, with a row of tabs to choose between them.
[[nodiscard]] std::unique_ptr<Widget> make_scopes_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  auto& tabs = panel->emplace<Box>(Axis::Horizontal);
  auto& view = panel->emplace<cutline::ui::ScopesView>();
  if (app != nullptr) {
    app->scopes = &view;
    view.set_kind(app->scope_kind);
    // Whatever was last measured. The panel may be rebuilt by a rearrangement
    // or a theme change long after the frame it is showing was rendered, and a
    // scope that went blank because a tab moved would be a puzzle.
    view.set_readings(app->scope_readings);
  }

  // One button that cycles rather than four tabs. Four names as long as
  // "Vectorscope" do not fit a panel docked in a narrow column — the first
  // attempt lost two of them off the edge — and this is the idiom the sort
  // button already established for a short list in a tight row.
  auto& choice = tabs.emplace<Button>(
      std::string(cutline::ui::to_string(app == nullptr ? cutline::ui::ScopeKind::Histogram
                                                        : app->scope_kind)),
      [app] {
        if (app == nullptr) return;
        constexpr std::array kOrder{
            cutline::ui::ScopeKind::Histogram, cutline::ui::ScopeKind::Waveform,
            cutline::ui::ScopeKind::Parade, cutline::ui::ScopeKind::Vectorscope};
        const auto at = std::ranges::find(kOrder, app->scope_kind);
        const std::size_t next =
            at == kOrder.end() ? 0 : (static_cast<std::size_t>(at - kOrder.begin()) + 1) %
                                         kOrder.size();
        choose_scope(*app, kOrder[next]);
      });
  tabs.emplace<Spacer>();
  if (app != nullptr) app->scope_button = &choice;

  if (app != nullptr) invalidate_scopes(*app);
  return panel;
}

/// The source monitor: one source, its own playhead, and the marks that decide
/// which part of it gets placed.
[[nodiscard]] std::unique_ptr<Widget> make_source_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  // The name of what is showing, and the way to the others. One control rather
  // than a label and a list, which is what Premiere does and for the reason
  // that the name *is* the thing you press to change it.
  auto& choice = panel->emplace<Dropdown>(std::vector<std::string>{"No source"});
  if (app != nullptr) {
    app->source_choice = &choice;
    choice.set_on_change([app](std::size_t index) {
      if (index >= app->source_recent.size()) return;
      show_source(*app, app->source_recent[index]);
    });
  }

  auto& picture = panel->emplace<MonitorView>();
  picture.set_placeholder("Select something in the project panel.");
  if (app != nullptr) {
    app->source_monitor = &picture;
    // Premiere's shortest statement of "use this": what is in the monitor is
    // what is about to be placed, so drag it where it goes. The marked span
    // comes with it, exactly as it does from the pool.
    picture.set_on_drag_out([app](double x, double y) {
      if (app->timeline == nullptr) return;
      app->timeline->set_drop_ghost(std::nullopt);
      const auto where = app->timeline->drop_at(x, y);
      if (!where.has_value()) return;  // released somewhere that means nothing
      place_media_from(*app, app->session.source_media(), where);
    });
  }

  auto& envelope = panel->emplace<WaveformView>();
  envelope.set_visible(false);
  if (app != nullptr) app->source_waveform = &envelope;

  auto& bar = panel->emplace<ScrubBar>();
  // So a click on it takes the keyboard, which is what makes the transport and
  // the marking keys land here rather than on the sequence.
  bar.set_focusable(true);
  if (app != nullptr) {
    app->source_panel = panel.get();
    app->source_scrub = &bar;
    // The bar does not move its own playhead. This decides where the playhead
    // really is — snapped to the source's own frame grid — and sets it back, so
    // the picture and the mark that follows it agree on which frame is meant.
    bar.on_scrub = [app](double time) {
      const cutline::core::Media* media = source_media_of(*app);
      const double fps = media != nullptr ? media->fps.value_or(0.0) : 0.0;
      app->source_playhead = fps > 0.0 ? cutline::core::snap_to_frame(time, fps) : time;
      refresh_source(*app);
    };
  }

  auto& row = panel->emplace<Box>(Axis::Horizontal);

  // A frame either way. The buttons and the arrow keys go through the same
  // call, so the two can never disagree about what a frame is.
  //
  // Drawn icons rather than "<" and ">", which is what the rest of the
  // application uses and — the reason it changed — half the width. Spelled out,
  // this row and a Play button together were fourteen controls squeezed in two
  // themes of four.
  auto& back = row.emplace<IconButton>(IconButton::Icon::ArrowLeft, [app] {
    if (app != nullptr) step_source(*app, -1.0);
  });
  back.set_tooltip("Back one frame (Left)");
  back.set_narrow(true);
  auto& forward = row.emplace<IconButton>(IconButton::Icon::ArrowRight, [app] {
    if (app != nullptr) step_source(*app, 1.0);
  });
  forward.set_tooltip("On one frame (Right)");
  // A pair that belongs together, so they take about the room one control
  // would — which is what buys the Play button its place on the row.
  forward.set_narrow(true);

  auto& play = row.emplace<Button>("Play", [app] {
    if (app != nullptr) toggle_source_playback(*app);
  });
  play.set_tooltip("Play the source, with its sound (Space)");
  if (app != nullptr) app->source_play_button = &play;

  // "In" and "Out" rather than "Mark In" and "Mark Out": this panel is a third
  // of the width the program monitor's row has, and with the two step buttons
  // beside them `--check` found fourteen controls squeezed and clipped in three
  // themes of four. The tooltips carry the whole name.
  auto& mark_in = row.emplace<Button>("In", [app] {
    if (app != nullptr) mark_source(*app, false);
  });
  mark_in.set_tooltip("Mark in on the source (I)");
  auto& mark_out = row.emplace<Button>("Out", [app] {
    if (app != nullptr) mark_source(*app, true);
  });
  mark_out.set_tooltip("Mark out on the source (O)");
  // A cross rather than the word, which is the last thing Aero's roomier chrome
  // had room for — spelled out it was fourteen controls squeezed, in that theme
  // alone. Taking both marks away is a rarer thing to want than setting either.
  auto& clear = row.emplace<IconButton>(IconButton::Icon::Cross, [app] {
    if (app == nullptr) return;
    if (const cutline::core::Media* media = source_media_of(*app); media != nullptr) {
      app->session.apply(
          cutline::core::clear_source_marks(app->session.project(), media->id));
      refresh_all(*app);
    }
  });
  clear.set_tooltip("Take both of the source's marks away");
  row.emplace<Spacer>();

  return panel;
}

[[nodiscard]] std::unique_ptr<Widget> make_panel(App* app, const PanelId& id) {
  if (id == "scopes") return make_scopes_panel(app);
  if (id == "source") return make_source_panel(app);
  if (id == "project") return make_project_panel(app);
  if (id == "monitor") return make_monitor_panel(app);
  if (id == "timeline") return make_timeline_panel(app);
  if (id == "effects") return make_effects_panel(app);
  if (id == "library") return make_library_panel(app);
  if (id == "audio") return make_audio_panel(app);

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
    // Whatever was filling the window stops: a panel dropped into an
    // arrangement nobody can see is a move nobody can check.
    if (cutline::ui::restore_maximised(layout_of(*app))) app->dock_stale = true;
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
    // The same reason a drop restores: a panel torn out of a window showing one
    // panel would leave that window showing nothing.
    if (cutline::ui::restore_maximised(layout_of(*app))) app->dock_stale = true;

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

/// Whether a panel is anywhere in the arrangement, docked or torn out.
[[nodiscard]] bool panel_is_open(App& app, const PanelId& panel) {
  const std::vector<PanelId> open = cutline::ui::panels_in(layout_of(app));
  return std::ranges::find(open, panel) != open.end();
}

/// What the Window menu does to a panel: opens a closed one, and puts a hidden
/// tab of an open one to the front.
///
/// Closing from here as well, which is what Premiere's Window menu does and
/// what makes the tick honest — a menu that can only ever add ticks is one
/// where the tick is decoration.
void toggle_panel(App& app, const PanelId& panel) {
  DockLayout& layout = layout_of(app);
  if (!panel_is_open(app, panel)) {
    if (cutline::ui::open_panel(layout, panel)) app.dock_stale = true;
    return;
  }
  // Open, but possibly behind another tab. Bringing it forward is the useful
  // answer when that is so, and closing it is the useful answer when it is
  // already the one showing.
  if (cutline::ui::activate_panel(layout, panel)) {
    app.dock_stale = true;
    return;
  }
  if (cutline::ui::close_panel(layout, panel)) app.dock_stale = true;
}

/// The Window menu: every panel this build has, ticked where it is open.
///
/// Made on the click rather than kept, because what it says is a reading of the
/// arrangement at that moment and the arrangement changes under it constantly.
[[nodiscard]] std::unique_ptr<Widget> build_window_menu(App& app) {
  std::vector<std::string> labels;
  std::vector<bool> open;
  for (const auto& [id, title] : kPanels) {
    labels.emplace_back(title);
    open.push_back(panel_is_open(app, PanelId(id)));
  }

  auto list = std::make_unique<MenuList>(std::move(labels));
  list->set_checked(std::move(open));
  list->set_on_choose([&app](std::size_t index) {
    if (app.main.host != nullptr) app.main.host->close_popup();
    if (index < kPanels.size()) toggle_panel(app, PanelId(kPanels[index].first));
  });
  return list;
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
    // One panel filling the main window, when something is maximised. Built
    // here rather than by rewriting the tree, which is what makes coming back
    // exact — the arrangement was never taken apart.
    DockNode only;
    if (!layout_of(app).maximised.empty() && shell->is_main()) {
      only = DockNode::tabs({layout_of(app).maximised});
      node = &only;
    } else if (!shell->is_main()) {
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
  // Only here. A torn-out panel's caption is a title bar too, and stamping the
  // product mark on every one of them turns branding into wallpaper.
  caption.set_mark(true);
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

  // A menu bar, where a Windows application keeps the things that are done to
  // the document rather than with it: opening and saving, what the sequence is,
  // and what the application looks like.
  //
  // Built out of buttons that open popups rather than out of a menu widget of
  // its own, because that is what every other menu here already is — the
  // effects list, the track menu, the right-click on a clip. One idiom.
  auto& bar = shell->emplace<Box>(Axis::Horizontal);
  bar.set_padding(cutline::ui::Edges::all(4.0));

  /// One top-level menu: a button, and the list it drops.
  struct MenuEntry {
    const char* label;
    std::function<void()> act;
    /// Whether the thing this entry turns on is on, for the entries that are
    /// switches rather than actions. Asked at the moment the menu drops, since
    /// that is the only moment the answer is worth anything — a menu built once
    /// at startup would show the state the application began with for ever.
    std::function<bool()> ticked;
  };
  const auto menu = [app, &bar](const char* title, std::vector<MenuEntry> entries) {
    auto& button = bar.emplace<Button>(title);
    button.set_on_click([app, control = &button, entries = std::move(entries)] {
      if (app == nullptr || app->main.host == nullptr) return;

      std::vector<std::string> labels;
      std::vector<bool> checked;
      bool any_switches = false;
      labels.reserve(entries.size());
      for (const MenuEntry& entry : entries) {
        labels.emplace_back(entry.label);
        checked.push_back(entry.ticked && entry.ticked());
        if (entry.ticked) any_switches = true;
      }

      auto list = std::make_unique<MenuList>(std::move(labels));
      // Only for menus that have a switch in them. A menu of plain actions
      // would otherwise hold a column open for ticks none of them can have.
      if (any_switches) list->set_checked(std::move(checked));
      list->set_on_choose([app, entries](std::size_t index) {
        if (app->main.host != nullptr) app->main.host->close_popup();
        if (index < entries.size() && entries[index].act) entries[index].act();
      });
      // Under the button rather than at the pointer, which is where a menu bar
      // drops its menus and what makes two of them line up.
      app->main.host->open_popup(std::move(list), control->bounds());
    });
  };

  menu("File", {
      {"New Project", [app] { if (app != nullptr) new_project(*app); }},
      {"Open Project...", [app] { if (app != nullptr) open_project(*app); }},
      {"Save Project", [app] { if (app != nullptr) save_project(*app, false); }},
      {"Save Project As...", [app] { if (app != nullptr) save_project(*app, true); }},
      {"Import Media...", [app] { if (app != nullptr) import_media(*app); }},
      {"Export...", [app] { if (app != nullptr) open_export_dialog(*app); }},
      {"Exit", [app] {
         if (app != nullptr && app->main.window != nullptr) {
           PostMessageW(app->main.window, WM_CLOSE, 0, 0);
         }
       }},
  });

  menu("Edit", {
      {"Undo", [app] { if (app != nullptr) run_command(*app, cutline::editor::Command::Undo); }},
      {"Redo", [app] { if (app != nullptr) run_command(*app, cutline::editor::Command::Redo); }},
      {"Cut", [app] { if (app != nullptr) run_command(*app, cutline::editor::Command::Cut); }},
      {"Copy", [app] { if (app != nullptr) run_command(*app, cutline::editor::Command::Copy); }},
      {"Paste", [app] { if (app != nullptr) run_command(*app, cutline::editor::Command::Paste); }},
      {"Paste Insert",
       [app] { if (app != nullptr) run_command(*app, cutline::editor::Command::PasteInsert); }},
      // Premiere keeps these on the Clip menu, which this has no equivalent of;
      // the timeline's right-click is where a clip's own commands live, and
      // these act on the *source* rather than on a clip.
      {"Trim Previous Edit to Playhead", [app] {
         if (app != nullptr) run_command(*app, cutline::editor::Command::TrimPreviousToPlayhead);
       }},
      {"Trim Next Edit to Playhead", [app] {
         if (app != nullptr) run_command(*app, cutline::editor::Command::TrimNextToPlayhead);
       }},
      {"Insert", [app] { if (app != nullptr) run_command(*app, cutline::editor::Command::Insert); }},
      {"Overwrite",
       [app] { if (app != nullptr) run_command(*app, cutline::editor::Command::Overwrite); }},
  });

  menu("Project", {
      // Premiere's "Link Media...", under the same menu and acting on what the
      // pool has selected. Not a button on the project panel's row: that row is
      // four controls wide already, and this is a thing wanted rarely and
      // urgently rather than often.
      {"Relink Media...", [app] { if (app != nullptr) relink_pool_entry(*app); }},
      // The two halves kept apart, as Premiere keeps them: making proxies is a
      // job that runs for minutes, and reading from them is a switch. Somebody
      // who has made them once will use the second of these all week and the
      // first only when new footage arrives.
      {"Make Proxies", [app] { if (app != nullptr) make_proxies(*app); }},
      // A tick rather than a label that changes, so the entry says both what it
      // is and what it is set to. "Use Proxies" that became "Stop Using
      // Proxies" would make somebody read it twice to find out which.
      {"Use Proxies", [app] { if (app != nullptr) toggle_use_proxies(*app); },
       [app] { return app != nullptr && app->session.project().use_proxies; }},
      // Reachable at all, which matters more than where it is: a transcode
      // queued over a card of footage is an hour of the machine, and without
      // this the only way to stop it is to close the application.
      {"Stop Making Proxies", [app] { if (app != nullptr) stop_making_proxies(*app); }},
      {"Project Settings...", [app] {
         if (app == nullptr || app->main.host == nullptr) return;
         app->main.host->open_popup(build_project_settings_popup(*app), settings_anchor());
       }},
  });

  menu("Settings", {
      {"Application Settings...", [app] {
         if (app == nullptr || app->main.host == nullptr) return;
         app->main.host->open_popup(build_application_settings_popup(*app), settings_anchor());
       }},
  });

  // The Window menu, which is not built from a fixed list of entries like the
  // others: what it says depends on which panels are open at the moment it is
  // dropped, so it is made on the click rather than at startup.
  //
  // This is the only way back to a panel whose tab has been closed. Without it
  // closing one is permanent short of resetting the whole workspace, and a
  // close button that can throw a panel away for good is a trap.
  {
    auto& windows = bar.emplace<Button>("Window");
    windows.set_on_click([app, control = &windows] {
      if (app == nullptr || app->main.host == nullptr) return;
      app->main.host->open_popup(build_window_menu(*app), control->bounds());
    });
  }

  menu("Help", {
      {"Check for Updates...", [app] { if (app != nullptr) check_for_updates(*app); }},
  });

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
  /// One stream per timeline track rather than one summed stream.
  bool separate = false;
};
constexpr std::array kExportMixdowns{
    MixdownChoice{"Stereo", 2},
    MixdownChoice{"Mono", 1},
    // Last, because it is the one that produces a file most players will only
    // play the first stream of. It is for handing the cut to somebody who is
    // going to mix it, and a mixdown is what everything else wants.
    MixdownChoice{"Separate tracks", 2, true},
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
  settings.separate_audio = kExportMixdowns[app.export_setup.mixdown].separate;
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
    // The zoom keys. `VK_OEM_PLUS` is the unshifted key, so this is the one
    // labelled `=` on a keyboard and `+` in every editor's menu.
    case VK_OEM_PLUS: return Key::Equal;
    case VK_OEM_MINUS: return Key::Minus;
    case VK_OEM_5: return Key::Backslash;
    // Insert and overwrite. `Key::Comma` and `Key::Period` have been bound
    // since the nudge was written and have never once arrived: the virtual key
    // for a comma is `VK_OEM_COMMA`, not the character, so the translation
    // dropped it and the binding sat there doing nothing.
    case VK_OEM_COMMA: return Key::Comma;
    case VK_OEM_PERIOD: return Key::Period;
    // Maximise. The same trap as the comma above, and it caught this too: the
    // binding was written, the key was pressed, and nothing happened because
    // the backtick arrives as `VK_OEM_3` rather than as its character.
    case VK_OEM_3: return Key::Backtick;
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
  // A gesture lasts exactly as long as the pointer is held, and the host is the
  // only thing that knows that. See `App::live_gesture` for why the control
  // cannot be trusted to say when it is finished.
  if (app.live_gesture && app.main.host != nullptr && app.main.host->captured() == nullptr) {
    app.live_gesture = false;
    app.live_project.reset();
    app.preview_stale = true;
    refresh_handles(app);
  }

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
  // After the preview, and only when the panel is open. Measuring costs a
  // second render of the frame, so a scope nobody is looking at costs nothing.
  refresh_scopes(app);
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

  // Metrics change with the theme, so everything has to be measured again —
  // in every window, not only the one whose button was pressed.
  for (Shell* shell : app.shells()) {
    if (shell->host != nullptr) shell->host->request_layout();
    shell->dirty = true;
    if (shell->window != nullptr) InvalidateRect(shell->window, nullptr, FALSE);
  }
}

// ----------------------------------------------------------------- cursors --

/// A tool cursor, drawn from the same art as the tool's button.
///
/// Rendered rather than shipped as a resource, and rendered from
/// `ui::draw_icon` rather than from a second drawing of the same shape: a razor
/// that looked one way in the palette and another under the pointer would be
/// two razors, and the second one would drift the first time either was
/// touched.
///
/// Thirty-two pixels, which is what Windows expects and what a hot spot in the
/// middle of leaves room around. The mark is drawn white with a black halo
/// under it, because a cursor has to be visible over a dark timeline and over a
/// light one, and the alternative — a cursor that takes the theme's colours —
/// is invisible over its own panel half the time.
constexpr int kCursorSize = 32;

/// The mark a tool cursor is made of, drawn onto a transparent surface.
///
/// Split from making the cursor itself so `--check` can look at the pixels. A
/// mark that comes out empty is a cursor that vanishes over the timeline, and
/// nothing else in the application would notice.
[[nodiscard]] sk_sp<SkSurface> draw_cursor_art(cutline::ui::IconButton::Icon icon) {
  constexpr double kReach = 9.0;

  const sk_sp<SkSurface> surface =
      SkSurfaces::Raster(SkImageInfo::MakeN32Premul(kCursorSize, kCursorSize));
  if (surface == nullptr) return nullptr;
  surface->getCanvas()->clear(SK_ColorTRANSPARENT);

  const std::unique_ptr<cutline::ui::SkiaPainter> painter =
      cutline::ui::SkiaPainter::create(surface->getCanvas());
  if (painter == nullptr) return nullptr;

  const Rect area{0.0, 0.0, static_cast<double>(kCursorSize),
                  static_cast<double>(kCursorSize)};
  // The halo first, as the same mark drawn fatter underneath. Cheaper than an
  // outline and it cannot come apart at a corner the way a traced one does.
  cutline::ui::draw_icon(*painter, icon, area, cutline::ui::Color{0.0F, 0.0F, 0.0F, 0.85F},
                         kReach, 5.0);
  cutline::ui::draw_icon(*painter, icon, area, cutline::ui::Color{1.0F, 1.0F, 1.0F, 1.0F},
                         kReach, 2.0);
  return surface;
}

[[nodiscard]] HCURSOR render_tool_cursor(cutline::ui::IconButton::Icon icon) {
  constexpr int kSize = kCursorSize;
  const sk_sp<SkSurface> surface = draw_cursor_art(icon);
  if (surface == nullptr) return nullptr;

  // Into a DIB, because that is what an icon is made of. Top-down, so the rows
  // are in the order Skia wrote them rather than upside down.
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(BITMAPV5HEADER);
  header.bV5Width = kSize;
  header.bV5Height = -kSize;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;

  void* bits = nullptr;
  const HDC screen = GetDC(nullptr);
  const HBITMAP colour = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                                          DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (colour == nullptr || bits == nullptr) return nullptr;

  const SkImageInfo info = SkImageInfo::Make(kSize, kSize, kBGRA_8888_SkColorType,
                                             kUnpremul_SkAlphaType);
  if (!surface->readPixels(info, bits, kSize * 4, 0, 0)) {
    DeleteObject(colour);
    return nullptr;
  }

  // The mask is ignored for a 32-bit icon with an alpha channel, but one has to
  // be there or `CreateIconIndirect` refuses.
  const HBITMAP mask = CreateBitmap(kSize, kSize, 1, 1, nullptr);
  ICONINFO icon_info{};
  icon_info.fIcon = FALSE;
  // The middle. Every one of these marks is symmetrical about its own centre,
  // and a hot spot anywhere else would mean the razor cut somewhere other than
  // where its blade was drawn.
  icon_info.xHotspot = kSize / 2;
  icon_info.yHotspot = kSize / 2;
  icon_info.hbmMask = mask;
  icon_info.hbmColor = colour;

  const HCURSOR made = reinterpret_cast<HCURSOR>(CreateIconIndirect(&icon_info));
  DeleteObject(colour);
  DeleteObject(mask);
  return made;
}

/// What each cursor is on this platform.
///
/// The tool ones are made once and kept: rendering thirty-two pixels is cheap,
/// and doing it on every mouse move over a timeline is thirty-two pixels sixty
/// times a second for no reason.
[[nodiscard]] HCURSOR system_cursor(cutline::ui::Cursor cursor) {
  using cutline::ui::Cursor;
  using Icon = cutline::ui::IconButton::Icon;

  switch (cursor) {
    case Cursor::Text: return LoadCursorW(nullptr, IDC_IBEAM);
    case Cursor::ResizeWE: return LoadCursorW(nullptr, IDC_SIZEWE);
    case Cursor::ResizeNS: return LoadCursorW(nullptr, IDC_SIZENS);
    case Cursor::Move: return LoadCursorW(nullptr, IDC_SIZEALL);
    case Cursor::Arrow: return LoadCursorW(nullptr, IDC_ARROW);

    case Cursor::Razor:
    case Cursor::RateStretch:
    case Cursor::Slip:
    case Cursor::Slide:
    case Cursor::Ripple:
    case Cursor::Roll: break;
  }

  static std::map<cutline::ui::Cursor, HCURSOR> made;
  if (const auto found = made.find(cursor); found != made.end()) {
    return found->second != nullptr ? found->second : LoadCursorW(nullptr, IDC_ARROW);
  }

  const Icon icon = cursor == Cursor::Razor         ? Icon::Razor
                    : cursor == Cursor::RateStretch ? Icon::RateStretch
                    : cursor == Cursor::Slip        ? Icon::Slip
                    : cursor == Cursor::Slide       ? Icon::Slide
                    : cursor == Cursor::Ripple      ? Icon::Ripple
                                                    : Icon::Roll;
  const HCURSOR drawn = render_tool_cursor(icon);
  made.emplace(cursor, drawn);
  return drawn != nullptr ? drawn : LoadCursorW(nullptr, IDC_ARROW);
}

/// Everything a message does. Wrapped by `window_proc`, which is the boundary
/// an exception must not cross.
LRESULT handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
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

      // After the move, so the browser has been told how far the drag has got.
      // Only the main window has both a pool and a timeline in it; a floating
      // one may have either, and asking costs nothing when it has neither.
      if (app != nullptr) refresh_drop_ghost(*app);

      // Any movement takes the tooltip away and starts the wait again. A box
      // that stayed up while the pointer moved on would be labelling the wrong
      // thing, and one that never restarted would only ever appear once.
      shell->host->hide_tooltip();
      SetTimer(window, kTooltipTimer, kTooltipDelayMs, nullptr);
      // Not unconditionally: the pointer crossing a panel that does not care
      // leaves the picture exactly as it was, and a full repaint costs
      // milliseconds. `--benchmark` is where that number comes from.
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;
    }
    case WM_SETCURSOR: {
      // Only over the client area. The frame's own edges and buttons are the
      // system's to draw a cursor for, and taking those would leave a window
      // that cannot be resized by its border without guessing where it is.
      if (LOWORD(lparam) != HTCLIENT) break;
      SetCursor(system_cursor(shell->host->cursor()));
      return TRUE;
    }

    case WM_MOUSELEAVE:
      KillTimer(window, kTooltipTimer);
      shell->host->hide_tooltip();
      shell->host->mouse_exit();
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;

    case WM_LBUTTONDOWN:
      KillTimer(window, kTooltipTimer);
      shell->host->hide_tooltip();
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
      // A drag released anywhere — over the tracks, over a panel, off the
      // window — is a drag that is over, and the promise goes with it. The
      // browser has already forgotten the gesture by now, so this works the
      // answer out the same way every other move does and finds there is none.
      if (app != nullptr) refresh_drop_ghost(*app);
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;

    // The right button, which until now reached nothing at all: `MouseEvent`
    // has carried a button since the first widget and this layer only ever
    // translated the left one, so every context menu in the application was
    // blocked on three lines that were never written.
    //
    // No `SetCapture`. A right-click is a single event rather than a gesture —
    // there is no right-drag anywhere — and holding the pointer for one would
    // only mean the release goes somewhere surprising. The host does not
    // capture on it either, for the same reason.
    case WM_RBUTTONDOWN:
      shell->host->mouse_down(mouse_from(lparam, MouseButton::Right));
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;
    case WM_RBUTTONUP:
      shell->host->mouse_up(mouse_from(lparam, MouseButton::Right));
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;

    // Swallowed. Windows sends this after the release, and letting it through
    // to `DefWindowProc` puts the system's own menu up over ours.
    case WM_CONTEXTMENU:
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
        // The zoom, with the tools and for the same reason: it changes the
        // view rather than the document. Premiere's keys — `=` and `-` about
        // the playhead, and `\` for the whole sequence.
        if (app->timeline != nullptr &&
            (pressed == Key::Equal || pressed == Key::Minus || pressed == Key::Backslash)) {
          if (pressed == Key::Backslash) {
            app->timeline->zoom_to_fit();
          } else {
            app->timeline->zoom_about_playhead(pressed == Key::Equal);
          }
          mark_dirty(*app);
          return 0;
        }

        // Premiere's S. Here with the tools rather than in the command table
        // because it changes the view and not the document, so there is nothing
        // for `run` to report and nothing for undo to put back.
        if (pressed == Key::S) {
          toggle_snapping(*app);
          return 0;
        }

        // J, K and L: back, stop, forward, pressing again to go faster. The
        // same reasoning as the tool keys — these are muscle memory from every
        // editor there has ever been, and one that only worked when nothing
        // happened to be focused would be one nobody trusted.
        if (pressed == Key::J) {
          set_shuttle(*app, next_shuttle(app->shuttle, -1.0));
          return 0;
        }
        if (pressed == Key::K) {
          set_shuttle(*app, 0.0);
          if (app->playing()) stop_playback(*app);
          return 0;
        }
        if (pressed == Key::L) {
          set_shuttle(*app, app->playing() ? next_shuttle(1.0, 1.0)
                                           : next_shuttle(app->shuttle, 1.0));
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

      // Premiere's `~`: the panel under the pointer fills the window, and again
      // puts it back. Under the *pointer* rather than whatever has the keyboard,
      // because it is used to get a closer look at something being watched, and
      // watching a panel does not focus it.
      if (event.key == Key::Backtick && held.none() && shell->is_main()) {
        if (shell->dock != nullptr) {
          POINT at{};
          GetCursorPos(&at);
          ScreenToClient(window, &at);
          const auto over = shell->dock->panel_at(static_cast<double>(at.x),
                                                  static_cast<double>(at.y));
          const PanelId panel =
              over.value_or(layout_of(*app).maximised);
          if (cutline::ui::toggle_maximised(layout_of(*app), panel)) app->dock_stale = true;
        }
        return 0;
      }

      // After the tree, and only if it did not want the key. A focused slider
      // owns the arrows before the playhead does.
      if (!shell->host->key_down(event)) {
        // Premiere sends the transport and the marking keys to whichever
        // monitor is focused, and the source is a monitor. Before the sequence
        // bindings, because these are the same keys meaning the same thing
        // about a different thing — an `I` with the source in hand is a mark on
        // the source, not on the timeline behind it.
        if (!handle_source_key(*app, event.key, held)) {
          run_binding(*app, kTransportKeys, event.key, held);
        }
      }
      mark_dirty(*app);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;  // every pixel is painted, so erasing only causes a flash

    case WM_TIMER:
      if (wparam == kTooltipTimer) {
        // Once. The pointer has rested; nothing more is due until it moves
        // again, which is what restarts the wait.
        KillTimer(window, kTooltipTimer);
        if (std::string say = shell->host->tooltip_at_pointer(); !say.empty()) {
          shell->host->show_tooltip(std::move(say));
          if (shell->host->needs_paint()) shell->dirty = true;
        }
        return 0;
      }
      if (wparam == kAutosaveTimer) {
        poll_autosave(*app);
        return 0;
      }
      break;

    case kUpdateChanged:
      settle_update(*app);
      return 0;

#if CUTLINE_HAVE_PREVIEW
    case kMediaReady: {
      // Rebuilt rather than patched: what arrives belongs to a source, and
      // which blocks show it is the binding's answer, not this one's. Both
      // flags are taken before the test, so a burst of arrivals is one rebuild
      // and neither cache is left holding a flag the other consumed.
      const bool waves = app->waveforms.take_arrival();
      const bool strips = app->filmstrips.take_arrival();
      if (waves || strips) refresh_timeline(*app);
      // Outside the test as well: the last arrival of a burst brings the count
      // to zero, and a label still saying "Reading media (1)" over an idle
      // machine is worse than no label at all.
      refresh_busy(*app);
      return 0;
    }

    case kProxyChanged:
      // Both, and in this order: the ones that finished are attached first, so
      // the line in the corner is redrawn from what is actually left.
      collect_proxies(*app);
      refresh_busy(*app);
      return 0;
#endif

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
      // The editor itself, with something unsaved. Three answers rather than
      // two, because "are you sure" with only yes and no makes cancelling and
      // discarding the same button — and one of those loses an afternoon.
      if (app->session.modified() && !confirm_discard(*app)) return 0;
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

/// The window procedure, and the one place an exception is caught.
///
/// A window procedure is a callback the *kernel* invokes, and an exception
/// leaving one is `STATUS_FATAL_USER_CALLBACK_EXCEPTION`: the process is
/// terminated on the spot, with no message, no chance to save, and nothing in
/// the log but a fault offset inside `KERNELBASE`. That is how resizing a
/// torn-out window looked like the application simply vanishing.
///
/// Caught rather than left to it, and said out loud. Swallowing a fault is not
/// the point — the point is that whatever went wrong is worth one dialogue and
/// a chance to save the project, and that a bug in one message must not take
/// the document with it.
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) try {
  return handle_message(window, message, wparam, lparam);
} catch (const std::exception& failure) {
  complain(window, std::string("Something went wrong handling a window message.\n\n") +
                       failure.what() +
                       "\n\nThe editor is still running. Save your work.");
  return 0;
} catch (...) {
  complain(window, "Something went wrong handling a window message.\n\n"
                   "The editor is still running. Save your work.");
  return 0;
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

  // Four frames of flat colour — enough to see that tiles land where they
  // should and that the strip advances along the clip.
  auto strip = std::make_shared<cutline::ui::Filmstrip>();
  for (int f = 0; f < 4; ++f) {
    cutline::ui::FilmFrame frame;
    frame.t = f * 8.0;
    frame.width = 32;
    frame.height = 18;
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
    int squeezed = 0;
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
      if (widget.bounds().x < -0.5 || widget.bounds().right() > kWidth + 0.5) ++escaped;

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
        cutline::ui::activate_panel(layout, "effects");
        // And the mixer, which is a tab behind the monitor and holds a strip
        // per audio track — an arrangement that grows with the project and so
        // is worth laying out rather than assuming.
        cutline::ui::activate_panel(layout, "audio");
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

      // The settings popups, for the same reason: they live on the popup layer
      // and nothing else would ever lay them out. Both are rows of numbers and
      // presets, which is exactly the shape that gets squeezed in a theme with
      // a wider font.
      // The Window menu is here too: it is the one menu whose rows carry ticks,
      // and a tick gutter that a theme's wider font pushes the labels out of
      // would show up nowhere else.
      for (int which = 0; which < 3; ++which) {
        app.main.host->close_popup();
        app.main.host->open_popup(which == 0   ? build_project_settings_popup(app)
                                  : which == 1 ? build_application_settings_popup(app)
                                               : build_window_menu(app),
                                  settings_anchor());
        app.main.host->update_layout(context);
        app.main.host->paint(*painter, theme);
        if (Widget* settings = app.main.host->popup(); settings != nullptr) {
          walk(*settings);
        } else {
          std::println("{}: a settings popup did not open", theme.id);
          ++failures;
        }
      }
      app.main.host->close_popup();
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
        "{:<10} {:>3} widgets, {} empty, {} outside the window, {} clipped away, {} squeezed",
        theme.id, counted, empty, escaped, clipped, squeezed);
    if (empty > 0 || escaped > 0 || clipped > 0 || squeezed > 0) ++failures;
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
  // Deliberately null. With a class cursor Windows resets the pointer to it on
  // every move over the client area, and `WM_SETCURSOR` is where the interface
  // gets to say what it should be instead.
  window_class.hCursor = nullptr;
  // The application's own icon, for the taskbar, Alt+Tab and the window's own
  // corner. `hIcon` is the large one and `hIconSm` the small one; naming both
  // is what stops Windows scaling the 256-pixel image down to sixteen and
  // producing a grey smudge where the mark should be, since the .ico carries a
  // drawn-for-the-size version of each.
  const HINSTANCE self = window_class.hInstance;
  window_class.hIcon = static_cast<HICON>(LoadImageW(self, MAKEINTRESOURCEW(kIconResource),
                                                     IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
  window_class.hIconSm = static_cast<HICON>(
      LoadImageW(self, MAKEINTRESOURCEW(kIconResource), IMAGE_ICON,
                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
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

  // And the saved presets, before the library is built, so they are in the tree
  // from the first frame rather than appearing after it.
  if (auto read = cutline::editor::read_presets(cutline::editor::default_presets_path());
      read.has_value()) {
    app.presets = std::move(*read);
  }
  if (auto read = cutline::editor::read_bins(cutline::editor::default_bins_path());
      read.has_value()) {
    app.bins = std::move(*read);
  }

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

#if CUTLINE_HAVE_PREVIEW
  app.timeline_media.waveforms = [&app](std::string_view media_id, int stream) {
    return app.waveforms.find(media_id, stream);
  };
  app.timeline_media.filmstrips = [&app](std::string_view media_id) {
    return app.filmstrips.find(media_id);
  };
  // Set once the window exists, since posting to one that does not is the
  // message going nowhere. Runs on a worker's thread, so each does the one
  // thing that is safe from there and leaves the rest to the message handler.
  app.waveforms.set_on_arrival([window] { PostMessageW(window, kMediaReady, 0, 0); });
  app.filmstrips.set_on_arrival([window] { PostMessageW(window, kMediaReady, 0, 0); });
  app.proxies.set_on_change([window] { PostMessageW(window, kProxyChanged, 0, 0); });
#endif
  // Same shape, and for the same reason: the answer arrives on a worker while
  // the loop is blocked on its queue.
  app.updater.set_on_change([window] { PostMessageW(window, kUpdateChanged, 0, 0); });

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

  // Anything left over from a session that did not close cleanly. Only the
  // never-saved document is offered here; a recovery copy of a project on disk
  // is offered when that project is opened, which is where somebody is
  // expecting to be asked about it.
  (void)offer_recovery(app, {});

  // A file named on the command line, which is what "Open with", a shortcut
  // with an argument, and a project dropped on the executable all come through
  // as. After the recovery offer, so a crashed session is still asked about
  // first — and after the window is up, because everything here may need to
  // complain, and a message box with no owner appears behind everything.
  if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
    open_from_command_line(app, std::filesystem::path(argv[1]));
  }

  SetTimer(window, kAutosaveTimer, kAutosaveTickMs, nullptr);

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
    if (app.playing() || app.shuttling() || app.source_playing() || app.exporting() ||
        app.export_job.finished.load()) {
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
        advance_shuttle(app);
        advance_source_playback(app);
        poll_export(app);
        // Nothing is due this instant when only the export is running, and the
        // encoder wants the core far more than this loop does.
        if (!app.playing() && !app.shuttling() && !app.source_playing()) Sleep(8);
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

  KillTimer(window, kAutosaveTimer);

  // Closing now asks about unsaved work, so anything still unsaved here was
  // deliberately discarded — and the recovery copy goes with it. What is left
  // in the recovery directory is exactly what was never saved and never
  // answered for, which is to say: what a crash took.
  clear_autosave(app, app.session.path());

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
