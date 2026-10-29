#include "cutline/engine/audio_mixer.hpp"

#include "cutline/core/multicam.hpp"
#include "cutline/core/nesting.hpp"
#include "cutline/core/sequences.hpp"

#include "cutline/audio/chain.hpp"
#include "cutline/audio/limiter.hpp"
#include "cutline/audio/loudness.hpp"
#include "cutline/audio/meter.hpp"
#include "cutline/audio/stretch.hpp"
#include "cutline/core/keyframe.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/routing.hpp"
#include "cutline/media/audio.hpp"
#include "cutline/render/mix.hpp"

#include <algorithm>
#include <optional>
#include <ranges>
#include <filesystem>
#include <atomic>
#include <memory>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <cmath>
#include <numbers>
#include <span>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace cutline::engine {
namespace {

/// A decoded source range. Keyed by file, stream ordinal and the range itself,
/// so two clips with the same trim decode once and two clips with different
/// trims do not each drag in the whole file.
///
/// Decoding whole streams was the obvious first implementation and does not
/// survive contact with the reference footage: those captures run ten minutes
/// with four audio streams each, where placing a twenty-second clip cost about
/// four seconds and 230 MB.
struct SourceKey {
  std::string path;
  int stream = 0;
  double source_in = 0.0;
  double source_out = 0.0;

  friend auto operator<=>(const SourceKey&, const SourceKey&) = default;
};

/// A little either side of the trim, so interpolating at the very last sample
/// has a neighbour to reach for and a seek landing slightly late does not clip
/// the start.
constexpr double kDecodeMargin = 0.05;

/// A clip's range up to this long is decoded whole and never thought about
/// again.
///
/// Two minutes of stereo is about 46 MB, and a cut is made of clips far shorter
/// than that — so the windowing below is machinery that the ordinary project
/// never touches, and the simplest behaviour stays the common one. It is the
/// ten-minute capture dropped in untrimmed that needs the other path.
constexpr double kWholeSourceSeconds = 120.0;

/// How much of a long source is kept either side of where the mix has reached.
///
/// Ahead is what playback eats into and behind is what a scrub back lands in.
/// Both are generous on purpose: a window is only worth having if ordinary
/// movement stays inside it, since going outside costs a decode. Seventy-five
/// seconds of stereo is about 29 MB per stream.
constexpr double kWindowBehind = 15.0;
constexpr double kWindowAhead = 60.0;

/// What has been read of one source, and where in the source it sits.
///
/// Immutable once published. That is what lets the mixing thread take a
/// pointer to it and read for a whole block while a reader thread is building
/// the next one: nothing it is looking at can change underneath it, and the
/// swap is a single atomic store.
struct Window {
  double from = 0.0;  ///< source seconds this window begins at
  double to = 0.0;    ///< and ends at
  media::AudioBuffer audio;
};

/// One source's audio, as much of it as is worth holding at once.
struct StreamedSource {
  SourceKey key;
  int sample_rate = 48000;
  int channels = 2;
  /// True when the whole range is resident and there is nothing to refill.
  bool whole = false;

  /// The resident window. Read on the mixing thread, replaced on the reader's.
  ///
  /// An atomic shared pointer rather than a mutex, and that is the whole reason
  /// this is safe: the mixing thread takes a reference and reads through it for
  /// the length of a block, while the reader builds the next window beside it
  /// and publishes with one store. Nothing blocks, and nothing the mixer is
  /// looking at can be freed while it looks.
  std::atomic<std::shared_ptr<const Window>> resident;

  /// Where the mix last needed audio, in source seconds. Written by the mixing
  /// thread, read by the reader.
  std::atomic<double> wanted{0.0};
  /// Set once when the file will not decode, so a broken source is not tried
  /// again on every block for the life of the mix.
  std::atomic<bool> failed{false};

  /// When the earliest clip using this source is first heard, in timeline
  /// seconds. What the reader fills in order of, so the audio arrives in the
  /// order it is going to be wanted rather than in whatever order a map
  /// happened to hold it.
  double needed_at = 0.0;
};

/// Decodes the part of `source` that should be resident when the mix is at
/// `at`, or the whole range when it is short enough to hold.
[[nodiscard]] std::shared_ptr<const Window> read_window(const StreamedSource& source, double at) {
  const double from = source.whole
                          ? source.key.source_in
                          : std::clamp(at - kWindowBehind, source.key.source_in,
                                       source.key.source_out);
  const double to = source.whole
                        ? source.key.source_out
                        : std::min(source.key.source_out, from + kWindowBehind + kWindowAhead);
  if (!(to > from)) return nullptr;

  auto decoded = media::decode_audio(source.key.path, source.key.stream,
                                     {.sample_rate = source.sample_rate,
                                      .channels = source.channels,
                                      .start = from,
                                      .duration = to - from});
  if (!decoded) return nullptr;

  auto window = std::make_shared<Window>();
  window->from = from;
  window->to = to;
  window->audio = std::move(*decoded);
  return window;
}

/// Whether what is resident still answers for a mix that has reached `at`.
///
/// Refilled before the edge rather than at it: a window whose end is exactly
/// where playback has got to is one that has already run out.
[[nodiscard]] bool covers(const Window* window, const StreamedSource& source, double at) noexcept {
  if (window == nullptr) return false;
  if (source.whole) return true;
  // Comfortably inside, or up against the end of the source, where there is
  // nothing further to read and the window is as complete as it can be.
  const bool room_ahead =
      at + kWindowAhead * 0.25 < window->to || window->to >= source.key.source_out - 1e-6;
  const bool room_behind =
      at >= window->from || window->from <= source.key.source_in + 1e-6;
  return room_ahead && room_behind;
}

/// How often an animated effect parameter is re-read, in frames.
///
/// A control rate rather than a sample rate. Sixty-four frames is about a
/// millisecond and a half at 48 kHz — finer than any automation curve anybody
/// draws, and coarse enough that recomputing a filter's coefficients is not
/// what the mixer spends its time on. Aligned to the timeline rather than to
/// the caller's buffer, which is what keeps the preview and the export the
/// same.
constexpr std::int64_t kControlBlock = 64;

/// Runs a chain over one interleaved block, retuning animated parameters on the
/// control grid.
///
/// An animated stack is retuned on a fixed grid **aligned to the timeline**,
/// not once per call. Retuning per call would have been simpler and would have
/// made the preview and the export disagree: they ask for different buffer
/// sizes, so a sweep would land on different coefficients in each. Aligning the
/// grid to absolute sample positions means splitting a buffer anywhere gives
/// the same samples, which is the guarantee the rest of this mixer keeps.
///
/// Per sample would be both expensive and wrong — moving an IIR filter's
/// coefficients under state that assumes the old ones is how a filter is made
/// to ring.
///
/// `origin` is what the chain's keyframe times are measured from: a clip's
/// start for a clip's stack, and zero for a track's, whose keyframes are in
/// timeline seconds. Shared by both so the grid cannot come to mean two things.
void run_chain_over(audio::EffectChain& chain, std::span<float> block,
                    std::int64_t first_sample, double rate, std::size_t channels,
                    double origin) {
  if (channels == 0) return;
  const std::size_t frames = block.size() / channels;
  if (frames == 0) return;

  if (chain.fixed()) {
    chain.process(block);
    return;
  }

  std::size_t at = 0;
  while (at < frames) {
    const std::int64_t absolute = first_sample + static_cast<std::int64_t>(at);
    const std::int64_t into = ((absolute % kControlBlock) + kControlBlock) % kControlBlock;
    const auto until = static_cast<std::size_t>(kControlBlock - into);
    const std::size_t chunk = std::min(frames - at, until);

    // Retuned to the time of the grid step this chunk sits *in*, not to where
    // the chunk happens to begin. A call that starts halfway through a step has
    // to use the coefficients that step already had, or the sound would depend
    // on where the caller's buffer boundaries fell.
    chain.retune(static_cast<double>(absolute - into) / rate - origin);
    chain.process(block.subspan(at * channels, chunk * channels));
    at += chunk;
  }
}

/// How much of the timeline one rendered piece covers.
///
/// A nest's mix and a retimed clip's stretch used to be made whole when the
/// mixer was built: 8 s before a 140-second clip at half speed could play, and
/// a nest mixed end to end before a note of it was heard. Made a piece at a
/// time instead, around wherever the mix has reached, the way a long source is
/// read. Ten seconds, because the piece under the playhead is made before Play
/// can start, and a stretch costs about 30 ms a second of what it makes. A
/// clip no longer than one piece is made in one, exactly as it was.
constexpr double kPieceSeconds = 10.0;

/// How much is made before a piece's start and thrown away.
///
/// A stretch and a nest's own effects both carry a memory of what came before,
/// and a piece made from cold starts without one. Starting it a little early
/// lets that settle before anything is heard.
constexpr double kPieceLeadIn = 0.5;

/// How long two neighbouring pieces are crossfaded over, at the end of the
/// earlier one.
///
/// Two stretches of the same audio, started in different places, line up to
/// within a fraction of a window but not to the sample. A short crossfade is
/// what makes the join inaudible rather than a click. At the end of a piece
/// rather than the start, because playback starting where a piece starts is
/// common -- and with the fade there it needed the piece before as well, two
/// pieces made before Play instead of one.
constexpr double kPieceCrossfade = 0.05;

/// How many pieces are kept behind and ahead of the one being heard.
constexpr int kPiecesBehind = 1;
constexpr int kPiecesAhead = 3;

/// One piece of a rendered clip, in timeline time.
struct Piece {
  /// Seconds into the clip that the first sample is at.
  double from = 0.0;
  media::AudioBuffer audio;
};

using Pieces = std::map<int, std::shared_ptr<const Piece>>;

/// A clip whose audio is made rather than read: a nest, mixed by a mixer of
/// its own, or a clip that is reversed, retimed or conformed. Its own, never
/// shared, because what is made depends on the clip's speed and direction.
struct RenderedSource {
  render::PlannedAudioClip planned;
  int sample_rate = 48000;
  int channels = 2;
  /// How long the clip lasts on the timeline, and so how many pieces it has.
  double length = 0.0;
  int count = 0;

