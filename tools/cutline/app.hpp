#pragma once

/// What every part of the application window shares: the window, the
/// application, and the calls one part makes on another.
///
/// `main.cpp` grew to fourteen thousand lines with all of this inside one
/// anonymous namespace, where nothing could be moved to a file of its own
/// without taking everything it touched with it. Declared here, in a named
/// namespace, a part of the window -- a dialog, a menu, the inspector -- can
/// live in its own file and still reach the application it belongs to.

#include "cutline/core/edit.hpp"
#include "cutline/core/version.hpp"
#include "cutline/core/effects.hpp"
#include "cutline/core/model.hpp"
#include "cutline/core/pool.hpp"
#include "cutline/audio/chain.hpp"
#include "cutline/audio/loudness.hpp"
#include "cutline/core/animate.hpp"
#include "cutline/core/interpret.hpp"
#include "cutline/core/attributes.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/roles.hpp"
#include "cutline/core/routing.hpp"
#include "cutline/core/sequences.hpp"
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
#include "cutline/editor/settings.hpp"
#include "cutline/editor/shortcuts.hpp"
#include "cutline/editor/timeline_binding.hpp"
#include "cutline/editor/titles.hpp"
#include "cutline/editor/transitions.hpp"
#include "cutline/editor/workspace.hpp"
#include "cutline/render/preview.hpp"
#include "cutline/ui/browser.hpp"
#include "cutline/ui/color_picker.hpp"
#include "cutline/ui/controls.hpp"
#include "cutline/ui/dialog.hpp"
#include "cutline/ui/dock.hpp"
#include "cutline/ui/dock_view.hpp"
#include "cutline/ui/effects_browser.hpp"
#include "cutline/ui/keyboard_view.hpp"
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
#include "cutline/app/preview_engine.hpp"
#include "cutline/app/updater.hpp"
#include "cutline/app/media_cache.hpp"
#include "cutline/app/proxies.hpp"
#include "cutline/app/thumbnails.hpp"
#include "cutline/app/waveforms.hpp"
#include "cutline/engine/exporter.hpp"
#include "cutline/engine/audio_mixer.hpp"
#include "cutline/engine/player.hpp"
#include "cutline/media/decoder.hpp"
#endif

#include <windows.h>
// Both of these need windows.h first: one for the message parameters it
// defines, the other for the types in its own signatures.
#include <commdlg.h>
#include <dwmapi.h>
// For `ShellExecuteW`, which is what starts the downloaded installer.
#include <shellapi.h>
// For `IFileDialog`, the only folder picker on this platform that can be typed
// into. The old `SHBrowseForFolder` draws a tree from Windows 2000 and cannot.
#include <shobjidl.h>
// For `timeBeginPeriod`: the default 15.6 ms scheduler tick is most of a frame
// at 60 Hz, and the playback loop cannot pace itself with one that coarse.
#include <timeapi.h>
// For `PlaySoundW`: the interface's sounds, played from the system's scheme.
#include <mmsystem.h>
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
#include <numeric>
#include <thread>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::tool {

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
using cutline::ui::MenuEntry;
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
using cutline::ui::RadioGroup;
using cutline::ui::Rect;
using cutline::ui::ScrollView;
using cutline::ui::ScrubBar;
using cutline::ui::SkiaPainter;
using cutline::ui::Slider;
using cutline::ui::Space;
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

/// What a new document is. Defined in `main.cpp`; declared here because a
/// fresh `App` starts with one.
[[nodiscard]] cutline::core::Project new_project_model();

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

/// How often the displays are asked whether they are showing HDR. Turning HDR
/// on, or moving Windows' SDR brightness slider, sends no message a window
/// hears, so it is looked for -- every two seconds costs nothing measurable.
constexpr UINT_PTR kDisplayTimer = 3;
constexpr UINT kDisplayTickMs = 2000;
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

/// Posted by a monitor's render thread when a picture is ready to be taken.
///
/// A message rather than a flag for the reason `kMediaReady` gives: the loop
/// sleeps in `GetMessage` while nothing is playing, and a scrub's picture that
/// waited for the next mouse move would arrive one move late, every time.
constexpr UINT kPreviewReady = WM_APP + 4;

/// Which settings window: the person's, or the cut's.
enum class SettingsKind {
  /// Premiere's Preferences. Belongs to the person, saved in `settings.json`.
  Application,
  /// Premiere's Project Settings. Belongs to the cut, saved inside it.
  Project,
};

