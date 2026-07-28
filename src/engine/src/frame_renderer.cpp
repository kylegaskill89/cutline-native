#include "cutline/engine/frame_renderer.hpp"

#include "cutline/media/av_headers.hpp"
#include "cutline/media/decoder.hpp"
#include "cutline/render/effects.hpp"
#include "cutline/render/plan.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace cutline::engine {
namespace {

using gpu::BlendMode;
using gpu::Color;
using gpu::Layer;
using gpu::LayerEffects;
using media::VideoDecoder;

/// How far ahead it is still worth decoding through rather than seeking.
///
/// A seek costs roughly one GOP of decoding plus the seek itself; decoding
/// forward costs about 1.6 ms a frame. Two seconds of 60fps footage is ~120
/// frames, about 190 ms, against ~28 ms for a seek — so the break-even is well
/// under a second. Half a second is chosen to stay clearly on the safe side of
/// it while still letting ordinary playback gaps (a dropped frame, a paused
/// scrub) run forwards.
constexpr double kDecodeForwardWindow = 0.5;

/// Frames within this of the request are close enough. Anything tighter starts
/// chasing floating-point noise in the timestamps.
constexpr double kFrameEpsilon = 1e-4;

[[nodiscard]] BlendMode to_gpu_blend(core::BlendMode mode) noexcept {
  switch (mode) {
    case core::BlendMode::Add:
      return BlendMode::Add;
    case core::BlendMode::Screen:
      return BlendMode::Screen;
    case core::BlendMode::Multiply:
      return BlendMode::Multiply;
    case core::BlendMode::Overlay:
      return BlendMode::Overlay;
    case core::BlendMode::Darken:
      return BlendMode::Darken;
    case core::BlendMode::Lighten:
      return BlendMode::Lighten;
    case core::BlendMode::Difference:
      return BlendMode::Difference;
    default:
      return BlendMode::Normal;
  }
}

[[nodiscard]] Color to_linear(render::EffectColor color, float alpha = 1.0f) noexcept {
  return Color::from_srgb(color.r, color.g, color.b, alpha);
}

/// Translates a resolved effect stack into the compositor's form. Flip lives on
/// the layer rather than in the effects because it is geometry, applied in the
/// vertex stage.
[[nodiscard]] LayerEffects to_gpu_effects(const render::EffectParams& params) noexcept {
  LayerEffects out;
  out.brightness = params.brightness;
  out.contrast = params.contrast;
  out.saturation = params.saturation;
  out.hue_degrees = params.hue_degrees;
  out.invert = params.invert;
  out.vignette = params.vignette;
  out.crop_left = params.crop_left;
  out.crop_top = params.crop_top;
  out.crop_right = params.crop_right;
  out.crop_bottom = params.crop_bottom;
  out.blur_sigma = params.blur_sigma;
  out.chroma_key = params.chroma_key;
  out.chroma_similarity = params.chroma_similarity;
  out.chroma_blend = params.chroma_blend;
  out.chroma_color = to_linear(params.chroma_color);
  return out;
}

/// Describes a decoded frame for the compositor. Returns false for layouts the
/// GPU path does not handle, which is reported rather than guessed at.
[[nodiscard]] bool describe(const AVFrame* frame, gpu::FrameView& out) noexcept {
  if (frame == nullptr) return false;

  switch (frame->format) {
    case AV_PIX_FMT_NV12:
      out.layout = gpu::PixelLayout::Nv12;
      break;
    case AV_PIX_FMT_YUV420P:
      out.layout = gpu::PixelLayout::Yuv420p;
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
      out.space = gpu::ColorSpace::Bt601;
      break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
      out.space = gpu::ColorSpace::Bt2020;
      break;
    default:
      out.space = gpu::ColorSpace::Bt709;
      break;
  }

  switch (frame->color_trc) {
    case AVCOL_TRC_SMPTE2084:
      out.transfer = gpu::TransferFunction::Smpte2084;
      break;
    case AVCOL_TRC_ARIB_STD_B67:
      out.transfer = gpu::TransferFunction::AribStdB67;
      break;
    default:
      out.transfer = gpu::TransferFunction::Bt709;
      break;
  }

  for (int i = 0; i < 3; ++i) out.planes[i] = {frame->data[i], frame->linesize[i]};
  return true;
}

/// One open source file, and where its decoder currently sits.
struct Source {
  std::unique_ptr<VideoDecoder> decoder;
  double position = -1.0;  ///< timestamp of the frame currently decoded
  bool exhausted = false;  ///< ran off the end; holds the last frame
  bool usable = true;      ///< opened, and producing a layout we can draw
};

}  // namespace

