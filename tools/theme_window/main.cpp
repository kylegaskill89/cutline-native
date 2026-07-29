/// A window to actually look at the interface in.
///
/// Everything under `src/ui` is tested without a window on purpose, and that
/// catches a great deal — but not whether the result is something a person
/// would want to use. This puts the real widget tree on screen, in each of the
/// built-in themes, so the claim that they differ in *chrome* rather than in
/// colour can be checked by looking at it.
///
/// The chrome is drawn by Skia into a raster surface and blitted with GDI. That
/// is deliberate and not a placeholder to feel bad about: interface drawing is
/// a few hundred rounded rectangles a frame, it only happens when something
/// changes, and going through the GPU for it would mean sharing a device and a
/// swapchain with the compositor for no measurable gain. Video frames have
/// their own path already.
///
/// Not the editor. A harness for the parts of it that exist.

#include "cutline/core/edit.hpp"
#include "cutline/core/model.hpp"
#include "cutline/core/time.hpp"
#include "cutline/editor/commands.hpp"
#include "cutline/editor/document.hpp"
#include "cutline/editor/inspector.hpp"
#include "cutline/editor/session.hpp"
#include "cutline/editor/timeline_binding.hpp"
#include "cutline/ui/controls.hpp"
#include "cutline/ui/monitor.hpp"
#include "cutline/ui/skia_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/timeline.hpp"
#include "cutline/ui/widget.hpp"
#include "cutline/ui/widgets.hpp"

#if CUTLINE_HAVE_PREVIEW
#include "cutline/app/preview.hpp"
#endif

#include <windows.h>
// Both of these need windows.h first: one for the message parameters it
// defines, the other for the types in its own signatures.
#include <commdlg.h>
#include <dwmapi.h>
#include <windowsx.h>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
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
using cutline::ui::Key;
using cutline::ui::KeyEvent;
using cutline::ui::Label;
using cutline::ui::LayoutContext;
using cutline::ui::Modifiers;
using cutline::ui::MonitorView;
using cutline::ui::MouseButton;
using cutline::ui::MouseEvent;
using cutline::ui::Panel;
using cutline::ui::Rect;
using cutline::ui::ScrollView;
using cutline::ui::SkiaPainter;
using cutline::ui::Slider;
using cutline::ui::Spacer;
using cutline::ui::Splitter;
using cutline::ui::Theme;
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

struct App {
  std::unique_ptr<WidgetHost> host;
  /// The switcher's buttons, so the selected one can be moved without
  /// rebuilding the tree. Rebuilding would destroy the button whose click is
  /// still on the stack.
  std::vector<Button*> theme_buttons;
  /// Asked which parts of the caption are draggable, so the buttons in it stay
  /// clickable while the rest of it moves the window.
  TitleBar* title_bar = nullptr;
  CaptionButton* maximise = nullptr;
  HWND window = nullptr;

  /// The document being edited. Everything the timeline shows is derived from
  /// it, and everything a drag does goes back through it.
  cutline::editor::Session session{sample_project()};
  TimelineView* timeline = nullptr;
  Label* readout = nullptr;
  /// The column the parameter controls live in, inside the inspector's scroll
  /// view. A `Box` rather than the panel, so clearing it does not take the
  /// scrolling with it.
  Box* inspector = nullptr;
  MonitorView* monitor = nullptr;
  /// Shown when there is nothing to decode — which is always under the skia
  /// preset, and until something is imported under the ui one.
  TestPattern pattern;

#if CUTLINE_HAVE_PREVIEW
  /// Made on first use rather than at startup: creating a Direct3D device
  /// costs a moment, and a window that only ever shows chrome should not pay
  /// for one.
  std::unique_ptr<cutline::app::ProjectPreview> preview;
  bool preview_failed = false;
  /// Set when the picture no longer matches the playhead or the project.
  bool preview_stale = true;
#endif

  std::size_t theme = 0;
  bool dirty = true;
  /// Set when the inspector needs rebuilding, and acted on at the next render.
  ///
  /// Deferred rather than done on the spot because the thing asking for it is
  /// usually a slider *inside* the inspector, and rebuilding would destroy
  /// that slider while its own callback was still running.
  bool inspector_stale = true;

