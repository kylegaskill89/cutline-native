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

#include "cutline/core/time.hpp"
#include "cutline/ui/controls.hpp"
#include "cutline/ui/skia_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/timeline.hpp"
#include "cutline/ui/widget.hpp"
#include "cutline/ui/widgets.hpp"

#include <windows.h>
// Both of these need windows.h first: one for the message parameters it
// defines, the other for the types in its own signatures.
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

#include <cstddef>
#include <memory>
#include <print>
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
using cutline::ui::TimelineBlock;
using cutline::ui::TimelineModel;
using cutline::ui::TimelineTrack;
using cutline::ui::TimelineView;
using cutline::ui::TimeScale;
using cutline::ui::TitleBar;
using cutline::ui::ValueRange;
using cutline::ui::WheelEvent;
using cutline::ui::Widget;
using cutline::ui::WidgetHost;
using cutline::ui::built_in_themes;

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

  std::size_t theme = 0;
  bool dirty = true;

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

/// The window's own widget tree: a switcher along the top, a browser down the
/// left, a monitor and a timeline stacked on the right. Enough of the editor's
/// shape to tell whether a theme holds together across it.
///
/// Something for the timeline to show. Invented rather than loaded: the point
/// is to see how a theme handles a wall of clips, not to decode anything.
[[nodiscard]] TimelineModel sample_timeline() {
  TimelineModel model;
  model.fps = 30.0;

  const auto run = [](double from, double to, std::string label) {
    return TimelineBlock{.start = from, .end = to, .label = std::move(label)};
  };

  model.tracks = {
      TimelineTrack{.name = "V2", .blocks = {run(6.0, 11.5, "title"), run(24.0, 29.0, "lower third")}},
      TimelineTrack{.name = "V1",
                    .blocks = {run(0.0, 6.2, "wide"), run(6.2, 14.0, "close"),
                               run(14.0, 22.5, "cutaway"), run(22.5, 38.0, "walk out")}},
      TimelineTrack{.name = "A1",
                    .audio = true,
                    .blocks = {run(0.0, 22.5, "dialogue"), run(22.5, 38.0, "room tone")}},
      TimelineTrack{
          .name = "A2", .audio = true, .muted = true, .blocks = {run(2.0, 38.0, "score")}},
  };
  return model;
}

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
  auto& transport = monitor.emplace<Box>(Axis::Horizontal);
  transport.emplace<Button>("Mark In");
  transport.emplace<Button>("Mark Out");
  transport.emplace<Spacer>();
  transport.emplace<Button>("Play");
  transport.emplace<Button>("Export");

  auto& timeline = right.emplace<Panel>("Timeline");
  auto& tools = timeline.emplace<Box>(Axis::Horizontal);
  tools.emplace<Button>("Select");
  tools.emplace<Button>("Razor");
  tools.emplace<Button>("Slip");
  tools.emplace<Spacer>();
  auto& readout = tools.emplace<Label>("00:00:00:00");
  readout.set_align(cutline::ui::TextAlign::Right);

  auto& tracks = timeline.emplace<TimelineView>();
  tracks.set_model(sample_timeline());
  tracks.set_scale(TimeScale{.pixels_per_second = 60.0});
  // Enough to see the readout follow a scrub, which is the point of having it.
  tracks.set_on_scrub([&readout, fps = sample_timeline().fps](double at) {
    readout.set_text(cutline::core::seconds_to_timecode(at, fps));
  });

  // The inspector, which is what the value controls are for. Each row is a
  // label above the control, the way an effect's parameters are laid out.
  auto& inspector = middle.emplace<Panel>("Effect Controls");
  const auto parameter = [&inspector](std::string name, ValueRange range, double value,
                                      double fallback) -> Slider& {
    inspector.emplace<Label>(std::move(name)).set_small(true);
    auto& slider = inspector.emplace<Slider>(range, value);
    slider.set_default_value(fallback);
    return slider;
  };

  parameter("Opacity", ValueRange{.minimum = 0.0, .maximum = 100.0}, 100.0, 100.0);
  parameter("Scale", ValueRange{.minimum = 0.0, .maximum = 400.0}, 100.0, 100.0);
  parameter("Rotation", ValueRange{.minimum = -180.0, .maximum = 180.0}, 0.0, 0.0);
  parameter("Feather", ValueRange{.minimum = 0.0, .maximum = 100.0, .step = 5.0}, 20.0, 0.0);

  inspector.emplace<Checkbox>("Reverse", false);
  inspector.emplace<Checkbox>("Preserve pitch", true);
  inspector.emplace<Spacer>();

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
      const KeyEvent event{.key = key_from_win32(wparam),
                           .modifiers = modifiers_now(),
                           .repeat = (lparam & (1 << 30)) != 0};
      if (event.key == Key::Tab) {
        app->host->focus_next(event.modifiers.shift);
      } else {
        app->host->key_down(event);
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
  SetWindowTextA(window, ("Cutline - " + app.current().name).c_str());

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