struct FrameRenderer::Impl {
  std::shared_ptr<gpu::Device> device;
  std::unique_ptr<gpu::Compositor> compositor;

  /// Keyed by media id. Decoders are expensive to open and expensive to seek,
  /// so they live as long as the timeline keeps asking for them.
  std::map<std::string, Source> sources;

  std::vector<std::string> missing;

  /// Frame views must outlive the compose call that reads them, and they point
  /// into decoder-owned memory, so both are kept alive across the call.
  std::vector<gpu::FrameView> views;

  /// Positions the source at `time` and returns its frame, or null.
  [[nodiscard]] const AVFrame* frame_at(const core::Media& media, double time);
};

const AVFrame* FrameRenderer::Impl::frame_at(const core::Media& media, double time) {
  auto found = sources.find(media.id);
  if (found == sources.end()) {
    Source source;
    // Software decode, deliberately for now: hardware frames stay in GPU memory
    // as D3D11 textures, and the compositor still uploads from plane pointers.
    // Sharing them across to D3D12 without a round trip is the pending piece of
    // work, and asking for hardware here before that exists would just produce
    // frames nothing can draw.
    auto opened = VideoDecoder::open(media.path,
                                     {.preferred = media::Acceleration::Software});
    if (!opened) {
      source.usable = false;
    } else {
      source.decoder = std::move(*opened);
    }
    found = sources.emplace(media.id, std::move(source)).first;
  }

  Source& source = found->second;
  if (!source.usable || !source.decoder) return nullptr;

  // Already there. Scrubbing within one frame's worth of time asks for the same
  // picture repeatedly, and re-decoding it would be pure waste.
  if (source.position >= 0.0 && std::abs(source.position - time) <= kFrameEpsilon) {
    return source.decoder->frame();
  }

  const bool backwards = source.position < 0.0 || time < source.position;
  const bool far_ahead = time > source.position + kDecodeForwardWindow;

  if (backwards || far_ahead) {
    if (!source.decoder->seek(time)) {
      source.usable = false;
      return nullptr;
    }
    source.exhausted = false;
    source.position = -1.0;
  }

  // Decode forwards until the frame covering `time` is in hand. A frame is
  // shown from its own timestamp until the next one, so the right frame is the
  // last whose timestamp does not exceed the request.
  while (!source.exhausted) {
    if (source.position >= time - kFrameEpsilon && source.position >= 0.0) break;

    const auto got = source.decoder->next_frame();
    if (!got) {
      source.usable = false;
      return nullptr;
    }
    if (!*got) {
      // Past the end: hold the last frame rather than dropping to black, which
      // is what a clip trimmed slightly beyond its source should look like.
      source.exhausted = true;
      break;
    }
    source.position = source.decoder->timestamp();
  }

  return source.decoder->frame();
}

FrameRenderer::FrameRenderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FrameRenderer::~FrameRenderer() = default;

gpu::Compositor& FrameRenderer::compositor() noexcept { return *impl_->compositor; }

const std::vector<std::string>& FrameRenderer::missing_media() const noexcept {
  return impl_->missing;
}

