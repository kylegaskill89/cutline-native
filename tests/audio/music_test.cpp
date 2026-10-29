/// Remix's listening and its arranging, on a song made here: 120 beats a
/// minute, a kick on every beat and a hi-hat between, and a chord on each bar
/// in a four-bar progression, laid out verse, verse, chorus, verse with the
/// chorus in other chords and a brighter sound. Everything a remix needs to
/// know about it is known exactly, which is what lets a cut be judged.

#include "cutline/audio/music.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

namespace cutline::audio {
namespace {

constexpr int kRate = kMusicAnalysisRate;
constexpr double kBeat = 0.5;
constexpr double kBar = kBeat * kBeatsPerBar;
/// Silence before the first beat, so the start of the song is not a beat.
constexpr double kPickup = 0.3;
constexpr int kBars = 32;
constexpr double kTail = 1.0;

struct Song {
  std::vector<float> samples;
  double duration = 0.0;
};

[[nodiscard]] double bar_start(int bar) { return kPickup + bar * kBar; }

/// Which section a bar is in: 0 for the verse, 1 for the chorus.
[[nodiscard]] int section_of(int bar) { return bar / 8 == 2 ? 1 : 0; }

void add_tone(std::vector<float>& into, double freq, double amplitude, double from, double to,
              double brightness) {
  const auto a = static_cast<std::size_t>(from * kRate);
  const auto b = std::min(into.size(), static_cast<std::size_t>(to * kRate));
  for (std::size_t i = a; i < b; ++i) {
    const double t = static_cast<double>(i) / kRate - from;
    // A soft attack and release, so the chord change is not itself an onset
    // louder than the kick.
    const double envelope = std::min({1.0, t / 0.02, (to - from - t) / 0.02});
    double v = std::sin(2.0 * std::numbers::pi * freq * t);
    v += brightness * 0.5 * std::sin(2.0 * std::numbers::pi * 2.0 * freq * t);
    v += brightness * 0.33 * std::sin(2.0 * std::numbers::pi * 3.0 * freq * t);
    into[i] += static_cast<float>(amplitude * envelope * v);
  }
}

[[nodiscard]] double midi(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

/// The song, or with `one_chord` the same bar over and over, which can be cut
/// anywhere a bar starts.
[[nodiscard]] Song make_song(bool one_chord = false) {
  Song song;
  song.duration = bar_start(kBars) + kTail;
  song.samples.assign(static_cast<std::size_t>(song.duration * kRate), 0.0f);
  std::mt19937 random(7);
  std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

  // Verse: C, Am, F, G. Chorus: Dm, Bb, Eb, F — another key's worth of
  // pitch classes, so its bars cannot pass for the verse's.
  const int verse[4][3] = {{60, 64, 67}, {57, 60, 64}, {53, 57, 60}, {55, 59, 62}};
  const int chorus[4][3] = {{62, 65, 69}, {58, 62, 65}, {63, 67, 70}, {65, 69, 72}};
  for (int bar = 0; bar < kBars; ++bar) {
    const bool in_chorus = !one_chord && section_of(bar) == 1;
    const auto& chord = one_chord ? verse[0] : in_chorus ? chorus[bar % 4] : verse[bar % 4];
    for (const int note : chord) {
      add_tone(song.samples, midi(note), 0.08, bar_start(bar), bar_start(bar + 1),
               in_chorus ? 1.0 : 0.2);
    }
    for (int beat = 0; beat < kBeatsPerBar; ++beat) {
      const double at = bar_start(bar) + beat * kBeat;
      // The kick, harder on the first beat of the bar.
      const double weight = beat == 0 ? 1.0 : 0.6;
      const auto from = static_cast<std::size_t>(at * kRate);
      for (std::size_t i = 0; i < static_cast<std::size_t>(0.2 * kRate); ++i) {
        const double t = static_cast<double>(i) / kRate;
        const double v = weight * 0.6 * std::exp(-t / 0.06) *
                          std::sin(2.0 * std::numbers::pi * (55.0 + 60.0 * std::exp(-t / 0.02)) * t);
        if (from + i < song.samples.size()) song.samples[from + i] += static_cast<float>(v);
      }
      // A hi-hat on the off-beat.
      const auto hat = static_cast<std::size_t>((at + kBeat / 2.0) * kRate);
      float last = 0.0f;
      for (std::size_t i = 0; i < static_cast<std::size_t>(0.03 * kRate); ++i) {
        const float n = noise(random);
        const double t = static_cast<double>(i) / kRate;
        if (hat + i < song.samples.size()) {
          song.samples[hat + i] += static_cast<float>(0.15 * std::exp(-t / 0.01) * (n - last));
        }
        last = n;
      }
    }
  }
  return song;
}

const Song& song() {
  static const Song made = make_song();
  return made;
}

const MusicAnalysis& analysis() {
  static const MusicAnalysis heard = analyse_music(song().samples, kRate);
  return heard;
}

/// The bar a time is the start of, or -1 when it is not near one.
[[nodiscard]] int bar_at(double t) {
  const double bars = (t - kPickup) / kBar;
  const double nearest = std::round(bars);
  if (std::abs(bars - nearest) * kBar > 0.03) return -1;
  return static_cast<int>(nearest);
}

TEST(MusicAnalysis, FindsTheTempo) {
  EXPECT_NEAR(analysis().tempo, 120.0, 2.0);
}

TEST(MusicAnalysis, PutsEachBeatOnTheAttackOfTheKick) {
  const MusicAnalysis& heard = analysis();
  ASSERT_GE(heard.beats.size(), static_cast<std::size_t>(kBars * kBeatsPerBar - 4));
  for (const double beat : heard.beats) {
    const double beats = (beat - kPickup) / kBeat;
    EXPECT_NEAR(beat, kPickup + std::round(beats) * kBeat, 0.012) << beat;
  }
}

TEST(MusicAnalysis, FindsWhichBeatTheBarStartsOn) {
  const MusicAnalysis& heard = analysis();
  int on_bar_lines = 0;
  int downbeats = 0;
  for (std::size_t i = 0; i < heard.beats.size(); ++i) {
    if (!heard.is_downbeat(i)) continue;
    ++downbeats;
    if (bar_at(heard.beats[i]) >= 0) ++on_bar_lines;
  }
  EXPECT_GT(downbeats, kBars - 3);
  EXPECT_EQ(on_bar_lines, downbeats);
}

TEST(MusicAnalysis, FindsNoBeatInNoiseOrSilence) {
  std::mt19937 random(3);
  std::normal_distribution<float> noise(0.0f, 0.1f);
  std::vector<float> hiss(static_cast<std::size_t>(20.0 * kRate));
  for (float& v : hiss) v = noise(random);
  EXPECT_FALSE(analyse_music(hiss, kRate).usable());
  EXPECT_FALSE(analyse_music(std::vector<float>(hiss.size(), 0.0f), kRate).usable());
}

[[nodiscard]] double total(const RemixPlan& plan) {
  double sum = 0.0;
  for (const MusicSpan& span : plan.spans) sum += span.to - span.from;
  return sum;
}

/// Every join is just before a bar line on both sides, into the bar that has
/// the same place in the progression and the same section as the one the
/// song would have gone on to: the cut a person would make.
void expect_seamless(const RemixPlan& plan) {
  ASSERT_FALSE(plan.spans.empty());
  EXPECT_DOUBLE_EQ(plan.spans.front().from, 0.0);
  EXPECT_DOUBLE_EQ(plan.spans.back().to, song().duration);
  for (std::size_t i = 1; i < plan.spans.size(); ++i) {
    const int left = bar_at(plan.spans[i - 1].to + kRemixLead);
    const int into = bar_at(plan.spans[i].from + kRemixLead);
    ASSERT_GE(left, 0) << plan.spans[i - 1].to;
    ASSERT_GE(into, 0) << plan.spans[i].from;
    EXPECT_EQ(left % 4, into % 4) << "bar " << left << " into bar " << into;
    EXPECT_EQ(section_of(left), section_of(into)) << "bar " << left << " into bar " << into;
  }
}

// This song changes chord on every bar in a progression of four, so a cut that
// keeps the progression changes its length by eight seconds at a time: the
// nearest the target a seamless remix can come is four seconds off, inside
// what Premiere promises.
TEST(RemixPlan, ShortensTheSongToTheTargetCuttingOnlyBetweenBarsThatMatch) {
  const RemixPlan plan = plan_remix(analysis(), {.target = 40.0});
  EXPECT_NEAR(plan.length, 40.0, 5.0);
  EXPECT_NEAR(total(plan), plan.length, 1e-9);
  EXPECT_GE(plan.spans.size(), 2u);
  expect_seamless(plan);
}

TEST(RemixPlan, LengthensItByPlayingAPassageAgain) {
  const RemixPlan plan = plan_remix(analysis(), {.target = 100.0});
  EXPECT_NEAR(plan.length, 100.0, 5.0);
  expect_seamless(plan);
  bool repeated = false;
  for (std::size_t i = 1; i < plan.spans.size(); ++i) {
    repeated = repeated || plan.spans[i].from < plan.spans[i - 1].to;
  }
  EXPECT_TRUE(repeated);
}

TEST(RemixPlan, ComesWithinASecondOfTheTargetWhenAnyBarCanFollowAnother) {
  const Song loop = make_song(true);
  const MusicAnalysis heard = analyse_music(loop.samples, kRate);
  ASSERT_TRUE(heard.usable());
  for (const double target : {23.0, 41.0, 90.0}) {
    const RemixPlan plan = plan_remix(heard, {.target = target});
    EXPECT_NEAR(plan.length, target, 1.0) << target;
  }
}

TEST(RemixPlan, LeavesTheSongWholeWhenItIsAlreadyTheLengthAsked) {
  const RemixPlan plan = plan_remix(analysis(), {.target = song().duration + 0.1});
  ASSERT_EQ(plan.spans.size(), 1u);
  EXPECT_DOUBLE_EQ(plan.spans.front().from, 0.0);
  EXPECT_DOUBLE_EQ(plan.spans.front().to, song().duration);
}

TEST(RemixPlan, NeverRunsPastTheRoomItHas) {
  const RemixPlan plan = plan_remix(analysis(), {.target = 60.0, .maximum = 50.0});
  EXPECT_LE(plan.length, 50.0 + 1e-6);
  EXPECT_GT(plan.length, 45.0);
  expect_seamless(plan);
}

TEST(RemixPlan, MoreSegmentsMeansMoreAndShorterStretches) {
  const RemixPlan few = plan_remix(analysis(), {.target = 90.0, .segments = 0});
  const RemixPlan many = plan_remix(analysis(), {.target = 90.0, .segments = 100});
  EXPECT_GE(many.spans.size(), few.spans.size());
  EXPECT_LE(std::abs(many.length - 90.0), std::abs(few.length - 90.0) + 0.1);
  expect_seamless(few);
  expect_seamless(many);
}

TEST(RemixPlan, CutsMusicWithNoBeatShortAndOtherwiseLeavesIt) {
  MusicAnalysis drone;
  drone.duration = 30.0;
  const RemixPlan shorter = plan_remix(drone, {.target = 10.0});
  ASSERT_EQ(shorter.spans.size(), 1u);
  EXPECT_DOUBLE_EQ(shorter.spans.front().to, 10.0);
  const RemixPlan longer = plan_remix(drone, {.target = 50.0});
  ASSERT_EQ(longer.spans.size(), 1u);
  EXPECT_DOUBLE_EQ(longer.length, 30.0);
}

}  // namespace
}  // namespace cutline::audio