  /// The file, for a clip; empty for a nest.
  std::string path;
  int stream = 0;
  /// The mixer for what is inside a nest. Used by one thread at a time: the
  /// reader during playback, the mixing thread during an export.
  std::unique_ptr<AudioMixer> inner;

  /// The pieces made so far. Published whole, like a source's window, so the
  /// mixing thread can read through them while the reader makes the next.
  std::atomic<std::shared_ptr<const Pieces>> resident;
  /// Seconds into the clip that the mix last needed. Written by the mixing
  /// thread, read by the reader.
  std::atomic<double> wanted{0.0};
  std::atomic<bool> failed{false};
  /// When the clip is first heard, in timeline seconds, for the reader's order.
  double needed_at = 0.0;

  [[nodiscard]] int piece_at(double offset) const noexcept {
    const int k = static_cast<int>(std::floor(std::max(0.0, offset) / kPieceSeconds));
    return std::clamp(k, 0, std::max(0, count - 1));
  }
};

/// One planned clip, with the samples and DSP it needs.
struct Voice {
  render::PlannedAudioClip planned;
  /// The shared source this reads from, or null when its audio is rendered or
  /// when the media could not be read at all, in which case the voice
  /// contributes silence.
  StreamedSource* stream = nullptr;
  /// Where its audio is made, when it is a nest or retimed. Owned by the mixer.
  RenderedSource* rendered = nullptr;
  audio::EffectChain chain;
  /// The mix frame its stack carries on from, so a block that does not is
  /// known to need the stack started again. -1 before it has been heard.
  std::int64_t next_frame = -1;
};

/// Whether a clip needs its audio rendered ahead of time rather than read
/// straight from the source.
/// How far past the start position audio is read before playback begins.
///
/// Two seconds, which is what the reader needs to get ahead of the playhead:
/// it wakes ten times a second and a decode is tens of milliseconds, so it is
/// hundreds of times faster than the sound is consumed. What this buys is the
/// gap between `create` returning and the reader's first pass — everything
/// after that it stays comfortably in front of.
constexpr double kEagerAhead = 2.0;

/// Whether this clip is heard at or shortly after where playback is starting.
///
/// Both ends matter: a clip already running at the start position has to be
/// resident, and so does the one after it, because the reader has not woken up
/// yet when the first block is mixed.
[[nodiscard]] bool heard_within(const render::PlannedAudioClip& planned, double start_at) noexcept {
  return planned.end > start_at - 1e-6 && planned.start < start_at + kEagerAhead;
}

[[nodiscard]] bool needs_retiming(const render::PlannedAudioClip& planned) noexcept {
  return planned.reverse || std::abs(planned.speed - 1.0) > 1e-6 ||
         std::abs(planned.conform - 1.0) > 1e-6 || !planned.remix.empty();
}

/// How long each join in a remix is crossfaded over. Short, because both sides
/// of a join are a few hundredths of a second before a downbeat, in the gap
/// before the attack: long enough not to click, and over before either beat
/// is struck.
constexpr double kRemixCrossfade = 0.03;

/// A remix's sound from `from` to `to` seconds along it, made from the
/// stretches of the file it plays, each faded in and out over the join with
/// the next — equal power, since the two sides are different bars and not the
/// same sound.
[[nodiscard]] std::optional<media::AudioBuffer> assemble_remix(
    const std::string& path, int stream, std::span<const core::RemixSpan> spans, double from,
    double to, int sample_rate, int channels) {
  media::AudioBuffer out;
  out.sample_rate = sample_rate;
  out.channels = channels;
  out.start_time = from;
  const auto rate = static_cast<double>(sample_rate);
  const auto lanes = static_cast<std::size_t>(channels);
  const auto frames = static_cast<std::size_t>(std::max(0.0, std::round((to - from) * rate)));
  out.samples.assign(frames * lanes, 0.0f);
  if (frames == 0) return out;

  const double half = kRemixCrossfade / 2.0;
  double at = 0.0;
  for (std::size_t k = 0; k < spans.size(); ++k) {
    const core::RemixSpan& span = spans[k];
    const double length = span.source_out - span.source_in;
    const bool first = k == 0;
    const bool last = k + 1 == spans.size();
    // Along the remix, the stretch is heard a little either side of its own
    // length, over the joins, except at the remix's two ends.
    const double lo = first ? at : at - half;
    const double hi = last ? at + length : at + length + half;
    const double a = std::max(lo, from);
    const double b = std::min(hi, to);
    if (b > a) {
      const double read_from = std::max(0.0, span.source_in + (a - at) - kDecodeMargin);
      const double read_to = span.source_in + (b - at) + kDecodeMargin;
      auto decoded = media::decode_audio(path, stream,
                                         {.sample_rate = sample_rate,
                                          .channels = channels,
                                          .start = read_from,
                                          .duration = read_to - read_from});
      if (!decoded) return std::nullopt;
      const std::size_t have = decoded->frame_count();
      const auto first_frame = static_cast<std::size_t>(std::max(0.0, std::ceil((a - from) * rate)));
      const auto end_frame = std::min(frames, static_cast<std::size_t>(std::ceil((b - from) * rate)));
      for (std::size_t i = first_frame; i < end_frame; ++i) {
        const double t = from + static_cast<double>(i) / rate;
        double gain = 1.0;
        if (!first && t < at + half) {
          gain *= std::sin(std::clamp((t - (at - half)) / kRemixCrossfade, 0.0, 1.0) *
                           std::numbers::pi / 2.0);
        }
        if (!last && t > at + length - half) {
          gain *= std::cos(std::clamp((t - (at + length - half)) / kRemixCrossfade, 0.0, 1.0) *
                           std::numbers::pi / 2.0);
        }
        const double position = (span.source_in + (t - at) - decoded->start_time) * rate;
        if (position < 0.0) continue;
        const auto index = static_cast<std::size_t>(position);
        if (index >= have) continue;
        const std::size_t next = std::min(index + 1, have - 1);
        const auto fraction = static_cast<float>(position - static_cast<double>(index));
        for (std::size_t c = 0; c < lanes; ++c) {
          const float x = decoded->samples[index * lanes + c];
          const float y = decoded->samples[next * lanes + c];
          out.samples[i * lanes + c] += static_cast<float>(gain) * (x + (y - x) * fraction);
        }
      }
    }
    at += length;
  }
  return out;
}

/// Renders a retimed clip's audio into timeline time.
///
/// Reading a source faster would shorten it *and* raise its pitch, which is a
/// tape-speed effect rather than a speed change. The reference used FFmpeg's
/// `atempo`, so the length changes here and the pitch does not.
[[nodiscard]] media::AudioBuffer render_retimed(const media::AudioBuffer& source,
                                                const render::PlannedAudioClip& planned,
                                                int sample_rate, int channels) {
  media::AudioBuffer out;
  out.sample_rate = sample_rate;
  out.channels = channels;

  const auto lanes = static_cast<std::size_t>(channels);
  const auto rate = static_cast<double>(sample_rate);
  const auto frames = source.frame_count();

  // Relative to the buffer's own start, since only the clip's range was
  // decoded and it begins wherever the decoder was asked to begin.
  const auto begin = static_cast<std::size_t>(
      std::max(0.0, (planned.source_in - source.start_time) * rate));
  const auto end = std::min(
      frames,
      static_cast<std::size_t>(std::max(0.0, (planned.source_out - source.start_time) * rate)));
  if (begin >= end) return out;

  std::vector<float> trimmed(
      source.samples.begin() + static_cast<std::ptrdiff_t>(begin * lanes),
      source.samples.begin() + static_cast<std::ptrdiff_t>(end * lanes));

  if (planned.reverse) {
    // Frame by frame, not sample by sample: reversing the interleaved buffer
    // outright would swap the channels as well as the time.
    const std::size_t count = trimmed.size() / lanes;
    for (std::size_t i = 0; i < count / 2; ++i) {
      for (std::size_t c = 0; c < lanes; ++c) {
        std::swap(trimmed[i * lanes + c], trimmed[(count - 1 - i) * lanes + c]);
      }
    }
  }

  // A conform first, and it is a *resample* rather than a stretch. The file is
  // being replayed on a different clock — 60 fps shown at 24 is tape run slow
  // — so the pitch goes down with the speed. Correcting it here would hide the
  // one cue that says the conform took effect, and would be a treatment nobody
  // asked for on top of it.
  //
  // The trimmed span is in file seconds, and dividing by the conform brings it
  // back to source seconds, which is the length the clip occupies before its
  // own speed is applied.
  if (std::abs(planned.conform - 1.0) > 1e-6) {
    trimmed = audio::resample_by(trimmed, channels, 1.0 / planned.conform);
  }

  // And then the clip's own speed, which is the opposite bargain: a clip at
  // speed 2 lasts half as long and sounds the same, so the factor is its
  // inverse and the pitch is held.
  out.samples = audio::time_stretch(trimmed, channels, 1.0 / planned.speed);
  return out;
}

/// Makes the piece of a rendered clip between `from` and `to`, in seconds into
/// the clip.
///
/// The part of the file or the nest those seconds come from is read, a little
/// either side, and put through the same reversal, conform and stretch as the
/// whole clip used to be; a nest that is not retimed comes through unchanged.
[[nodiscard]] std::optional<media::AudioBuffer> make_piece(RenderedSource& source, double from,
                                                          double to) {
  const render::PlannedAudioClip& planned = source.planned;
  const double rate = planned.speed * planned.conform;
  double file_from = planned.reverse ? planned.source_out - to * rate : planned.source_in + from * rate;
  double file_to = planned.reverse ? planned.source_out - from * rate : planned.source_in + to * rate;
  file_from = std::clamp(file_from, planned.source_in, planned.source_out);
  file_to = std::clamp(file_to, planned.source_in, planned.source_out);
  if (!(file_to > file_from)) return std::nullopt;

  media::AudioBuffer raw;
  if (source.inner != nullptr) {
    const auto frames = static_cast<std::size_t>((file_to - file_from) * source.sample_rate);
    if (frames == 0) return std::nullopt;
    raw.sample_rate = source.sample_rate;
    raw.channels = source.channels;
    raw.start_time = file_from;
    raw.samples.assign(frames * static_cast<std::size_t>(source.channels), 0.0f);
    // From cold each time: the lead-in is what settles the nest's own effects,
    // and a piece must not depend on which one was made before it.
    source.inner->reset();
    if (!source.inner->mix(file_from, raw.samples)) return std::nullopt;
  } else if (!planned.remix.empty()) {
    // A remix's own seconds rather than the file's: made from its stretches.
    auto made = assemble_remix(source.path, source.stream, planned.remix, file_from, file_to,
                               source.sample_rate, source.channels);
    if (!made) return std::nullopt;
    raw = std::move(*made);
  } else {
    const double start = std::max(0.0, file_from - kDecodeMargin);
    auto decoded = media::decode_audio(source.path, source.stream,
                                       {.sample_rate = source.sample_rate,
                                        .channels = source.channels,
                                        .start = start,
                                        .duration = file_to + kDecodeMargin - start});
    if (!decoded) return std::nullopt;
    raw = std::move(*decoded);
  }

  // The same treatment as the whole clip, over just these seconds of it.
  render::PlannedAudioClip span = planned;
  span.source_in = file_from;
  span.source_out = file_to;
  return render_retimed(raw, span, source.sample_rate, source.channels);
}

/// Which pieces should be held when the mix is `offset` seconds into the clip.
[[nodiscard]] std::pair<int, int> pieces_wanted(const RenderedSource& source,
                                                double offset) noexcept {
  const int k = source.piece_at(offset);
  return {std::max(0, k - kPiecesBehind), std::min(source.count - 1, k + kPiecesAhead)};
}

/// Makes whatever of `first` to `last` is not already held, and keeps nothing
/// else, publishing each piece as soon as it is made.
///
/// Outwards from `centre`, ahead before behind: the piece being heard first,
/// then the ones playback is heading into, and the one behind last. Made in
/// order from the first and published only once all were there, a piece a
/// second on a long capture meant the next one was four seconds coming.
void fill_pieces(RenderedSource& source, int first, int last, int centre) {
  std::shared_ptr<const Pieces> have = source.resident.load();

  // What is no longer wanted goes first, so the memory is not held for the
  // length of the making.
  if (have != nullptr && std::ranges::any_of(*have, [&](const auto& entry) {
        return entry.first < first || entry.first > last;
      })) {
    auto kept = std::make_shared<Pieces>();
    for (const auto& [k, piece] : *have) {
      if (k >= first && k <= last) kept->emplace(k, piece);
    }
    have = kept;
    source.resident.store(kept);
  }

  centre = std::clamp(centre, first, last);
  std::vector<int> order;
  for (int k = centre; k <= last; ++k) order.push_back(k);
  for (int k = centre - 1; k >= first; --k) order.push_back(k);

  for (const int k : order) {
    if (have != nullptr && have->contains(k)) continue;
    // Started early, for the reason `kPieceLeadIn` gives, which also covers
    // the end of the piece before as it fades out.
    const double begin = k * kPieceSeconds;
    const double from = k == 0 ? 0.0 : begin - kPieceLeadIn;
    const double to = std::min(source.length, begin + kPieceSeconds);
    auto made = make_piece(source, from, to);
    if (!made) {
      source.failed.store(true, std::memory_order_relaxed);
      return;
    }
    auto piece = std::make_shared<Piece>();
    piece->from = from;
    piece->audio = std::move(*made);

    // Beside what is being read rather than over it, as a window is.
    auto grown = have != nullptr ? std::make_shared<Pieces>(*have) : std::make_shared<Pieces>();
    grown->emplace(k, std::move(piece));
    have = grown;
    source.resident.store(std::move(grown));
  }
}

/// One frame of a decoded source at a fractional position, linearly
/// interpolated.
///
/// Interpolation matters for retimed clips, where the read position advances by
/// a non-integer step: point sampling would quantise it to whole samples and
/// add a rasp of quantisation noise on top of the retime.
void sample_into(const media::AudioBuffer& source, double position, std::size_t channels,
                 std::span<const int> channel_map, std::span<float> frame) noexcept {
  const auto frames = source.frame_count();
  if (frames == 0 || position < 0.0) return;

  const double floored = std::floor(position);
  const auto index = static_cast<std::size_t>(floored);
  if (index >= frames) return;

  const auto source_channels = static_cast<std::size_t>(std::max(source.channels, 1));
  const auto fraction = static_cast<float>(position - floored);
  const std::size_t next = std::min(index + 1, frames - 1);

  for (std::size_t c = 0; c < channels; ++c) {
    // The clip's map when it has one, and the default when it does not: a mono
    // source feeds every output channel, and a source with more channels than
    // the mix has is truncated rather than downmixed, which is what the
    // decoder's own resampling already arranged for.
    //
    // A map naming a channel the source does not have is silence rather than a
    // clamp to the nearest. Somebody asking for channel four of a stereo file
    // has said something about the shoot that turned out not to be true, and
    // quietly giving them channel two would hide it.
    int wanted = static_cast<int>(std::min(c, source_channels - 1));
    if (c < channel_map.size()) wanted = channel_map[c];
    if (wanted < 0 || static_cast<std::size_t>(wanted) >= source_channels) {
      frame[c] = 0.0f;
      continue;
    }

    const auto source_channel = static_cast<std::size_t>(wanted);
    const float a = source.samples[index * source_channels + source_channel];
    const float b = source.samples[next * source_channels + source_channel];
    frame[c] = a + (b - a) * fraction;
  }
}

}  // namespace