  /// Rebuilt whenever the window changes size. Skia's raster surface owns the
  /// pixels; the blit reads them straight out.
  sk_sp<SkSurface> surface;
  int width = 0;
  int height = 0;
  /// Set by `WM_SIZE`, cleared by the next render. Laying out on the message
  /// itself would need a painter to measure with, and there is no canvas at
  /// that point — the same reason input defers layout.
  bool resized = true;

  [[nodiscard]] const Theme& current() const { return built_in_themes()[theme]; }
};

void set_theme(App& app, std::size_t index);
void refresh_timeline(App& app);
void invalidate_preview(App& app);
void complain(HWND owner, const std::string& message);

/// Rebuilds the inspector for whatever is selected.
///
/// A loop over `clip_parameters` rather than a hand-built form, so a property
/// added to the model turns up here without anyone remembering to come and add
/// a control for it.
void refresh_inspector(App& app) {
  if (app.inspector == nullptr || app.host == nullptr) return;

  // Rebuilt from nothing each time. `clear_children` tells the host to drop
  // hover, focus and capture first, so a slider that was mid-drag when the
  // selection changed is not freed underneath the drag.
  app.inspector->clear_children();

  const auto selection = app.session.selection();
  if (selection.empty()) {
    app.inspector->emplace<Label>("Nothing selected").set_small(true);
    app.inspector->emplace<Spacer>();
    app.host->request_layout();
    app.dirty = true;
    return;
  }

  const std::string clip_id{selection.front()};
  for (const cutline::editor::ParamSpec& spec :
       cutline::editor::clip_parameters(app.session.project(), clip_id)) {
    app.inspector->emplace<Label>(spec.name).set_small(true);

    auto& slider = app.inspector->emplace<Slider>(spec.range, spec.value);
    slider.set_default_value(spec.fallback);

    // On commit rather than on change: the control follows the pointer as it
    // goes, and the project is written once, at the end of the gesture.
    slider.set_on_commit([&app, clip_id, param = spec.param](double value) {
      app.session.apply(cutline::editor::set_clip_parameter(app.session.project(), clip_id,
                                                            param, value));
      refresh_timeline(app);
      invalidate_preview(app);
      // Marked, not rebuilt: this lambda belongs to the slider that a rebuild
      // would destroy. It has to be rebuilt though — the model may have
      // clamped the value, and a speed change alters the clip's length, which
      // moves the bounds of both fade controls.
      app.inspector_stale = true;
    });
  }
  app.inspector->emplace<Spacer>();

  app.host->request_layout();
  app.dirty = true;
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
  app.dirty = true;
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
    auto made = cutline::app::ProjectPreview::create(project.canvas_w, project.canvas_h);
    if (!made.has_value()) {
      // Once, and then never again: a machine with no usable device will not
      // acquire one, and complaining on every scrub would be unbearable.
      app.preview_failed = true;
      complain(app.window, "Preview is unavailable.\n\n" + made.error());
      return;
    }
    app.preview = std::move(*made);
  }

  const auto frame = app.preview->frame_at(project, app.session.playhead());
  if (!frame.has_value()) {
    app.preview_failed = true;
    complain(app.window, "Could not render the preview.\n\n" + frame.error());
    return;
  }

  app.monitor->set_frame(*frame);
  app.monitor->set_canvas_aspect(static_cast<double>(project.canvas_w) / project.canvas_h);
  app.dirty = true;
#else
  (void)app;
#endif
}

/// Marks the picture as no longer matching the playhead or the project.
void invalidate_preview(App& app) {
#if CUTLINE_HAVE_PREVIEW
  app.preview_stale = true;
  app.dirty = true;
#else
  (void)app;
#endif
}

/// Puts the document's name where it can be seen — in the caption the theme
/// draws, and in the window text the taskbar reads.
void refresh_title(App& app) {
  const std::string title = app.session.document_title() + " - Cutline";
  if (app.title_bar != nullptr) app.title_bar->set_title(title);
  if (app.window != nullptr) SetWindowTextA(app.window, title.c_str());
  app.dirty = true;
}

