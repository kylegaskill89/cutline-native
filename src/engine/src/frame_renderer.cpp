#include "cutline/engine/frame_renderer.hpp"

#include "cutline/core/graphic.hpp"
#include "cutline/core/interpret.hpp"
#include "cutline/core/nesting.hpp"
#include "cutline/core/sequences.hpp"
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
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <map>
#include <span>
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

/// How far down the sequence to look for the next cut.
///
/// Long enough for the cut reader to absorb the worst seek measured — 287 ms
/// on a 4K capture — and short enough that a scrub does not spend the whole
/// time preparing for cuts it never reaches.
constexpr double kCutLookahead = 0.5;

/// How far back down the sequence to look for the next cut while it plays
/// backwards. Twice the forward distance, because a backwards cut costs more:
/// the run has to end on the far side's last frame, so it is a whole run plus
/// the group of pictures before it -- measured at 440 to 650 ms on the
/// reference capture while the run before was being read beside it, which
/// half a second could not hold. The third decoder is free again the moment a
/// cut is taken, so looking further costs nothing but the earlier start.
constexpr double kCutLookbehind = 1.0;

/// How many frames to have waiting on the far side of a cut.
///
/// Only the join has to be covered: once the playhead is across, the serving
/// decoder is seeked by the ordinary path and carries on as normal. A quarter
/// of a second at 60 fps, which is long enough for that to happen unnoticed
/// and short enough that the surfaces it borrows are not missed.
constexpr std::size_t kJumpRunFrames = 16;

/// How many sequences deep a nest may go before it stops being drawn.
///
/// Not what prevents a cycle — that is refused when the nest is made, by
/// `core::sequence_contains`, because there is no sensible behaviour for one.
/// This is the floor under a project that already holds one: a file written by
/// hand, or by a build that did not check. Past it the nest draws nothing,
/// which is a hole somebody can see rather than a stack that runs out.
///
/// Eight, which is far past anything anybody nests on purpose and far short of
/// anything that costs.
constexpr int kMaxNestDepth = 8;