struct AudioMixer::Impl {
  /// Owned rather than referenced: the plan points into it, and a mixer that
  /// outlived the project it was built from would be reading freed clips.
  core::Project project;
  AudioMixSettings settings;

  /// Unique pointers so a source's address, and the atomics in it, survive the
  /// map growing.
  std::map<SourceKey, std::unique_ptr<StreamedSource>> sources;
  /// The same sources, in the order their audio is going to be wanted. What the
  /// reader walks, so a backlog is cleared in play order.
  std::vector<StreamedSource*> pending;
  /// Nests and retimed clips, a source each, in the order they are first heard.
  std::vector<std::unique_ptr<RenderedSource>> rendered;
  std::vector<Voice> voices;
  /// Scratch for a frame from the piece being faded into at a join.
  std::vector<float> join_next;
  std::vector<std::string> missing;

  /// Reads ahead of the mix, for real-time playback only. Nothing offline has
  /// one: an export decodes what it needs where it needs it, which is simpler
  /// and exactly reproducible.
  std::thread reader;
  std::mutex reader_lock;
  std::condition_variable reader_wake;
  bool reader_stopping = false;

  ~Impl() {
    {
      const std::lock_guard<std::mutex> lock(reader_lock);
      reader_stopping = true;
    }
    reader_wake.notify_all();
    if (reader.joinable()) reader.join();
  }

