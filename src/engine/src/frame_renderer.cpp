#include "cutline/engine/frame_renderer.hpp"

#include "cutline/media/av_headers.hpp"
#include "cutline/media/decoder.hpp"
#include "cutline/render/effect_passes.hpp"
#include "cutline/render/effects.hpp"
#include "cutline/render/plan.hpp"

#if CUTLINE_HAVE_TEXT
#include "cutline/text/raster.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <map>
#include <vector>

namespace cutline::engine {

bool can_draw_text() noexcept {
#if CUTLINE_HAVE_TEXT
  return true;
#else
  return false;
#endif
}

namespace {

using gpu::BlendMode;
using gpu::Color;
using gpu::Layer;
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

/// Assumed source frame interval when a stream does not report a usable rate.
constexpr double kAssumedFrameGap = 1.0 / 30.0;

/// How much memory to spend keeping decoded frames behind the playhead.
///
/// This is the reverse-playback budget. Playing backwards cannot decode
/// backwards — a compressed stream is only enterable at a keyframe — so every
/// step back costs a seek and a whole group of pictures. Keeping the frames
/// that decode produced turns one seek per *frame* into one seek per *run*, and
/// the length of the run is what decides how much of that cost is amortised.
///
/// Measured on a 106 Mbps 4K60 capture, playing 120 frames backwards:
///
///     kept    ms/frame    seeks   decoded
///     none      261.0       119     ~16000
///     24         55.4         6        803
///     48         24.2         3        437
///     96         13.1         2        291
///
/// Bytes rather than a count, because a frame is not a fixed thing: the same
/// run is 96 frames of 1080p or 24 of 4K for the same memory, and the smaller
/// picture both needs less and can afford more. A count would have made one of
/// those two wrong.
///
/// Half a gigabyte is the most this will spend. It is a great deal for a cache
/// and still not enough to play 4K backwards at full rate — that would want the
/// whole group of pictures, which is measured above at over a gigabyte for this
/// footage alone. What it buys is 4K reverse that runs rather than crawls, and
/// 1080p reverse that is simply smooth.
constexpr std::size_t kRememberedBytes = 384u * 1024u * 1024u;

/// The most and fewest frames to keep, whatever the budget works out to.
///
/// The ceiling is there because a hardware decoder lends its frames from a pool
/// sized when it opens, and every surface reserved is video memory whether
/// reverse is ever used or not. The floor is there because a run of two frames
/// buys nothing and a decoder still has to be told a number.
constexpr int kMinKeptFrames = 8;
constexpr int kMaxKeptFrames = 64;

/// How many frames of this media the budget affords.
///
/// Answered from the media rather than from a decoded frame, because the
/// decoder has to be told how many surfaces to allocate *before* it opens —
/// there is no asking for one later. A media that does not know its own size is
/// assumed to be the common one; getting it wrong costs memory or smoothness,
/// not correctness.
[[nodiscard]] int frames_to_keep(const core::Media& media) noexcept {
  const auto width = static_cast<std::size_t>(std::max(1, media.width.value_or(1920)));
  const auto height = static_cast<std::size_t>(std::max(1, media.height.value_or(1080)));
  // A byte and a half a pixel: NV12 and its relatives are a full luma plane and
  // a quarter-resolution pair of chroma planes.
  const std::size_t per_frame = width * height * 3 / 2;
  if (per_frame == 0) return kMinKeptFrames;

  const auto afford = static_cast<int>(kRememberedBytes / per_frame);
  return std::clamp(afford, kMinKeptFrames, kMaxKeptFrames);
}

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

/// Translates one planned pass into the compositor's form.
///
/// The two types are the same shape and deliberately separate: `render` is pure
/// and testable without a device, and `gpu` must not learn what a clip is. The
/// kinds are declared in the same order in both, and this is the one place that
/// depends on it.
[[nodiscard]] gpu::EffectPass to_gpu_pass(const render::EffectPass& pass) noexcept {
  gpu::EffectPass out;
  out.kind = static_cast<gpu::PassKind>(pass.kind);
  out.values = pass.values;
  out.mask = {pass.mask.shape,        pass.mask.x,            pass.mask.y,
              pass.mask.width,        pass.mask.height,       pass.mask.cos_rotation,
              pass.mask.sin_rotation, pass.mask.feather,      pass.mask.opacity,
              pass.mask.inverted};
  return out;
}

[[nodiscard]] std::vector<gpu::EffectPass> to_gpu_passes(
    const std::vector<render::EffectPass>& passes) {
  std::vector<gpu::EffectPass> out;
  out.reserve(passes.size());
  for (const render::EffectPass& pass : passes) out.push_back(to_gpu_pass(pass));
  return out;
}

/// Describes a decoded frame for the compositor. Returns false for layouts the
/// GPU path does not handle, which is reported rather than guessed at.
[[nodiscard]] bool describe(const AVFrame* frame, const media::VideoDecoder* from,
                            gpu::FrameView& out) noexcept {
  if (frame == nullptr) return false;

  // A hardware frame carries a handle rather than pixels. Everything the
  // compositor reads off the frame besides the pixels — its size, its colour
  // tagging, its range — is the same either way, so only this one branch and
  // the planes at the end differ.
  bool decoded_on_the_card = false;
  switch (frame->format) {
    // Both card formats, because which one a machine offers is the driver's
    // business. The decoder answers `hardware_texture` with a Direct3D 12
    // resource either way — that is the whole reason the crossing lives down
    // there rather than up here.
    case AV_PIX_FMT_D3D12:
    case AV_PIX_FMT_D3D11:
      out.layout = gpu::PixelLayout::Nv12;
      decoded_on_the_card = true;
      break;
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

  if (decoded_on_the_card) {
    if (from == nullptr) return false;
    // This frame, not whatever the decoder happens to be holding: it may be one
    // from the run of remembered frames, or the held last frame of an exhausted
    // stream, and neither is the decoder's current one.
    const std::optional<media::HardwareTexture> texture = from->hardware_texture(frame);
    if (!texture.has_value()) return false;
    out.texture = gpu::SourceTexture{.resource = texture->resource,
                                     .subresource = texture->subresource,
                                     .fence = texture->fence,
                                     .fence_value = texture->fence_value};
    return true;
  }

  for (int i = 0; i < 3; ++i) out.planes[i] = {frame->data[i], frame->linesize[i]};
  return true;
}

struct FrameDeleter {
  void operator()(AVFrame* frame) const noexcept {
    if (frame != nullptr) av_frame_free(&frame);
  }
};

/// One open source file, and where its decoder currently sits.
struct Source {
  std::unique_ptr<VideoDecoder> decoder;
  double position = -1.0;  ///< timestamp of the frame currently decoded
  /// How many frames this source may keep behind the playhead, decided when it
  /// opened because that is when the decoder's surface pool was sized.
  std::size_t keep = 0;
  bool exhausted = false;  ///< ran off the end
  bool usable = true;      ///< opened, and producing a layout we can draw

  /// A reference to the last frame decoded. The decoder drops its own at end of
  /// stream, so without this a clip trimmed even slightly past its source would
  /// go black for its final frames instead of holding.
  std::unique_ptr<AVFrame, FrameDeleter> held;

  void hold(const AVFrame* frame) {
    if (frame == nullptr) return;
    if (!held) held.reset(av_frame_alloc());
    if (!held) return;
    av_frame_unref(held.get());
    // Reference, not copy: this is a refcount bump, not a pixel copy.
    av_frame_ref(held.get(), frame);
  }

  /// Frames already decoded, newest last, so a step *backwards* need not decode
  /// a whole group of pictures over again.
  ///
  /// Playing forwards costs one decode per frame. Playing backwards used to
  /// cost a seek and a whole GOP per frame, because a compressed stream cannot
  /// be entered anywhere but a keyframe and cannot be run in reverse at all.
  /// Measured on a 106 Mbps 4K60 capture: 1.98 ms a frame forwards and
  /// **261 ms** backwards, which is 132 times worse and nowhere near playable.
  ///
  /// The frames were all decoded anyway. Reaching frame N from a keyframe K
  /// means decoding every frame between them, and the next thing reverse asks
  /// for is N-1 — one this decoder held in its hands a moment ago and threw
  /// away. Keeping a run of them turns one seek per frame into one seek per
  /// run.
  ///
  /// References rather than copies, so this costs a refcount and not a picture.
  /// A **contiguous run** and not a set: a hole in it would be indistinguishable
  /// from the end of it, and "the frame covering time t" is the last one at or
  /// before t — which is the wrong frame entirely if the right one was skipped.
  /// Cleared on a seek for that reason.
  struct Recent {
    double at = 0.0;
    std::unique_ptr<AVFrame, FrameDeleter> frame;
  };
  std::deque<Recent> recent;

  void remember(const AVFrame* frame, double at) {
    if (frame == nullptr || keep == 0) return;
    std::unique_ptr<AVFrame, FrameDeleter> copy(av_frame_alloc());
    if (!copy) return;
    if (av_frame_ref(copy.get(), frame) < 0) return;
    recent.push_back(Recent{.at = at, .frame = std::move(copy)});
    while (recent.size() > keep) recent.pop_front();
  }

  /// The remembered frame covering `time`, or null. The last one at or before
  /// it, which is the same rule the decoder follows: a frame is shown from its
  /// own timestamp until the next one.
  [[nodiscard]] const AVFrame* remembered(double time) const {
    if (recent.empty()) return nullptr;
    // Before the run began, so whatever covers this was never decoded here.
    if (time < recent.front().at - kFrameEpsilon) return nullptr;
    // And past the end of it. "The last frame at or before t" is only an answer
    // *within* the run: beyond the newest entry the right frame is one nobody
    // has decoded yet, and handing back the newest would freeze the picture
    // there for ever while playing forwards.
    if (time > recent.back().at + kFrameEpsilon) return nullptr;
    const AVFrame* found = nullptr;
    for (const Recent& entry : recent) {
      if (entry.at > time + kFrameEpsilon) break;
      found = entry.frame.get();
    }
    return found;
  }
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

  DecodeStats stats;

#if CUTLINE_HAVE_TEXT
  /// Rasterised titles, keyed by media id.
  ///
  /// Drawing text costs about as much as decoding a frame and a title does not
  /// change between frames, so it is drawn once and kept. The spec it was drawn
  /// from is kept beside it: the only thing that can invalidate a title is an
  /// edit to the text or its styling, and comparing the spec catches every one
  /// of those without anyone having to remember to say so.
  struct Title {
    core::TextSpec spec;
    text::Raster raster;
  };
  std::map<std::string, Title> titles;

  /// The rasterised title for a media, drawing it if it is not in hand.
  /// Null when it cannot be drawn at all.
  [[nodiscard]] const text::Raster* title_for(const core::Media& media);
#endif

  /// How big a title comes out on the canvas. Empty when text cannot be drawn,
  /// which is what makes a title fall back to filling the canvas.
  [[nodiscard]] core::Size measure(const core::Media& media);

  /// Positions the source at `time` and returns its frame, or null.
  /// `from` receives the decoder that produced the frame, which is what a
  /// hardware frame needs asking for its texture. Null past the end of a
  /// stream, where the held frame stands in and no decoder owns it.
  [[nodiscard]] const AVFrame* frame_at(const core::Media& media, double time,
                                        const media::VideoDecoder** from = nullptr);
};

#if CUTLINE_HAVE_TEXT

const text::Raster* FrameRenderer::Impl::title_for(const core::Media& media) {
  if (!media.text.has_value()) return nullptr;

  const auto found = titles.find(media.id);
  if (found != titles.end()) {
    if (found->second.spec == *media.text) return &found->second.raster;
    // Edited since it was drawn.
    titles.erase(found);
  }

  auto drawn = text::rasterise(*media.text);
  if (!drawn.has_value()) return nullptr;

  const auto inserted =
      titles.emplace(media.id, Title{.spec = *media.text, .raster = std::move(*drawn)});
  return &inserted.first->second.raster;
}

core::Size FrameRenderer::Impl::measure(const core::Media& media) {
  if (!media.text.has_value()) return {};
  // Measured by drawing, not by a second code path that could disagree: the
  // quad has to be exactly the size of the picture that fills it, or the text
  // is stretched.
  const text::Raster* raster = title_for(media);
  if (raster == nullptr) return {};
  return core::Size{static_cast<double>(raster->width), static_cast<double>(raster->height)};
}

#else

core::Size FrameRenderer::Impl::measure(const core::Media&) { return {}; }

#endif

const AVFrame* FrameRenderer::Impl::frame_at(const core::Media& media, double time,
                                            const media::VideoDecoder** from) {
  auto found = sources.find(media.id);
  if (found == sources.end()) {
    Source source;
    // Decided here because the decoder's surface pool is sized when it opens
    // and cannot be added to afterwards.
    const int keeping = frames_to_keep(media);
    source.keep = static_cast<std::size_t>(keeping);
    // Decoded on the card, and given this device so the picture can cross onto
    // it. Measured on a 106 Mbps 4K60 capture: 2.76 ms a frame against 7.24 in
    // software, and the software decoder is where most of this application's
    // memory went besides.
    //
    // **Direct3D 11**, not 12, and that is the driver's choice rather than a
    // preference. D3D12 video decode is the path that needs no crossing at all,
    // and on the machine this was written for it fails every HEVC picture and
    // removes the device with it:
    //
    //     [hevc] hardware accelerator failed to decode picture
    //     DXGI_ERROR_DRIVER_INTERNAL_ERROR: strong evidence that the driver has
    //     performed an undefined operation
    //
    // The stock `ffmpeg.exe` fails identically on the same file, so it is not
    // something this is doing wrong — and the removal takes the *compositor's*
    // device, since that is the one the decoder was handed. D3D11 decodes the
    // same file at 558 fps and the frames cross with one copy inside the card.
    //
    // Software is still the floor, and `open` falls to it on its own when
    // neither works.
    auto opened = VideoDecoder::open(
        media.path,
        {.preferred = media::Acceleration::D3D11Va,
         // Surfaces for the run kept behind the playhead. Without this the pool
         // empties the moment reverse starts and decoding stops outright:
         // "Static surface pool size exceeded", then `get_buffer() failed`.
         .extra_frames = keeping,
         .d3d12_device = device != nullptr ? device->native_device() : nullptr});
    if (!opened) {
      source.usable = false;
    } else {
      source.decoder = std::move(*opened);
    }
    found = sources.emplace(media.id, std::move(source)).first;
  }

  Source& source = found->second;
  if (!source.usable || !source.decoder) return nullptr;

  // Whichever way this leaves, the answer to "which decoder made that" is the
  // same one — and it has to be set on *every* path out, not just the last.
  // Missing it on the short-circuit below meant a card frame arrived with no
  // decoder to ask for its texture, so the layer was dropped and the frame came
  // out black: about every other frame during playback, which is exactly what
  // "flickering between frames" looks like.
  const auto answer = [&](const AVFrame* current) {
    if (from != nullptr) *from = current != nullptr ? source.decoder.get() : nullptr;
    return current != nullptr ? current : source.held.get();
  };

  // Already there. Scrubbing within one frame's worth of time asks for the same
  // picture repeatedly, and re-decoding it would be pure waste.
  if (source.position >= 0.0 && std::abs(source.position - time) <= kFrameEpsilon) {
    return answer(source.decoder->frame());
  }

  // Decoded a moment ago and kept. This is what makes playing backwards
  // possible at all: every frame of the run behind the playhead is one this
  // decoder already produced on its way forward, and handing it back costs
  // nothing where decoding it again costs a seek and a whole GOP.
  if (const AVFrame* known = source.remembered(time); known != nullptr) {
    ++stats.frames_remembered;
    if (from != nullptr) *from = source.decoder.get();
    return known;
  }

  // Decoding stops at the first frame whose timestamp reaches the request, so
  // the decoder usually sits a little *ahead* of where it was asked for — up to
  // one frame. A later request that is closer than that overshoot then looks
  // like a move backwards, and seeking costs a whole GOP.
  //
  // Playing a 60fps source measured 17 of these in six seconds, and they were
  // most of the decoding: 1827 source frames for 184 drawn. Tolerating a
  // backwards move smaller than one frame shows a picture at most one frame
  // early, which no one can see, instead of re-decoding from a keyframe.
  const double fps = source.decoder->stream().fps;
  const double frame_gap = fps > 0.0 ? 1.0 / fps : kAssumedFrameGap;
  const bool backwards = source.position < 0.0 || time < source.position - frame_gap;
  // An exhausted source has nothing further to find, so seeking ahead in one
  // would re-seek and re-decode to end of stream for every remaining frame.
  // That is not a small cost: it turned a clip trimmed two seconds past its
  // source into half a second per frame.
  const bool far_ahead = !source.exhausted && time > source.position + kDecodeForwardWindow;

  if (backwards || far_ahead) {
    ++stats.seeks;
    if (backwards) {
      ++stats.backward_seeks;
    } else {
      ++stats.forward_seeks;
    }
    if (!source.decoder->seek(time)) {
      source.usable = false;
      return nullptr;
    }
    source.exhausted = false;
    source.position = -1.0;
    // The run is contiguous or it is nothing, and a seek breaks it. Keeping the
    // old entries would leave a hole that reads as the end of the run, and
    // "the last frame at or before t" would then answer with one from the
    // wrong side of it.
    source.recent.clear();
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
    ++stats.frames_decoded;
    source.position = source.decoder->timestamp();
    source.hold(source.decoder->frame());
    // Kept on the way past, which is the whole trick: reaching frame N from a
    // keyframe decodes everything between them, and reverse is about to ask for
    // every one of them in turn.
    source.remember(source.decoder->frame(), source.position);
  }

  // Past the end, the decoder has released its frame, so the held one stands in.
  return answer(source.decoder->frame());
}

FrameRenderer::FrameRenderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FrameRenderer::~FrameRenderer() = default;

gpu::Compositor& FrameRenderer::compositor() noexcept { return *impl_->compositor; }

const std::vector<std::string>& FrameRenderer::missing_media() const noexcept {
  return impl_->missing;
}

FrameRenderer::DecodeStats FrameRenderer::decode_stats() const noexcept { return impl_->stats; }

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

  // The measurer is what sizes a title's quad to its text; without it a title
  // would be stretched to fill the canvas.
  const std::vector<render::PlannedLayer> planned = render::plan_frame(
      project, t, [&d](const core::Media& media) { return d.measure(media); });

  // Reserved up front: the layers hold pointers into this, so a reallocation
  // partway through would leave earlier layers pointing at freed memory.
  d.views.clear();
  d.views.reserve(planned.size());

  std::vector<Layer> layers;
  layers.reserve(planned.size());

  // The layers borrow their passes, so the vectors have to outlive the compose
  // call and must not move while it is being built up. Reserved for the same
  // reason `views` is: a reallocation partway through leaves earlier layers
  // pointing at freed memory.
  std::vector<std::vector<gpu::EffectPass>> stacks;
  stacks.reserve(planned.size());

  for (const render::PlannedLayer& source : planned) {
    Layer layer;
    layer.quad = {static_cast<float>(source.box.center_x),
                  static_cast<float>(source.box.center_y),
                  static_cast<float>(source.box.width),
                  static_cast<float>(source.box.height),
                  static_cast<float>(source.box.rotation_deg)};
    layer.opacity = static_cast<float>(std::clamp(source.alpha, 0.0, 1.0));
    layer.anti_flicker = static_cast<float>(std::clamp(source.anti_flicker, 0.0, 1.0));
    layer.blend = to_gpu_blend(source.blend);

    stacks.push_back(source.clip == nullptr
                         ? std::vector<gpu::EffectPass>{}
                         : to_gpu_passes(render::plan_effect_passes(
                               *source.clip, t - source.clip->start)));
    layer.passes = stacks.back();

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

      case render::LayerContent::Text: {
#if CUTLINE_HAVE_TEXT
        if (source.media == nullptr) continue;
        const text::Raster* raster = d.title_for(*source.media);
        // Nothing drawable — no text, or no font at all. Skipped rather than
        // drawn as a blank rectangle, so a title that failed is visibly absent
        // instead of quietly wrong.
        if (raster == nullptr || raster->empty()) continue;

        gpu::FrameView view;
        view.width = raster->width;
        view.height = raster->height;
        view.layout = gpu::PixelLayout::Rgba8;
        // Not video: a rasteriser works in full-range sRGB, and treating it as
        // studio-range would lift the blacks of every title.
        view.full_range = true;
        view.planes[0] = gpu::PlaneView{.data = raster->pixels.data(),
                                        .stride = raster->stride()};

        d.views.push_back(view);
        layer.frame = &d.views.back();
        break;
#else
        // Built without a text rasteriser, so there is nothing to draw with.
        continue;
#endif
      }

      case render::LayerContent::Still:
      case render::LayerContent::Video: {
        if (source.media == nullptr || source.media->path.empty()) {
          if (source.clip != nullptr) d.missing.push_back(source.clip->media_id);
          continue;
        }

        const media::VideoDecoder* from = nullptr;
        const AVFrame* frame = d.frame_at(*source.media, source.source_time, &from);
        gpu::FrameView view;
        if (!describe(frame, from, view)) {
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