/// Everything a view needs told after the document has been replaced.
void refresh_all(App& app) {
  refresh_timeline(app);
  refresh_title(app);
  invalidate_preview(app);
  app.inspector_stale = true;
  if (app.monitor != nullptr) {
    const cutline::core::Project& project = app.session.project();
    app.monitor->set_canvas_aspect(static_cast<double>(project.canvas_w) / project.canvas_h);
  }
  if (app.host != nullptr) app.host->request_layout();
}

/// Brings a file into the project and drops it at the playhead.
void import_media(App& app) {
#if CUTLINE_HAVE_PREVIEW
  std::array<wchar_t, MAX_PATH> buffer{};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = app.window;
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
    complain(app.window, "Could not read that file.\n\n" + source.error());
    return;
  }

  // At the playhead, which is where an editor expects a drop to land.
  app.session.apply(cutline::editor::import_and_place(app.session.project(), *source,
                                                      app.session.playhead()));
  refresh_all(app);
#else
  complain(app.window, "This build has no media layer, so there is nothing to import with.");
#endif
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

void complain(HWND owner, const std::string& message) {
  MessageBoxA(owner, message.c_str(), "Cutline", MB_OK | MB_ICONWARNING);
}

void open_project(App& app) {
  const auto path = choose_file(app.window, false);
  if (!path.has_value()) return;

  const auto loaded = cutline::editor::read_project(*path);
  if (!loaded.has_value()) {
    complain(app.window, "Could not open that project.\n\n" + loaded.error());
    return;
  }

  app.session.reset(loaded->project, *path);
  refresh_all(app);

  // Warnings are not failures: a project whose footage has moved still opens,
  // and saying so is more use than refusing it.
  if (!loaded->warnings.empty()) {
    std::string message = "The project opened with warnings:\n";
    for (const std::string& warning : loaded->warnings) message += "\n" + warning;
    complain(app.window, message);
  }
}