  std::unique_ptr<audio::Limiter> limiter;

  /// Atomic because the master fader is the one thing about a mix that can be
  /// changed while it is playing: the player's render thread reads this on
  /// every block while the interface writes it from the message loop.
  std::atomic<double> master_gain{1.0};

  /// The master's automation, copied at build time. Empty when there is none or
  /// the mode is Off, so the mix reads the live fader instead — deciding it
  /// here rather than per block keeps `mix` free of what Off means.
  std::vector<core::Keyframe> master_curve;

  /// The meter belongs to whichever thread is mixing; its reading is published
  /// under a lock for whoever is drawing. A reading is a few dozen bytes and is
  /// taken once a block, so the lock is never contended for long enough to
  /// matter — and the alternative, tearing a stereo pair across two atomics,
  /// shows up as a meter whose channels disagree.
  std::unique_ptr<audio::Meter> meter;
  mutable std::mutex levels_lock;
  audio::MeterReading levels;

  /// One meter per track lane, so a mixer strip can show what its own track is
  /// putting into the mix rather than what the mix came to.
  ///
  /// Indexed by the plan's `track_index`, and as long as the highest lane that
  /// carries audio — a lane with nothing on it keeps a meter that reads silence
  /// rather than being absent, so a caller never has to ask whether a track has
  /// an entry before it can draw one.
  ///
  /// They share `levels_lock` with the master's reading. The whole set is
  /// published in one place once a block, which is also what stops a strip
  /// showing a level from one block beside a neighbour showing the last.
  std::vector<std::unique_ptr<audio::Meter>> track_meters;
  std::vector<audio::MeterReading> track_levels;

  /// What each lane's fader was set to when the mix was planned, and what it is
  /// set to now.
  ///
  /// The fader is applied to the lane, once, after the lane's stack — so the
  /// live value *is* the fader rather than a ratio against a gain already
  /// folded into the clips. It used to be a ratio, because the plan multiplied
  /// the track's gain into every clip on it; that put a track fader ahead of
  /// each clip's own effects, and left a pre-fader send with no pre-fader
  /// signal to be taken from.
  ///
  /// Atomic for the same reason the master's is: the render thread reads them
  /// on every block while the interface writes them from the message loop.
  std::vector<double> built_track_gain;
  std::vector<std::atomic<double>> live_track_gain;

  /// Each lane's fader and panner automation, in timeline seconds, when it has
  /// any. A curve wins over the live fader, exactly as the master's does: an
  /// automated fader is being played back.
  std::vector<std::vector<core::Keyframe>> lane_gain_curve;
  std::vector<double> lane_pan;
  std::vector<std::vector<core::Keyframe>> lane_pan_curve;

  /// Whether each lane is heard at all — its mute, and solo elsewhere.
  ///
  /// The plan already drops clips on a silenced track, so this exists for the
  /// one kind of lane the plan has nothing to say about: a submix, which has no
  /// clips to drop and is silenced by not being poured onward.
  std::vector<bool> lane_audible;

  /// Where each lane goes, in the order it has to be run. See
  /// `core::bus_routes`.
  std::vector<core::BusRoute> routes;

  /// One effect chain per lane, run on what the lane sums to before it reaches
  /// the mix. Empty for a track with no stack, which is nearly all of them and
  /// costs nothing to skip.
  std::vector<audio::EffectChain> track_chains;
  /// The master's, run on the whole mix after the fader and before the limiter.
  audio::EffectChain master_chain;

  /// Which meter belongs to a project track, or -1 for one carrying no audio.
  ///
  /// Two things have been called a track index. The **plan** numbers audio
  /// tracks 0, 1, 2 and skips the video ones; everything above counts tracks as
  /// the project lists them. The two agree exactly when a project is all audio,
  /// which is what let the confusion past a green suite and a set of tests —
  /// and then showed up on screen as a meter that stayed dark on the one
  /// arrangement anybody actually has, a video track with sound under it.
  ///
  /// The public API takes the *project's* numbering, because that is what
  /// "track 1" means everywhere else. Inside, a lane is the plan's.
  std::vector<int> lane_of_track;

  [[nodiscard]] int lane_for(int track_index) const noexcept {
    if (track_index < 0) return -1;
    const auto at = static_cast<std::size_t>(track_index);
    return at < lane_of_track.size() ? lane_of_track[at] : -1;
  }

  /// A lane's fader at timeline time `t`: its curve when it has one, and the
  /// live value — which is what the interface writes — when it does not.
  [[nodiscard]] double lane_gain_at(std::size_t lane, double t) const noexcept {
    if (lane >= live_track_gain.size()) return 1.0;
    if (!lane_gain_curve[lane].empty()) {
      return std::clamp(core::eval_keyframes(lane_gain_curve[lane], t), 0.0, core::kMaxGain);
    }
    return live_track_gain[lane].load(std::memory_order_relaxed);
  }

  [[nodiscard]] double lane_pan_at(std::size_t lane, double t) const noexcept {
    if (lane >= lane_pan.size()) return 0.0;
    if (!lane_pan_curve[lane].empty()) {
      return std::clamp(core::eval_keyframes(lane_pan_curve[lane], t), -1.0, 1.0);
    }
    return lane_pan[lane];
  }

  /// Scratch, reused across calls so mixing does not allocate per block.
  std::vector<float> voice_block;
  /// The frames a stack with repair in it is given before its first block.
  std::vector<float> voice_prime;
  /// One block's worth per track, summed as the voices go by. A track can hold
  /// several clips at once, so a track's level is not any one voice's — it is
  /// what they come to together, which is only known once they have all been
  /// through.
  std::vector<std::vector<float>> track_blocks;
};

AudioMixer::AudioMixer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AudioMixer::~AudioMixer() = default;