struct App;

/// One category down the side of a settings window, and what it fills the pane
/// with when it is chosen.
struct SettingsPage {
  std::string name;
  void (*build)(App& app, cutline::ui::Form& into) = nullptr;
};

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
  /// Alt has gone down and nothing else has yet. Released like that, it
  /// moves the keyboard to the menu bar, as it does in every Windows program.
  bool alt_alone = false;

  std::unique_ptr<WidgetHost> host;
  DockView* dock = nullptr;
  /// Asked which parts of the caption are draggable, so the buttons in it stay
  /// clickable while the rest of it moves the window.
  TitleBar* title_bar = nullptr;
  CaptionButton* maximise = nullptr;

  /// Rebuilt whenever the window changes size. Skia's raster surface owns the
  /// pixels; the blit reads them straight out.
  std::unique_ptr<cutline::ui::SkiaWindow> surface;
  /// Whether the system's glass is behind the window, so a frame leaves the
  /// title bar clear for it rather than painting the background under it.
  bool glass = false;
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
  /// The display the window was last found on, so moving within it does not
  /// ask Windows about HDR on every step of a drag.
  HMONITOR monitor = nullptr;
  /// For a dialog window, the name its size is remembered under.
  std::string size_key;
  /// Pixels per unit of the interface: the window's display DPI over 96. The
  /// widget tree is laid out in units and the window scales it as it draws, so
  /// a 125% display gets the interface drawn at its own resolution rather than
  /// a 100% bitmap stretched by Windows -- which blurred every edge and every
  /// letter on it.
  double scale = 1.0;
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
  /// The button that swaps the pool between a list and a grid of pictures.
  Button* view_button = nullptr;
  /// The view the settings asked for, applied when the panel is built — the
  /// browser does not exist yet when the file is read.
  cutline::ui::BrowserView pool_view = cutline::ui::BrowserView::List;
  cutline::editor::BrowserSort browser_sort = cutline::editor::BrowserSort::Pool;
  bool browser_descending = false;
  /// The bins showing what is inside them.
  ///
  /// Here rather than in the project: which folders happen to be open is about
  /// the looking rather than about the cut, and saving it would mean two people
  /// opening the same file argue about it through the version control.
  std::vector<std::string> expanded_bins;

  /// What a press on a clip does. Held here rather than only on the view,
  /// because the timeline panel is rebuilt whenever the arrangement or the theme
  /// changes and the tool somebody chose has to survive that.
  cutline::ui::Tool tool = cutline::ui::Tool::Selection;
  /// The palette's buttons, so the current one can be lit without rebuilding
  /// the row — which would destroy the button whose click is still running, the
  /// same trap the theme switcher has.
  std::vector<IconButton*> tool_buttons;
  /// The menu bar's titles, in order, for the keyboard: Alt and a letter,
  /// and the arrows from one menu to the next.
  std::vector<Button*> menu_buttons;
  /// The list the menu bar last dropped, and whose it is. For comparing with
  /// the host's open popup, never for following.
  const Widget* open_menu_list = nullptr;
  std::size_t open_menu = 0;

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

  /// Where the settings file is, so a test or a portable copy can point
  /// somewhere else without the writing having to be told twice.
  std::filesystem::path settings_file = cutline::editor::default_settings_path();

  /// The settings window, when one is open. One at a time, whichever kind:
  /// they are modal, and two modals would each be waiting for the other.
  std::unique_ptr<Shell> settings_window;
  SettingsKind settings_kind = SettingsKind::Application;
  /// Which category is showing, and the box its page fills.
  std::size_t settings_page = 0;
  cutline::ui::Form* settings_body = nullptr;
  /// The category list, so the chosen row can be lit without rebuilding it —
  /// which would destroy the widget whose click is still running.
  MenuList* settings_tabs = nullptr;
  /// Set when the page needs rebuilding, and acted on at the next frame.
  ///
  /// Deferred for the same reason the inspector is: what asks for it is a
  /// control *inside* the page — a label being renamed, which the dropdowns
  /// below it list by name — and rebuilding on the spot would destroy that
  /// control while its own callback was still running.
  bool settings_page_stale = false;

  /// The Keyboard Shortcuts window, when it is open, and its parts.
  std::unique_ptr<Shell> keyboard_window;
  cutline::ui::KeyboardView* keyboard_view = nullptr;
  Box* keyboard_list = nullptr;
  Label* keyboard_custom = nullptr;
  Label* keyboard_note_label = nullptr;
  TextField* keyboard_search_field = nullptr;
  /// What the list is narrowed to, which layer of the keyboard is drawn, the
  /// command waiting for its key, and what the last change did.
  std::string keyboard_search;
  cutline::ui::Modifiers keyboard_modifiers;
  std::string keyboard_capturing;
  std::string keyboard_note;
  /// Deferred for the reason the settings page is: what asks is a button in
  /// the list being rebuilt.
  bool keyboard_stale = false;

  /// The preferences, as read and as written.
  ///
  /// The ones with a live field of their own — snapping, the theme, the pool's
  /// order — are read out of this at startup and back into it on every save,
  /// because those fields are what the rest of the application already asks.
  /// The durations have no such field and live here alone.
  cutline::editor::Settings settings;
  /// Which keys run which actions: a preset with the person's changes over
  /// it, from keys.json.
  cutline::editor::Keymap keys = cutline::editor::preset_keymap(cutline::editor::kPremierePreset);
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
  /// The History panel's list, and the steps it was last filled with, so it is
  /// refilled when they change rather than on every frame.
  MenuList* history_list = nullptr;
  std::vector<std::string> history_shown;
  std::size_t history_at = static_cast<std::size_t>(-1);
  /// The picture the program monitor was last given, kept so a monitor made
  /// afresh — a panel undocked or torn out into a window of its own gets a new
  /// one — starts with it. The engine only hands over a picture better than
  /// the last, so without this a new monitor stayed empty until the playhead
  /// moved. Valid until the canvas changes size, when the next take replaces it.
  cutline::ui::TextureView shown_preview;

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

  /// What was last ticked in the Paste Attributes box.
  ///
  /// Remembered so the second clip is one keystroke and a button rather than
  /// the same six ticks again — putting one look onto six shots is the case
  /// the command exists for. Not saved with the project, and not part of it:
  /// it is a question about a gesture, not a property of the cut.
  cutline::core::ClipAttributes paste_attributes;

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
  cutline::ui::Fader* master_fader = nullptr;
  /// A mixer strip's meter, and which lane it draws. Rebuilt with the panel, so
  /// the list is cleared whenever the strips are.
  struct TrackMeter {
    std::size_t index = 0;
    cutline::ui::MeterView* view = nullptr;
  };
  std::vector<TrackMeter> track_meters;

  /// The mixer's own panel, so its strips can be rebuilt without rebuilding the
  /// dock around them.
  Panel* mixer = nullptr;
  /// Set when the strips no longer describe the project — a track added or
  /// removed, a route changed, a stack gained its first effect.
  ///
  /// Deferred for the reason the inspector's is, and it is not a theoretical
  /// reason here: the thing that asks is usually a control *inside* a strip,
  /// and rebuilding on the spot would free the dropdown whose callback was
  /// still running.
  bool mixer_stale = false;

  /// A fader laying down automation while the sequence plays.
  ///
  /// The keyframes are held here rather than written to the document as they
  /// arrive: a pass is one point per displayed frame, so applying them as they
  /// came would be sixty undo entries a second. The whole pass goes in as one
  /// edit when playback stops, which is also what makes taking it back one
  /// press.
  struct AutomationPass {
    std::string track_id;
    /// The fader's position, in linear gain, as the strip last set it.
    double gain = 1.0;
    /// Whether the hand is on it right now — what Touch writes on and Latch
    /// waits for.
    bool holding = false;
    /// Latch has been touched, so it goes on writing until the sequence stops.
    bool latched = false;
    std::vector<cutline::core::Keyframe> written;
  };
  std::vector<AutomationPass> automation;
  /// The master's, which has no track id to be found by.
  AutomationPass master_automation;
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

  /// The playhead as the playback loop last wrote it, or below zero when it is
  /// not playing.
  ///
  /// What tells "the sound has moved on" apart from "somebody moved the
  /// playhead". The loop reads the position off the sound card and writes it to
  /// the session every frame, so anything else setting the playhead mid-play
  /// was overwritten on the very next turn unless it also seeked the player.
  /// Three paths could move it and only two remembered to — which is why
  /// dragging the playhead worked while the transport keys did nothing.
  double playhead_from_playback = -1.0;

  /// How far each monitor is zoomed in, and the control that says so.
  ///
  /// One per monitor, because they are answering different questions: the
  /// program monitor is zoomed to check a mask edge on the cut, and the source
  /// monitor to check whether a take was in focus. Tying them together would
  /// mean one of the two is never at the level it was put at.
  ///
  /// Held here rather than only on the widget for the reason `aspect_locked`
  /// is: a rearrangement builds the panel again from nothing, and a setting
  /// that lives only in the widget goes with it.
  double monitor_zoom = cutline::ui::kMonitorFit;
  double source_zoom = cutline::ui::kMonitorFit;

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
  /// The source monitor's, for the same reason as `shown_preview`.
  cutline::ui::TextureView shown_source;
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
  std::unique_ptr<cutline::app::PreviewEngine> source_preview;
  /// The one-clip sequence the source monitor shows, held so that asking for a
  /// later moment of the same source is the same project to the engine. Built
  /// afresh per refresh, it would be a new project every frame of playback and
  /// nothing could ever be rendered ahead.
  std::shared_ptr<const cutline::core::Project> source_snapshot;
  /// What the source monitor shows when the windows cannot share the card and
  /// the picture has to come back through memory.
  cutline::gpu::Image source_pixels;
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

  /// The program monitor's picture, rendered on a thread of its own so that a
  /// seek, a cut or a heavy stack never stops the window. See `PreviewEngine`.
  std::unique_ptr<cutline::app::PreviewEngine> preview;
  bool preview_failed = false;
  /// What the preview was last asked for, and what it was made from. The same
  /// document at the same scale is the same pointer, which is what lets the
  /// engine keep the frames it has rendered ahead; and the scaled copy is made
  /// once per change rather than once per frame, which is what it used to cost.
  std::shared_ptr<const cutline::core::Project> preview_snapshot;
  std::uint64_t preview_snapshot_revision = 0;
  double preview_snapshot_scale = 0.0;
  /// The pixels, when the windows cannot share the card.
  cutline::gpu::Image preview_pixels;
  /// The sources the renderer last reported missing, so the pool's offline
  /// marks are redrawn when that changes rather than on the next edit.
  std::vector<std::string> preview_missing;
  /// Set when the picture no longer matches the playhead or the project.
  bool preview_stale = true;
  /// The SDR white of the HDR display any window is on, or nothing when none
  /// is. What the preview engines are told, so they make HDR pictures only
  /// when something can show them.
  std::optional<float> hdr_white;

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

  /// Whether playback has been *asked for*, which is not the same as the player
  /// still running.
  ///
  /// A player that reaches the end stops itself, so `playing()` goes false at
  /// exactly the moment there is something left to decide — loop round, or stop
  /// and tidy up. `advance_playback` guarded on `playing()` and therefore
  /// returned before it could decide either, which is why looping a range that
  /// ran to the end of the sequence sat on the last frame for ever.
  bool playback_wanted = false;

  /// Whether there is still playback to advance: the user has asked for it, and
  /// either it is running or it has just finished and wants handling.
  [[nodiscard]] bool playback_live() const noexcept {
    return playback_wanted && player != nullptr && (player->playing() || player->finished());
  }

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
    /// 0 matches the sequence; after that, the working spaces in order.
    std::size_t colour = 0;
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
  /// Whether any window has something fading, which keeps the loop turning
  /// until it settles.
  [[nodiscard]] bool animating() noexcept;
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
    // Included so it is painted and invalidated with the rest, and kept out of
    // `floats` so the layout reconciler does not treat it as a torn-out panel
    // and close it. Leaving it out is what made the settings window draw once
    // and then never again: its category buttons worked, and nothing on screen
    // ever said so.
    if (settings_window != nullptr) all.push_back(settings_window.get());
    if (keyboard_window != nullptr) all.push_back(keyboard_window.get());
