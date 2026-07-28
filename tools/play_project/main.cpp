/// Plays a project: picture and sound, in a window, from the timeline.
///
/// Where `preview_window` drives the compositor by hand from a single file,
/// this is the real preview path — the same `FrameRenderer` an export uses,
/// driven by the same `AudioMixer`, with the sound card keeping time.
///
/// The loop is the point. It does not decide when the next frame is due; it
/// asks the player where the playhead is and renders that. Video is what
/// adapts, because it is the one that can: a repeated or dropped frame is
/// invisible at 60 Hz, where a gap of the same length in audio is a click.
///
///     play_project <project.json> [--start S] [--play]

#include "cutline/core/query.hpp"
#include "cutline/core/serialize.hpp"
#include "cutline/engine/frame_renderer.hpp"
#include "cutline/engine/player.hpp"
#include "cutline/gpu/presenter.hpp"

#include <windows.h>

#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <print>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using cutline::core::Project;
using cutline::engine::FrameRenderer;
using cutline::engine::Player;
using cutline::gpu::Device;
using cutline::gpu::Presenter;

struct AppState {
  std::shared_ptr<Device> device;
  std::unique_ptr<FrameRenderer> renderer;
  std::unique_ptr<Presenter> presenter;
  std::unique_ptr<Player> player;

  Project project;
  double last_drawn = -1.0;

  /// Whether the preview is keeping up. A frame drawn per playhead step means
  /// it is; fewer means the compositor is behind the sound card, which is the
  /// thing worth knowing about a preview and is invisible while watching it.
  long long frames_drawn = 0;
  double played_seconds = 0.0;

  /// Split so a slow preview can be attributed. These answer very different
  /// questions: compositing time is ours to improve, while present time is
  /// mostly waiting for the display's vertical blank.
  double render_seconds = 0.0;
  double present_seconds = 0.0;
};

/// Renders the project at `t` and puts it on screen.
void draw(AppState& state, double t) {
  using Clock = std::chrono::steady_clock;

  const auto before_render = Clock::now();
  if (auto ok = state.renderer->render(state.project, t); !ok) {
    std::println(stderr, "render failed at {:.3f}s: {}", t, ok.error());
    return;
  }
  const auto before_present = Clock::now();
  if (auto ok = state.presenter->present(state.renderer->compositor()); !ok) {
    std::println(stderr, "present failed: {}", ok.error());
  }
  const auto after = Clock::now();

  state.render_seconds += std::chrono::duration<double>(before_present - before_render).count();
  state.present_seconds += std::chrono::duration<double>(after - before_present).count();
  state.last_drawn = t;
}