std::expected<std::unique_ptr<AudioMixer>, std::string> AudioMixer::create(
    const core::Project& project, AudioMixSettings settings) {
  if (settings.sample_rate <= 0) return std::unexpected("the sample rate must be positive");
  if (settings.channels <= 0) return std::unexpected("the channel count must be positive");

  auto impl = std::make_unique<Impl>();
  impl->project = project;
  impl->settings = settings;

  const auto planned = render::plan_audio(impl->project);

  std::set<std::string> missing;
  for (const render::PlannedAudioClip& entry : planned) {
    // One track at a time when asked for, which is what an export writing a
    // stream per track does. Skipped before the source is decoded rather than
    // muted after: a mixer for one track should not pay for the others' audio.
    if (settings.only_track.has_value() && entry.track_index != *settings.only_track) continue;

    Voice voice;
    voice.planned = entry;
    // Built at the clip's own start, which is where playback of it begins and
    // therefore what an animated parameter is worth the first time it is heard.
    voice.chain = audio::EffectChain::build(entry.clip->audio_effects,
                                            static_cast<double>(settings.sample_rate),
                                            settings.channels, 0.0);

    const bool nest = entry.media != nullptr && core::is_nested_sequence(*entry.media);
    if (nest || (needs_retiming(entry) && entry.media != nullptr && !entry.media->path.empty())) {
      // Made rather than read: a nest is heard by mixing the sequence inside
      // it, and a retimed clip by reversing and stretching what the file has.
      // Either way it is made a piece at a time, in timeline time.
      auto source = std::make_unique<RenderedSource>();
      source->planned = entry;
      source->sample_rate = settings.sample_rate;
      source->channels = settings.channels;
      source->length = std::max(0.0, entry.end - entry.start);
      source->count = std::max(1, static_cast<int>(std::ceil(source->length / kPieceSeconds - 1e-9)));
      source->needed_at = entry.start;

      bool usable = source->length > 0.0;
      if (nest) {
        core::Project within = project;
        within.open = core::sequence_index(project, entry.media->sequence_id);
        usable = usable && settings.depth < kMaxNestDepth && within.open != std::string::npos;
        // A multicam is heard as the camera it is set to: the other cameras'
        // tracks are muted inside it.
        if (usable && entry.clip != nullptr && core::shows_camera(project, *entry.clip)) {
          core::Sequence& inside = within.sequence();
          for (std::size_t t = 0; t < inside.tracks.size(); ++t) {
            if (inside.tracks[t].kind != core::TrackKind::Audio) continue;
            if (!core::camera_uses_track(inside, t, entry.clip->camera)) {
              inside.tracks[t].muted = true;
            }
          }
        }
        if (usable) {
          // Reading as it mixes rather than everything up front: a nest of a
          // hundred cuts would otherwise decode a hundred clips before the
          // first piece of it could be made.
          auto inner = AudioMixer::create(within, {.sample_rate = settings.sample_rate,
                                                   .channels = settings.channels,
                                                   .depth = settings.depth + 1,
                                                   .read_as_mixed = true});
          if (inner.has_value()) {
            for (const std::string& lost : (*inner)->missing_media()) missing.insert(lost);
            source->inner = std::move(*inner);
          } else {
            usable = false;
          }
        }
      } else {
        source->path = entry.media->path;
        source->stream = entry.audio_stream;
      }

      if (usable) {
        // Made here only when it is about to be heard, or for an export, which
        // starts at the start and finds out now whether the file is there.
        // The rest is made by the reader, or by an export as it gets there.
        const bool now = settings.realtime ? heard_within(entry, settings.start_at)
                                           : !settings.read_as_mixed;
        const double offset =
            settings.realtime ? std::max(0.0, settings.start_at - entry.start) : 0.0;
        source->wanted.store(offset, std::memory_order_relaxed);
        if (now) {
          // Only the piece being heard, and the one before it when that is
          // still fading out. The rest is the reader's, or the export's.
          const int k = source->piece_at(offset);
          const bool joining =
              k + 1 < source->count && (k + 1) * kPieceSeconds - offset <= kPieceCrossfade;
          fill_pieces(*source, k, joining ? k + 1 : k, k);
        } else if (!nest && !std::filesystem::exists(source->path)) {
          source->failed.store(true, std::memory_order_relaxed);
        }
        if (source->failed.load(std::memory_order_relaxed)) {
          // Recorded and mixed as silence, as a file that will not read is.
          if (!nest) missing.insert(entry.media->id);
        } else {
          voice.rendered = source.get();
          impl->rendered.push_back(std::move(source));
        }
      }
      impl->voices.push_back(std::move(voice));
      continue;
    }

    if (entry.media != nullptr && !entry.media->path.empty()) {
      // Only the clip's own trim, not the whole file.
      const double from = std::max(0.0, entry.source_in - kDecodeMargin);
      const double to = entry.source_out + kDecodeMargin;
      const SourceKey key{entry.media->path, entry.audio_stream, from, to};

      auto found = impl->sources.find(key);
      if (found == impl->sources.end()) {
        auto source = std::make_unique<StreamedSource>();
        source->key = key;
        source->sample_rate = settings.sample_rate;
        source->channels = settings.channels;
        // Short enough to hold.
        source->whole = (to - from) <= kWholeSourceSeconds;
        source->wanted.store(from, std::memory_order_relaxed);

        source->needed_at = entry.start;
        found = impl->sources.emplace(key, std::move(source)).first;
      }

      StreamedSource& source = *found->second;
      source.needed_at = std::min(source.needed_at, entry.start);

      // Read here only when it is about to be heard. A mixer has to come back
      // able to play, and the audio that has to be resident for that is the
      // audio at the playhead — the rest arrives while the first seconds run,
      // because decoding is hundreds of times faster than playing.
      //
      // Everything used to be read here, and a cut sequence is made of clips
      // short enough to be held whole, so a press of Play paid one decode per
      // cut and got slower every time somebody cut again.
      //
      // Offline is unconditional: an export must not depend on a thread having
      // got somewhere. Except inside a nest, which reads as it mixes.      //
      // Asked **per voice** rather than when the source is made, because two
      // clips can share one: the same trim used twice is one decode, and
      // deciding at creation let the first of them answer for both. A clip
      // under the playhead then went silent because another clip elsewhere in
      // the sequence happened to name the same range first.
      const bool now = settings.realtime ? heard_within(entry, settings.start_at)
                                         : !settings.read_as_mixed;
      if (now && source.resident.load() == nullptr &&
          !source.failed.load(std::memory_order_relaxed)) {
        auto window = read_window(source, source.key.source_in);
        if (window == nullptr) {
          // Recorded and mixed as silence: an export should complete with a
          // hole rather than fail at the last step.
          missing.insert(entry.media->id);
          source.failed.store(true, std::memory_order_relaxed);
        }
        source.resident.store(std::move(window));
      }

      if (!now && settings.read_as_mixed && source.resident.load() == nullptr &&
          !std::filesystem::exists(source.key.path)) {
        // Nothing is read until a nest's piece wants it, so a file that is not
        // there would otherwise be found out only as silence.
        missing.insert(entry.media->id);
        source.failed.store(true, std::memory_order_relaxed);
      }
      if (!source.failed.load(std::memory_order_relaxed)) {
        // Safe to hold: the map owns the source through a unique pointer, so
        // its address survives every later insertion.
        voice.stream = &source;
      }
    } else {
      missing.insert(entry.clip->media_id);
    }

    impl->voices.push_back(std::move(voice));
  }

  impl->missing.assign(missing.begin(), missing.end());
  impl->master_gain.store(std::clamp(project.sequence().master_gain, 0.0, core::kMaxMasterGain),
                          std::memory_order_relaxed);
  if (core::is_master_gain_animated(project)) impl->master_curve = project.sequence().master_gain_keyframes;
  if (!project.sequence().master_effects.empty()) {
    impl->master_chain =
        audio::EffectChain::build(project.sequence().master_effects, static_cast<double>(settings.sample_rate),
                                  settings.channels, 0.0, false);
  }
  const audio::LimiterSettings limiting;
  impl->limiter = std::make_unique<audio::Limiter>(
      limiting, static_cast<double>(settings.sample_rate), settings.channels);
  // The meter's "over" is the limiter's ceiling rather than full scale, so the
  // warning means "the limiter is working" — which happens first, and is the
  // thing that can still be acted on.
  impl->meter = std::make_unique<audio::Meter>(static_cast<double>(settings.sample_rate),
                                               settings.channels,
                                               audio::MeterSettings{.over = limiting.limit});
  impl->levels = impl->meter->read();

  // A meter per audio track, whether or not anything is on it.
  //
  // Sized from the track list rather than from the plan, which is the change a
  // submix forced: a bus has no clips, so the plan has nothing to say about it,
  // and a lane sized out of existence is a bus that cannot be fed. Empty lanes
  // cost a meter and a cleared block each.
  std::size_t lanes = 0;
  for (const core::Track& track : impl->project.sequence().tracks) {
    if (track.kind == core::TrackKind::Audio) ++lanes;
  }
  impl->track_meters.reserve(lanes);
  for (std::size_t i = 0; i < lanes; ++i) {
    impl->track_meters.push_back(std::make_unique<audio::Meter>(
        static_cast<double>(settings.sample_rate), settings.channels,
        audio::MeterSettings{.over = limiting.limit}));
  }
  impl->track_levels.assign(lanes, impl->meter->read());
  impl->track_blocks.resize(lanes);

  // Which lane each project track owns, walked in the same order the plan
  // walks them so the two numberings cannot disagree.
  impl->lane_of_track.assign(impl->project.sequence().tracks.size(), -1);
  impl->built_track_gain.assign(lanes, 1.0);
  impl->track_chains.resize(lanes);
  impl->lane_gain_curve.assign(lanes, {});
  impl->lane_pan.assign(lanes, 0.0);
  impl->lane_pan_curve.assign(lanes, {});
  impl->lane_audible.assign(lanes, true);
  int ordinal = 0;
  for (std::size_t i = 0; i < impl->project.sequence().tracks.size(); ++i) {
    const core::Track& track = impl->project.sequence().tracks[i];
    if (track.kind != core::TrackKind::Audio) continue;
    const auto lane = static_cast<std::size_t>(ordinal++);
    impl->lane_of_track[i] = static_cast<int>(lane);
    if (lane >= lanes) continue;

    // The lane's own stack. Built at zero because a track's effect keyframes
    // are in timeline seconds and the sequence starts there.
    if (!track.audio_effects.empty()) {
      impl->track_chains[lane] =
          audio::EffectChain::build(track.audio_effects, static_cast<double>(settings.sample_rate),
                                    settings.channels, 0.0, false);
    }

    // The fader and panner, read from the project rather than from the plan.
    // The plan carries them too, folded into each clip, and that copy is for
    // callers that mix a clip at a time; here the lane is the bus and its fader
    // belongs to the lane.
    impl->built_track_gain[lane] = std::clamp(track.gain, 0.0, core::kMaxGain);
    impl->lane_pan[lane] = std::clamp(track.pan, -1.0, 1.0);
    if (core::is_track_gain_animated(track)) impl->lane_gain_curve[lane] = track.gain_keyframes;
    if (core::is_track_pan_animated(track)) impl->lane_pan_curve[lane] = track.pan_keyframes;
    impl->lane_audible[lane] = core::is_track_audible(impl->project, track);
  }
  impl->routes = core::bus_routes(impl->project);
  impl->live_track_gain = std::vector<std::atomic<double>>(lanes);
  for (std::size_t lane = 0; lane < lanes; ++lane) {
    impl->live_track_gain[lane].store(impl->built_track_gain[lane], std::memory_order_relaxed);
  }

  // In the order the audio is going to be wanted, so what is coming next is
  // read before what is five cuts away.
  impl->pending.reserve(impl->sources.size());
  for (auto& [key, source] : impl->sources) impl->pending.push_back(source.get());
  std::ranges::sort(impl->pending, {}, &StreamedSource::needed_at);

  // Something to run out of, or something not read yet. The second is the
  // ordinary case now: a cut sequence is all short sources, every one of which
  // is held whole and only the first few are read before playback starts.
  const bool anything_to_do =
      std::ranges::any_of(impl->sources,
                          [](const auto& entry) {
                            return !entry.second->whole || entry.second->resident.load() == nullptr;
                          }) ||
      !impl->rendered.empty();
  std::ranges::sort(impl->rendered, {}, &RenderedSource::needed_at);
  if (settings.realtime && anything_to_do) {
    Impl* raw = impl.get();
    raw->reader = std::thread([raw] {
      for (;;) {
        {
          std::unique_lock<std::mutex> lock(raw->reader_lock);
          // A short wait rather than a signal per block: the mixing thread must
          // not pay for waking anybody, and audio decodes hundreds of times
          // faster than it plays, so checking ten times a second is far more
          // often than a window can be used up.
          raw->reader_wake.wait_for(lock, std::chrono::milliseconds(100),
                                    [raw] { return raw->reader_stopping; });
          if (raw->reader_stopping) return;
        }

        for (StreamedSource* source : raw->pending) {
          // `covers` answers false for a source with nothing resident, whole or
          // not, which is what makes one that has never been read the reader's
          // job rather than something it walks past.
          if (source->failed.load(std::memory_order_relaxed)) continue;
          const double at = source->wanted.load(std::memory_order_relaxed);
          const std::shared_ptr<const Window> have = source->resident.load();
          if (covers(have.get(), *source, at)) continue;

          // Built beside what is being read rather than over it, so the mixing
          // thread's pointer stays valid for as long as it holds it.
          if (auto fresh = read_window(*source, at)) {
            source->resident.store(std::move(fresh));
          } else {
            source->failed.store(true, std::memory_order_relaxed);
          }
        }

        for (const std::unique_ptr<RenderedSource>& source : raw->rendered) {
          if (source->failed.load(std::memory_order_relaxed)) continue;
          const double at = source->wanted.load(std::memory_order_relaxed);
          const auto [first, last] = pieces_wanted(*source, at);
          fill_pieces(*source, first, last, source->piece_at(at));
        }
      }
    });
  }

  return std::unique_ptr<AudioMixer>(new AudioMixer(std::move(impl)));
}

