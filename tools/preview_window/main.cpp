/// A throwaway viewport for driving the compositor by hand.
///
/// This is not the editor's UI and will not become it. It exists so the render
/// pipeline is exercisable months before there is a real interface: open a
/// file, scrub the playhead, and see the frame that comes out of the Direct3D 12
/// path with its colour conversion applied.
///
/// Left/Right step a frame, Shift accelerates, Space plays and pauses, Home
/// returns to the start.

#include "cutline/gpu/renderer.hpp"
#include "cutline/media/decoder.hpp"
#include "cutline/media/probe.hpp"

#include <windows.h>

#include <chrono>
#include <memory>
#include <print>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

using cutline::gpu::ColorSpace;
using cutline::gpu::FrameView;
using cutline::gpu::PixelLayout;
using cutline::gpu::Renderer;
using cutline::gpu::TransferFunction;
using cutline::media::Acceleration;
using cutline::media::VideoDecoder;

struct AppState {
  std::unique_ptr<Renderer> renderer;
  std::unique_ptr<VideoDecoder> decoder;
  std::string path;
  double fps = 30.0;
  double playhead = 0.0;
  bool playing = false;
  bool needs_redraw = true;
  FrameView view;
  bool have_view = false;
};

/// Translates a decoded frame into the renderer's view of it. Only the layouts
/// the decoder actually produces for our sources are handled; anything else is
/// reported rather than guessed at.
[[nodiscard]] bool describe(const AVFrame* frame, FrameView& out) {
  if (frame == nullptr) return false;

  switch (frame->format) {
    case AV_PIX_FMT_NV12:
      out.layout = PixelLayout::Nv12;
      break;
    case AV_PIX_FMT_YUV420P:
      out.layout = PixelLayout::Yuv420p;
      break;
    default:
      return false;
  }

  out.width = frame->width;
  out.height = frame->height;
  out.full_range = frame->color_range == AVCOL_RANGE_JPEG;

  switch (frame->colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
      out.space = ColorSpace::Bt601;
      break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
      out.space = ColorSpace::Bt2020;
      break;
    default:
      out.space = ColorSpace::Bt709;
      break;
  }

  switch (frame->color_trc) {
    case AVCOL_TRC_SMPTE2084:
      out.transfer = TransferFunction::Smpte2084;
      break;
    case AVCOL_TRC_ARIB_STD_B67:
      out.transfer = TransferFunction::AribStdB67;
      break;
    default:
      out.transfer = TransferFunction::Bt709;
      break;
  }

  for (int i = 0; i < 3; ++i) {
    out.planes[i] = {frame->data[i], frame->linesize[i]};
  }
  return true;
}

/// Seeks to the keyframe before `target` and decodes forward to it. Scrubbing
/// is the one place random access is wanted; export never does this.
void show(AppState& state, double target) {
  if (!state.decoder) return;
  state.playhead = target < 0.0 ? 0.0 : target;

  if (!state.decoder->seek(state.playhead)) return;
  while (true) {
    const auto got = state.decoder->next_frame();
    if (!got || !*got) break;
    if (state.decoder->timestamp() + 1e-6 >= state.playhead) break;
  }
  state.have_view = describe(state.decoder->frame(), state.view);
  state.needs_redraw = true;
}

/// Steps forward without re-seeking, which is what playback should do.
void advance(AppState& state) {
  if (!state.decoder) return;
  const auto got = state.decoder->next_frame();
  if (!got || !*got) {
    state.playing = false;
    return;
  }
  state.playhead = state.decoder->timestamp();
  state.have_view = describe(state.decoder->frame(), state.view);
  state.needs_redraw = true;
}