/// How far a cut's target may drift and still be the same cut.
///
/// The target is read off the plan at `t + kCutLookahead`, so as the playhead
/// approaches a cut it names a moment slightly further into the clip after it
/// on every turn. Treating each of those as a new run re-seeked the second
/// decoder every frame and finished nothing — the prefetch ran the whole way to
/// the cut and arrived with one frame decoded. What is wanted is the run that
/// *starts* at the cut, so anything within the lookahead of the run in flight
/// is that run.
constexpr double kSameCut = kCutLookahead + kFrameEpsilon;

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
/// This is the budget for **one** run, and up to three are held while playing
/// backwards through cuts: the one being played out, the one before it, and
/// the far side of the next cut. So the real ceiling is three times this —
/// about 1.1 GB on 4K footage, and proportionally less on anything smaller,
/// which is a great deal for a cache and is what buys reverse playback that
/// does not stop. Forwards only the first and the far side of a cut are held,
/// and that one is a quarter of a second rather than a run.
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
  // a quarter-resolution pair of chroma planes. Twice that past eight bits,
  // where a sample takes sixteen -- a 10-bit 4K pool sized for eight would
  // hold twice the memory the budget says.
  const std::size_t per_frame = width * height * 3 / 2 * (media.bit_depth.value_or(8) > 8 ? 2 : 1);
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
    case AV_PIX_FMT_D3D11: {
      // What the card decoded into, which for 10-bit footage is P010.
      const auto* frames = frame->hw_frames_ctx != nullptr
                               ? reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data)
                               : nullptr;
      const bool deep = frames != nullptr && frames->sw_format == AV_PIX_FMT_P010LE;
      out.layout = deep ? gpu::PixelLayout::P010 : gpu::PixelLayout::Nv12;
      out.bits = deep ? 10 : 8;
      decoded_on_the_card = true;
      break;
    }
    case AV_PIX_FMT_NV12:
      out.layout = gpu::PixelLayout::Nv12;
      break;
    case AV_PIX_FMT_YUV420P:
      out.layout = gpu::PixelLayout::Yuv420p;
      break;
    case AV_PIX_FMT_P010LE:
      out.layout = gpu::PixelLayout::P010;
      out.bits = 10;
      break;
    case AV_PIX_FMT_YUV420P10LE:
      out.layout = gpu::PixelLayout::Yuv420p16;
      out.bits = 10;
      break;
    case AV_PIX_FMT_YUV420P12LE:
      out.layout = gpu::PixelLayout::Yuv420p16;
      out.bits = 12;
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

  out.primaries = frame->color_primaries == AVCOL_PRI_BT2020 ? gpu::ColourPrimaries::Bt2020
                                                             : gpu::ColourPrimaries::Bt709;

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

  /// A second decoder, which reads the run before the one in hand, for
  /// playing backwards, and nothing else.
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

  /// What the second decoder is building: the run *before* the one in hand,
  /// for playing backwards, bounded above by where it meets that run.
  enum class Ahead {
    None,
    Previous,
  };
  Ahead ahead_for = Ahead::None;

  /// A third decoder, which reads the far side of cuts and nothing else.
  ///
  /// With two, the reading decoder had two jobs that both wanted it at once
  /// when playing backwards through short clips: the run before the one in
  /// hand, and the far side of the next cut. Done in turn, the second started
  /// with half a second left for up to 280 frames of 4K decoding, and fifty
  /// one-second cuts played backwards hitched three to six times in six
  /// seconds. The owner chose a third decoder over the hitches, knowing it
  /// costs another surface pool -- about 450 MB of video memory at 4K -- once
  /// a source has a cut read.
  std::unique_ptr<VideoDecoder> jumper;
  double jumper_position = -1.0;
  /// Where its run begins, in source seconds; below zero when there is none.
  double jump_from = -1.0;
  /// A run built for playing *backwards* across a cut, which ends at the far
  /// side's last frame rather than starting at its first. `jump_until` is that
  /// frame, in source seconds.
  bool jump_back = false;
  double jump_until = -1.0;
  /// Still being read.
  bool jumping = false;
  /// Turned off when the third decoder cannot be opened or stops working.
  bool keep_jumping = true;

  /// Where the clip showing this source begins, in source seconds, while the
  /// sequence plays backwards; below zero when there is no such edge -- not
  /// reversing, or the clip before it carries on from the same moment.
  ///
  /// Reverse goes no further back than this before it cuts to somewhere else,
  /// so the run before a run that already reaches it is a run nobody sees.
  /// Asked for anyway, it held the second decoder and shared the card with the
  /// cut being read beside it, and both arrived late.
  double reverse_floor = -1.0;
  /// The second decoder's run was built for the far side of the next cut, not
  /// for the run in hand, and is kept for it until the cut is crossed.
  bool before_cut = false;

  /// Which shared decoding device each decoder is on, swapped with them when
  /// runs are taken, so a decoder opened later goes on the one left free and
  /// no two of a source's decoders queue behind each other.
  int decoder_device = 0;
  int ahead_device = -1;
  int jumper_device = -1;
  [[nodiscard]] int free_device() const noexcept {
    for (int device = 0; device < 3; ++device) {
      if (device != decoder_device && device != ahead_device && device != jumper_device) {
        return device;
      }
    }
    return 0;
  }
  /// What the reader thread and this thread share about the run being built.
  ///
  /// The reader decodes the far side of a cut on a thread of its own, so the
  /// second decoder, `pending` and everything describing them belong to
  /// whichever of the two holds `lock`. Behind a pointer because a source is
  /// moved into the map, and neither a mutex nor an atomic can be.
  struct Shared {
    std::mutex lock;
    /// The source times the reader's job will answer, from `target` up to
    /// `target_end`. Read without the lock, which is the point: the serving
    /// path asks on every frame whether it has to wait, and must not wait to
    /// find out.
    ///
    /// A span rather than a start and an assumed length. The forward run
    /// starts at its cut, but a backwards one *ends* there, and the run before
    /// the one in hand ends where that one begins. Read as "half a second from
    /// the start", the frame a backwards run was built for fell a frame past
    /// the window, the serving path declined to wait, and it sought the same
    /// group of pictures the reader was a few frames from finishing.
    std::atomic<double> target{-1.0};
    std::atomic<double> target_end{-1.0};
    /// A job for this source is queued or running. Under `lock`.
    bool posted = false;

    /// The same three for the third decoder, which has a lock of its own so
    /// that reading a cut and reading the run before never wait for each other.
    std::mutex jump_lock;
    std::atomic<double> jump_begins{-1.0};
    std::atomic<double> jump_ends{-1.0};
    bool jump_posted = false;
  };
  std::unique_ptr<Shared> shared = std::make_unique<Shared>();

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

  /// The run *before* `recent`, decoded on the run reader while the run in
  /// hand is still being played out.
  ///
  /// The cache turned twenty-nine frames of a thirty-frame run into a refcount
  /// bump and left the whole cost of the thirtieth exactly where it was. Driven
  /// on screen that reads as a stall of 264 ms arriving every 520 — the run
  /// being used up, over and over, half the wall clock spent stopped. The
  /// average frame time said 8 ms and was no help at all: it cannot tell thirty
  /// even frames from twenty-nine and a stop.
  ///
  /// So the work is moved instead of shrunk. It was spread first, a few
  /// frames in each of the render thread's turns, and that made those turns
  /// cost 25 to 37 ms; on a thread of its own the next run is read whole while
  /// thirty frames of this one are shown.
  std::deque<Recent> pending;
  /// The timestamp `pending` is being decoded up to: the start of the run in
  /// hand, so the two meet without a gap.
  double pending_until = -1.0;
  bool extending = false;

  /// The third decoder's run: the far side of the next cut.
  std::deque<Recent> jumped;

  /// The last time asked for, so the direction of travel can be read off. Only
  /// reverse gets the prefetch: forwards never leaves the run behind it.
  double last_request = -1.0;

  void remember(const AVFrame* frame, double at) {
    if (frame == nullptr || keep == 0) return;
    std::unique_ptr<AVFrame, FrameDeleter> copy(av_frame_alloc());
    if (!copy) return;
    if (av_frame_ref(copy.get(), frame) < 0) return;
    recent.push_back(Recent{.at = at, .frame = std::move(copy)});
    trim_recent();
  }

  /// Keeps the two runs together inside one run's worth of surfaces.
  ///
  /// They are drawn from the same pool, which was sized once at `keep`, so
  /// holding a full run *and* a full prefetch would empty it — and an empty
  /// pool is decoding stopping, not slowing. What makes room is that reverse
  /// consumes the run from its newest end: everything above the playhead has
  /// been shown and been left behind, and dropping it hands the surfaces
  /// straight to the prefetch.
  /// Each run is capped on its own, not the runs together, because each pool
  /// is sized for one. Capping the sum instead starves whichever is growing.
  /// And each by the thread that owns it: the run in hand is the serving
  /// thread's, the other two the readers' while they work.
  void trim_recent() {
    while (recent.size() > keep) recent.pop_front();
  }
  /// A prefetch overruns its cap by everything it had to decode from the
  /// keyframe to reach the frames actually wanted. Those are the oldest, the
  /// furthest from where reverse is heading, and already spent.
  void trim_pending() {
    while (pending.size() > keep) pending.pop_front();
  }
  void trim_jumped() {
    while (jumped.size() > keep) jumped.pop_front();
  }

  /// Nothing decoded ahead, and nothing part-decoded either.
  void forget_pending() {
    before_cut = false;
    pending.clear();
    pending_until = -1.0;
    ahead_for = Ahead::None;
    extending = false;
  }

  /// No cut read, and none part-read.
  void forget_jump() {
    jumped.clear();
    jump_from = -1.0;
    jump_back = false;
    jump_until = -1.0;
    jumping = false;
  }

  /// How near a frame's timestamp has to be to a request to answer it: half a
  /// source frame, set when the decoder opens and its rate is known.
  ///
  /// The same slack the forward decode loop uses, and it has to be. A run is
  /// what reverse plays from, so with a tighter rule here the two directions
  /// disagreed about which frame a moment *is*: on Matroska's millisecond grid
  /// a request at 16.667 ms took the frame stamped 17 going forwards and the
  /// one stamped 0 coming back.
  double slack = kFrameEpsilon;

  /// The remembered frame nearest `time`, or null. The last one at or before
  /// it once half a frame of slack is allowed, which is the rule the decoder
  /// follows going forwards: a frame answers from half a frame before its own
  /// timestamp until half a frame before the next one's.
  [[nodiscard]] static const AVFrame* covering(const std::deque<Recent>& run, double time,
                                               double slack) {
    if (run.empty()) return nullptr;
    // Before the run began, so whatever covers this was never decoded here.
    if (time < run.front().at - slack) return nullptr;
    // And past the end of it. "The last frame at or before t" is only an answer
    // *within* the run: beyond the newest entry the right frame is one nobody
    // has decoded yet, and handing back the newest would freeze the picture
    // there for ever while playing forwards.
    if (time > run.back().at + slack) return nullptr;
    const AVFrame* found = nullptr;
    for (const Recent& entry : run) {
      if (entry.at > time + slack) break;
      found = entry.frame.get();
    }
    return found;
  }

  [[nodiscard]] const AVFrame* remembered(double time) const {
    return covering(recent, time, slack);
  }

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
    if (pending.empty()) return false;
    if (time < pending.front().at - slack) return false;
    if (pending_until < 0.0) return false;
    return time < pending_until - kFrameEpsilon;
  }

  /// Whether the far side of a cut covers `time`. It has no run in hand to
  /// meet, so what bounds it is simply how much of it has been decoded, and
  /// refusing anything past its newest frame is right here for the same reason
  /// it is wrong above: beyond it, nobody has decoded the frame yet.
  [[nodiscard]] bool jumped_covers(double time) const {
    return covering(jumped, time, slack) != nullptr;
  }
};

}  // namespace

struct FrameRenderer::Impl {
  std::shared_ptr<gpu::Device> device;
  /// The queue every compositor here records onto, its nests' included. Null
  /// for the device's own.
  std::shared_ptr<gpu::Queue> queue;
  std::unique_ptr<gpu::Compositor> compositor;

  /// Keyed by media id. Decoders are expensive to open and expensive to seek,
  /// so they live as long as the timeline keeps asking for them.
  std::map<std::string, Source> sources;

  /// Whether to read from proxies. Off unless somebody asks, so that export —
  /// which never asks — cannot write the small copy by forgetting something.
  bool use_proxies = false;

  /// A renderer of its own per nested sequence, keyed by sequence id.
  ///
  /// Its own rather than this one recursing into itself, and that is not a
  /// convenience: a compositor holds one scene target, and a nest has to be
  /// composited whole *before* the frame it sits in is. Two renderers means two
  /// targets, two sets of decoders, and no state to unwind between them.
  ///
  /// Kept between frames for the same reason the decoders are: a nest that had
  /// to be rebuilt every frame would reopen every file inside it.
  std::map<std::string, std::unique_ptr<FrameRenderer>> nested;