/// Returns whether it was written, so a cancelled dialog is not mistaken for
/// a successful save.
bool save_project(App& app, bool ask_where) {
  std::filesystem::path path = app.session.path();
  if (ask_where || path.empty()) {
    const auto chosen = choose_file(app.window, true);
    if (!chosen.has_value()) return false;
    path = cutline::editor::with_project_extension(*chosen);
  }

  const auto written = cutline::editor::write_project(path, app.session.project());
  if (!written.has_value()) {
    complain(app.window, "Could not save the project.\n\n" + written.error());
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

/// Runs whichever command the key is bound to, if any.
bool run_binding(App& app, std::span<const Binding> bindings, Key key,
                 const Modifiers& modifiers) {
  for (const Binding& binding : bindings) {
    if (binding.key != key) continue;
    if (binding.control != modifiers.control || binding.shift != modifiers.shift) continue;

    // The command decides whether it applies. Nothing here needs to know when
    // a razor has anything to cut.
    if (cutline::editor::run(app.session, binding.command)) {
      refresh_timeline(app);
      refresh_title(app);
      invalidate_preview(app);
      app.inspector_stale = true;
    }
    app.dirty = true;
    return true;
  }
  return false;
}

/// The window's own widget tree: a switcher along the top, a browser down the
/// left, a monitor and a timeline stacked on the right. Enough of the editor's
/// shape to tell whether a theme holds together across it.
///
/// `app` may be null, so the headless check can build the same tree with no
/// window behind it.
[[nodiscard]] std::unique_ptr<Widget> build_interface(App* app) {
  auto shell = std::make_unique<Box>(Axis::Vertical);
  shell->set_spacing(0.0);

  // The window's own caption. The system one cannot be themed, so it is turned
  // off in `WM_NCCALCSIZE` and this is drawn in its place.
  auto& caption = shell->emplace<TitleBar>("Cutline");
  if (app != nullptr) app->title_bar = &caption;

  caption.emplace<CaptionButton>(CaptionButton::Kind::Minimise, [app] {
    if (app != nullptr && app->window != nullptr) ShowWindow(app->window, SW_MINIMIZE);
  });
  auto& maximise = caption.emplace<CaptionButton>(CaptionButton::Kind::Maximise, [app] {
    if (app == nullptr || app->window == nullptr) return;
    ShowWindow(app->window, IsZoomed(app->window) ? SW_RESTORE : SW_MAXIMIZE);
  });
  caption.emplace<CaptionButton>(CaptionButton::Kind::Close, [app] {
    if (app != nullptr && app->window != nullptr) PostMessageW(app->window, WM_CLOSE, 0, 0);
  });
  if (app != nullptr) app->maximise = &maximise;

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

  auto root = std::make_unique<Splitter>(Axis::Horizontal);
  root->set_fractions({0.28, 0.72});

  auto& browser = root->emplace<Panel>("Project");
  auto& list = browser.emplace<ScrollView>(Axis::Vertical);
  auto clips = std::make_unique<Box>(Axis::Vertical);
  for (int i = 1; i <= 24; ++i) {
    clips->emplace<Button>("Clip " + std::to_string(i));
  }
  list.set_content(std::move(clips));

  auto& middle = root->emplace<Splitter>(Axis::Horizontal);
  middle.set_fractions({0.72, 0.28});

  auto& right = middle.emplace<Splitter>(Axis::Vertical);
  right.set_fractions({0.55, 0.45});

  auto& monitor = right.emplace<Panel>("Program Monitor");
  auto& picture = monitor.emplace<MonitorView>();
  if (app != nullptr) {
    app->monitor = &picture;
    picture.set_frame(app->pattern.view());
  }

  auto& transport = monitor.emplace<Box>(Axis::Horizontal);
  transport.emplace<Button>("Mark In");
  transport.emplace<Button>("Mark Out");
  transport.emplace<Spacer>();
  transport.emplace<Button>("Play");
  transport.emplace<Button>("Export");

  auto& timeline = right.emplace<Panel>("Timeline");
  auto& tools = timeline.emplace<Box>(Axis::Horizontal);
  tools.emplace<Button>("Undo", [app] {
    if (app == nullptr || !app->session.undo()) return;
    refresh_timeline(*app);
    app->inspector_stale = true;
  });
  tools.emplace<Button>("Redo", [app] {
    if (app == nullptr || !app->session.redo()) return;
    refresh_timeline(*app);
    app->inspector_stale = true;
  });
  tools.emplace<Spacer>();
  auto& readout = tools.emplace<Label>("00:00:00:00");
  readout.set_align(cutline::ui::TextAlign::Right);
  if (app != nullptr) app->readout = &readout;

  auto& tracks = timeline.emplace<TimelineView>();
  tracks.set_scale(TimeScale{.pixels_per_second = 60.0});
  if (app != nullptr) app->timeline = &tracks;

  tracks.set_on_scrub([app](double at) {
    if (app == nullptr) return;
    app->session.set_playhead(at);
    if (app->readout != nullptr) {
      app->readout->set_text(
          cutline::core::seconds_to_timecode(app->session.playhead(), app->session.project().fps));
    }
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
  tracks.set_on_edit([app](cutline::ui::BlockRef ref, cutline::ui::DragMode mode,
                           cutline::ui::TimelineBlock block) {
    if (app == nullptr || app->timeline == nullptr) return;
    const auto id = cutline::editor::block_clip_id(app->timeline->model(), ref);
    if (!id.has_value()) return;

    app->session.apply(cutline::editor::apply_timeline_edit(app->session.project(), *id, mode,
                                                            block.start, block.end));
    // Rebuilt whether or not the edit applied: when it did not, the view is
    // showing where the pointer went rather than where the clip is allowed to
    // be, and it has to snap back.
    refresh_timeline(*app);
  });

  if (app != nullptr) refresh_timeline(*app);

  // The inspector. Its contents are rebuilt from whatever is selected, so all
  // that is built here is somewhere to put them — inside a scroll view,
  // because a clip has more parameters than fit in a panel.
  auto& inspector_panel = middle.emplace<Panel>("Effect Controls");
  auto& inspector_scroll = inspector_panel.emplace<ScrollView>(Axis::Vertical);
  auto& rows = inspector_scroll.set_content(std::make_unique<Box>(Axis::Vertical));
  if (app != nullptr) app->inspector = static_cast<Box*>(&rows);

  shell->add(std::move(root));
  return shell;
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

void resize_surface(App& app, int width, int height) {
  if (width <= 0 || height <= 0) return;
  if (app.surface != nullptr && width == app.width && height == app.height) return;

  app.width = width;
  app.height = height;
  app.surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
}

/// Draws the tree and puts the pixels on the window.
void render(App& app, HWND window) {
  if (app.surface == nullptr) return;

  SkCanvas* canvas = app.surface->getCanvas();
  const Theme& theme = app.current();

  const std::unique_ptr<SkiaPainter> painter = SkiaPainter::create(canvas);
  if (painter == nullptr) return;

  const Rect client{0.0, 0.0, static_cast<double>(app.width), static_cast<double>(app.height)};
  const LayoutContext context{theme, *painter};

  // Rebuilt here for the same reason layout is: whatever asked for it was
  // usually a control inside the panel being rebuilt.
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

  // The one place a theme and a text measurer are both in hand, which is
  // exactly why layout is deferred to here rather than done where the size
  // changed or the drag happened.
  if (app.resized) {
    app.host->resize(client, context);
    app.resized = false;
  } else {
    app.host->update_layout(context);
  }

  // Through the painter rather than a canvas clear, so a themed background
  // that is a gradient stays one.
  canvas->clear(SK_ColorBLACK);
  paint_surface(*painter, client, theme.style(cutline::ui::Part::Window));
  app.host->paint(*painter, theme);

  SkPixmap pixels;
  if (!app.surface->peekPixels(&pixels)) return;

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = app.width;
  // Negative, because Skia's rows run top to bottom and a DIB's run the other
  // way by default. Without this the whole interface comes out upside down.
  info.bmiHeader.biHeight = -app.height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  const HDC dc = GetDC(window);
  StretchDIBits(dc, 0, 0, app.width, app.height, 0, 0, app.width, app.height, pixels.addr(),
                &info, DIB_RGB_COLORS, SRCCOPY);
  ReleaseDC(window, dc);
}

void set_theme(App& app, std::size_t index) {
  if (index >= built_in_themes().size() || index == app.theme) return;
  app.theme = index;

  for (std::size_t i = 0; i < app.theme_buttons.size(); ++i) {
    app.theme_buttons[i]->set_selected(i == index);
  }

  // Metrics change with the theme, so everything has to be measured again.
  app.host->request_layout();
  app.dirty = true;
  if (app.window != nullptr) {
    SetWindowTextA(app.window, ("Cutline - " + app.current().name).c_str());
  }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (app == nullptr || app->host == nullptr) {
    return DefWindowProcW(window, message, wparam, lparam);
  }

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
      if (app->title_bar != nullptr &&
          app->host->root().at(static_cast<double>(at.x), static_cast<double>(at.y)) ==
              app->title_bar) {
        return HTCAPTION;
      }
      return HTCLIENT;
    }

    case WM_SIZE:
      resize_surface(*app, LOWORD(lparam), HIWORD(lparam));
      app->resized = true;
      app->dirty = true;
      // Maximising swaps which glyph the middle caption button shows.
      if (app->maximise != nullptr) {
        app->maximise->set_kind(IsZoomed(window) ? CaptionButton::Kind::Restore
                                                 : CaptionButton::Kind::Maximise);
      }
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT paint;
      BeginPaint(window, &paint);
      render(*app, window);
      EndPaint(window, &paint);
      app->dirty = false;
      return 0;
    }

    case WM_MOUSEMOVE: {
      // Asked for once per entry, so WM_MOUSELEAVE arrives and hover clears
      // when the pointer goes somewhere else entirely.
      TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0};
      TrackMouseEvent(&track);
      app->host->mouse_move(mouse_from(lparam, MouseButton::Left));
      app->dirty = true;
      return 0;
    }
    case WM_MOUSELEAVE:
      app->host->mouse_exit();
      app->dirty = true;
      return 0;

    case WM_LBUTTONDOWN:
      SetCapture(window);
      app->host->mouse_down(mouse_from(lparam, MouseButton::Left));
      app->dirty = true;
      return 0;
    case WM_LBUTTONDBLCLK:
      app->host->mouse_down(mouse_from(lparam, MouseButton::Left, 2));
      app->dirty = true;
      return 0;
    case WM_LBUTTONUP:
      ReleaseCapture();
      app->host->mouse_up(mouse_from(lparam, MouseButton::Left));
      app->dirty = true;
      return 0;

    case WM_MOUSEWHEEL: {
      // Wheel messages carry screen coordinates, unlike every other mouse
      // message, so they have to be brought back into the client area.
      POINT at{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window, &at);
      const double notches =
          -static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
      app->host->wheel(WheelEvent{.x = static_cast<double>(at.x),
                                  .y = static_cast<double>(at.y),
                                  .delta_y = notches,
                                  .modifiers = modifiers_now()});
      app->dirty = true;
      return 0;
    }

    case WM_KEYDOWN: {
      if (wparam >= '1' && wparam <= '9') {
        set_theme(*app, static_cast<std::size_t>(wparam - '1'));
        return 0;
      }

      // Application shortcuts, taken before the widget tree sees them: undo is
      // not something any one control should be able to swallow.
      const Modifiers held = modifiers_now();
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
      const KeyEvent event{.key = key_from_win32(wparam),
                           .modifiers = held,
                           .repeat = (lparam & (1 << 30)) != 0};

      // Before the tree: nothing should be able to swallow undo.
      if (run_binding(*app, kApplicationKeys, event.key, held)) return 0;

      if (event.key == Key::Tab) {
        app->host->focus_next(event.modifiers.shift);
        app->dirty = true;
        return 0;
      }

      // After the tree, and only if it did not want the key. A focused slider
      // owns the arrows before the playhead does.
      if (!app->host->key_down(event)) {
        run_binding(*app, kTransportKeys, event.key, held);
      }
      app->dirty = true;
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;  // every pixel is painted, so erasing only causes a flash

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

/// Renders one frame of every theme with no window, and reports what came out.
///
/// The unit tests cover each control on its own; this covers the whole tree
/// laid out together, which is where a widget that demands more room than
/// exists or a container that hands out nothing would show up. Being able to
/// run it without a display also means a crash in the paint path is a command
/// away rather than something to notice by launching.
[[nodiscard]] int self_check() {
  constexpr int kWidth = 1280;
  constexpr int kHeight = 800;

  int failures = 0;
  std::vector<std::string> fingerprints;

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
    int counted = 0;
    const auto walk = [&](this const auto& self, const Widget& widget) -> void {
      ++counted;
      if (widget.bounds().empty()) ++empty;
      if (widget.bounds().x < -0.5 || widget.bounds().right() > kWidth + 0.5) ++escaped;
      for (const auto& child : widget.children()) self(*child);
    };
    walk(host.root());

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

    std::println("{:<10} {:>3} widgets, {} empty, {} outside the window", theme.id, counted,
                 empty, escaped);
    if (empty > 0 || escaped > 0) ++failures;
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
  if (argc > 1 && std::string_view(argv[1]) == "--check") return self_check();

  App app;
  app.host = std::make_unique<WidgetHost>(build_interface(&app));

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(WNDCLASSEXW);
  window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"CutlineThemeWindow";
  RegisterClassExW(&window_class);

  const HWND window =
      CreateWindowExW(0, window_class.lpszClassName, L"Cutline", WS_OVERLAPPEDWINDOW,
                      CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, instance,
                      nullptr);
  if (window == nullptr) {
    std::println("could not create a window");
    return 1;
  }

  app.window = window;
  SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));
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

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
    // Redrawn only when something actually changed. An interface that repaints
    // at sixty hertz while sitting still is heat for nothing, and the frames
    // that matter belong to the video.
    if (app.dirty) InvalidateRect(window, nullptr, FALSE);
  }
  return 0;
}