#if CUTLINE_HAVE_PREVIEW
    if (export_window != nullptr) all.push_back(export_window.get());
#endif
    return all;
  }
};

// ----------------------------------------------------- shared tables --

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

/// The rates worth offering, which are the ones cameras and screens produce.
constexpr std::array<double, 7> kFrameRates{23.976, 24.0, 25.0, 29.97, 30.0, 50.0, 60.0};


/// The panels the window can show, and what their tabs say.
///
/// A panel is a name from here plus whatever `make_panel` builds for it. The
/// layout only ever moves the name around, which is what lets an arrangement be
/// saved and restored without saving any widgets.
constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kPanels{{
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
    // Premiere's undo list, every step by name. Undo and redo walk it one step
    // at a time; this is for going back ten, or for seeing what the last few
    // presses actually did.
    {"history", "History"},
}};

/// The tool palette, in the order it is drawn and offered, and the action in
/// the keymap that picks each -- whose keys are Premiere's unless somebody
/// changes them.
struct ToolEntry {
  cutline::ui::Tool tool;
  IconButton::Icon icon;
  std::string_view action;
  /// What it is called, in the menu and in its tooltip beside its key.
  const char* name;
};

constexpr std::array kTools{
    ToolEntry{cutline::ui::Tool::Selection, IconButton::Icon::Pointer, "tool.selection",
              "Selection"},
    // Premiere's B and N, in Premiere's order: the two edge tools sit together
    // between the pointer and the razor, because they are variations on the
    // trim the pointer already does.
    ToolEntry{cutline::ui::Tool::Ripple, IconButton::Icon::Ripple, "tool.ripple", "Ripple Edit"},
    ToolEntry{cutline::ui::Tool::Roll, IconButton::Icon::Roll, "tool.roll", "Rolling Edit"},
    ToolEntry{cutline::ui::Tool::Razor, IconButton::Icon::Razor, "tool.razor", "Razor"},
    ToolEntry{cutline::ui::Tool::RateStretch, IconButton::Icon::RateStretch, "tool.rate_stretch",
              "Rate Stretch"},
    ToolEntry{cutline::ui::Tool::Slip, IconButton::Icon::Slip, "tool.slip", "Slip"},
    ToolEntry{cutline::ui::Tool::Slide, IconButton::Icon::Slide, "tool.slide", "Slide"},
};