void FrameRenderer::release_sources() { impl_->sources.clear(); }

std::expected<std::unique_ptr<FrameRenderer>, std::string> FrameRenderer::create(
    std::shared_ptr<gpu::Device> device, int canvas_width, int canvas_height) {
  if (!device) return std::unexpected("a frame renderer needs a device");

  auto impl = std::make_unique<Impl>();
  impl->device = std::move(device);

  auto compositor = gpu::Compositor::create(impl->device, canvas_width, canvas_height);
  if (!compositor) return std::unexpected(compositor.error());
  impl->compositor = std::move(*compositor);

  return std::unique_ptr<FrameRenderer>(new FrameRenderer(std::move(impl)));
}

std::expected<void, std::string> FrameRenderer::resize(int width, int height) {
  return impl_->compositor->resize(width, height);
}

std::expected<gpu::Image, std::string> FrameRenderer::read_back() {
  return impl_->compositor->read_back();
}

std::expected<void, std::string> FrameRenderer::render(const core::Project& project, double t) {
  Impl& d = *impl_;
  d.missing.clear();

  if (auto ok = d.compositor->resize(project.canvas_w, project.canvas_h); !ok) {
    return std::unexpected(ok.error());
  }

  const std::vector<render::PlannedLayer> planned = render::plan_frame(project, t);

  // Reserved up front: the layers hold pointers into this, so a reallocation
  // partway through would leave earlier layers pointing at freed memory.
  d.views.clear();
  d.views.reserve(planned.size());

  std::vector<Layer> layers;
  layers.reserve(planned.size());

  for (const render::PlannedLayer& source : planned) {
    Layer layer;
    layer.quad = {static_cast<float>(source.box.center_x),
                  static_cast<float>(source.box.center_y),
                  static_cast<float>(source.box.width),
                  static_cast<float>(source.box.height),
                  static_cast<float>(source.box.rotation_deg)};
    layer.opacity = static_cast<float>(std::clamp(source.alpha, 0.0, 1.0));
    layer.blend = to_gpu_blend(source.blend);

    const render::EffectParams effects =
        source.clip == nullptr
            ? render::EffectParams{}
            : render::resolve_effect_params(*source.clip, t - source.clip->start);
    layer.effects = to_gpu_effects(effects);
    layer.flip_x = effects.flip_x;
    layer.flip_y = effects.flip_y;

    switch (source.content) {
      case render::LayerContent::Adjustment:
        layer.adjustment = true;
        break;

      case render::LayerContent::Color: {
        constexpr render::EffectColor fallback{0.10f, 0.10f, 0.10f};  // #1a1a1a
        layer.color = to_linear(
            source.media == nullptr ? fallback
                                    : render::parse_hex_color(source.media->color, fallback));
        if (source.media != nullptr && source.media->gradient) {
          layer.gradient = true;
          layer.gradient_color = to_linear(
              render::parse_hex_color(source.media->gradient->color2, fallback));
          layer.gradient_angle_deg = static_cast<float>(source.media->gradient->angle);
        }
        break;
      }

      case render::LayerContent::Text:
        // Titles need a text rasteriser, which is not here yet. Skipped rather
        // than drawn as a blank rectangle, so an unrendered title is visibly
        // absent instead of quietly wrong.
        continue;

      case render::LayerContent::Still:
      case render::LayerContent::Video: {
        if (source.media == nullptr || source.media->path.empty()) {
          if (source.clip != nullptr) d.missing.push_back(source.clip->media_id);
          continue;
        }

        const AVFrame* frame = d.frame_at(*source.media, source.source_time);
        gpu::FrameView view;
        if (!describe(frame, view)) {
          d.missing.push_back(source.media->id);
          continue;
        }

        d.views.push_back(view);
        layer.frame = &d.views.back();
        break;
      }
    }

    layers.push_back(layer);
  }

  return d.compositor->compose(layers);
}

}  // namespace cutline::engine
