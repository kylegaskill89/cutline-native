/// A throwaway viewport for driving the compositor by hand.
///
/// This is not the editor's UI and will not become it. It exists so the render
/// pipeline is exercisable months before there is a real interface: open a
/// file, scrub the playhead, move the layer around, and see what comes out of
/// the Direct3D 12 path with its colour conversion and transform applied.
///
/// The video is composited over a colour matte rather than straight onto the
/// canvas, so every frame goes through the same multi-layer path an actual
/// timeline will.

#include "cutline/core/layout.hpp"
#include "cutline/gpu/compositor.hpp"
#include "cutline/gpu/presenter.hpp"
#include "cutline/media/av_headers.hpp"
#include "cutline/media/decoder.hpp"
#include "cutline/media/probe.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <numbers>
#include <print>
#include <string>
#include <vector>

namespace {

using cutline::core::LayerBox;
using cutline::gpu::BlendMode;
using cutline::gpu::Color;
using cutline::gpu::ColorSpace;
using cutline::gpu::Compositor;
using cutline::gpu::Device;
using cutline::gpu::FrameView;
using cutline::gpu::Layer;
using cutline::gpu::PixelLayout;
using cutline::gpu::Presenter;
using cutline::gpu::Quad;
using cutline::gpu::TransferFunction;
using cutline::media::Acceleration;
using cutline::media::VideoDecoder;

/// The blend modes, in the order the number keys select them.
constexpr std::array<std::pair<BlendMode, const char*>, 8> kBlendModes{{
    {BlendMode::Normal, "normal"},
    {BlendMode::Add, "add"},
    {BlendMode::Screen, "screen"},
    {BlendMode::Multiply, "multiply"},
    {BlendMode::Overlay, "overlay"},
    {BlendMode::Darken, "darken"},
    {BlendMode::Lighten, "lighten"},
    {BlendMode::Difference, "difference"},
}};

struct AppState {
  std::shared_ptr<Device> device;
  std::unique_ptr<Compositor> compositor;
  std::unique_ptr<Presenter> presenter;
  std::unique_ptr<VideoDecoder> decoder;

  std::string path;
  int canvas_w = 1920;
  int canvas_h = 1080;
  int media_w = 0;
  int media_h = 0;

  double fps = 30.0;
  double playhead = 0.0;
  bool playing = false;
  bool needs_redraw = true;

  FrameView view;
  bool have_view = false;

  // The transform under the keyboard's control, in the model's units: position
  // as a canvas fraction, scale as a multiplier, rotation in degrees.
  double x = 0.5;
  double y = 0.5;
  double scale = 1.0;
  double rotation = 0.0;
  float opacity = 1.0f;
  std::size_t blend = 0;
  bool flip_x = false;
  bool flip_y = false;