void update_title(HWND window, const AppState& state) {
  // Wide, not narrow: this is a UNICODE build, and SetWindowTextA reads a UTF-8
  // string as ANSI, which mangles anything outside ASCII.
  const std::wstring title = std::format(L"Cutline preview — {:.3f}s{}", state.playhead,
                                         state.playing ? L"  [playing]" : L"");
  SetWindowTextW(window, title.c_str());
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

  switch (message) {
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    case WM_SIZE:
      if (state != nullptr && state->renderer != nullptr) {
        const int width = LOWORD(lparam);
        const int height = HIWORD(lparam);
        if (auto ok = state->renderer->resize(width, height); !ok) {
          std::println(stderr, "resize failed: {}", ok.error());
        }
        state->needs_redraw = true;
      }
      return 0;

    case WM_KEYDOWN: {
      if (state == nullptr) return 0;
      const bool fast = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      const double step = (fast ? 10.0 : 1.0) / (state->fps > 0.0 ? state->fps : 30.0);

      switch (wparam) {
        case VK_RIGHT:
          // A single frame forward is just the next one; only a jump re-seeks.
          if (fast) {
            show(*state, state->playhead + step);
          } else {
            advance(*state);
          }
          break;
        case VK_LEFT:
          show(*state, state->playhead - step);
          break;
        case VK_HOME:
          show(*state, 0.0);
          break;
        case VK_SPACE:
          state->playing = !state->playing;
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
    std::println(stderr, "usage: preview_window <file>");
    return 2;
  }

  AppState state;
  state.path = argv[1];

  const auto info = cutline::media::probe(state.path);
  if (!info) {
    std::println(stderr, "probe failed: {}", info.error());
    return 1;
  }
  const auto* video = info->primary_video();
  if (video == nullptr) {
    std::println(stderr, "no video stream");
    return 1;
  }
  state.fps = video->fps > 0.0 ? video->fps : 30.0;

  std::println("{}  {}x{} @ {:.3f} fps", video->codec, video->width, video->height, video->fps);
  std::println("colour: {} / {}  {}-bit  {}", to_string(video->color.primaries),
               to_string(video->color.transfer), video->color.bits_per_component,
               video->color.is_hdr() ? "HDR" : "SDR");

  // Software decode: these frames are uploaded from system memory. Keeping
  // hardware frames on the GPU end to end is the next phase's work.
  auto decoder = VideoDecoder::open(state.path, {.preferred = Acceleration::Software});
  if (!decoder) {
    std::println(stderr, "cannot open decoder: {}", decoder.error());
    return 1;
  }
  state.decoder = std::move(*decoder);

  const WNDCLASSEXW window_class{
      .cbSize = sizeof(WNDCLASSEXW),
      .lpfnWndProc = window_proc,
      .hInstance = GetModuleHandleW(nullptr),
      .hCursor = LoadCursorW(nullptr, IDC_ARROW),
      .lpszClassName = L"CutlinePreview",
  };
  RegisterClassExW(&window_class);

  // Sized to a comfortable fraction of the source rather than its full 4K.
  const int width = 1280;
  const int height = std::max(1, video->height * width / std::max(1, video->width));

  RECT bounds{0, 0, width, height};
  AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);

  HWND window = CreateWindowExW(0, window_class.lpszClassName, L"Cutline preview",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr,
                                nullptr, window_class.hInstance, nullptr);
  if (window == nullptr) {
    std::println(stderr, "cannot create a window");
    return 1;
  }
  SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

  auto renderer = Renderer::create(window, width, height);
  if (!renderer) {
    std::println(stderr, "cannot create the renderer: {}", renderer.error());
    return 1;
  }
  state.renderer = std::move(*renderer);
  std::println("adapter: {}", state.renderer->adapter_name());
  std::println("");
  std::println("space play/pause   left/right step   shift+arrow jump   home start   esc quit");

  ShowWindow(window, SW_SHOW);
  advance(state);
  update_title(window, state);

  using Clock = std::chrono::steady_clock;
  auto next_frame_due = Clock::now();

  MSG message{};
  while (message.message != WM_QUIT) {
    if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
      continue;
    }

    if (state.playing && Clock::now() >= next_frame_due) {
      advance(state);
      next_frame_due = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                          std::chrono::duration<double>(1.0 / state.fps));
      update_title(window, state);
    }

    if (state.needs_redraw) {
      if (auto ok = state.renderer->render(state.have_view ? &state.view : nullptr); !ok) {
        std::println(stderr, "render failed: {}", ok.error());
        break;
      }
      state.needs_redraw = false;
    } else if (!state.playing) {
      // Idle rather than spin when there is nothing to draw.
      WaitMessage();
    }
  }

  state.renderer->wait_for_idle();
  return 0;
}