/// Where a popup opens under the menu bar, at its left edge.
///
/// Not at the pointer, and not over the bar it came from — a panel that covers
/// the menu it was opened from hides the thing you would click to close it.
[[nodiscard]] constexpr Rect settings_anchor() noexcept { return Rect{8.0, 72.0, 0.0, 0.0}; }

// ----------------------------------------------- calls between the parts --

// The window's own helpers, defined in `main.cpp`.
[[nodiscard]] double scale_of(HWND window);
/// Asks the system for its glass behind a window when the theme's frame is
/// glass and the window can show it, and for none otherwise.
void apply_backdrop(Shell& shell, const Theme& theme);
/// What every window of the application is made with: no redirection bitmap,
/// so what a frame leaves clear reaches the screen clear.
constexpr DWORD kWindowExStyle = WS_EX_NOREDIRECTIONBITMAP;
[[nodiscard]] int to_pixels(double units, double scale);
[[nodiscard]] DockLayout& layout_of(App& app);
void mark_dirty(App& app);
[[nodiscard]] std::string canvas_label(const cutline::core::Project& project);
void apply_canvas(App& app, int width, int height);
void apply_fps(App& app, double fps);
[[nodiscard]] std::string fps_label(double fps);

[[nodiscard]] std::optional<std::filesystem::path> choose_movie_file(HWND owner);