  /// How deep the nesting being rendered is, counted from the outermost.
  ///
  /// A cycle is refused when it is *made* — see `core::sequence_contains` — so
  /// this is not what stops one. It is what stops a project that already holds
  /// one, written by hand or by an older build, from taking the process down
  /// with it: past the limit the nest simply draws nothing, which is a visible
  /// hole rather than a stack overflow.
  int depth = 0;

  std::vector<std::string> missing;

  /// Frame views must outlive the compose call that reads them, and they point
  /// into decoder-owned memory, so both are kept alive across the call.
  std::vector<gpu::FrameView> views;

  DecodeStats stats;
  /// The time last rendered, so the direction of play can be told apart from
  /// the direction a cut jumps in the source. Below zero before the first.
  double last_time = -1.0;
  /// Whether this frame is earlier on the timeline than the last. Decided once
  /// per frame, before any source is asked for anything.
  bool playing_backwards = false;
  /// Frames the second decoder has produced. Counted from two threads.
  std::atomic<std::uint64_t> decoded_ahead{0};

#if CUTLINE_HAVE_TEXT
  /// Graphic layers as measured and drawn, keyed by media and layer id.
  ///
  /// Drawing text costs about as much as decoding a frame, and a layer's
  /// picture does not change when it only moves -- which is what an animated
  /// graphic mostly does -- so each is drawn once and kept, and the compositor
  /// moves it. What it was drawn from is kept beside it: comparing the look
  /// catches every edit without anyone having to remember to say so. A few
  /// looks per layer, because a shape stretched by its pins and a typewriter
  /// part-way through are drawn more than one way in a frame.
  struct DrawnLayer {
    core::GraphicLayer look;
    std::size_t characters = core::kAllCharacters;
    core::LayerExtent extent;
    std::optional<text::Raster> raster;
  };
  std::map<std::string, std::vector<DrawnLayer>> drawn_layers;

  /// The entry for a layer as it looks now, made if it is not in hand.
  [[nodiscard]] DrawnLayer& drawn(const std::string& media_id, const core::GraphicLayer& layer,
                                  std::size_t characters);

  /// One compositor per graphic on screen, keyed by clip: two graphics in a
  /// frame are two pictures, both alive until the frame is composited.
  std::map<std::string, std::unique_ptr<gpu::Compositor>> graphic_compositors;

  /// Composites a graphic's layers into a frame-sized picture, still on the
  /// card, as a nest is. Empty when there is nothing to draw.
  [[nodiscard]] gpu::SceneTexture graphic_picture(const core::Project& project,
                                                  const core::Clip& clip,
                                                  const core::Media& media, double t);
#endif

  /// Positions the source at `time` and returns its frame, or null.
  /// `from` receives the decoder that produced the frame, which is what a
  /// hardware frame needs asking for its texture. Null past the end of a
  /// stream, where the held frame stands in and no decoder owns it.
  /// Decodes the run before the one in hand, which begins at `meet`, on the
  /// run reader. Passed in rather than read here because the run in hand is
  /// the serving thread's.
  void decode_ahead(Source& source, double meet);

  /// Asks the run reader for the run before the one in hand, if it is not
  /// already at it. On the serving thread, with the second decoder's lock held.
  void read_before(Source& source);

  /// Asks the run reader for the run that ends at `meet`. Same thread, same
  /// lock, and the second decoder already open.
  void post_before(Source& source, double meet);

  /// Composites a nested sequence and hands back the picture, still on the
  /// card. Empty when there is nothing to draw, or the nesting is too deep.
  [[nodiscard]] gpu::SceneTexture nested_picture(const core::Project& project,
                                                 const core::Media& media, double at);

  /// Decodes a run at `target`, somewhere else in the source, because the
  /// sequence cuts to it shortly.
  ///
  /// The forward twin of `decode_ahead`. A rough cut is made of pieces taken
  /// from scattered parts of a capture, so crossing a cut means seeking and
  /// decoding from the keyframe before the new position — measured on a 4K
  /// capture at 96 to 287 ms, on the render thread, at *every* boundary. The
  /// frames either side of a cut were fine; the frame on it stopped.
  ///
  /// Moved rather than shrunk: there is no making a group of pictures cheaper,
  /// only somewhere else to put the cost, and the cut reader has the half
  /// second before a cut to itself.
  void prefetch_jump(Source& source, double target);

  /// The same for playing backwards: a run that *ends* at `until`, the last
  /// frame of the clip the sequence is about to cut back into.
  ///
  /// Backwards, a cut lands on the end of the earlier clip, which is as likely
  /// as not the middle of a group of pictures -- the whole group decoded to
  /// reach it, on the frame of the cut. Fifty one-second cuts played in
  /// reverse stalled up to 364 ms, nine times over 50 ms in six seconds.
  void prefetch_jump_back(Source& source, double until);

  /// Opens the third decoder, on the serving thread with its lock held.
  /// Returns whether there is one.
  [[nodiscard]] bool open_jumper(Source& source);

  /// Looks a little way back down the sequence while it plays backwards.
  void prefetch_cuts_back(const core::Project& project, double t,
                          std::span<const render::PlannedLayer> now);

  /// Looks a little way down the sequence and starts whatever the cut after
  /// this one is going to need.
  void prefetch_cuts(const core::Project& project, double t,
                     std::span<const render::PlannedLayer> now);

  /// Opens one decoder for a file, sized to lend `keeping` frames.
  ///
  /// `decoding_device` is which shared decoding device. A source's decoders are always
  /// on different ones, so that one reading ahead never queues its work in
  /// front of another's.
  [[nodiscard]] std::expected<std::unique_ptr<media::VideoDecoder>, std::string> open_decoder(
      const std::string& path, int keeping, int decoding_device);

  [[nodiscard]] const AVFrame* frame_at(const core::Media& media, double time,
                                        const media::VideoDecoder** from = nullptr);

  /// Decodes ahead of need on a thread of its own.
  ///
  /// A cut into the middle of a long group of pictures costs a seek and up to
  /// the whole group: on the reference capture a keyframe every 4.17 s, so as
  /// many as 250 frames, two to four hundred milliseconds of decoding. Done in
  /// twelve-millisecond slices of the render thread's turns it did not fit in
  /// the half second before a cut, and a montage of one-second clips stalled
  /// at every one of them -- fourteen gaps over 50 ms in fifteen seconds, the
  /// worst 842. On a thread of its own it runs at the decoder's full speed
  /// alongside the frames still being shown. The run before the one in hand,
  /// for reverse, was in the same slices, and those slices made whole
  /// stretches of reverse cost 25 to 37 ms a frame.
  struct Reader {
    std::mutex lock;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    bool busy = false;
    bool stopping = false;
    std::thread thread;

    Reader() {
      thread = std::thread([this] {
        while (true) {
          std::function<void()> job;
          {
            std::unique_lock<std::mutex> held(lock);
            wake.wait(held, [this] { return stopping || !jobs.empty(); });
            if (stopping) return;
            job = std::move(jobs.front());
            jobs.pop_front();
            busy = true;
          }
          job();
          {
            const std::lock_guard<std::mutex> held(lock);
            busy = false;
          }
          wake.notify_all();
        }
      });
    }
    ~Reader() {
      {
        const std::lock_guard<std::mutex> held(lock);
        stopping = true;
        jobs.clear();
      }
      wake.notify_all();
      if (thread.joinable()) thread.join();
    }
    void post(std::function<void()> job) {
      {
        const std::lock_guard<std::mutex> held(lock);
        jobs.push_back(std::move(job));
      }
      wake.notify_all();
    }
    /// Everything queued, finished. Called before any source is let go of.
    void drain() {
      std::unique_lock<std::mutex> held(lock);
      wake.wait(held, [this] { return jobs.empty() && !busy; });
    }
  };
  /// One for cuts and one for the run before, each with its own decoder, so
  /// that neither job ever queues behind the other.
  ///
  /// Declared last so they are destroyed first: they hold references to
  /// sources, and stop before those go.
  Reader cut_reader;
  Reader run_reader;