  /// The knobs the keys turn. Kept as plain numbers here and turned into a
  /// pass list when a frame is composed: this is a tool for poking at the
  /// compositor by hand, so one key holding one value reads better than a
  /// stack that has to be edited.
  struct Knobs {
    float brightness = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float hue_degrees = 0.0f;
    bool invert = false;
    float vignette = 0.0f;
    float crop = 0.0f;
    bool chroma_key = false;
    float blur_sigma = 0.0f;
  } effects;
};

/// The knobs as a pass list, in the order the old flat resolver applied them.
[[nodiscard]] std::vector<cutline::gpu::EffectPass> passes_of(const AppState& state) {
  using cutline::gpu::EffectPass;
  using cutline::gpu::PassKind;

  std::vector<EffectPass> passes;
  const AppState::Knobs& knobs = state.effects;

  if (knobs.brightness != 0.0f || knobs.contrast != 1.0f || knobs.saturation != 1.0f ||
      knobs.hue_degrees != 0.0f) {
    passes.push_back(EffectPass{
        PassKind::Color,
        {knobs.brightness, knobs.contrast, knobs.saturation,
         static_cast<float>(knobs.hue_degrees * std::numbers::pi / 180.0)}});
  }
  if (knobs.invert) passes.push_back(EffectPass{PassKind::Invert, {}});
  if (state.flip_x || state.flip_y) {
    passes.push_back(EffectPass{
        PassKind::Flip, {state.flip_x ? -1.0f : 1.0f, state.flip_y ? -1.0f : 1.0f}});
  }
  if (knobs.crop > 0.0f) {
    passes.push_back(EffectPass{PassKind::Crop, {knobs.crop, knobs.crop, knobs.crop, knobs.crop}});
  }
  if (knobs.chroma_key) {
    passes.push_back(EffectPass{PassKind::ChromaKey, {0.0f, 208.0f / 255.0f, 0.0f, 0.3f, 0.1f}});
  }
  if (knobs.vignette > 0.0f) {
    passes.push_back(EffectPass{PassKind::Vignette, {knobs.vignette}});
  }
  if (knobs.blur_sigma > 0.0f) {
    passes.push_back(EffectPass{PassKind::Blur, {knobs.blur_sigma}});
  }
  return passes;
}

/// Translates a decoded frame into the compositor's view of it. Only the
/// layouts the decoder actually produces for our sources are handled; anything
/// else is reported rather than guessed at.
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

/// Builds the layer stack for the current state: a matte underneath, the video
/// over it. The geometry comes from core, exactly as the real renderer will
/// compute it.
/// The passes go in `passes`, which the caller keeps alive: a layer borrows
/// them and the compose call reads them.
[[nodiscard]] std::vector<Layer> build_layers(const AppState& state,
                                              const std::vector<cutline::gpu::EffectPass>& passes) {
  std::vector<Layer> layers;

  // A dark matte, so a transformed or partly transparent video layer visibly
  // has something behind it rather than sitting on an ambiguous black.
  Layer matte;
  matte.color = Color::from_srgb(0.10f, 0.11f, 0.16f);
  matte.quad = {static_cast<float>(state.canvas_w) * 0.5f,
                static_cast<float>(state.canvas_h) * 0.5f,
                static_cast<float>(state.canvas_w), static_cast<float>(state.canvas_h), 0.0f};
  layers.push_back(matte);

  if (!state.have_view) return layers;

  // The same aspect-fit core uses, so what is on screen is what a project at
  // this canvas size would render.
  const double fit = std::min(static_cast<double>(state.canvas_w) / state.media_w,
                              static_cast<double>(state.canvas_h) / state.media_h);

  Layer video;
  video.frame = &state.view;
  video.quad = {static_cast<float>(state.x * state.canvas_w),
                static_cast<float>(state.y * state.canvas_h),
                static_cast<float>(state.media_w * fit * state.scale),
                static_cast<float>(state.media_h * fit * state.scale),
                static_cast<float>(state.rotation)};
  video.opacity = state.opacity;
  video.blend = kBlendModes[state.blend].first;
  video.passes = passes;
  layers.push_back(video);

  return layers;
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
  const std::wstring blend(kBlendModes[state.blend].second,
                           kBlendModes[state.blend].second +
                               std::strlen(kBlendModes[state.blend].second));
  const std::wstring title =
      std::format(L"Cutline preview — {:.3f}s   scale {:.2f}  rot {:.0f}°  a {:.2f}  {}{}",
                  state.playhead, state.scale, state.rotation, state.opacity, blend,
                  state.playing ? L"  [playing]" : L"");
  SetWindowTextW(window, title.c_str());
}

void redraw(AppState& state) {
  const std::vector<cutline::gpu::EffectPass> passes = passes_of(state);
  const std::vector<Layer> layers = build_layers(state, passes);
  if (auto ok = state.compositor->compose(layers); !ok) {
    std::println(stderr, "compose failed: {}", ok.error());
    return;
  }
  if (auto ok = state.presenter->present(*state.compositor); !ok) {
    std::println(stderr, "present failed: {}", ok.error());
  }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

  switch (message) {
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    case WM_SIZE:
      if (state != nullptr && state->presenter != nullptr) {
        // Only the window changed; the canvas keeps its size, so the frame is
        // letterboxed rather than re-rendered at the window's aspect.
        if (auto ok = state->presenter->resize(LOWORD(lparam), HIWORD(lparam)); !ok) {
          std::println(stderr, "resize failed: {}", ok.error());
        }
        state->needs_redraw = true;
      }
      return 0;

    case WM_KEYDOWN: {
      if (state == nullptr) return 0;
      const bool fast = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      const bool transform = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      const double step = (fast ? 10.0 : 1.0) / (state->fps > 0.0 ? state->fps : 30.0);

      if (transform) {
        // Ctrl turns the arrows into a nudge, which is the quickest way to see
        // whether the quad geometry is right.
        constexpr double kNudge = 0.01;
        switch (wparam) {
          case VK_LEFT:
            state->x -= kNudge;
            break;
          case VK_RIGHT:
            state->x += kNudge;
            break;
          case VK_UP:
            state->y -= kNudge;
            break;
          case VK_DOWN:
            state->y += kNudge;
            break;
          default:
            break;
        }
        state->needs_redraw = true;
        update_title(window, *state);
        return 0;
      }

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
        case VK_OEM_PLUS:
        case VK_ADD:
          state->scale *= 1.1;
          state->needs_redraw = true;
          break;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
          state->scale /= 1.1;
          state->needs_redraw = true;
          break;
        case 'R':
          state->rotation = std::fmod(state->rotation + (fast ? -15.0 : 15.0), 360.0);
          state->needs_redraw = true;
          break;
        case 'O':
          state->opacity = state->opacity > 0.55f ? 0.5f : 1.0f;
          state->needs_redraw = true;
          break;
        case 'B':
          state->blend = (state->blend + 1) % kBlendModes.size();
          state->needs_redraw = true;
          break;

        // Effects. Shift steps each one down rather than up, so a value can be
        // walked back without resetting everything.
        case 'C':
          state->effects.contrast += fast ? -0.25f : 0.25f;
          state->effects.contrast = std::max(0.0f, state->effects.contrast);
          state->needs_redraw = true;
          break;
        case 'G':
          state->effects.brightness += fast ? -0.1f : 0.1f;
          state->needs_redraw = true;
          break;
        case 'S':
          state->effects.saturation += fast ? -0.25f : 0.25f;
          state->effects.saturation = std::max(0.0f, state->effects.saturation);
          state->needs_redraw = true;
          break;
        case 'H':
          state->effects.hue_degrees = std::fmod(
              state->effects.hue_degrees + (fast ? -30.0f : 30.0f), 360.0f);
          state->needs_redraw = true;
          break;
        case 'I':
          state->effects.invert = !state->effects.invert;
          state->needs_redraw = true;
          break;
        case 'V':
          // A quarter turn is the maximum the amount maps to.
          state->effects.vignette = state->effects.vignette > 0.0f ? 0.0f : 1.2f;
          state->needs_redraw = true;
          break;
        case 'X':
          state->effects.crop = state->effects.crop > 0.0f ? 0.0f : 0.15f;
          state->needs_redraw = true;
          break;
        case 'K':
          state->effects.chroma_key = !state->effects.chroma_key;
          state->needs_redraw = true;
          break;
        case 'U':
          state->effects.blur_sigma += fast ? -4.0f : 4.0f;
          state->effects.blur_sigma = std::max(0.0f, state->effects.blur_sigma);
          state->needs_redraw = true;
          break;
        case 'F':
          if (fast) {
            state->flip_y = !state->flip_y;
          } else {
            state->flip_x = !state->flip_x;
          }
          state->needs_redraw = true;
          break;

        case '0':
          state->x = 0.5;
          state->y = 0.5;
          state->scale = 1.0;
          state->rotation = 0.0;
          state->opacity = 1.0f;
          state->blend = 0;
          state->flip_x = false;
          state->flip_y = false;
          state->effects = {};
          state->needs_redraw = true;
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
  state.media_w = video->width;
  state.media_h = video->height;
  // The canvas takes the source's shape, which is what a new project does.
  state.canvas_w = video->width;
  state.canvas_h = video->height;

  std::println("{}  {}x{} @ {:.3f} fps", video->codec, video->width, video->height, video->fps);
  std::println("colour: {} / {}  {}-bit  {}", to_string(video->color.primaries),
               to_string(video->color.transfer), video->color.bits_per_component,
               video->color.is_hdr() ? "HDR" : "SDR");

  // Software decode: these frames are uploaded from system memory. Keeping
  // hardware frames on the GPU end to end is still to come.
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

  auto device = Device::create();
  if (!device) {
    std::println(stderr, "cannot create a device: {}", device.error());
    return 1;
  }
  state.device = std::move(*device);

  auto compositor = Compositor::create(state.device, state.canvas_w, state.canvas_h);
  if (!compositor) {
    std::println(stderr, "cannot create the compositor: {}", compositor.error());
    return 1;
  }
  state.compositor = std::move(*compositor);

  auto presenter = Presenter::create(state.device, window, width, height);
  if (!presenter) {
    std::println(stderr, "cannot create the presenter: {}", presenter.error());
    return 1;
  }
  state.presenter = std::move(*presenter);

  std::println("adapter: {}", state.device->adapter_name());
  std::println("canvas: {}x{}", state.canvas_w, state.canvas_h);
  std::println("");
  std::println("space play/pause   left/right step   shift+arrow jump   home start");
  std::println("ctrl+arrows move   +/- scale   r rotate   o opacity   b blend   f flip");
  std::println("g brightness   c contrast   s saturation   h hue   i invert");
  std::println("v vignette   x crop   k chroma key   u blur   shift reverses");
  std::println("0 reset   esc quit");

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
      redraw(state);
      state.needs_redraw = false;
    } else if (!state.playing) {
      // Idle rather than spin when there is nothing to draw.
      WaitMessage();
    }
  }

  state.device->wait_for_idle();
  return 0;
}
