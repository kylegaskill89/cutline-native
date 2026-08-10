#include "cutline/engine/frame_renderer.hpp"

#include "cutline/core/interpret.hpp"
#include "cutline/core/query.hpp"
#include "cutline/media/av_headers.hpp"
#include "cutline/media/decoder.hpp"
#include "cutline/render/effect_passes.hpp"
#include "cutline/render/effects.hpp"
#include "cutline/render/plan.hpp"

#if CUTLINE_HAVE_TEXT
#include "cutline/text/raster.hpp"
#endif

#include <algorithm>
#include <chrono>
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

/// How much of a turn the prefetch may spend decoding the next run.
///
/// The number that matters is not the frame interval but how much has to be
/// decoded before the run in hand runs out. Reaching the frames wanted means
/// decoding from the keyframe before them — about 110 frames on this footage,
/// some 230 ms — and there are only as many turns to do it in as the run has
/// frames left. Twenty-two frames against 230 ms is a shade over ten each.
///
/// Eight was measured and was not enough: the prefetch finished late, the
/// finish had to happen in one lump after all, and six stalls in ten seconds
/// survived. Twelve leaves the run comfortably ahead and still fits inside a
/// 60 fps frame beside the composite and the paint.
constexpr auto kPrefetchBudget = std::chrono::milliseconds(12);

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
/// This is the budget for **one** run, and two are held: the one being played
/// out and the one being decoded ahead of it. So the real ceiling is twice
/// this — about 768 MB on 4K footage, and proportionally less on anything
/// smaller, which is a great deal for a cache and is what buys reverse playback
/// that does not stop twice a second.
///
/// It is still not enough to play 4K backwards with no seeking at all: that
/// would want the whole group of pictures resident, measured above at over a
/// gigabyte for this footage alone. What it buys is a seek that nobody sees.
constexpr std::size_t kRememberedBytes = 384u * 1024u * 1024u;

/// The most and fewest frames to keep, whatever the budget works out to.
///
/// The ceiling is there because a hardware decoder lends its frames from a pool
/// sized when it opens, and every surface reserved is video memory whether
/// reverse is ever used or not. The floor is there because a run of two frames
/// buys nothing and a decoder still has to be told a number.
constexpr int kMinKeptFrames = 8;
constexpr int kMaxKeptFrames = 64;

