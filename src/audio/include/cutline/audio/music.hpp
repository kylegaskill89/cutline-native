#pragma once

/// Finding the beats, bars and phrases in a piece of music, and rearranging it
/// to last a different length: Premiere's Remix.
///
/// A song cannot be made shorter or longer by stretching it without everybody
/// hearing that it has been: the tempo changes, and past a few per cent so does
/// the character. What an editor does by hand instead is cut it — out of the
/// end of one bar and into the start of another that sounds like the bar that
/// would have come next — and Remix is that, searched for. The song is cut
/// only where a bar begins, only into a bar whose surroundings match the ones
/// being left, and the beginning and the ending are kept, so it still starts
/// and finishes the way it was written to.
///
/// Two halves. `analyse_music` listens once: the tempo, where each beat lands,
/// which beat each bar starts on, and what each beat sounds like — its harmony
/// and its colour. `plan_remix` then searches that for the arrangement nearest
/// a length, which is cheap enough to do again on every change of the target.

#include <array>
#include <limits>
#include <span>
#include <vector>

namespace cutline::audio {

/// The rate music is best analysed at. Anything is accepted; this is what the
/// caller decoding a file should ask for, because nothing a beat is found from
/// is above 11 kHz and half the samples are half the work.
inline constexpr int kMusicAnalysisRate = 22050;

/// How many beats a bar is taken to have.
///
/// Four, without asking. Almost everything anybody lays under a picture is in
/// four, and Premiere does not ask either; a waltz is cut on every fourth beat
/// as well, which lands on a bar line one time in three, and that is what
/// Variations and Segments are there to work around.
inline constexpr int kBeatsPerBar = 4;

/// What was heard.
struct MusicAnalysis {
  /// How long what was analysed lasts, in seconds.
  double duration = 0.0;
  /// Beats per minute.
  double tempo = 0.0;
  /// Where each beat starts, in seconds, in order: the onset of whatever is
  /// struck on it, which is the moment a cut into it has to be just before.
  std::vector<double> beats;
  /// Which of the first `kBeatsPerBar` beats is the first beat of a bar.
  int bar_phase = 0;
  /// Per beat, how much of each of the twelve pitch classes is sounding,
  /// scaled to unit length: what makes two bars of the same chord alike.
  std::vector<std::array<float, 12>> harmony;
  /// Per beat, the loudness in twelve bands from the bass to the cymbals, in
  /// decibels: what makes the chorus unlike the verse when the chords agree.
  std::vector<std::array<float, 12>> timbre;

  /// Whether there is enough of a beat to rearrange around: four bars, at
  /// least, of which only the middle ones can be cut at.
  [[nodiscard]] bool usable() const noexcept { return beats.size() >= 4 * kBeatsPerBar; }

  [[nodiscard]] bool is_downbeat(std::size_t beat) const noexcept {
    return static_cast<int>(beat % kBeatsPerBar) == bar_phase;
  }
};

/// Listens to mono audio at `sample_rate`.
///
/// The tempo comes from how regularly the onsets recur, the beats from the
/// onsets nearest that regular grid (Ellis's dynamic programme, which follows
/// a tempo that drifts), and each beat's exact moment from the steepest rise
/// in loudness near it, so a cut lands before the attack rather than through
/// it. Bars start on the beat the harmony tends to change on.
///
/// Music without a beat — speech, a drone, silence — analyses as unusable
/// rather than as a guess.
[[nodiscard]] MusicAnalysis analyse_music(std::span<const float> mono, int sample_rate);

/// What a remix is asked for.
struct RemixRequest {
  /// The length wanted, in seconds.
  double target = 0.0;
  /// The longest it may be, which is the room the clip has on its track. The
  /// target is a wish and this is a wall.
  double maximum = std::numeric_limits<double>::infinity();
  /// Premiere's Segments, 0 to 100: few long stretches of the song with as few
  /// cuts as will reach the target, up to many short ones that can come
  /// nearer it.
  int segments = 50;
  /// Premiere's Variations, 0 to 100: what makes two bars alike enough to cut
  /// between, from their colour — the instruments, how loud — up to their
  /// harmony. A different arrangement comes out of each.
  int variations = 50;
};

/// One stretch of the song, played straight through, in seconds of it.
struct MusicSpan {
  double from = 0.0;
  double to = 0.0;

  friend bool operator==(const MusicSpan&, const MusicSpan&) = default;
};

/// What was found: the stretches to play in order, the first starting where
/// the song does and the last ending where it does.
///
/// Each join is a few hundredths of a second before the downbeat on both sides
/// of it, so the cut is made in the quiet before the attack and the beat after
/// it is struck whole, from the bar being cut into.
struct RemixPlan {
  std::vector<MusicSpan> spans;
  double length = 0.0;
};

/// How far before a downbeat each join is put.
inline constexpr double kRemixLead = 0.02;

/// The arrangement nearest `request.target` that is no longer than its
/// maximum.
///
/// A search over every way through the song's bars, where playing on costs
/// nothing and a cut costs how unlike the bars around the two ends of it are,
/// plus something for being a cut at all, and a little more for one that
/// breaks the four-bar phrase. Lengthening repeats a passage; shortening
/// leaves one out. Segments sets both what a cut costs and how many bars must
/// play between two of them. Accuracy is to within a beat or so of the target
/// unless the song cannot reach it; Premiere promises five seconds and usually
/// manages one.
///
/// A song that is not `usable` is cut short or left as it is, which is all
/// that can be done without knowing where its bars are.
[[nodiscard]] RemixPlan plan_remix(const MusicAnalysis& analysis, const RemixRequest& request);

}  // namespace cutline::audio