std::expected<void, std::string> AudioMixer::mix(double t, std::span<float> out) {
  const auto channels = static_cast<std::size_t>(impl_->settings.channels);
  if (channels == 0) return std::unexpected("the mixer has no channels");
  if (out.size() % channels != 0) {
    return std::unexpected("the output block is not a whole number of frames");
  }

  std::ranges::fill(out, 0.0f);
  const std::size_t frames = out.size() / channels;
  if (frames == 0) return {};

  // Cleared rather than reallocated. `assign` on a vector that has already held
  // a block of this size keeps its storage, which is what keeps the mixing
  // thread free of the allocator — the same reason `voice_block` is a member.
  for (std::vector<float>& lane : impl_->track_blocks) lane.assign(out.size(), 0.0f);

  const double rate = static_cast<double>(impl_->settings.sample_rate);

  // Sample positions come from an absolute index rather than from the caller's
  // `t` arithmetic. Deriving each frame's time as `t + i/rate` makes the result
  // depend on where the caller happened to split its blocks, because the two
  // groupings round differently; anchoring to an index means a span mixed in
  // one call and the same span mixed in twenty are bit-for-bit identical.
  const auto base = static_cast<std::int64_t>(std::llround(t * rate));
  const double begin = static_cast<double>(base) / rate;
  const double block_end = static_cast<double>(base + static_cast<std::int64_t>(frames)) / rate;

  for (Voice& voice : impl_->voices) {
    if (voice.stream == nullptr && voice.rendered == nullptr) continue;
    if (block_end <= voice.planned.start || begin >= voice.planned.end) continue;

    // Resolved once for the block rather than per frame. A retimed voice reads
    // its own baked audio; a shared source is whatever window is resident, and
    // holding the pointer for the whole block is what makes it safe to read
    // while a reader thread builds the next one.
    std::shared_ptr<const Window> window;
    std::shared_ptr<const Pieces> pieces;
    const media::AudioBuffer* audio = nullptr;
    if (voice.rendered != nullptr) {
      RenderedSource& source = *voice.rendered;
      const double from = std::max(begin, voice.planned.start) - voice.planned.start;
      const double to = std::min(block_end, voice.planned.end) - voice.planned.start;
      source.wanted.store(from, std::memory_order_relaxed);
      pieces = source.resident.load();
      if (!impl_->settings.realtime && !source.failed.load(std::memory_order_relaxed)) {
        // Made here and now, for the same reason a window is read here offline.
        // From the block's first frame to the piece after its last, which a
        // join at the end of the block fades into.
        const int first = source.piece_at(from);
        const int last = std::min(source.count - 1, source.piece_at(to) + 1);
        const bool held = pieces != nullptr && std::ranges::all_of(
            std::views::iota(first, last + 1), [&](int k) { return pieces->contains(k); });
        if (!held) {
          const auto [keep_first, keep_last] = pieces_wanted(source, from);
          fill_pieces(source, std::min(first, keep_first), std::max(last, keep_last),
                      source.piece_at(from));
          pieces = source.resident.load();
        }
      }
      if (pieces == nullptr || pieces->empty()) continue;  // not made yet: silent
    } else {
      const double at =
          render::audio_source_time_at(voice.planned, std::max(begin, voice.planned.start));
      voice.stream->wanted.store(at, std::memory_order_relaxed);

      window = voice.stream->resident.load();
      if (!impl_->settings.realtime && !covers(window.get(), *voice.stream, at)) {
        // Nothing is waiting on this thread, so read it here and now. That is
        // what makes an export deterministic: no thread decides how much had
        // arrived by the time a block was mixed.
        if (auto fresh = read_window(*voice.stream, at)) {
          voice.stream->resident.store(fresh);
          window = std::move(fresh);
        }
      }
      if (window == nullptr) continue;  // not read yet: this voice is silent
      audio = &window->audio;
    }
    if (pieces == nullptr && (audio == nullptr || audio->frame_count() == 0)) continue;

    // The sub-range of this block the clip actually covers. Feeding the effect
    // chain only these frames keeps it contiguous in the clip's own time, which
    // is what its filter state is a memory of.
    const auto first = static_cast<std::size_t>(
        std::max(0.0, std::ceil(voice.planned.start * rate) - static_cast<double>(base)));
    const auto last = std::min(
        frames, static_cast<std::size_t>(std::max(
                    0.0, std::ceil(voice.planned.end * rate) - static_cast<double>(base))));
    if (first >= last) continue;

    const std::size_t span = last - first;

    // The clip's sound from `at_index` on, with its gain and pan, as the
    // effects receive it. Silence outside the clip: a stack fed ahead of time
    // reaches past the clip's end, and what is past it is not the clip's.
    const auto fill = [&](std::span<float> block, std::int64_t at_index, std::size_t count) {
      for (std::size_t i = 0; i < count; ++i) {
        const double now = static_cast<double>(at_index + static_cast<std::int64_t>(i)) / rate;
        if (now < voice.planned.start || now >= voice.planned.end) continue;

        const std::span<float> frame = block.subspan(i * channels, channels);
        if (pieces != nullptr) {
          // Made in timeline time already, so the read position is how far into
          // the clip this is -- and across the last moments of a piece, a fade
          // into the next.
          const double offset = now - voice.planned.start;
          const int k = voice.rendered->piece_at(offset);
          const auto here = pieces->find(k);
          const double left = (k + 1) * kPieceSeconds - offset;
          const auto next = k + 1 < voice.rendered->count && left < kPieceCrossfade
                                ? pieces->find(k + 1)
                                : pieces->end();
          if (here != pieces->end()) {
            sample_into(here->second->audio, (offset - here->second->from) * rate, channels,
                        voice.planned.channel_map, frame);
          }
          if (next != pieces->end()) {
            impl_->join_next.assign(channels, 0.0f);
            sample_into(next->second->audio, (offset - next->second->from) * rate, channels,
                        voice.planned.channel_map, impl_->join_next);
            const auto towards =
                here != pieces->end()
                    ? static_cast<float>(std::clamp(1.0 - left / kPieceCrossfade, 0.0, 1.0))
                    : 1.0f;
            for (std::size_t c = 0; c < channels; ++c) {
              frame[c] = frame[c] * (1.0f - towards) + impl_->join_next[c] * towards;
            }
          }
        } else {
          // Read from the shared source at the source time the plan asks for.
          const double position =
              (render::audio_source_time_at(voice.planned, now) - audio->start_time) * rate;
          sample_into(*audio, position, channels, voice.planned.channel_map, frame);
        }

        // Gain and fades before the effects, matching the reference's chain.
        // The clip's own, and only the clip's: the track's fader is applied to
        // the lane below, after the lane's stack, which is where Premiere has it
        // and the only place a pre-fader send can be tapped from.
        const auto gain = static_cast<float>(render::clip_gain_at(voice.planned, now));
        for (std::size_t c = 0; c < channels; ++c) frame[c] *= gain;

        // The panner, on the two sides of a stereo bus and nothing else. A bus
        // with one channel has no sides to balance, and one with more than two
        // has sides this control does not name — turning only the first two of a
        // 5.1 mix would be worse than leaving it alone.
        if (channels == 2) {
          const render::StereoGain pan = render::clip_pan_at(voice.planned, now);
          frame[0] *= static_cast<float>(pan.left);
          frame[1] *= static_cast<float>(pan.right);
        }
      }

    };

    // A stack with repair in it hands each sample back `lead` frames late, so
    // it is fed that far ahead and what comes out lines up with the picture.
    // Fed from cold, it would hand back silence for its first `lead` frames,
    // so wherever the clip is not carrying on from where it last was -- its
    // first block, a seek, a stretch it was not heard -- the stack is started
    // afresh and given those frames first.
    const std::size_t lead = voice.chain.latency();
    const std::int64_t from_index = base + static_cast<std::int64_t>(first);
    if (lead > 0 && voice.next_frame != from_index) {
      voice.chain.reset();
      impl_->voice_prime.assign(lead * channels, 0.0f);
      fill(impl_->voice_prime, from_index, lead);
      run_chain_over(voice.chain, impl_->voice_prime, from_index, rate, channels,
                     voice.planned.clip_start);
    }
    impl_->voice_block.assign(span * channels, 0.0f);
    fill(impl_->voice_block, from_index + static_cast<std::int64_t>(lead), span);
    voice.next_frame = base + static_cast<std::int64_t>(last);

    // Clip-local, because a clip's effect keyframes are.
    run_chain_over(voice.chain, impl_->voice_block, from_index + static_cast<std::int64_t>(lead),
                   rate, channels, voice.planned.clip_start);

    // Into the lane's own block rather than straight into the mix. A plain sum:
    // `normalize=0`, so clips add rather than being scaled down by how many of
    // them there are.
    //
    // This used to add into `out` as well, with the lane block kept alongside
    // as a tap for the meter. It cannot any more: a track has an effect stack
    // now, and a stack has to run on what the lane came to before that reaches
    // the mix. So the lane block *is* the signal path, and the sum into `out`
    // happens once per lane after its chain, below.
    const auto lane = static_cast<std::size_t>(voice.planned.track_index);
    if (lane < impl_->track_blocks.size()) {
      std::vector<float>& into = impl_->track_blocks[lane];
      for (std::size_t i = 0; i < span * channels; ++i) {
        into[first * channels + i] += impl_->voice_block[i];
      }
    }
  }

  // A copy of one bus into another, at a level. Never the same bus twice: the
  // routes are loop-free by construction, so a lane cannot pour into itself.
  const auto pour_into = [&](int to_lane, const std::vector<float>& from, double level) {
    if (to_lane < 0 || static_cast<std::size_t>(to_lane) >= impl_->track_blocks.size()) return;
    std::vector<float>& into = impl_->track_blocks[static_cast<std::size_t>(to_lane)];
    const auto gain = static_cast<float>(level);
    const std::size_t n = std::min(from.size(), into.size());
    for (std::size_t i = 0; i < n; ++i) into[i] += from[i] * gain;
  };

  // Each lane through its own stack and its own fader, and then wherever it
  // goes — another bus, or the mix.
  //
  // In feed order rather than lane order, which is the whole reason
  // `bus_routes` exists: a submix cannot be run until everything pouring into
  // it has been, and running it early does not fail — it plays the previous
  // block through this block's compressor, one buffer late.
  for (const core::BusRoute& route : impl_->routes) {
    const auto lane = static_cast<std::size_t>(route.lane);
    if (lane >= impl_->track_blocks.size()) continue;
    std::vector<float>& block = impl_->track_blocks[lane];

    // The lane's stack, pre-fader.
    if (lane < impl_->track_chains.size() && !impl_->track_chains[lane].empty()) {
      run_chain_over(impl_->track_chains[lane], block, base, rate, channels, 0.0);
    }

    // Pre-fader sends are taken here, before the fader below touches the block.
    // That is the whole difference between the two kinds: a pre-fader send goes
    // on feeding its bus at the same level while the dry signal is ridden.
    for (const core::SendRoute& send : route.sends) {
      if (!send.pre_fader) continue;
      pour_into(send.to_lane, block, send.level);
    }

    // The fader and panner. Per sample only when there is a curve to follow;
    // otherwise one number for the block, which is every lane in nearly every
    // project.
    const bool gain_curve = lane < impl_->lane_gain_curve.size() &&
                            !impl_->lane_gain_curve[lane].empty();
    const bool pan_curve = lane < impl_->lane_pan_curve.size() &&
                           !impl_->lane_pan_curve[lane].empty();
    if (gain_curve || pan_curve) {
      for (std::size_t i = 0; i < frames; ++i) {
        const double now = static_cast<double>(base + static_cast<std::int64_t>(i)) / rate;
        const auto gain = static_cast<float>(impl_->lane_gain_at(lane, now));
        for (std::size_t c = 0; c < channels; ++c) block[i * channels + c] *= gain;
        if (channels == 2) {
          const double pan = impl_->lane_pan_at(lane, now);
          block[i * channels] *= static_cast<float>(pan > 0.0 ? 1.0 - pan : 1.0);
          block[i * channels + 1] *= static_cast<float>(pan < 0.0 ? 1.0 + pan : 1.0);
        }
      }
    } else {
      const auto gain = static_cast<float>(impl_->lane_gain_at(lane, begin));
      if (gain != 1.0f) {
        for (float& sample : block) sample *= gain;
      }
      const double pan = impl_->lane_pan_at(lane, begin);
      if (channels == 2 && pan != 0.0) {
        const auto left = static_cast<float>(pan > 0.0 ? 1.0 - pan : 1.0);
        const auto right = static_cast<float>(pan < 0.0 ? 1.0 + pan : 1.0);
        for (std::size_t i = 0; i < frames; ++i) {
          block[i * channels] *= left;
          block[i * channels + 1] *= right;
        }
      }
    }

    // Metered after the fader, which is what a strip shows: moving a fader has
    // to move the meter beside it, or one of the two is lying.
    if (lane < impl_->track_meters.size()) impl_->track_meters[lane]->process(block);

    // A silenced lane is metered and then goes nowhere. The plan already drops
    // the clips on a muted track, so this is what silences the one kind of lane
    // it has nothing to say about: a bus, which has no clips to drop.
    if (lane < impl_->lane_audible.size() && !impl_->lane_audible[lane]) continue;

    for (const core::SendRoute& send : route.sends) {
      if (send.pre_fader) continue;
      pour_into(send.to_lane, block, send.level);
    }

    if (route.output_lane >= 0) {
      pour_into(route.output_lane, block, 1.0);
    } else {
      for (std::size_t i = 0; i < block.size(); ++i) out[i] += block[i];
    }
  }

  // The master fader, ahead of the limiter: pulling it down has to take a hot
  // mix *out* of limiting, and after the limiter it would only make an already
  // squashed mix quieter.
  //
  // The curve wins over the live fader when there is one, exactly as a track's
  // does: an automated fader is being *played back*, and letting the atomic
  // override it would mean the number the interface last wrote silently
  // flattening somebody's pass.
  const double master = impl_->master_curve.empty()
                            ? impl_->master_gain.load(std::memory_order_relaxed)
                            : std::clamp(core::eval_keyframes(impl_->master_curve, begin), 0.0,
                                         core::kMaxMasterGain);
  if (master != 1.0) {
    const auto gain = static_cast<float>(master);
    for (float& sample : out) sample *= gain;
  }

  // Metered here, between the fader and the limiter: this is the mix as it was
  // made, which is what a meter is for. See `levels`.
  // The master's own stack, after its fader and before the limiter. The
  // limiter is last on purpose: it exists to catch whatever the rest of the
  // chain produced, and a stack after it would be processing something already
  // squashed and could push it back over the ceiling it had just come under.
  if (!impl_->master_chain.empty()) {
    run_chain_over(impl_->master_chain, out, base, rate, channels, 0.0);
  }

  impl_->meter->process(out);

  // The lanes were metered above, each as it was run, and *before* the master
  // fader — unlike the mix. A track meter answers "what is this track putting
  // out", and pulling the master down does not change that: a set of strips
  // that all dropped together when the master moved would be five meters
  // showing one thing.

  // One lock for the whole set, so a strip can never show a level from this
  // block beside a neighbour still showing the last.
  {
    const std::lock_guard lock(impl_->levels_lock);
    impl_->levels = impl_->meter->read();
    for (std::size_t lane = 0; lane < impl_->track_meters.size(); ++lane) {
      impl_->track_levels[lane] = impl_->track_meters[lane]->read();
    }
  }

  impl_->limiter->process(out);
  return {};
}