/// The most surfaces to ask *one* hardware decoder to lend.
///
/// `extra_hw_frames` is a request, not a guarantee. D3D11 video decoding hands
/// out slices of one texture array, and the driver stops honouring the number
/// well before the API's limit — asking for 68 and holding 65 still produced
///
///     [AVHWFramesContext] Static surface pool size exceeded.
///
/// which is decoding stopping. Found by measuring: with the pool starved, the
/// same 1240 decodes that should have cost 2.6 s cost 10.3 s, so the prefetch
/// looked like a five-fold *regression* when what it had actually done was walk
/// into a ceiling.
///
/// Per decoder, which is the thing that was missed first time round. Two runs
/// in one pool had to share this and left runs of twenty-two frames; two pools
/// hold a full run each and neither comes close.
constexpr int kMaxPooledFrames = 40;

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
  // One run per decoder, and each decoder gets its own pool, so the whole
  // ceiling is available to a run — less the few surfaces the decoder itself
  // has in flight beside the ones lent out.
  const int by_pool = kMaxPooledFrames - 6;
  return std::clamp(std::min(afford, by_pool), kMinKeptFrames, kMaxKeptFrames);
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
  // The corners, which the brace list above cannot carry and used not to.
  //
  // Without this a free-drawn mask reached the card with an empty path, the
  // shader found no corners to test a pixel against, and the effect applied to
  // the whole layer — so the outline could be drawn and dragged on the monitor
  // and masked nothing whatever. The two shapes made of numbers were fine,
  // because all ten of their numbers are in that list.
  out.mask.points = pass.mask.points;
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

  /// A second decoder, which does nothing but read ahead of the first.
  ///
  /// Two rather than one for two separate reasons, and either alone would
  /// justify it.
  ///
  /// The prefetch used to seek the *serving* decoder, so the moment a request
  /// missed the run in hand the decoder was somewhere else entirely and the
  /// ordinary path had to seek all the way back — the abandoned prefetch and
  /// the recovery both paid for the same group of pictures.
  ///
  /// And the pool ceiling is per decoder, not per source. Holding two runs in
  /// one decoder's pool ran into a limit the driver does not document at around
  /// sixty-five surfaces, which forced runs down to twenty-two frames — barely
  /// enough turns to spread a group of pictures across, and the reason a stall
  /// survived. Two pools of thirty-two are nowhere near it.
  std::unique_ptr<VideoDecoder> ahead;
  double ahead_position = -1.0;
  /// Turned off when the second decoder cannot be opened or stops working.
  /// Losing it is the stall coming back, not the picture: the run in hand still
  /// serves every frame it holds and the ordinary path still fetches the rest.
  bool keep_ahead = true;
  /// Kept so the second decoder can be opened later, on the first backwards
  /// step, rather than paying for one in every project that never reverses.
  std::string path;
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

  /// The run *before* `recent`, decoded a few frames at a time while the run in
  /// hand is still being played out.
  ///
  /// The cache turned twenty-nine frames of a thirty-frame run into a refcount
  /// bump and left the whole cost of the thirtieth exactly where it was. Driven
  /// on screen that reads as a stall of 264 ms arriving every 520 — the run
  /// being used up, over and over, half the wall clock spent stopped. The
  /// average frame time said 8 ms and was no help at all: it cannot tell thirty
  /// even frames from twenty-nine and a stop.
  ///
  /// So the work is spread instead of shrunk. Serving a frame from the run
  /// costs about a millisecond of a thirty-millisecond budget, and there are
  /// thirty of them before the next seek falls due — which is room enough to
  /// decode the next run a few frames at a time and have it in hand before it
  /// is wanted.
  std::deque<Recent> pending;
  /// The timestamp `pending` is being decoded up to: the start of the run in
  /// hand, so the two meet without a gap.
  double pending_until = -1.0;
  bool extending = false;

  /// The last time asked for, so the direction of travel can be read off. Only
  /// reverse gets the prefetch: forwards never leaves the run behind it.
  double last_request = -1.0;

  void remember(const AVFrame* frame, double at) {
    if (frame == nullptr || keep == 0) return;
    std::unique_ptr<AVFrame, FrameDeleter> copy(av_frame_alloc());
    if (!copy) return;
    if (av_frame_ref(copy.get(), frame) < 0) return;
    recent.push_back(Recent{.at = at, .frame = std::move(copy)});
    trim();
  }

  /// Keeps the two runs together inside one run's worth of surfaces.
  ///
  /// They are drawn from the same pool, which was sized once at `keep`, so
  /// holding a full run *and* a full prefetch would empty it — and an empty
  /// pool is decoding stopping, not slowing. What makes room is that reverse
  /// consumes the run from its newest end: everything above the playhead has
  /// been shown and been left behind, and dropping it hands the surfaces
  /// straight to the prefetch.
  /// Each run is capped on its own, not the pair together, because the pool is
  /// sized for both. Capping the sum instead starves whichever is growing.
  void trim() {
    while (recent.size() > keep) recent.pop_front();
    // A prefetch overruns its cap by everything it had to decode from the
    // keyframe to reach the frames actually wanted. Those are the oldest, the
    // furthest from where reverse is heading, and already spent.
    while (pending.size() > keep) pending.pop_front();
  }

  /// Nothing decoded ahead, and nothing part-decoded either.
  void forget_pending() {
    pending.clear();
    pending_until = -1.0;
    extending = false;
  }

  /// The remembered frame covering `time`, or null. The last one at or before
  /// it, which is the same rule the decoder follows: a frame is shown from its
  /// own timestamp until the next one.
  [[nodiscard]] static const AVFrame* covering(const std::deque<Recent>& run, double time) {
    if (run.empty()) return nullptr;
    // Before the run began, so whatever covers this was never decoded here.
    if (time < run.front().at - kFrameEpsilon) return nullptr;
    // And past the end of it. "The last frame at or before t" is only an answer
    // *within* the run: beyond the newest entry the right frame is one nobody
    // has decoded yet, and handing back the newest would freeze the picture
    // there for ever while playing forwards.
    if (time > run.back().at + kFrameEpsilon) return nullptr;
    const AVFrame* found = nullptr;
    for (const Recent& entry : run) {
      if (entry.at > time + kFrameEpsilon) break;
      found = entry.frame.get();
    }
    return found;
  }

  [[nodiscard]] const AVFrame* remembered(double time) const { return covering(recent, time); }

  /// Whether the run decoded ahead covers `time`.
  ///
  /// Bounded above by where it *meets* the run in hand, not by its own newest
  /// frame. `covering` refuses anything past the last frame it holds, which is
  /// right for a run still being decoded — beyond it the answer is a frame
  /// nobody has produced — and wrong for this one, which was decoded to join a
  /// run that starts at `pending_until`.
  ///
  /// The difference is a rounding width and it cost the whole feature. A
  /// request at 29.6333 against a newest frame at 29.6330 is the frame covering
  /// it, but sits 0.3 ms past the guard: every handover was refused, the run
  /// was thrown away, and the same group of pictures decoded again by the
  /// ordinary path. Prefetch ran, delivered nothing, and measured three times
  /// slower than having none.
  [[nodiscard]] bool pending_covers(double time) const {
    if (pending.empty() || pending_until < 0.0) return false;
    if (time < pending.front().at - kFrameEpsilon) return false;
    return time < pending_until - kFrameEpsilon;
  }
};

}  // namespace