// The Export window, in `export_window.cpp`.
void close_export_dialog(App& app);
[[nodiscard]] std::unique_ptr<Widget> build_export_interface(App* app, Shell* shell);
/// Its size as designed, which `--check` lays it out at too: sized by guess,
/// it once left its own Export button below its bottom edge.
constexpr int kExportWindowWidth = 460;
constexpr int kExportWindowHeight = 520;

// Menu rows, defined in `main.cpp`.
/// A row that runs something, with the keys that do the same written beside
/// it. Only words: whoever handles the keys is elsewhere.
[[nodiscard]] MenuEntry action_entry(std::string label, std::function<void()> act,
                                     std::string keys = {}, bool enabled = true);
/// A row that is a switch, ticked when it is on.
[[nodiscard]] MenuEntry switch_entry(std::string label, bool on, std::function<void()> act,
                                     std::string keys = {}, bool enabled = true);
/// A row that opens another beside it, greyed when that would be empty.
[[nodiscard]] MenuEntry submenu_entry(std::string label, std::vector<MenuEntry> rows);
[[nodiscard]] MenuEntry command_entry(App& app, std::string label,
                                      cutline::editor::Command command);
/// The key an action is on, as a menu writes it, or empty. Premiere's keys
/// when there is no application, which is the headless check building panels.
[[nodiscard]] std::string shortcut_of(const App* app, std::string_view action);
[[nodiscard]] std::string shortcut_for(const App* app, cutline::editor::Command command);
/// A tooltip's text with the action's key after it in brackets, when it has
/// one -- so the tooltip says what the key is now, not what it was.
[[nodiscard]] std::string with_keys(const App* app, std::string text, std::string_view action);
/// Runs what a key's action does. `shell` is the window the key arrived in.
void run_action(App& app, Shell& shell, std::string_view action);
/// Writes the keymap to keys.json.
void save_keys(App& app);
/// Premiere's Keyboard Shortcuts window, in `keyboard_window.cpp`.
void open_keyboard_dialog(App& app);
void close_keyboard_dialog(App& app);
void refresh_keyboard_dialog(App& app);
[[nodiscard]] std::unique_ptr<Widget> build_keyboard_interface(App& app, Shell* shell);
[[nodiscard]] std::vector<MenuEntry> clip_entries(App& app);