void AudioMixer::set_master_gain(double gain) noexcept {
  impl_->master_gain.store(std::clamp(gain, 0.0, core::kMaxMasterGain),
                           std::memory_order_relaxed);
}

double AudioMixer::master_gain() const noexcept {
  return impl_->master_gain.load(std::memory_order_relaxed);
}

audio::MeterReading AudioMixer::levels() const noexcept {
  const std::lock_guard lock(impl_->levels_lock);
  return impl_->levels;
}

audio::MeterReading AudioMixer::track_levels(int track_index) const noexcept {
  const std::lock_guard lock(impl_->levels_lock);
  const int lane = impl_->lane_for(track_index);
  if (lane < 0 || static_cast<std::size_t>(lane) >= impl_->track_levels.size()) {
    // A track carrying no audio, or one this mixer was not built for. Silence
    // is the true answer and saves every caller a check it would get wrong.
    audio::MeterReading quiet;
    quiet.count = impl_->settings.channels;
    return quiet;
  }
  return impl_->track_levels[static_cast<std::size_t>(lane)];
}

std::size_t AudioMixer::track_count() const noexcept { return impl_->track_levels.size(); }

void AudioMixer::set_track_gain(int track_index, double gain) noexcept {
  const int lane = impl_->lane_for(track_index);
  if (lane < 0 || static_cast<std::size_t>(lane) >= impl_->live_track_gain.size()) return;
  impl_->live_track_gain[static_cast<std::size_t>(lane)].store(
      std::clamp(gain, 0.0, core::kMaxGain), std::memory_order_relaxed);
}