struct FrameRenderer::Impl {
  std::shared_ptr<gpu::Device> device;
  std::unique_ptr<gpu::Compositor> compositor;

  /// Keyed by media id. Decoders are expensive to open and expensive to seek,
  /// so they live as long as the timeline keeps asking for them.
  std::map<std::string, Source> sources;

  /// Whether to read from proxies. Off unless somebody asks, so that export —
  /// which never asks — cannot write the small copy by forgetting something.
  bool use_proxies = false;

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
  /// Decodes a little of the run before the one in hand, spreading the cost of
  /// the next seek across the frames still to be shown from this one.
  /// `budgeted` false runs it to completion, for the case where the playhead
  /// has already arrived and waiting for the rest is cheaper than starting over.
  void decode_ahead(Source& source, bool budgeted = true);

  /// Opens one decoder for a file, sized to lend `keeping` frames.
  [[nodiscard]] std::expected<std::unique_ptr<media::VideoDecoder>, std::string> open_decoder(
      const std::string& path, int keeping);

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

/// Opens one decoder for a file, sized to lend `keeping` frames.
///
/// Decoded on the card, and given this device so the picture can cross onto it.
/// Measured on a 106 Mbps 4K60 capture: 2.76 ms a frame against 7.24 in
/// software, and the software decoder is where most of this application's
/// memory went besides.
///
/// **Direct3D 11**, not 12, and that is the driver's choice rather than a
/// preference. D3D12 video decode is the path that needs no crossing at all,
/// and on the machine this was written for it fails every HEVC picture and
/// removes the device with it:
///
///     [hevc] hardware accelerator failed to decode picture
///     DXGI_ERROR_DRIVER_INTERNAL_ERROR: strong evidence that the driver has
///     performed an undefined operation
///
/// The stock `ffmpeg.exe` fails identically on the same file, so it is not
/// something this is doing wrong — and the removal takes the *compositor's*
/// device, since that is the one the decoder was handed. D3D11 decodes the same
/// file at 558 fps and the frames cross with one copy inside the card.
///
/// Software is still the floor, and `open` falls to it on its own when neither
/// works.
std::expected<std::unique_ptr<VideoDecoder>, std::string> FrameRenderer::Impl::open_decoder(
    const std::string& path, int keeping) {
  return VideoDecoder::open(
      path, {.preferred = media::Acceleration::D3D11Va,
             // Surfaces for the run this decoder is asked to keep. Without them
             // the pool empties the moment reverse starts and decoding stops
             // outright: "Static surface pool size exceeded", then
             // `get_buffer() failed`.
             //
             // The slack is not optional. A full run is held by the caller and
             // beside it sit the held last frame and whatever the decoder has
             // in flight; sized at exactly a run the pool empties again.
             .extra_frames = keeping + 6,
             .d3d12_device = device != nullptr ? device->native_device() : nullptr});
}

/// Decodes a little of the run *before* the one in hand, so that run is ready
/// before the playhead reaches the end of this one.
///
/// A time budget rather than a frame count. What has to be decoded to reach a
/// given frame is a whole group of pictures, which is a hundred-odd frames on
/// this footage and a handful on other footage, so counting frames would spend
/// wildly different amounts of the turn depending on the file. The budget is
/// the thing that must not be exceeded, so the budget is what is counted.
void FrameRenderer::Impl::decode_ahead(Source& source, bool budgeted) {
  if (!source.usable || !source.keep_ahead || source.keep == 0 || source.recent.empty()) {
    return;
  }

  // The prefetch has to meet the run in hand exactly: a gap between them reads
  // as the end of a run, and "the last frame at or before t" would then answer
  // from the wrong side of the hole.
  const double meet = source.recent.front().at;
  if (meet <= kFrameEpsilon) return;  // nothing before the start of the source

  const double fps = source.decoder->stream().fps;
  const double gap = fps > 0.0 ? 1.0 / fps : kAssumedFrameGap;

  if (source.pending.size() >= source.keep && !source.extending) return;
  const std::size_t room = source.keep;

  // The reading decoder, opened the first time one is wanted. A project that is
  // never played backwards never pays for it.
  if (source.ahead == nullptr) {
    auto opened = open_decoder(source.path, static_cast<int>(source.keep));
    if (!opened.has_value()) {
      // Not fatal. Without a second decoder the run in hand still serves every
      // frame it holds, and the ordinary path still fetches the rest — it is
      // the stall coming back, not the picture.
      source.keep_ahead = false;
      return;
    }
    source.ahead = std::move(*opened);
  }

  if (!source.extending) {
    // Far enough back that a seek lands on a keyframe at or before the frames
    // wanted. Aiming at the frame just before the run in hand would seek to the
    // keyframe before *that* and decode the same pictures either way, so the
    // span is chosen to fill the room rather than to be short.
    const double from_time = std::max(0.0, meet - static_cast<double>(room) * gap);
    if (!source.ahead->seek(from_time)) {
      source.keep_ahead = false;
      return;
    }
    source.ahead_position = -1.0;
    source.pending_until = meet;
    source.extending = true;
  }

  // How much of the run in hand is still to be shown. Reverse consumes it from
  // the newest end, so what is left is everything below the playhead.
  std::size_t left = 0;
  for (const Source::Recent& entry : source.recent) {
    if (entry.at < source.last_request + kFrameEpsilon) ++left;
  }

  // A budget that grows as the run runs out. What has to be decoded is a whole
  // group of pictures and there is no telling how long that is until it has
  // been done, so a fixed share of each turn is a guess that is sometimes
  // wrong: measured, about one run in four finished late and each one of those
  // was a stall of a quarter of a second. Spending more of the last few turns
  // makes those turns heavier and makes the stall not happen, which is the
  // trade this whole mechanism exists to make.
  auto budget = kPrefetchBudget;
  if (left * 4 < source.keep) budget *= 3;
  else if (left * 2 < source.keep) budget *= 2;

  const auto started = std::chrono::steady_clock::now();
  while (source.extending) {
    if (budgeted && std::chrono::steady_clock::now() - started >= budget) return;

    const auto got = source.ahead->next_frame();
    if (!got) {
      source.keep_ahead = false;
      source.forget_pending();
      return;
    }
    if (!*got) {
      // The source ended before the run did, which means the run in hand is
      // already at the end of the file. Nothing to decode ahead of it.
      source.forget_pending();
      return;
    }
    source.ahead_position = source.ahead->timestamp();

    // Everything on the way past that belongs before the run in hand. The
    // frames decoded before the wanted span are what a seek to a keyframe
    // costs, and they are dropped by `trim` as newer ones arrive.
    if (source.ahead_position < source.pending_until - kFrameEpsilon) {
      std::unique_ptr<AVFrame, FrameDeleter> copy(av_frame_alloc());
      if (copy && av_frame_ref(copy.get(), source.ahead->frame()) >= 0) {
        source.pending.push_back(
            Source::Recent{.at = source.ahead_position, .frame = std::move(copy)});
        ++stats.frames_decoded_ahead;
        source.trim();
      }
      continue;
    }

    // Met the run in hand. Done, and the join is exact.
    source.extending = false;
  }
}

const AVFrame* FrameRenderer::Impl::frame_at(const core::Media& media, double time,
                                            const media::VideoDecoder** from) {
  const std::string& wanted = core::source_path(media, use_proxies);

  // Into the file's own time. Everything above this line — the plan, the
  // segments, a clip's source range — is in *source* seconds, which are the
  // file's own unless somebody has conformed the footage to another rate. This
  // is one of the two places in the application that knows the difference, and
  // the other is where the mixer asks for sound.
  //
  // Below one for the usual case, 60 shown at 24: ten seconds of source is
  // four seconds of file, which is what makes every frame a real one.
  time *= core::conform_speed(media);

  auto found = sources.find(media.id);
  // Open on a different file from the one this source now means. Proxies being
  // switched on or off is the case, and a relink is the other; sources are kept
  // by media id, which is right until the file behind that id changes.
  //
  // Dropped rather than reported, because the answer is simply to open the
  // right one — and left to the caller it would be a rule somebody has to
  // remember, which is the same rule that would be forgotten.
  if (found != sources.end() && found->second.path != wanted) {
    sources.erase(found);
    found = sources.end();
  }

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
    source.path = wanted;
    auto opened = open_decoder(wanted, keeping);
    if (!opened.has_value()) {
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

  // Which way we are travelling, read off the requests themselves. Only reverse
  // wants a prefetch: playing forwards never leaves the run behind it.
  const double previous = source.last_request;
  source.last_request = time;
  const bool reversing = previous >= 0.0 && time < previous - kFrameEpsilon;

  // The playhead has left the run in hand. If a prefetch was on its way to
  // exactly this frame, finish it rather than throwing it away — the work is
  // already part done, and abandoning it means seeking back and decoding the
  // very same group of pictures a second time. That was the whole of why an
  // early version measured *worse* than no prefetch: 556 frames decoded ahead,
  // two runs of it ever used, and every abandoned one paid for twice.
  if (source.remembered(time) == nullptr && source.extending &&
      time < source.pending_until + kFrameEpsilon) {
    decode_ahead(source, false);
  }

  // The run decoded ahead of need has caught up with the playhead. Taking it
  // whole rather than merging: the two were decoded to meet exactly, and this
  // is the moment the stall used to happen.
  if (source.remembered(time) == nullptr && source.pending_covers(time)) {
    source.recent = std::move(source.pending);
    // And the decoders change places with the runs, because a frame belongs to
    // the pool it was lent from: these were decoded by the reading decoder, and
    // asking the other one for their textures would hand back a picture from
    // somewhere else entirely. Swapping is also what leaves the reading decoder
    // positioned where the *next* run has to start from.
    std::swap(source.decoder, source.ahead);
    std::swap(source.position, source.ahead_position);
    source.exhausted = false;
    source.forget_pending();
    ++stats.runs_taken_ahead;
  }

  // Decoded a moment ago and kept. This is what makes playing backwards
  // possible at all: every frame of the run behind the playhead is one this
  // decoder already produced on its way forward, and handing it back costs
  // nothing where decoding it again costs a seek and a whole GOP.
  if (const AVFrame* known = source.remembered(time); known != nullptr) {
    ++stats.frames_remembered;
    if (reversing) decode_ahead(source);
    if (from != nullptr) *from = source.decoder.get();
    return known;
  }

  // Out of cache, so whatever was being decoded ahead is about to be overtaken
  // by an ordinary seek. Dropping it first returns its surfaces to the pool,
  // which the decode below is about to want.
  source.forget_pending();

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
  //
  // **Half a frame of slack, not floating-point noise.** A source's timestamps
  // are only as fine as its container's time base, and Matroska's is a whole
  // millisecond — so a 60 fps capture is stamped 0, 17, 33, 50, 67, 83 ms while
  // playback asks for 0, 16.667, 33.333, 50, 66.667, 83.333. Against a tolerance
  // of 0.1 ms the frame stamped 33 does not answer a request for 33.333, so this
  // decoded past it to 50 — skipping that frame, and then answering the *next*
  // request with the 50 it had already shown. One request in three, which is why
  // a 4K60 capture played at 38 of its 60 frames with every part of the renderer
  // keeping up: the frames were being asked for wrongly, not arriving late.
  //
  // Half a frame is what "the nearest frame" means, and it is the largest slack
  // that cannot reach into the neighbouring frame. It shows a picture at most
  // half a frame early, which is the same trade the backwards tolerance above
  // already makes and is a good deal smaller.
  const double slack = std::max(kFrameEpsilon, frame_gap * 0.5);
  while (!source.exhausted) {
    if (source.position >= time - slack && source.position >= 0.0) break;

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

void FrameRenderer::set_use_proxies(bool use_proxies) { impl_->use_proxies = use_proxies; }
bool FrameRenderer::use_proxies() const noexcept { return impl_->use_proxies; }

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

  if (auto ok = d.compositor->resize(project.sequence().canvas_w, project.sequence().canvas_h); !ok) {
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