// What the menus run, defined in `main.cpp`.
[[nodiscard]] double next_shuttle(double current, double direction) noexcept;
void choose_tool(App& app, cutline::ui::Tool tool);
void set_workspace(App& app, std::string_view name);
void reset_workspace(App& app);
void open_project(App& app);
void open_fit_dialog(App& app);
void new_sequence(App& app);
void new_project(App& app);
[[nodiscard]] bool panel_is_open(App& app, const PanelId& panel);
void toggle_panel(App& app, const PanelId& panel);

// The menu bar's menus, in `menus.cpp`.
[[nodiscard]] std::vector<MenuEntry> file_menu(App& app);
[[nodiscard]] std::vector<MenuEntry> edit_menu(App& app);
[[nodiscard]] std::vector<MenuEntry> clip_menu(App& app);
[[nodiscard]] std::vector<MenuEntry> sequence_menu(App& app);
[[nodiscard]] std::vector<MenuEntry> markers_menu(App& app);
[[nodiscard]] std::vector<MenuEntry> view_menu(App& app);
[[nodiscard]] std::vector<MenuEntry> window_menu(App& app);
[[nodiscard]] std::vector<MenuEntry> help_menu(App& app);
/// The Window menu on its own, for `--check`, which measures its ticks.
[[nodiscard]] std::unique_ptr<Widget> build_window_menu(App& app);

// The window, defined in `main.cpp`.
[[nodiscard]] cutline::core::Project check_project();
[[nodiscard]] DockLayout default_layout();
[[nodiscard]] std::unique_ptr<Widget> build_interface(App* app);
void refresh_inspector(App& app);
void show_playback_frame(App& app, double at);
void run_command(App& app, cutline::editor::Command command);
void open_speed_dialog(App& app, std::span<const std::string> clips);
void open_marker_dialog(App& app, std::size_t index);
[[nodiscard]] sk_sp<SkSurface> draw_cursor_art(cutline::ui::IconButton::Icon icon,
                                               double scale = 1.0);

// The inspector, and what it shares with the timeline and the library, in
// `inspector.cpp`.
void follow_playhead(App& app);
void scroll_to_playhead(App& app);
void keep_bins(App& app);
[[nodiscard]] std::vector<cutline::ui::EffectEntry> library_entries(const App& app);
void refresh_library(App& app);

// The Audio Master panel and the audio dialogs, in `audio_panel.cpp`.
[[nodiscard]] std::unique_ptr<Widget> make_audio_panel(App* app);

// The project panel's pool, in `pool.cpp`.
void remove_from_pool(App& app);
void sort_by_column(App& app, cutline::ui::MediaColumn column);
void place_from_pool(App& app, std::size_t index,
                     std::optional<cutline::ui::DropPoint> where = std::nullopt);
void place_media_from(App& app, const std::string& media_id,
                      std::optional<cutline::ui::DropPoint> where = std::nullopt);

// The monitors and playback, in `playback.cpp`.
void refresh_preview(App& app);
void advance_playback(App& app);
/// The zoom dropdown that goes under a monitor's picture.
void add_zoom_control(Box& transport, MonitorView& picture, double chosen,
                      std::function<void(double)> on_choose);