  /// Everything either reader has queued, finished. Before any source goes.
  void drain_readers() {
    cut_reader.drain();
    run_reader.drain();
  }
};

#if CUTLINE_HAVE_TEXT

namespace {

/// Whether two versions of a layer draw the same picture: everything but
/// where it is and how it moves, which the compositor handles.
[[nodiscard]] bool same_look(const core::GraphicLayer& a, const core::GraphicLayer& b) {
  return a.kind == b.kind && a.text == b.text && a.box_width == b.box_width &&
         a.width == b.width && a.height == b.height && a.corner_radius == b.corner_radius &&
         a.path == b.path && a.style == b.style;
}

/// Looks kept per layer before the oldest is let go.
constexpr std::size_t kLooksPerLayer = 4;

}  // namespace

FrameRenderer::Impl::DrawnLayer& FrameRenderer::Impl::drawn(const std::string& media_id,
                                                           const core::GraphicLayer& layer,
                                                           std::size_t characters) {
  std::vector<DrawnLayer>& looks = drawn_layers[media_id + '\x1f' + layer.id];
  for (DrawnLayer& entry : looks) {
    if (entry.characters == characters && same_look(entry.look, layer)) return entry;
  }
  if (looks.size() >= kLooksPerLayer) looks.erase(looks.begin());
  // Measured by the same layout the drawing uses, so the quad is exactly the
  // size of the picture that fills it.
  looks.push_back(DrawnLayer{.look = layer, .characters = characters,
                             .extent = text::measure(layer), .raster = std::nullopt});
  return looks.back();
}

gpu::SceneTexture FrameRenderer::Impl::graphic_picture(const core::Project& project,
                                                       const core::Clip& clip,
                                                       const core::Media& media, double t) {
  if (!media.graphic.has_value()) return {};
  const core::Sequence& sequence = project.sequence();
  const int width = sequence.canvas_w;
  const int height = sequence.canvas_h;
  if (width <= 0 || height <= 0) return {};

  const std::vector<core::PlacedLayer> placed = core::place_graphic(
      *media.graphic, t - clip.start, core::clip_duration(clip), width, height,
      [this, &media](const core::GraphicLayer& layer) {
        return drawn(media.id, layer, core::kAllCharacters).extent;
      });
  if (placed.empty()) return {};

  auto found = graphic_compositors.find(clip.id);
  if (found == graphic_compositors.end()) {
    auto made = gpu::Compositor::create(device, width, height, queue);
    if (!made.has_value()) return {};
    found = graphic_compositors.emplace(clip.id, std::move(*made)).first;
  }
  gpu::Compositor& graphic = *found->second;
  if (!graphic.resize(width, height).has_value()) return {};

  // Reserved: the layers point into both.
  std::vector<gpu::FrameView> frames;
  frames.reserve(placed.size());
  std::vector<std::vector<gpu::EffectPass>> passes;
  passes.reserve(placed.size());
  std::vector<Layer> layers;
  layers.reserve(placed.size());

  for (const core::PlacedLayer& at : placed) {
    // At the size its pins stretched it to, which is a different picture.
    core::GraphicLayer look = *at.layer;
    look.width = at.width;
    look.height = at.height;
    DrawnLayer& entry = drawn(media.id, look, at.characters);
    if (!entry.raster.has_value()) {
      auto made = text::rasterise(look, at.characters);
      // Nothing drawable -- no text yet, or a picture that cannot be read --
      // is left out rather than drawn as a blank rectangle.
      entry.raster = made.has_value() ? std::move(*made) : text::Raster{};
    }
    const text::Raster& raster = *entry.raster;
    if (raster.empty()) continue;

    gpu::FrameView view;
    view.width = raster.width;
    view.height = raster.height;
    view.layout = gpu::PixelLayout::Rgba8;
    // Not video: a rasteriser works in full-range sRGB, and treating it as
    // studio-range would lift the blacks of every title.
    view.full_range = true;
    view.planes[0] = gpu::PlaneView{.data = raster.pixels.data(), .stride = raster.stride()};
    frames.push_back(view);

    Layer layer;
    layer.frame = &frames.back();
    layer.quad = {static_cast<float>(at.box.center_x), static_cast<float>(at.box.center_y),
                  static_cast<float>(at.box.width), static_cast<float>(at.box.height),
                  static_cast<float>(at.box.rotation_deg)};
    layer.opacity = static_cast<float>(std::clamp(at.opacity, 0.0, 1.0));
    passes.emplace_back();
    if (at.wipe < 1.0) {
      // A wipe is a crop from the right that shrinks to nothing.
      passes.back().push_back(to_gpu_pass(
          render::crop_pass(0.0f, 0.0f, static_cast<float>(1.0 - std::max(0.0, at.wipe)), 0.0f)));
    }
    layer.passes = passes.back();
    layers.push_back(layer);
  }

  // In the sequence's colour, as a nest is: graphics white lands where the
  // sequence puts it, and an HDR sequence is taken out at its real brightness.
  graphic.set_colour(gpu::ColourSettings{
      .working = static_cast<gpu::WorkingColour>(sequence.colour),
      .graphics_white_nits = static_cast<float>(sequence.graphics_white),
      .tone_map = sequence.tone_map});
  const bool hdr = sequence.colour != core::WorkingColour::Rec709;
  graphic.set_hdr_display(hdr ? std::optional<float>(80.0f) : std::nullopt);
  if (!graphic.compose(layers).has_value()) return {};
  const auto picture = graphic.display_texture();
  return picture.has_value() ? *picture : gpu::SceneTexture{};
}

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
    const std::string& path, int keeping, int decoding_device) {
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
             .d3d12_device = device != nullptr ? device->native_device() : nullptr,
             .d3d11_device = decoding_device});
}

/// Decodes the run *before* the one in hand, so that run is ready before the
/// playhead reaches the end of this one. On the run reader, start to finish:
/// what has to be decoded to reach a given frame is a whole group of pictures,
/// and on a thread of its own there is no turn to fit it into.
void FrameRenderer::Impl::decode_ahead(Source& source, double meet) {
  if (!source.keep_ahead || source.keep == 0 || source.ahead == nullptr) return;

  // The prefetch has to meet the run in hand exactly: a gap between them reads
  // as the end of a run, and "the last frame at or before t" would then answer
  // from the wrong side of the hole.
  if (meet <= kFrameEpsilon) return;  // nothing before the start of the source

  const double fps = source.ahead->stream().fps;
  const double gap = fps > 0.0 ? 1.0 / fps : kAssumedFrameGap;

  if (source.pending.size() >= source.keep && !source.extending) return;

  if (!source.extending) {
    // Far enough back that a seek lands on a keyframe at or before the frames
    // wanted. Aiming at the frame just before the run in hand would seek to the
    // keyframe before *that* and decode the same pictures either way, so the
    // span is chosen to fill the room rather than to be short.
    const double from_time = std::max(0.0, meet - static_cast<double>(source.keep) * gap);
    if (!source.ahead->seek(from_time)) {
      source.keep_ahead = false;
      return;
    }
    source.pending.clear();
    source.ahead_position = -1.0;
    source.pending_until = meet;
    source.ahead_for = Source::Ahead::Previous;
    source.extending = true;
  }

  while (source.extending) {
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
    // costs, and they are dropped by the trim as newer ones arrive.
    if (source.ahead_position < source.pending_until - kFrameEpsilon) {
      std::unique_ptr<AVFrame, FrameDeleter> copy(av_frame_alloc());
      if (copy && av_frame_ref(copy.get(), source.ahead->frame()) >= 0) {
        source.pending.push_back(
            Source::Recent{.at = source.ahead_position, .frame = std::move(copy)});
        ++decoded_ahead;
        source.trim_pending();
      }
      continue;
    }

    // Met the run in hand. Done, and the join is exact.
    source.extending = false;
  }
}