double AudioMixer::track_gain(int track_index) const noexcept {
  const int lane = impl_->lane_for(track_index);
  if (lane < 0 || static_cast<std::size_t>(lane) >= impl_->live_track_gain.size()) return 0.0;
  return impl_->live_track_gain[static_cast<std::size_t>(lane)].load(std::memory_order_relaxed);
}

void AudioMixer::flush(std::span<float> out) { impl_->limiter->flush(out); }

std::size_t AudioMixer::latency_frames() const noexcept {
  return impl_->limiter->latency_frames();
}

void AudioMixer::reset() {
  for (Voice& voice : impl_->voices) {
    voice.chain.reset();
    voice.next_frame = -1;
  }
  // The track stacks as well, for exactly the reason the voices' are: a filter
  // carries a memory of what came before, and after a seek what came before is
  // not what is about to be played.
  for (audio::EffectChain& chain : impl_->track_chains) chain.reset();
  impl_->master_chain.reset();
  impl_->limiter->reset();

  // The meter too: levels only fall while audio is being measured, so a seek
  // that lands somewhere quiet would otherwise leave the bars where the last
  // loud thing left them.
  impl_->meter->reset();
  for (const std::unique_ptr<audio::Meter>& meter : impl_->track_meters) meter->reset();

  const std::lock_guard lock(impl_->levels_lock);
  impl_->levels = impl_->meter->read();
  for (std::size_t lane = 0; lane < impl_->track_meters.size(); ++lane) {
    impl_->track_levels[lane] = impl_->track_meters[lane]->read();
  }
}

bool AudioMixer::silent() const noexcept {
  return std::ranges::none_of(impl_->voices, [](const Voice& v) {
    return v.stream != nullptr || v.rendered != nullptr;
  });
}

const AudioMixSettings& AudioMixer::settings() const noexcept { return impl_->settings; }

std::expected<double, std::string> AudioMixer::measure_loudness(const core::Project& project,
                                                                AudioMixSettings settings) {
  // Never real time: this is an offline pass and wants the deterministic
  // behaviour, where a window that has not been read yet is read there and then
  // rather than mixing as silence. A measurement that depended on how much a
  // reader thread had managed would not be a measurement.
  settings.realtime = false;

  auto built = AudioMixer::create(project, settings);
  if (!built) return std::unexpected(built.error());
  AudioMixer& mixer = **built;

  const double duration = core::timeline_duration(project);
  if (mixer.silent() || !(duration > 0.0)) return audio::kAbsoluteGateLufs;

  audio::LoudnessMeter meter(static_cast<double>(settings.sample_rate), settings.channels);

  // A second at a time. Large enough that the per-block overhead disappears,
  // small enough that a long sequence does not need its whole mix in memory at
  // once — which is the same reason the mixer windows its sources.
  const auto frames = static_cast<std::size_t>(settings.sample_rate);
  const auto channels = static_cast<std::size_t>(settings.channels);
  std::vector<float> block(frames * channels);

  for (double at = 0.0; at < duration; at += 1.0) {
    if (auto mixed = mixer.mix(at, block); !mixed) return std::unexpected(mixed.error());
    meter.process(block);
  }

  // The limiter's look-ahead, so the tail of the programme is counted. A
  // sequence ending on its loudest moment would otherwise lose it.
  std::vector<float> tail(mixer.latency_frames() * channels, 0.0f);
  if (!tail.empty()) {
    mixer.flush(tail);
    meter.process(tail);
  }

  return meter.integrated_lufs();
}

const std::vector<std::string>& AudioMixer::missing_media() const noexcept {
  return impl_->missing;
}

}  // namespace cutline::engine