void update_title(HWND window, const AppState& state) {
  const Player& player = *state.player;
  const std::string text =
      std::format("Cutline — {:.2f} / {:.2f}s   {}   {} Hz {}ch{}", player.position(),
                  player.duration(), player.playing() ? "playing" : "paused",
                  player.sample_rate(), player.channels(),
                  player.silent() ? "   (no audio)" : "");

  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(std::max(size - 1, 0)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
  SetWindowTextW(window, wide.c_str());
}

/// Moves the playhead by `delta` seconds, clamped to the timeline.
void nudge(AppState& state, double delta) {
  const double target =
      std::clamp(state.player->position() + delta, 0.0, state.player->duration());
  state.player->seek(target);
  draw(state, target);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (state == nullptr || state->player == nullptr) {
    return DefWindowProcW(window, message, wparam, lparam);
  }

  switch (message) {
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    case WM_SIZE: {
      const int width = LOWORD(lparam);
      const int height = HIWORD(lparam);
      if (width > 0 && height > 0) {
        if (auto ok = state->presenter->resize(width, height); !ok) {
          std::println(stderr, "resize failed: {}", ok.error());
        }
        draw(*state, std::max(0.0, state->last_drawn));
      }
      return 0;
    }

    case WM_KEYDOWN: {
      const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      const double step = shift ? 5.0 : 1.0;

      switch (wparam) {
        case VK_SPACE:
          if (state->player->playing()) {
            state->player->pause();
          } else {
            state->player->play();
          }
          break;
        case VK_LEFT:
          nudge(*state, -step);
          break;
        case VK_RIGHT:
          nudge(*state, step);
          break;
        case VK_HOME:
          state->player->seek(0.0);
          draw(*state, 0.0);
          break;
        case VK_END:
          state->player->seek(std::max(0.0, state->player->duration() - 0.1));
          draw(*state, state->player->position());
          break;
        case VK_ESCAPE:
          PostQuitMessage(0);
          break;
        default:
          break;
      }
      update_title(window, *state);
      return 0;
    }

    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::println(stderr, "usage: play_project <project.json> [--start S]");
    return 2;
  }

  double start = 0.0;
  bool autoplay = false;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
      start = std::strtod(argv[++i], nullptr);
    } else if (std::strcmp(argv[i], "--play") == 0) {
      autoplay = true;
    } else {
      std::println(stderr, "unknown option: {}", argv[i]);
      return 2;
    }
  }

  std::ifstream file(argv[1]);
  if (!file) {
    std::println(stderr, "cannot open {}", argv[1]);
    return 1;
  }
  std::ostringstream text;
  text << file.rdbuf();

  auto loaded = cutline::core::from_json(text.str());
  if (!loaded) {
    std::println(stderr, "cannot read the project: {}", loaded.error());
    return 1;
  }

  AppState state;
  state.project = std::move(loaded->project);

  const double timeline = cutline::core::timeline_duration(state.project);
  if (timeline <= 0.0) {
    std::println(stderr, "the timeline is empty");
    return 1;
  }

  const WNDCLASSEXW window_class{
      .cbSize = sizeof(WNDCLASSEXW),
      .lpfnWndProc = window_proc,
      .hInstance = GetModuleHandleW(nullptr),
      .hCursor = LoadCursorW(nullptr, IDC_ARROW),
      .lpszClassName = L"CutlinePlayer",
  };
  RegisterClassExW(&window_class);

  const int width = 1280;
  const int height = std::max(
      1, state.project.canvas_h * width / std::max(1, state.project.canvas_w));

  RECT bounds{0, 0, width, height};
  AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);

  HWND window = CreateWindowExW(0, window_class.lpszClassName, L"Cutline", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
                                bounds.bottom - bounds.top, nullptr, nullptr,
                                window_class.hInstance, nullptr);
  if (window == nullptr) {
    std::println(stderr, "cannot create a window");
    return 1;
  }

  auto device = Device::create();
  if (!device) {
    std::println(stderr, "cannot create a device: {}", device.error());
    return 1;
  }
  state.device = std::move(*device);

  auto renderer =
      FrameRenderer::create(state.device, state.project.canvas_w, state.project.canvas_h);
  if (!renderer) {
    std::println(stderr, "cannot create the renderer: {}", renderer.error());
    return 1;
  }
  state.renderer = std::move(*renderer);

  auto presenter = Presenter::create(state.device, window, width, height);
  if (!presenter) {
    std::println(stderr, "cannot create the presenter: {}", presenter.error());
    return 1;
  }
  state.presenter = std::move(*presenter);

  // Decoding happens here, before the first audio buffer is due.
  auto player = Player::create(state.project);
  if (!player) {
    std::println(stderr, "cannot start playback: {}", player.error());
    return 1;
  }
  state.player = std::move(*player);

  std::println("adapter: {}", state.device->adapter_name());
  std::println("audio:   {} — {} Hz, {} channels{}", state.player->device_name(),
               state.player->sample_rate(), state.player->channels(),
               state.player->silent() ? "  (project has no audio)" : "");
  std::println("canvas:  {}x{}   timeline: {:.2f}s", state.project.canvas_w,
               state.project.canvas_h, timeline);
  std::println("");
  std::println("space play/pause   left/right seek   shift+arrow 5s   home/end   esc quit");

  // Windows' default scheduler tick is 15.6 ms, so a one-millisecond sleep is
  // most of a frame at 60 Hz and the loop cannot keep up however fast the
  // compositor is. Media players ask for a finer tick; this is why.
  timeBeginPeriod(1);

  SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
  ShowWindow(window, SW_SHOW);

  if (start > 0.0) state.player->seek(start);
  draw(state, state.player->position());
  update_title(window, state);
  if (autoplay) state.player->play();

  // The playhead is asked for rather than counted. Re-rendering the same time
  // twice is wasted work, so a frame is drawn only when the position has moved
  // by enough to land on a different one.
  const double frame_step = state.project.fps > 0.0 ? 1.0 / state.project.fps : 1.0 / 30.0;

  MSG message{};
  while (message.message != WM_QUIT) {
    if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
      continue;
    }

    if (const std::string failure = state.player->error(); !failure.empty()) {
      std::println(stderr, "playback stopped: {}", failure);
      break;
    }

    if (state.player->playing()) {
      const double now = state.player->position();
      // A whole frame, not half of one. Drawing more often than the project's
      // own rate cannot show anything new, and asking for a time the decoder
      // has already passed is what makes it look like a move backwards.
      if (std::abs(now - state.last_drawn) >= frame_step) {
        const double previous = state.last_drawn;
        draw(state, now);
        ++state.frames_drawn;
        if (previous >= 0.0 && now > previous) state.played_seconds += now - previous;
        update_title(window, state);
      } else {
        // The playhead has not reached the next frame yet. `Sleep(1)` really
        // sleeps about 15 ms at Windows' default timer resolution, which is
        // most of a frame at 60 Hz — hence `timeBeginPeriod` below.
        Sleep(1);
      }
    } else {
      WaitMessage();
    }
  }

  timeEndPeriod(1);
  state.player.reset();
  state.device->wait_for_idle();

  // Whether the preview kept up. The playhead is set by the sound card, so
  // fewer frames than seconds-times-rate means the compositor fell behind it —
  // which looks like stutter and is easy to miss while watching.
  if (state.played_seconds > 0.0) {
    const double drawn_per_second =
        static_cast<double>(state.frames_drawn) / state.played_seconds;
    const double target = state.project.fps > 0.0 ? state.project.fps : 30.0;
    const auto per_frame = [count = static_cast<double>(state.frames_drawn)](double total) {
      return count > 0.0 ? total / count * 1000.0 : 0.0;
    };

    std::println("");
    std::println("drew {} frames over {:.2f}s of timeline — {:.1f} fps against a {:.0f} fps "
                 "project ({:.0f}% of frames)",
                 state.frames_drawn, state.played_seconds, drawn_per_second, target,
                 100.0 * drawn_per_second / target);
    std::println("  {:.1f} ms compositing, {:.1f} ms presenting per frame",
                 per_frame(state.render_seconds), per_frame(state.present_seconds));

    // A seek costs roughly seventeen times a sequential decode, so a preview
    // that seeks per frame and one that decodes through look identical from
    // outside and want opposite fixes.
    const auto stats = state.renderer->decode_stats();
    std::println("  {} source frames decoded, {} seeks ({} back, {} forward)",
                 stats.frames_decoded, stats.seeks, stats.backward_seeks,
                 stats.forward_seeks);
  }
  return 0;
}