void FrameRenderer::Impl::read_before(Source& source) {
  if (!source.keep_ahead || source.keep == 0 || source.recent.empty()) return;
  if (source.shared->posted || source.extending || !source.pending.empty()) return;
  if (source.reverse_floor >= 0.0 &&
      source.recent.front().at <= source.reverse_floor + source.slack) {
    return;
  }

  // The reading decoder, opened the first time one is wanted. A project that is
  // never played backwards never pays for it.
  if (source.ahead == nullptr) {
    const int slot = source.free_device();
    auto opened = open_decoder(source.path, static_cast<int>(source.keep), slot);
    if (!opened.has_value()) {
      // Not fatal. Without a second decoder the run in hand still serves every
      // frame it holds, and the ordinary path still fetches the rest — it is
      // the stall coming back, not the picture.
      source.keep_ahead = false;
      return;
    }
    source.ahead = std::move(*opened);
    source.ahead_device = slot;
  }

  post_before(source, source.recent.front().at);
}

void FrameRenderer::Impl::post_before(Source& source, double meet) {
  const double fps = source.ahead->stream().fps;
  const double gap = fps > 0.0 ? 1.0 / fps : kAssumedFrameGap;
  source.shared->posted = true;
  // The span that run will answer, so the serving path waits for the reader on
  // arriving there rather than starting the same group of pictures over.
  source.shared->target.store(std::max(0.0, meet - static_cast<double>(source.keep) * gap));
  source.shared->target_end.store(meet);
  Source* raw = &source;
  run_reader.post([this, raw, meet] {
    const std::lock_guard<std::mutex> working(raw->shared->lock);
    raw->shared->posted = false;
    decode_ahead(*raw, meet);
  });
}

bool FrameRenderer::Impl::open_jumper(Source& source) {
  if (source.jumper != nullptr) return true;
  if (!source.keep_jumping) return false;
  const int slot = source.free_device();
  auto opened = open_decoder(source.path, static_cast<int>(source.keep), slot);
  if (!opened.has_value()) {
    // As above: the cut falls back to the ordinary path, a stall and not a
    // missing picture.
    source.keep_jumping = false;
    return false;
  }
  source.jumper = std::move(*opened);
  source.jumper_device = slot;
  return true;
}

void FrameRenderer::Impl::prefetch_jump(Source& source, double target) {
  if (!source.keep_jumping || source.keep == 0 || source.jumper == nullptr) return;
  // Already built for this cut, and the run begins at it. Nothing to add.
  const bool same_cut = source.jump_from >= 0.0 && !source.jump_back &&
                        target >= source.jump_from - kFrameEpsilon &&
                        target - source.jump_from < kSameCut;
  if (same_cut && !source.jumping) return;

  if (!same_cut) {
    // The playhead moved somewhere else while this was being built, so what is
    // half decoded is for a cut that is no longer next.
    source.jumped.clear();
    if (!source.jumper->seek(target)) {
      source.keep_jumping = false;
      source.forget_jump();
      return;
    }
    source.jumper_position = -1.0;
    source.jump_from = target;
    source.jump_until = -1.0;
    source.jump_back = false;
    source.jumping = true;
  }

  // Enough to carry the first moment of the new clip while the serving decoder
  // is seeked behind it by the ordinary path. Not a whole run: the point is to
  // cover the join, and every frame held here is a surface the pool has lent.
  const std::size_t wanted = std::min<std::size_t>(source.keep, kJumpRunFrames);

  while (source.jumping) {
    const auto got = source.jumper->next_frame();
    if (!got) {
      source.keep_jumping = false;
      source.forget_jump();
      return;
    }
    if (!*got) {
      // The source ended before the cut did. Whatever was gathered still
      // serves; there is simply no more of it.
      source.jumping = false;
      return;
    }
    source.jumper_position = source.jumper->timestamp();

    // Everything between the keyframe and the cut is what the seek cost, and
    // most of it belongs to a part of the source this clip does not show. All
    // but *one*: a frame is shown from its own timestamp until the next one, so
    // what covers the cut is the last frame at or before it, and that one is
    // usually a shade earlier than the cut itself. Dropping it left the run
    // starting after the moment it was built to answer, and the swap never
    // happened — the prefetch ran every time and was thrown away every time.
    if (source.jumper_position < source.jump_from - kFrameEpsilon) {
      std::unique_ptr<AVFrame, FrameDeleter> covering(av_frame_alloc());
      if (covering && av_frame_ref(covering.get(), source.jumper->frame()) >= 0) {
        // Only ever one, and always the newest of them.
        source.jumped.clear();
        source.jumped.push_back(
            Source::Recent{.at = source.jumper_position, .frame = std::move(covering)});
      }
      continue;
    }

    std::unique_ptr<AVFrame, FrameDeleter> copy(av_frame_alloc());
    if (copy && av_frame_ref(copy.get(), source.jumper->frame()) >= 0) {
      source.jumped.push_back(
          Source::Recent{.at = source.jumper_position, .frame = std::move(copy)});
      ++decoded_ahead;
    }
    if (source.jumped.size() >= wanted) source.jumping = false;
  }
}

void FrameRenderer::Impl::prefetch_jump_back(Source& source, double until) {
  if (!source.keep_jumping || source.keep == 0 || source.jumper == nullptr) return;
  const bool same_cut = source.jump_back && std::abs(until - source.jump_until) <= kFrameEpsilon;
  if (same_cut && !source.jumping) return;

  if (!same_cut) {
    source.jumped.clear();
    const double fps = source.jumper->stream().fps;
    const double gap = fps > 0.0 ? 1.0 / fps : kAssumedFrameGap;
    // A whole run back from the end, so reverse has a run's worth of frames
    // to play out while the one before it is read.
    const double from = std::max(0.0, until - static_cast<double>(source.keep - 1) * gap);
    if (!source.jumper->seek(from)) {
      source.keep_jumping = false;
      source.forget_jump();
      return;
    }
    source.jumper_position = -1.0;
    source.jump_from = from;
    source.jump_until = until;
    source.jump_back = true;
    source.jumping = true;
  }

  while (source.jumping) {
    const auto got = source.jumper->next_frame();
    if (!got) {
      source.keep_jumping = false;
      source.forget_jump();
      return;
    }
    if (!*got) {
      // The source ended first, so what was gathered runs to its end.
      source.jumping = false;
      return;
    }
    source.jumper_position = source.jumper->timestamp();
    // Past the clip's last frame: nothing more of it to show.
    if (source.jumper_position > source.jump_until + source.slack) {
      source.jumping = false;
      return;
    }

    std::unique_ptr<AVFrame, FrameDeleter> copy(av_frame_alloc());
    if (copy && av_frame_ref(copy.get(), source.jumper->frame()) >= 0) {
      source.jumped.push_back(
          Source::Recent{.at = source.jumper_position, .frame = std::move(copy)});
      ++decoded_ahead;
      // The newest run's worth, which is the end of the clip: the frames from
      // the keyframe up to it are what the seek cost.
      source.trim_jumped();
    }
    if (source.jumper_position >= source.jump_until - source.slack) source.jumping = false;
  }
}