// The panels, in `panels.cpp`.
/// What fills a panel with this name in the layout.
[[nodiscard]] std::unique_ptr<Widget> make_panel(App* app, const PanelId& id);
void refresh_history(App& app);
void new_bin(App& app);

// Defined in `main.cpp`, for the panels.
void set_pool_view(App& app, cutline::ui::BrowserView view);
void go_to_time(App& app, double at);

// The document's recovery copies, in `document.cpp`.
void poll_autosave(App& app);
void clear_autosave(App& app, const std::filesystem::path& document);
[[nodiscard]] bool offer_recovery(App& app, const std::filesystem::path& document);

// The dock and the workspaces, in `dock.cpp`.
void wire_dock(App* app, Shell* shell, DockView& dock);
void return_panels_home(App& app, const std::string& window_id);
void float_panel_under_cursor(App& app, Shell& shell);
void next_workspace(App& app);
void save_workspaces(App& app, bool complain_on_failure);

// Defined in `main.cpp`, for the dock.
[[nodiscard]] std::string panel_title(const PanelId& id);
[[nodiscard]] std::pair<double, double> to_units(const Shell& shell, POINT local);

// The headless check and the benchmarks, in `check.cpp`.
[[nodiscard]] int self_check(std::string_view shots = {});
[[nodiscard]] int benchmark();
[[nodiscard]] int benchmark_playback(const std::filesystem::path& path);

// The settings windows, in `settings_window.cpp`.
[[nodiscard]] std::vector<SettingsPage> pages_for(SettingsKind kind);
[[nodiscard]] std::unique_ptr<Widget> build_settings_interface(App& app, Shell* shell,
                                                              SettingsKind kind);

void set_theme(App& app, std::size_t index);
void open_paste_attributes_dialog(App& app);
void open_interpret_dialog(App& app, const std::string& media_id);
void show_playhead(App& app);
void save_settings(App& app);
[[nodiscard]] std::pair<int, int> dialog_size(const App& app, const std::string& name, int width,
                                              int height);
void remember_dialog_size(App& app, const Shell& shell);
void refresh_timeline(App& app);
void refresh_busy(App& app);
void refresh_drop_ghost(App& app);
void relink_pool_entry(App& app);
void rename_pool_entry(App& app);
void add_bin(App& app);
void toggle_bin(App& app, std::size_t row);
void file_into_bin(App& app, std::size_t from, std::size_t onto);
void open_pool_menu(App& app, double x, double y);
[[nodiscard]] cutline::core::Project labelled_new_media(const App& app,
                                                        cutline::core::Project project,
                                                        std::span<const std::string> media_ids);
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
void refresh_audio_panel(App& app);
void invalidate_preview(App& app);
void poll_preview(App& app);
void poll_source(App& app);
void invalidate_scopes(App& app);
void refresh_scopes(App& app);
void choose_scope(App& app, cutline::ui::ScopeKind kind);
void show_master_gain(App& app, double gain);
void refresh_meter(App& app);
void record_automation(App& app);
void open_channel_dialog(App& app, std::span<const std::string> clips);
void open_essential_sound(App& app, std::span<const std::string> clips);
void open_loudness_dialog(App& app);
void commit_automation(App& app);
void refresh_handles(App& app);
void refresh_title(App& app);
void show_snapping(App& app);
void toggle_snapping(App& app);
void choose_preview_scale(App& app, double scale);
void invalidate_playback(App& app);
void rebuild_playback(App& app);
void open_export_dialog(App& app);
void apply_media_cache(App& app);
void open_settings_dialog(App& app, SettingsKind kind);
void close_settings_dialog(App& app);
void refresh_settings_page(App& app);
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
void play_sound(const App& app, cutline::ui::Sound sound);
void give_sounds(App& app, WidgetHost& host);
[[nodiscard]] std::optional<std::filesystem::path> choose_image_file(HWND owner);
[[nodiscard]] std::optional<std::filesystem::path> choose_folder(HWND owner);
void take_snapshot(App& app);
void check_for_updates(App& app);
void settle_update(App& app);
[[nodiscard]] bool confirm_discard(App& app);
bool save_project(App& app, bool ask_where);

}  // namespace cutline::tool