void FrameRenderer::Impl::prefetch_cuts_back(const core::Project& project, double t,
                                             std::span<const render::PlannedLayer> now) {
  const double fps = project.sequence().fps > 0.0 ? project.sequence().fps : 30.0;

  // Where each clip on screen begins in its source, unless the clip before it
  // carries straight on from there, in which case reverse runs on with no edge.
  for (const render::PlannedLayer& live : now) {
    if (live.content != render::LayerContent::Video || live.clip == nullptr) continue;
    if (live.media == nullptr) continue;
    const auto found = sources.find(live.media->id);
    if (found == sources.end()) continue;
    Source& source = found->second;

    const double speed = core::conform_speed(*live.media);
    double starts = -1.0;
    for (const render::PlannedLayer& at : render::plan_frame(
             project, live.clip->start + 0.5 / fps)) {
      if (at.clip == live.clip) starts = at.source_time * speed;
    }
    if (starts >= 0.0 && live.clip->start >= 0.5 / fps) {
      for (const render::PlannedLayer& at : render::plan_frame(
               project, live.clip->start - 0.5 / fps)) {
        if (at.media != live.media || at.content != render::LayerContent::Video) continue;
        // One frame earlier in the same source: an edit nobody can see.
        if (std::abs(starts - at.source_time * speed - 1.0 / fps) <= source.slack * 2.0) {
          starts = -1.0;
        }
      }
    }
    source.reverse_floor = starts;

    // The run in hand already reaches the edge, so the second decoder is free
    // until the cut. It builds the run before the *next* clip's last run --
    // the far side of the cut, once the cut reader has it -- so that run is
    // in hand at the cut instead of being started there, with half a second
    // to do a whole group of pictures in.
    if (starts < 0.0 || source.recent.empty() || source.ahead == nullptr) continue;
    if (source.recent.front().at > starts + source.slack) continue;
    std::unique_lock<std::mutex> held(source.shared->lock, std::try_to_lock);
    if (!held.owns_lock() || source.shared->posted || source.extending ||
        !source.pending.empty() || !source.keep_ahead) {
      continue;
    }
    double meet = -1.0;
    {
      std::unique_lock<std::mutex> jumps(source.shared->jump_lock, std::try_to_lock);
      if (jumps.owns_lock() && source.jump_back && !source.jumping && !source.jumped.empty()) {
        meet = source.jumped.front().at;
      }
    }
    if (meet > kFrameEpsilon) {
      source.before_cut = true;
      post_before(source, meet);
    }
  }

  const double earlier = t - kCutLookbehind;
  if (earlier < 0.0) return;

  for (const render::PlannedLayer& layer : render::plan_frame(project, earlier)) {
    if (layer.content != render::LayerContent::Video || layer.clip == nullptr) continue;
    if (layer.media == nullptr || layer.media->path.empty()) continue;

    // The clip the playhead will cut back into, by its last frame. Planned at
    // that moment rather than worked out from the clip's trim, so speed and
    // everything else the plan knows about come with it -- and so the target
    // stays put as the playhead closes in, which a moment read off `earlier`
    // would not.
    const double last = core::clip_end(*layer.clip) - 0.5 / fps;
    // Still inside it, so it is no cut: the run before the one in hand is the
    // reverse prefetch's ordinary business.
    if (last >= t - kFrameEpsilon) continue;

    const auto found = sources.find(layer.media->id);
    // Not open, and opening costs a quarter of a second on this thread.
    if (found == sources.end()) continue;
    Source& source = found->second;
    if (!source.usable) continue;

    double until = -1.0;
    for (const render::PlannedLayer& at_end : render::plan_frame(project, last)) {
      if (at_end.clip == layer.clip) until = at_end.source_time * core::conform_speed(*layer.media);
    }
    if (until < 0.0) continue;

    // The same stretch of source either side of an edit, so reverse runs on
    // into it with nothing to prepare.
    const auto showing = std::ranges::find_if(now, [&](const render::PlannedLayer& live) {
      return live.media == layer.media && live.content == render::LayerContent::Video;
    });
    if (showing != now.end() && showing->clip != nullptr) {
      const double first = showing->clip->start + 0.5 / fps;
      for (const render::PlannedLayer& at_start : render::plan_frame(project, first)) {
        if (at_start.clip != showing->clip) continue;
        const double starts = at_start.source_time * core::conform_speed(*layer.media);
        if (std::abs(starts - until - source.slack * 2.0) <= source.slack * 2.0) until = -1.0;
      }
      if (until < 0.0) continue;
    }

    std::unique_lock<std::mutex> jumps(source.shared->jump_lock, std::try_to_lock);
    if (!jumps.owns_lock() || source.shared->jump_posted) continue;
    if (source.jump_back && std::abs(source.jump_until - until) <= kFrameEpsilon) continue;
    if (!open_jumper(source)) continue;
    source.shared->jump_posted = true;
    // The span the run will answer, which is what the serving path compares a
    // request against to decide whether waiting for the reader is worth it.
    const double fps_of_source = source.jumper->stream().fps;
    const double gap = fps_of_source > 0.0 ? 1.0 / fps_of_source : kAssumedFrameGap;
    source.shared->jump_begins.store(
        std::max(0.0, until - static_cast<double>(source.keep - 1) * gap));
    source.shared->jump_ends.store(until);
    jumps.unlock();
    Source* raw = &source;
    cut_reader.post([this, raw, until] {
      const std::lock_guard<std::mutex> working(raw->shared->jump_lock);
      raw->shared->jump_posted = false;
      prefetch_jump_back(*raw, until);
    });
  }
}

void FrameRenderer::Impl::prefetch_cuts(const core::Project& project, double t,
                                        std::span<const render::PlannedLayer> now) {
  // What the sequence will be showing shortly. Planned rather than guessed: the
  // plan already knows which source time every layer wants, and asking it about
  // a moment in the future is the same pure function.
  const std::vector<render::PlannedLayer> soon = render::plan_frame(project, t + kCutLookahead);

  for (const render::PlannedLayer& layer : soon) {
    if (layer.content != render::LayerContent::Video) continue;
    if (layer.media == nullptr || layer.media->path.empty()) continue;

    const auto found = sources.find(layer.media->id);
    // Not open yet. Opening one costs a quarter of a second and this runs on
    // the thread that has sixteen milliseconds, so a source nobody has needed
    // is left to the ordinary path — that cost belongs somewhere else.
    if (found == sources.end()) continue;

    Source& source = found->second;
    if (!source.usable) continue;

    const double wanted = layer.source_time * core::conform_speed(*layer.media);

    // Where this source is being read *now*, if it is on screen. Compared
    // against that rather than against the decoder's position, because the
    // decoder will have moved on by the time the cut arrives — measuring from
    // where it happens to sit made every ordinary frame of forward playback
    // look like a jump, and the prefetch re-seeked every turn and finished
    // nothing.
    const auto showing = std::ranges::find_if(now, [&](const render::PlannedLayer& live) {
      return live.media == layer.media && live.content == render::LayerContent::Video;
    });

    if (showing != now.end()) {
      // Ordinary progress carries the source forward by about the time that
      // passed. Anything much further is the sequence cutting to somewhere
      // else — and it is a *seek* only when it is further than decoding
      // forwards would have covered anyway.
      const double travelled = wanted - showing->source_time * core::conform_speed(*layer.media);
      // A range, not a ceiling: a cut back into the same source travels a
      // negative distance, and "no further than ordinary progress" let every
      // one of those through as if nothing were happening.
      if (travelled >= -kFrameEpsilon && travelled <= kCutLookahead + kDecodeForwardWindow) continue;
    } else if (source.position >= 0.0) {
      // Not on screen, so its decoder is parked wherever it was left.
      if (wanted > source.position - kFrameEpsilon &&
          wanted <= source.position + kDecodeForwardWindow) {
        continue;
      }
    }

    // Not skipped when it is *earlier* in the source, which it used to be on the
    // reasoning that backwards is the reverse prefetch's business. Backwards in
    // the *timeline* is; backwards in the source is just a cut. A montage cut
    // from one long capture jumps back into it about as often as forwards, and
    // every one of those went to the ordinary path and paid its whole seek on
    // the frame of the cut: eight of fifteen cuts, measured. The direction of
    // play is checked where this is called.

    // To the cut reader, unless it already has this source in hand: the reader
    // is the one holding the lock while it works, and waiting here would put
    // the decode straight back on this thread.
    std::unique_lock<std::mutex> jumps(source.shared->jump_lock, std::try_to_lock);
    if (!jumps.owns_lock() || source.shared->jump_posted) continue;
    if (!open_jumper(source)) continue;
    source.shared->jump_posted = true;
    source.shared->jump_begins.store(wanted);
    source.shared->jump_ends.store(wanted + kSameCut);
    jumps.unlock();
    Source* raw = &source;
    cut_reader.post([this, raw, wanted] {
      const std::lock_guard<std::mutex> working(raw->shared->jump_lock);
      raw->shared->jump_posted = false;
      prefetch_jump(*raw, wanted);
    });
  }
}

gpu::SceneTexture FrameRenderer::Impl::nested_picture(const core::Project& project,
                                                      const core::Media& media, double at) {
  if (depth >= kMaxNestDepth) return {};

  const core::Sequence* inner = core::find_sequence(project, media.sequence_id);
  if (inner == nullptr || inner->canvas_w <= 0 || inner->canvas_h <= 0) return {};

  auto found = nested.find(media.sequence_id);
  if (found == nested.end()) {
    auto made = FrameRenderer::create(device, inner->canvas_w, inner->canvas_h, queue);
    if (!made.has_value()) return {};
    (*made)->impl_->depth = depth + 1;
    (*made)->set_use_proxies(use_proxies);
    found = nested.emplace(media.sequence_id, std::move(*made)).first;
  }
  FrameRenderer& renderer = *found->second;

  // The same project with that sequence open. A renderer draws whichever
  // sequence a project says is open, so a nest is the outer document read from
  // one step further in — which is also what keeps a nest of a nest working
  // without anything here knowing how deep it is.
  core::Project within = project;
  within.open = core::sequence_index(project, media.sequence_id);
  if (within.open == std::string::npos) return {};

  // An HDR sequence is taken out at its real brightness, from the float copy
  // its compositor makes beside the 8-bit one; an SDR one as it always was.
  const bool hdr = inner->colour != core::WorkingColour::Rec709;
  renderer.compositor().set_hdr_display(hdr ? std::optional<float>(80.0f) : std::nullopt);
  if (!renderer.render(within, at).has_value()) return {};

  // Whatever the nest could not find is missing from the frame just as surely
  // as if it had been named out here.
  for (const std::string& lost : renderer.missing_media()) missing.push_back(lost);

  const auto picture = renderer.compositor().display_texture();
  return picture.has_value() ? *picture : gpu::SceneTexture{};
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
    drain_readers();
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
    auto opened = open_decoder(wanted, keeping, source.decoder_device);
    if (!opened.has_value()) {
      source.usable = false;
    } else {
      source.decoder = std::move(*opened);
      const double rate = source.decoder->stream().fps;
      source.slack = std::max(kFrameEpsilon, 0.5 / (rate > 0.0 ? rate : 1.0 / kAssumedFrameGap));
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
  //
  // The *timeline's* direction, not the source's. A cut back into the same
  // capture moves the source time backwards while the sequence plays forwards,
  // and reading direction off the source sent every such cut into the reverse
  // prefetch: thirty-six milliseconds decoding a run before a moment nobody was
  // going backwards from, on the frame of the cut.
  const double previous = source.last_request;
  source.last_request = time;
  const bool reversing = playing_backwards && previous >= 0.0 && time < previous - kFrameEpsilon;

  // The playhead has left the run in hand. If a prefetch was on its way to
  // exactly this frame, finish it rather than throwing it away — the work is
  // already part done, and abandoning it means seeking back and decoding the
  // very same group of pictures a second time. That was the whole of why an
  // early version measured *worse* than no prefetch: 556 frames decoded ahead,
  // two runs of it ever used, and every abandoned one paid for twice.
  //
  // Each reader's decoder and run are the reader's while it works on them, so
  // they are taken without waiting — except in the one case waiting is right:
  // the playhead has arrived inside the span the job is building, and
  // finishing it beats starting over. Inside the span, not merely past its
  // start: the times are the source's, and a cut back into the same capture has
  // a target *behind* where this decoder is reading.

  // The far side of a cut, from the third decoder.
  {
    std::unique_lock<std::mutex> jumps(source.shared->jump_lock, std::try_to_lock);
    if (!jumps.owns_lock() && source.remembered(time) == nullptr) {
      const double begins = source.shared->jump_begins.load();
      const double ends = source.shared->jump_ends.load();
      if (begins >= 0.0 && time >= begins - source.slack && time <= ends + source.slack) {
        jumps.lock();
      }
    }
    if (jumps.owns_lock() && source.remembered(time) == nullptr && source.jumping) {
      if (source.jump_back) {
        if (time >= source.jump_from - kFrameEpsilon &&
            time <= source.jump_until + source.slack) {
          prefetch_jump_back(source, source.jump_until);
        }
      } else if (time >= source.jump_from - kFrameEpsilon) {
        prefetch_jump(source, source.jump_from);
      }
    }
    if (jumps.owns_lock() && source.remembered(time) == nullptr && source.jumped_covers(time)) {
      source.recent = std::move(source.jumped);
      // The decoders change places with the runs, because a frame belongs to
      // the pool it was lent from: asking another decoder for these frames'
      // textures would hand back a picture from somewhere else entirely.
      std::swap(source.decoder, source.jumper);
      std::swap(source.position, source.jumper_position);
      std::swap(source.decoder_device, source.jumper_device);
      source.exhausted = false;
      source.forget_jump();
      ++stats.runs_taken_ahead;
    }
  }

  // The run before the one in hand, from the second decoder.
  std::unique_lock<std::mutex> held(source.shared->lock, std::try_to_lock);
  if (!held.owns_lock() && source.remembered(time) == nullptr) {
    const double begins = source.shared->target.load();
    const double ends = source.shared->target_end.load();
    if (begins >= 0.0 && time >= begins - source.slack && time <= ends + source.slack) held.lock();
  }

  // Built to meet a run that is no longer in hand — a cut was crossed while it
  // was being read — so it meets nothing, and its surfaces are better back in
  // the pool.
  //
  // Read under the second decoder's lock alone, so a run built ahead of the
  // next cut's run -- which is not in hand yet -- is spared only while the
  // playhead is still on this side of that cut: the cut's run is taken above,
  // first, so by the time the playhead is past it the two are in hand together.
  if (held.owns_lock() && !source.extending && !source.pending.empty() &&
      source.before_cut && source.remembered(time) != nullptr) {
    // Waiting for its cut.
  } else if (held.owns_lock() && !source.extending && !source.pending.empty() &&
             (source.recent.empty() ||
              std::abs(source.pending_until - source.recent.front().at) > kFrameEpsilon)) {
    source.forget_pending();
  }
  if (held.owns_lock() && source.remembered(time) == nullptr && source.extending &&
      time < source.pending_until + kFrameEpsilon) {
    decode_ahead(source, source.pending_until);
  }

  // The run decoded ahead of need has caught up with the playhead. Taking it
  // whole rather than merging: the two were decoded to meet exactly, and this
  // is the moment the stall used to happen.
  if (held.owns_lock() && source.remembered(time) == nullptr && source.pending_covers(time)) {
    source.recent = std::move(source.pending);
    // Swapped for the same reason as above, which also leaves the reading
    // decoder positioned where the *next* run has to start from.
    std::swap(source.decoder, source.ahead);
    std::swap(source.position, source.ahead_position);
    std::swap(source.decoder_device, source.ahead_device);
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
    if (reversing && held.owns_lock()) read_before(source);
    if (from != nullptr) *from = source.decoder.get();
    return known;
  }

  // Out of cache, so whatever was being decoded ahead is about to be overtaken
  // by an ordinary seek. Dropping it first returns its surfaces to the pool,
  // which the decode below is about to want. A run gathered for a *cut* is the
  // third decoder's, and stays: it is built while the clip before the cut is
  // still playing, and every frame of that clip comes through here.
  if (held.owns_lock()) source.forget_pending();

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
  while (!source.exhausted) {
    if (source.position >= time - source.slack && source.position >= 0.0) break;

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

FrameRenderer::DecodeStats FrameRenderer::decode_stats() const noexcept {
  DecodeStats out = impl_->stats;
  out.frames_decoded_ahead = impl_->decoded_ahead.load();
  return out;
}

void FrameRenderer::release_sources() {
  impl_->drain_readers();
  impl_->sources.clear();
}

void FrameRenderer::set_use_proxies(bool use_proxies) { impl_->use_proxies = use_proxies; }
bool FrameRenderer::use_proxies() const noexcept { return impl_->use_proxies; }

std::expected<std::unique_ptr<FrameRenderer>, std::string> FrameRenderer::create(
    std::shared_ptr<gpu::Device> device, int canvas_width, int canvas_height,
    std::shared_ptr<gpu::Queue> queue) {
  if (!device) return std::unexpected("a frame renderer needs a device");

  auto impl = std::make_unique<Impl>();
  impl->device = std::move(device);
  impl->queue = queue;

  auto compositor =
      gpu::Compositor::create(impl->device, canvas_width, canvas_height, std::move(queue));
  if (!compositor) return std::unexpected(compositor.error());
  impl->compositor = std::move(*compositor);

  return std::unique_ptr<FrameRenderer>(new FrameRenderer(std::move(impl)));
}

std::expected<void, std::string> FrameRenderer::resize(int width, int height) {
  return impl_->compositor->resize(width, height);
}

std::expected<gpu::Image, std::string> FrameRenderer::read_back(gpu::WorkingColour output) {
  return impl_->compositor->read_back(output);
}

std::expected<void, std::string> FrameRenderer::render(const core::Project& project, double t) {
  Impl& d = *impl_;
  d.missing.clear();
  d.playing_backwards = d.last_time >= 0.0 && t < d.last_time - kFrameEpsilon;
  d.last_time = t;

  if (auto ok = d.compositor->resize(project.sequence().canvas_w, project.sequence().canvas_h); !ok) {
    return std::unexpected(ok.error());
  }

  const std::vector<render::PlannedLayer> planned = render::plan_frame(project, t);

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
      case render::LayerContent::Nested: {
        if (source.media == nullptr) continue;
        const gpu::SceneTexture picture = d.nested_picture(project, *source.media,
                                                           source.source_time);
        if (picture.empty()) continue;

        gpu::FrameView view;
        view.width = picture.width;
        view.height = picture.height;
        // Coded R'G'B' with straight alpha, which is what a composited scene
        // is and what the shader's coded path samples untouched.
        view.layout = gpu::PixelLayout::Scene;
        view.full_range = true;
        view.texture = gpu::SourceTexture{.resource = picture.resource};
        // An HDR nest in scRGB instead, which this sequence brings into its
        // own space as it would any HDR source: kept, or tone mapped.
        if (picture.hdr_resource != nullptr) {
          view.texture = gpu::SourceTexture{.resource = picture.hdr_resource};
          view.transfer = gpu::TransferFunction::ScRgb;
          view.primaries = gpu::ColourPrimaries::Bt709;
        }

        d.views.push_back(view);
        layer.frame = &d.views.back();
        break;
      }

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

      case render::LayerContent::Graphic: {
#if CUTLINE_HAVE_TEXT
        if (source.media == nullptr || source.clip == nullptr) continue;
        const gpu::SceneTexture picture = d.graphic_picture(project, *source.clip, *source.media, t);
        // Nothing drawable -- no layers, or none that could be drawn -- is
        // skipped rather than drawn as a blank frame.
        if (picture.empty()) continue;

        gpu::FrameView view;
        view.width = picture.width;
        view.height = picture.height;
        // A composited scene, as a nest's is.
        view.layout = gpu::PixelLayout::Scene;
        view.full_range = true;
        view.texture = gpu::SourceTexture{.resource = picture.resource};
        if (picture.hdr_resource != nullptr) {
          view.texture = gpu::SourceTexture{.resource = picture.hdr_resource};
          view.transfer = gpu::TransferFunction::ScRgb;
          view.primaries = gpu::ColourPrimaries::Bt709;
        }
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
        // Read as somebody said, whatever the file is tagged.
        if (const auto colour = source.media->colour_override; colour.has_value()) {
          const bool hdr = *colour != core::WorkingColour::Rec709;
          view.space = hdr ? gpu::ColorSpace::Bt2020 : gpu::ColorSpace::Bt709;
          view.primaries = hdr ? gpu::ColourPrimaries::Bt2020 : gpu::ColourPrimaries::Bt709;
          view.transfer = *colour == core::WorkingColour::Rec2100Pq    ? gpu::TransferFunction::Smpte2084
                          : *colour == core::WorkingColour::Rec2100Hlg ? gpu::TransferFunction::AribStdB67
                                                                       : gpu::TransferFunction::Bt709;
        }

        d.views.push_back(view);
        layer.frame = &d.views.back();
        break;
      }
    }

    layers.push_back(layer);
  }

  // The sequence's colour management, which every source is brought into as
  // it is read. The model's names and the card's are declared in one order.
  static_assert(static_cast<int>(core::WorkingColour::Rec2100Pq) ==
                    static_cast<int>(gpu::WorkingColour::Rec2100Pq) &&
                static_cast<int>(core::WorkingColour::Rec2100Hlg) ==
                    static_cast<int>(gpu::WorkingColour::Rec2100Hlg));
  const core::Sequence& sequence = project.sequence();
  d.compositor->set_colour(gpu::ColourSettings{
      .working = static_cast<gpu::WorkingColour>(sequence.colour),
      .graphics_white_nits = static_cast<float>(sequence.graphics_white),
      .tone_map = sequence.tone_map});
  auto composed = d.compositor->compose(layers);

  // With the frame drawn, and before the turn is given back. A rough cut is
  // made of pieces from scattered parts of a capture, so the next cut is a seek
  // and a group of pictures — and the turns between here and there have room
  // for it, where the turn *on* the cut does not.
  //
  // After compositing rather than before, so a frame that is due is never
  // waiting behind work for a frame that is not.
  //
  // Each direction looks the way it is going: a cut "ahead" of a playhead going
  // the other way is one it will never reach.
  if (d.playing_backwards) {
    d.prefetch_cuts_back(project, t, planned);
  } else {
    for (auto& [id, source] : d.sources) source.reverse_floor = -1.0;
    d.prefetch_cuts(project, t, planned);
  }

  return composed;
}

}  // namespace cutline::engine
