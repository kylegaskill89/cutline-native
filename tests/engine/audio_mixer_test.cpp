/// The mixer: a project in, samples out.
///
/// These generate their own source file rather than reaching for the reference
/// footage, so they run anywhere. That costs an AAC round trip, which is lossy
/// — hence assertions on levels over a window rather than on individual
/// samples. The encoder also prepends priming samples, so nothing here assumes
/// a sample lands at an exact index.

#include "cutline/engine/audio_mixer.hpp"

#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/routing.hpp"
#include "cutline/media/encoder.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <process.h>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::engine {
namespace {

using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr double kToneAmplitude = 0.5;

/// A name in the temp directory that no other process will pick.
///
/// The process id, because ctest runs the suite as many processes at once and
/// each of them writes its own copy of the tone. A fixed name meant two of them
/// encoding over the top of each other, and the test that read the file
/// mid-write saw a mixer with no audio in it — which showed up as five
/// unrelated failures that all passed when run one at a time.
[[nodiscard]] std::string scratch_path(std::string_view name) {
  return (std::filesystem::temp_directory_path() /
          (std::string(name) + "_" + std::to_string(_getpid()) + ".mp4"))
      .string();
}

/// A ten-second tone, written once and shared by every test in this file.
/// Encoding it per test would dominate the runtime for no extra coverage.
class ToneSource {
 public:
  [[nodiscard]] static const std::string& path() {
    static const ToneSource instance;
    return instance.path_;
  }

 private:
  ToneSource() {
    path_ = scratch_path("cutline_mixer_tone");

    media::VideoEncodeSettings video;
    video.width = 64;
    video.height = 64;
    video.fps = 10;
    video.preference = media::EncoderPreference::Software;

    media::AudioEncodeSettings audio;
    audio.enabled = true;
    audio.sample_rate = kRate;
    audio.channels = kChannels;

    auto writer = media::MediaWriter::create(path_, video, audio);
    if (!writer) return;

    std::vector<float> samples(static_cast<std::size_t>(kRate) * kChannels);
    for (std::size_t i = 0; i < static_cast<std::size_t>(kRate); ++i) {
      const double phase = 2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kRate;
      const auto value = static_cast<float>(std::sin(phase) * kToneAmplitude);
      samples[i * kChannels] = value;
      samples[i * kChannels + 1] = value;
    }

    const std::vector<std::uint8_t> frame(64 * 64 * 4, 128);
    for (int second = 0; second < 10; ++second) {
      for (int f = 0; f < 10; ++f) {
        if (!writer.value()->write_frame(frame)) return;
      }
      if (!writer.value()->write_audio(samples)) return;
    }
    ok_ = writer.value()->finish().has_value();
  }

  std::string path_;
  bool ok_ = false;
};

[[nodiscard]] Media tone_media(std::string id = "m") {
  Media m;
  m.id = std::move(id);
  m.path = ToneSource::path();
  m.duration = 10.0;
  m.has_video = true;
  m.audio_stream_count = 1;
  return m;
}

[[nodiscard]] Clip audio_clip(std::string id, std::string media_id, double start,
                              double length, double source_in = 0.0) {
  Clip c;
  c.id = std::move(id);
  c.media_id = std::move(media_id);
  c.kind = TrackKind::Audio;
  c.start = start;
  c.source_in = source_in;
  c.source_out = source_in + length;
  return c;
}

[[nodiscard]] Track audio_track(std::string id, std::vector<Clip> clips) {
  Track t;
  t.id = std::move(id);
  t.kind = TrackKind::Audio;
  t.clips = std::move(clips);
  return t;
}

[[nodiscard]] Project one_clip_project(double length = 5.0) {
  Project p;
  p.media = {tone_media()};
  p.tracks = {audio_track("a1", {audio_clip("c", "m", 0.0, length)})};
  return p;
}

[[nodiscard]] std::unique_ptr<AudioMixer> mixer_for(const Project& p) {
  auto built = AudioMixer::create(p, {.sample_rate = kRate, .channels = kChannels});
  EXPECT_TRUE(built.has_value()) << (built ? "" : built.error());
  return built ? std::move(*built) : nullptr;
}

/// Mixes `seconds` starting at `t` and returns the interleaved result.
[[nodiscard]] std::vector<float> mix_span(AudioMixer& mixer, double t, double seconds) {
  const auto frames = static_cast<std::size_t>(seconds * kRate);
  std::vector<float> out(frames * kChannels);
  EXPECT_TRUE(mixer.mix(t, out).has_value());
  return out;
}

[[nodiscard]] double rms_of(const std::vector<float>& samples) {
  if (samples.empty()) return 0.0;
  double sum = 0.0;
  for (const float sample : samples) sum += static_cast<double>(sample) * sample;
  return std::sqrt(sum / static_cast<double>(samples.size()));
}

/// The same, over one side of the stereo bus only. What a panner is judged by:
/// the two channels stop being the same signal.
[[nodiscard]] double rms_of_channel(const std::vector<float>& samples, std::size_t channel) {
  double sum = 0.0;
  std::size_t counted = 0;
  for (std::size_t i = channel; i < samples.size(); i += kChannels) {
    sum += static_cast<double>(samples[i]) * samples[i];
    ++counted;
  }
  return counted == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(counted));
}

/// A full-scale sine has an RMS of its amplitude over root two.
[[nodiscard]] double expected_rms(double amplitude) {
  return amplitude / std::numbers::sqrt2;
}

// ------------------------------------------------------------------- setup --

TEST(AudioMixer, TheGeneratedSourceIsUsable) {
  // If this fails, every other test in this file is meaningless rather than
  // wrong, so it is worth stating separately.
  ASSERT_FALSE(ToneSource::path().empty());
  EXPECT_TRUE(std::filesystem::exists(ToneSource::path()));
}

TEST(AudioMixer, AProjectWithNoAudioIsSilent) {
  // The caller uses this to decide whether to create an audio stream at all.
  const auto mixer = mixer_for(Project{});
  ASSERT_NE(mixer, nullptr);
  EXPECT_TRUE(mixer->silent());
}

TEST(AudioMixer, ARateOrChannelCountOfZeroIsRejected) {
  EXPECT_FALSE(AudioMixer::create(Project{}, {.sample_rate = 0}).has_value());
  EXPECT_FALSE(AudioMixer::create(Project{}, {.channels = 0}).has_value());
}

// ---------------------------------------------------------- track meters --

/// Below this is silence as far as a meter is concerned. A reading floors at
/// its own bottom rather than at negative infinity, so "quiet" is a threshold
/// rather than an exact number.
constexpr double kSilenceDb = -90.0;

/// Two lanes, the second quieter than the first, so their meters have to
/// disagree for the feature to mean anything.
[[nodiscard]] Project two_lanes() {
  Project p;
  p.media = {tone_media()};
  Track loud = audio_track("a1", {audio_clip("c1", "m", 0.0, 5.0)});
  Track quiet = audio_track("a2", {audio_clip("c2", "m", 0.0, 5.0)});
  quiet.gain = 0.25;
  p.tracks = {std::move(loud), std::move(quiet)};
  return p;
}

/// A video track above an audio one, which is what every real project looks
/// like and what none of the tests around this used to have.
[[nodiscard]] Project video_over_audio() {
  Project p;
  p.media = {tone_media()};
  Track picture;
  picture.id = "v1";
  picture.kind = TrackKind::Video;
  p.tracks = {std::move(picture), audio_track("a1", {audio_clip("c1", "m", 0.0, 5.0)})};
  return p;
}

TEST(TrackMeters, ATrackIsNamedTheWayTheProjectNumbersItRatherThanThePlan) {
  // The fault driving found, and the reason it survived a green suite: the
  // plan numbers audio tracks 0, 1, 2 and skips the video ones, while
  // everything else counts tracks as the project lists them. The two agree
  // exactly when a project is all audio — which every test here was.
  auto mixer = mixer_for(video_over_audio());
  ASSERT_NE(mixer, nullptr);
  (void)mix_span(*mixer, 0.0, 0.5);

  EXPECT_GT(mixer->track_levels(1).channels[0].peak_db, kSilenceDb)
      << "track 1 is the audio lane, as the project lists it";
  EXPECT_LE(mixer->track_levels(0).channels[0].peak_db, kSilenceDb)
      << "track 0 is the video lane and carries no sound";
}

TEST(TrackFader, ItAlsoTakesTheProjectsNumbering) {
  Project p = video_over_audio();
  p.tracks[1].gain = 0.5;
  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);

  EXPECT_DOUBLE_EQ(mixer->track_gain(1), 0.5) << "the audio lane's own built gain";
  EXPECT_DOUBLE_EQ(mixer->track_gain(0), 0.0) << "a video lane has no fader";

  (void)mix_span(*mixer, 0.0, 0.2);
  const double before = mixer->track_levels(1).channels[0].peak_db;
  mixer->set_track_gain(1, 0.125);
  (void)mix_span(*mixer, 0.2, 2.0);
  EXPECT_NEAR(before - mixer->track_levels(1).channels[0].peak_db, 12.0, 2.0);
}

TEST(TrackMeters, ALaneWithNothingOnItReadsAsSilence) {
  // And does not need asking about first, which is what saves every caller a
  // check it would get wrong.
  const auto mixer = mixer_for(Project{});
  ASSERT_NE(mixer, nullptr);
  EXPECT_EQ(mixer->track_count(), 0u);
  EXPECT_LE(mixer->track_levels(0).channels[0].peak_db, kSilenceDb);
  EXPECT_LE(mixer->track_levels(-1).channels[0].peak_db, kSilenceDb);
  EXPECT_LE(mixer->track_levels(9999).channels[0].peak_db, kSilenceDb);
}

TEST(TrackMeters, EachLaneIsMeteredSeparately) {
  auto mixer = mixer_for(two_lanes());
  ASSERT_NE(mixer, nullptr);
  ASSERT_EQ(mixer->track_count(), 2u);
  (void)mix_span(*mixer, 0.0, 0.5);

  const double loud = mixer->track_levels(0).channels[0].peak_db;
  const double quiet = mixer->track_levels(1).channels[0].peak_db;
  EXPECT_GT(loud, kSilenceDb);
  EXPECT_GT(quiet, kSilenceDb);
  // A quarter of the amplitude is about twelve decibels down. The point is that
  // the two meters say different things about the same moment.
  EXPECT_NEAR(loud - quiet, 12.0, 2.0);
}

TEST(TrackMeters, ALaneReadsItsOwnLevelRatherThanTheMix) {
  // The reason a strip's meter is worth having: a mix that is too hot says
  // nothing about which track is making it so.
  auto mixer = mixer_for(two_lanes());
  ASSERT_NE(mixer, nullptr);
  (void)mix_span(*mixer, 0.0, 0.5);

  const double mix = mixer->levels().channels[0].peak_db;
  const double quiet = mixer->track_levels(1).channels[0].peak_db;
  EXPECT_GT(mix, quiet) << "the sum of both lanes is louder than the quieter one";
}

TEST(TrackMeters, TheMasterFaderDoesNotMoveThem) {
  // A track meter answers "what is this track putting out". Strips that all
  // dropped together when the master moved would be several meters showing one
  // thing.
  auto mixer = mixer_for(two_lanes());
  ASSERT_NE(mixer, nullptr);

  (void)mix_span(*mixer, 0.0, 0.5);
  const double before = mixer->track_levels(0).channels[0].peak_db;
  const double mix_before = mixer->levels().channels[0].peak_db;

  mixer->set_master_gain(0.25);
  (void)mix_span(*mixer, 0.5, 0.5);
  const double after = mixer->track_levels(0).channels[0].peak_db;
  const double mix_after = mixer->levels().channels[0].peak_db;

  EXPECT_NEAR(before, after, 1.0) << "the lane is unchanged";
  EXPECT_LT(mix_after, mix_before - 6.0) << "the mix followed the fader";
}

TEST(TrackMeters, ALaneWithSeveralClipsAtOnceMetersTheirSum) {
  // A track's level is not any one clip's. Two clips overlapping on one lane
  // are louder together than either is alone.
  Project one;
  one.media = {tone_media()};
  one.tracks = {audio_track("a1", {audio_clip("c1", "m", 0.0, 5.0)})};

  Project both;
  both.media = {tone_media()};
  both.tracks = {audio_track("a1", {audio_clip("c1", "m", 0.0, 5.0),
                                    audio_clip("c2", "m", 0.0, 5.0, 1.0)})};

  auto alone = mixer_for(one);
  auto together = mixer_for(both);
  ASSERT_NE(alone, nullptr);
  ASSERT_NE(together, nullptr);
  (void)mix_span(*alone, 0.0, 0.5);
  (void)mix_span(*together, 0.0, 0.5);

  EXPECT_GT(together->track_levels(0).channels[0].peak_db,
            alone->track_levels(0).channels[0].peak_db);
}

TEST(TrackMeters, AMutedLaneReadsAsSilence) {
  Project p = two_lanes();
  p.tracks[0].muted = true;
  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  (void)mix_span(*mixer, 0.0, 0.5);

  EXPECT_LE(mixer->track_levels(0).channels[0].peak_db, kSilenceDb);
  EXPECT_GT(mixer->track_levels(1).channels[0].peak_db, kSilenceDb);
}

// ------------------------------------------------------ live track faders --

TEST(TrackFader, AMixNobodyHasTouchedIsUnchanged) {
  // The property that makes this safe to have: the live fader starts at the
  // gain the track was built with, so a mixer nobody has touched applies
  // exactly what the project says and an export is what it always was.
  auto plain = mixer_for(two_lanes());
  auto same = mixer_for(two_lanes());
  ASSERT_NE(plain, nullptr);
  ASSERT_NE(same, nullptr);

  same->set_track_gain(0, same->track_gain(0));
  EXPECT_EQ(mix_span(*plain, 0.0, 0.2), mix_span(*same, 0.0, 0.2));
}

TEST(TrackFader, ItStartsAtWhatTheMixWasBuiltWith) {
  auto mixer = mixer_for(two_lanes());
  ASSERT_NE(mixer, nullptr);
  EXPECT_DOUBLE_EQ(mixer->track_gain(0), 1.0);
  EXPECT_DOUBLE_EQ(mixer->track_gain(1), 0.25);
}

TEST(TrackFader, MovingItIsHeardOnTheNextBlock) {
  auto mixer = mixer_for(two_lanes());
  ASSERT_NE(mixer, nullptr);

  const double before = rms_of(mix_span(*mixer, 0.0, 0.2));
  mixer->set_track_gain(0, 0.5);
  const double after = rms_of(mix_span(*mixer, 0.2, 0.2));
  EXPECT_LT(after, before);
}

TEST(TrackFader, ItMovesOnlyTheTrackItNames) {
  auto mixer = mixer_for(two_lanes());
  ASSERT_NE(mixer, nullptr);
  (void)mix_span(*mixer, 0.0, 0.2);
  const double other_before = mixer->track_levels(1).channels[0].peak_db;

  // Two seconds after the move, not two hundred milliseconds. A peak meter
  // falls at about twenty decibels a second by design — that is what makes a
  // transient readable — so a short block after a large cut is still showing
  // the peak on its way down rather than the level it is heading for. Getting
  // this wrong reads as the fader not working.
  mixer->set_track_gain(0, 0.1);
  (void)mix_span(*mixer, 0.2, 2.0);

  EXPECT_LT(mixer->track_levels(0).channels[0].peak_db, -20.0);
  EXPECT_NEAR(mixer->track_levels(1).channels[0].peak_db, other_before, 1.0);
}

TEST(TrackFader, TheTracksOwnMeterFollowsIt) {
  // The fader and the meter beside it have to agree, or the strip is showing a
  // level from before the hand moved.
  auto mixer = mixer_for(two_lanes());
  ASSERT_NE(mixer, nullptr);
  (void)mix_span(*mixer, 0.0, 0.2);
  const double before = mixer->track_levels(0).channels[0].peak_db;

  mixer->set_track_gain(0, 0.25);
  (void)mix_span(*mixer, 0.2, 2.0);
  const double after = mixer->track_levels(0).channels[0].peak_db;

  EXPECT_NEAR(before - after, 12.0, 2.0);
}

TEST(TrackFader, ALaneTheMixerHasNoneOfIsIgnored) {
  auto mixer = mixer_for(two_lanes());
  ASSERT_NE(mixer, nullptr);
  mixer->set_track_gain(-1, 0.5);
  mixer->set_track_gain(9999, 0.5);
  EXPECT_DOUBLE_EQ(mixer->track_gain(0), 1.0);
}

TEST(TrackFader, ALaneBuiltSilentStaysWhereTheMixPlannedIt) {
  // Zero has no ratio that brings it back, so moving the fader off it is a
  // rebuild rather than a trim. What matters is that it does not divide by
  // nothing and hand the mixing thread an infinity.
  Project p = two_lanes();
  p.tracks[0].gain = 0.0;
  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);

  mixer->set_track_gain(0, 1.0);
  const std::vector<float> out = mix_span(*mixer, 0.0, 0.2);
  for (const float sample : out) EXPECT_TRUE(std::isfinite(sample));
}

// ------------------------------------------------------- track automation --

TEST(TrackAutomation, AnUnautomatedTrackStillUsesItsConstant) {
  // The guard on the whole change: a project whose faders were set and left is
  // every project written before this existed.
  Project p = two_lanes();
  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  (void)mix_span(*mixer, 0.0, 0.5);
  EXPECT_NEAR(mixer->track_levels(0).channels[0].peak_db -
                  mixer->track_levels(1).channels[0].peak_db,
              12.0, 2.0);
}

TEST(TrackAutomation, TheFaderFollowsItsCurveDownTheSequence) {
  // A ramp from unity at the start to a quarter four seconds in. The curve is
  // in *timeline* seconds, unlike every other keyframe list in the model.
  Project p;
  p.media = {tone_media()};
  Track lane = audio_track("a1", {audio_clip("c1", "m", 0.0, 8.0)});
  lane.gain_keyframes = {core::Keyframe{.t = 0.0, .v = 1.0},
                         core::Keyframe{.t = 4.0, .v = 0.25}};
  p.tracks = {std::move(lane)};

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  const double head = rms_of(mix_span(*mixer, 0.0, 0.3));
  mixer->reset();
  const double tail = rms_of(mix_span(*mixer, 3.7, 0.3));

  EXPECT_GT(head, tail * 2.0) << "the ramp took it down";
}

TEST(TrackAutomation, ACurveBeatsTheConstantBesideIt) {
  Project p;
  p.media = {tone_media()};
  Track lane = audio_track("a1", {audio_clip("c1", "m", 0.0, 5.0)});
  lane.gain = 0.01;  // ignored: the curve is what is read
  lane.gain_keyframes = {core::Keyframe{.t = 0.0, .v = 1.0}};
  p.tracks = {std::move(lane)};

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  (void)mix_span(*mixer, 0.0, 0.3);
  EXPECT_GT(mixer->track_levels(0).channels[0].peak_db, -20.0);
}

TEST(TrackAutomation, ThePannerFollowsItsCurveToo) {
  Project p;
  p.media = {tone_media()};
  Track lane = audio_track("a1", {audio_clip("c1", "m", 0.0, 8.0)});
  lane.pan_keyframes = {core::Keyframe{.t = 0.0, .v = -1.0},
                        core::Keyframe{.t = 4.0, .v = 1.0}};
  p.tracks = {std::move(lane)};

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  const std::vector<float> head = mix_span(*mixer, 0.0, 0.3);
  mixer->reset();
  const std::vector<float> tail = mix_span(*mixer, 3.7, 0.3);

  EXPECT_GT(rms_of_channel(head, 0), rms_of_channel(head, 1)) << "hard left at the start";
  EXPECT_GT(rms_of_channel(tail, 1), rms_of_channel(tail, 0)) << "hard right by the end";
}

TEST(TrackAutomation, TheCurveIsReadAgainstTheSequenceRatherThanTheClip) {
  // The trap this is most likely to be broken by. A clip starting four seconds
  // in would read the curve at zero if the clock were clip-local, and the ramp
  // would be at the wrong place entirely.
  Project p;
  p.media = {tone_media()};
  Track lane = audio_track("a1", {audio_clip("c1", "m", 4.0, 4.0)});
  lane.gain_keyframes = {core::Keyframe{.t = 0.0, .v = 1.0},
                         core::Keyframe{.t = 4.0, .v = 0.05}};
  p.tracks = {std::move(lane)};

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  // The clip begins at timeline 4, where the curve has already fallen to 0.05.
  // Read clip-locally it would begin at unity instead.
  (void)mix_span(*mixer, 4.0, 0.3);
  EXPECT_LT(mixer->track_levels(0).channels[0].peak_db, -20.0);
}

// ----------------------------------------------------- channel mapping --

/// A source whose two channels differ, which is the only way a channel map can
/// be seen at all: the shared tone is the same on both sides.
[[nodiscard]] Project lopsided() {
  Project p = one_clip_project();
  // Hard left through the clip's panner, so the two channels of the *mix*
  // differ and a map that swaps them shows up.
  p.tracks[0].clips[0].pan = -1.0;
  return p;
}

TEST(ChannelMap, AnEmptyMapIsWhatItAlwaysWas) {
  // Every project written before this has one, so it must be sample for sample
  // what it was.
  Project mapped = one_clip_project();
  mapped.tracks[0].clips[0].channel_map = {};
  EXPECT_EQ(mix_span(*mixer_for(mapped), 0.0, 0.3),
            mix_span(*mixer_for(one_clip_project()), 0.0, 0.3));
}

TEST(ChannelMap, SilencingAChannelSilencesIt) {
  Project p = one_clip_project();
  p.tracks[0].clips[0].channel_map = {0, -1};
  const std::vector<float> out = mix_span(*mixer_for(p), 0.0, 0.3);

  EXPECT_GT(rms_of_channel(out, 0), 0.01);
  EXPECT_NEAR(rms_of_channel(out, 1), 0.0, 1e-6);
}

TEST(ChannelMap, OneChannelCanFeedBoth) {
  // The case this exists for: a lapel mic on channel one, wanted in both ears.
  Project p = one_clip_project();
  p.tracks[0].clips[0].channel_map = {0, 0};
  const std::vector<float> out = mix_span(*mixer_for(p), 0.0, 0.3);

  EXPECT_GT(rms_of_channel(out, 0), 0.01);
  EXPECT_NEAR(rms_of_channel(out, 0), rms_of_channel(out, 1), 1e-4);
}

TEST(ChannelMap, AChannelTheSourceDoesNotHaveIsSilenceRatherThanTheNearest) {
  // Somebody asking for channel four of a stereo file has said something about
  // the shoot that turned out not to be true, and quietly handing them channel
  // two would hide it.
  Project p = one_clip_project();
  p.tracks[0].clips[0].channel_map = {7, 7};
  const std::vector<float> out = mix_span(*mixer_for(p), 0.0, 0.3);
  EXPECT_NEAR(rms_of(out), 0.0, 1e-6);
}

TEST(ChannelMap, ItIsSetAcrossTheLinkedGroupAndOnlyOnTheAudio) {
  // An A/V pair is one take: a lapel that is on channel one is on channel one
  // for all of it. A picture has no channels and should not gain a field that
  // reads as something somebody set.
  Project p;
  p.media = {tone_media()};
  Clip picture = audio_clip("v-clip", "m", 0.0, 5.0);
  picture.kind = TrackKind::Video;
  picture.group_id = "g";
  Clip sound = audio_clip("a-clip", "m", 0.0, 5.0);
  sound.group_id = "g";

  Track vt{.id = "v1", .kind = TrackKind::Video};
  vt.clips = {std::move(picture)};
  Track at{.id = "a1", .kind = TrackKind::Audio};
  at.clips = {std::move(sound)};
  p.tracks = {std::move(vt), std::move(at)};

  p = core::set_clip_channel_map(std::move(p), "v-clip", {1, 1});

  EXPECT_TRUE(core::find_clip(p, "v-clip")->channel_map.empty());
  EXPECT_EQ(core::find_clip(p, "a-clip")->channel_map, (std::vector<int>{1, 1}));
}

// ------------------------------------------------------- track effects --

TEST(TrackEffects, ALaneWithNoStackIsUnchanged) {
  // The guard on restructuring the mix loop: lanes now sum into their own
  // block and are added to the mix after their chain, where they used to add
  // straight in. A project with no track effects must be sample-for-sample
  // what it was.
  auto plain = mixer_for(two_lanes());
  auto same = mixer_for(two_lanes());
  ASSERT_NE(plain, nullptr);
  ASSERT_NE(same, nullptr);
  EXPECT_EQ(mix_span(*plain, 0.0, 0.3), mix_span(*same, 0.0, 0.3));
}

TEST(TrackEffects, AStackOnATrackIsHeard) {
  Project quiet = one_clip_project();
  quiet.tracks[0].audio_effects.push_back(
      core::AudioClipEffect{.type = "gain", .params = {{"gain", -20.0}}});

  const double plain = rms_of(mix_span(*mixer_for(one_clip_project()), 0.0, 0.3));
  const double cut = rms_of(mix_span(*mixer_for(quiet), 0.0, 0.3));
  EXPECT_LT(cut, plain * 0.5);
}

TEST(TrackEffects, ItRunsOnWhatTheWholeLaneSumsTo) {
  // The whole point of a track stack, and what a clip stack cannot do: one
  // compressor across everything on the lane rather than one per clip. Two
  // clips at once through a -20 dB track gain come out quieter than the same
  // two clips with nothing on the track.
  Project both;
  both.media = {tone_media()};
  both.tracks = {audio_track("a1", {audio_clip("c1", "m", 0.0, 5.0),
                                    audio_clip("c2", "m", 0.0, 5.0, 1.0)})};
  Project cut = both;
  cut.tracks[0].audio_effects.push_back(
      core::AudioClipEffect{.type = "gain", .params = {{"gain", -20.0}}});

  EXPECT_LT(rms_of(mix_span(*mixer_for(cut), 0.0, 0.3)),
            rms_of(mix_span(*mixer_for(both), 0.0, 0.3)) * 0.5);
}

TEST(TrackEffects, ADisabledEntryContributesNothing) {
  Project off = one_clip_project();
  off.tracks[0].audio_effects.push_back(core::AudioClipEffect{
      .type = "gain", .enabled = false, .params = {{"gain", -20.0}}});

  EXPECT_EQ(mix_span(*mixer_for(off), 0.0, 0.3),
            mix_span(*mixer_for(one_clip_project()), 0.0, 0.3));
}

TEST(TrackEffects, OneLanesStackDoesNotReachAnother) {
  Project p = two_lanes();
  p.tracks[0].audio_effects.push_back(
      core::AudioClipEffect{.type = "gain", .params = {{"gain", -24.0}}});

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  (void)mix_span(*mixer, 0.0, 0.3);

  // The stacked lane is well below the untouched one, which is the whole
  // claim. Stated as a difference rather than as two absolute levels, so it
  // does not depend on where a peak meter happens to have fallen to.
  EXPECT_LT(mixer->track_levels(0).channels[0].peak_db,
            mixer->track_levels(1).channels[0].peak_db - 10.0);
}

TEST(TrackEffects, TheMeterReadsTheLaneAfterItsStack) {
  // A strip's meter answers "what is this track putting into the mix", and
  // after a track stack that is not what the clips summed to.
  Project p = one_clip_project();
  Project cut = p;
  cut.tracks[0].audio_effects.push_back(
      core::AudioClipEffect{.type = "gain", .params = {{"gain", -20.0}}});

  auto plain = mixer_for(p);
  auto quiet = mixer_for(cut);
  ASSERT_NE(plain, nullptr);
  ASSERT_NE(quiet, nullptr);
  (void)mix_span(*plain, 0.0, 0.3);
  (void)mix_span(*quiet, 0.0, 0.3);

  EXPECT_NEAR(plain->track_levels(0).channels[0].peak_db -
                  quiet->track_levels(0).channels[0].peak_db,
              20.0, 3.0);
}

TEST(TrackEffects, SplittingABlockGivesTheSameSamples) {
  // The guarantee the whole mixer keeps, now that a second chain runs on the
  // same grid: mixing a span in one call and in three must agree.
  Project p = one_clip_project();
  p.tracks[0].audio_effects.push_back(
      core::AudioClipEffect{.type = "lowpass", .params = {{"frequency", 800.0}}});

  auto whole = mixer_for(p);
  auto split = mixer_for(p);
  ASSERT_NE(whole, nullptr);
  ASSERT_NE(split, nullptr);

  const std::vector<float> once = mix_span(*whole, 0.0, 0.3);
  std::vector<float> thrice;
  for (int i = 0; i < 3; ++i) {
    const std::vector<float> part = mix_span(*split, 0.1 * i, 0.1);
    thrice.insert(thrice.end(), part.begin(), part.end());
  }
  ASSERT_EQ(once.size(), thrice.size());
  for (std::size_t i = 0; i < once.size(); ++i) EXPECT_FLOAT_EQ(once[i], thrice[i]) << i;
}

// ----------------------------------------------------------------- content --

TEST(AudioMixer, AClipIsHeardAtItsOwnLevel) {
  auto mixer = mixer_for(one_clip_project());
  ASSERT_NE(mixer, nullptr);
  ASSERT_FALSE(mixer->silent());

  // Skipped past the start so the limiter's look-ahead is behind us.
  const auto samples = mix_span(*mixer, 0.0, 2.0);
  std::vector<float> settled(samples.begin() + kRate, samples.end());
  EXPECT_NEAR(rms_of(settled), expected_rms(kToneAmplitude), 0.03);
}

TEST(AudioMixer, GainScalesTheClip) {
  Project p = one_clip_project();
  p.tracks[0].clips[0].gain = 0.5;

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  const auto samples = mix_span(*mixer, 0.0, 2.0);
  std::vector<float> settled(samples.begin() + kRate, samples.end());
  EXPECT_NEAR(rms_of(settled), expected_rms(kToneAmplitude * 0.5), 0.03);
}

TEST(AudioMixer, PanningHardLeftEmptiesTheRightAndLeavesTheLeftAlone) {
  Project p = one_clip_project();
  p.tracks[0].clips[0].pan = -1.0;

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  const auto samples = mix_span(*mixer, 0.0, 2.0);
  std::vector<float> settled(samples.begin() + kRate * kChannels, samples.end());

  // Balance rather than constant power: the side that stays is at exactly the
  // level it had, not three decibels above it.
  EXPECT_NEAR(rms_of_channel(settled, 0), expected_rms(kToneAmplitude), 0.03);
  EXPECT_LT(rms_of_channel(settled, 1), 0.001);
}

TEST(AudioMixer, PanningHalfRightTrimsTheLeftByHalf) {
  Project p = one_clip_project();
  p.tracks[0].clips[0].pan = 0.5;

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  const auto samples = mix_span(*mixer, 0.0, 2.0);
  std::vector<float> settled(samples.begin() + kRate * kChannels, samples.end());

  EXPECT_NEAR(rms_of_channel(settled, 0), expected_rms(kToneAmplitude * 0.5), 0.03);
  EXPECT_NEAR(rms_of_channel(settled, 1), expected_rms(kToneAmplitude), 0.03);
}

TEST(AudioMixer, ACentredClipIsUntouchedByThePanner) {
  // The property that matters most: every project made before there was a
  // panner has to sound exactly as it did.
  auto plain = mixer_for(one_clip_project());
  ASSERT_NE(plain, nullptr);
  const auto before = mix_span(*plain, 0.0, 2.0);

  Project p = one_clip_project();
  p.tracks[0].clips[0].pan = 0.0;
  auto panned = mixer_for(p);
  ASSERT_NE(panned, nullptr);
  const auto after = mix_span(*panned, 0.0, 2.0);

  ASSERT_EQ(before.size(), after.size());
  for (std::size_t i = 0; i < before.size(); ++i) ASSERT_FLOAT_EQ(before[i], after[i]) << i;
}

TEST(AudioMixer, NothingIsHeardBeforeOrAfterAClip) {
  Project p;
  p.media = {tone_media()};
  p.tracks = {audio_track("a1", {audio_clip("c", "m", 2.0, 2.0)})};

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);

  EXPECT_LT(rms_of(mix_span(*mixer, 0.0, 1.5)), 0.001);
  EXPECT_GT(rms_of(mix_span(*mixer, 2.5, 1.0)), 0.1);
  mixer->reset();
  EXPECT_LT(rms_of(mix_span(*mixer, 5.0, 1.0)), 0.001);
}

TEST(AudioMixer, AFadeInRampsUpFromSilence) {
  Project p = one_clip_project();
  p.tracks[0].clips[0].fade_in = 2.0;

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  const auto samples = mix_span(*mixer, 0.0, 3.0);

  const std::vector<float> first(samples.begin(), samples.begin() + kRate / 4 * kChannels);
  const std::vector<float> last(samples.end() - kRate * kChannels, samples.end());
  EXPECT_LT(rms_of(first), 0.05);
  EXPECT_NEAR(rms_of(last), expected_rms(kToneAmplitude), 0.05);
}

TEST(AudioMixer, TracksSumRatherThanAveraging) {
  // `normalize=0`: two tracks at unity really are twice as loud. Averaging
  // instead would make adding a track quieten everything already there.
  Project p;
  p.media = {tone_media()};
  p.tracks = {audio_track("a1", {audio_clip("c1", "m", 0.0, 5.0)}),
              audio_track("a2", {audio_clip("c2", "m", 0.0, 5.0)})};

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  const auto samples = mix_span(*mixer, 0.0, 2.0);
  std::vector<float> settled(samples.begin() + kRate, samples.end());

  // Two identical copies sum to double, which is over the limiter's ceiling, so
  // what comes out is the ceiling rather than 2x. That it is well above one
  // copy is the point.
  EXPECT_GT(rms_of(settled), expected_rms(kToneAmplitude) * 1.3);
}

TEST(AudioMixer, AMutedTrackContributesNothing) {
  Project p = one_clip_project();
  p.tracks[0].muted = true;

  const auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  EXPECT_TRUE(mixer->silent());
}

TEST(AudioMixer, AnEffectStackChangesTheSound) {
  // A high-pass well above the tone should remove most of it, which is the
  // check that the chain is actually reached rather than built and ignored.
  Project p = one_clip_project();
  p.tracks[0].clips[0].audio_effects = {
      core::AudioClipEffect{.type = "highpass", .params = {{"freq", 2000.0}}}};

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  const auto samples = mix_span(*mixer, 0.0, 2.0);
  std::vector<float> settled(samples.begin() + kRate, samples.end());
  EXPECT_LT(rms_of(settled), expected_rms(kToneAmplitude) * 0.3);
}

// ---------------------------------------------------------------- retiming --

/// Energy at one frequency, by the Goertzel algorithm — enough to tell whether
/// a retime moved the pitch, without a full spectrum.
[[nodiscard]] double energy_at(const std::vector<float>& interleaved, double freq) {
  const std::size_t frames = interleaved.size() / kChannels;
  if (frames == 0) return 0.0;

  const double omega = 2.0 * std::numbers::pi * freq / kRate;
  const double coefficient = 2.0 * std::cos(omega);

  double s1 = 0.0;
  double s2 = 0.0;
  for (std::size_t i = 0; i < frames; ++i) {
    const double s0 = static_cast<double>(interleaved[i * kChannels]) + coefficient * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(s1 * s1 + s2 * s2 - coefficient * s1 * s2) / static_cast<double>(frames);
}

TEST(AudioMixer, ARetimedClipKeepsItsPitch) {
  // Resampling would put a double-speed clip's 440 Hz tone at 880 Hz, which is
  // a tape-speed effect rather than a speed change.
  Project p;
  p.media = {tone_media()};
  p.tracks = {audio_track("a1", {audio_clip("c", "m", 0.0, 8.0)})};
  p.tracks[0].clips[0].speed = 2.0;

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  const auto samples = mix_span(*mixer, 1.0, 2.0);

  EXPECT_GT(energy_at(samples, 440.0), energy_at(samples, 880.0) * 5.0);
  EXPECT_GT(rms_of(samples), 0.1);
}

TEST(AudioMixer, ARetimedClipOccupiesItsRetimedSpan) {
  // Eight seconds of source at double speed is four seconds of timeline, and
  // there should be sound throughout and silence after.
  Project p;
  p.media = {tone_media()};
  p.tracks = {audio_track("a1", {audio_clip("c", "m", 0.0, 8.0)})};
  p.tracks[0].clips[0].speed = 2.0;

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  EXPECT_GT(rms_of(mix_span(*mixer, 3.0, 0.8)), 0.1);
  mixer->reset();
  EXPECT_LT(rms_of(mix_span(*mixer, 4.5, 0.5)), 0.001);
}

TEST(AudioMixer, ASlowedClipAlsoKeepsItsPitch) {
  Project p;
  p.media = {tone_media()};
  p.tracks = {audio_track("a1", {audio_clip("c", "m", 0.0, 2.0)})};
  p.tracks[0].clips[0].speed = 0.5;

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  const auto samples = mix_span(*mixer, 1.0, 2.0);

  EXPECT_GT(energy_at(samples, 440.0), energy_at(samples, 220.0) * 5.0);
  EXPECT_GT(rms_of(samples), 0.1);
}

TEST(AudioMixer, AReversedClipStillPlays) {
  Project p;
  p.media = {tone_media()};
  p.tracks = {audio_track("a1", {audio_clip("c", "m", 0.0, 5.0)})};
  p.tracks[0].clips[0].reverse = true;

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  const auto samples = mix_span(*mixer, 1.0, 2.0);
  EXPECT_NEAR(rms_of(samples), expected_rms(kToneAmplitude), 0.05);
}

// ----------------------------------------------------------------- plumbing --

TEST(AudioMixer, MixingInBlocksMatchesMixingItWhole) {
  // The exporter chooses its block size from the frame rate; the sound must not
  // depend on that choice.
  auto whole_mixer = mixer_for(one_clip_project());
  auto split_mixer = mixer_for(one_clip_project());
  ASSERT_NE(whole_mixer, nullptr);
  ASSERT_NE(split_mixer, nullptr);

  const auto whole = mix_span(*whole_mixer, 0.0, 1.0);

  std::vector<float> split;
  double at = 0.0;
  for (const std::size_t frames : {801u, 12000u, 5u, 35194u}) {
    std::vector<float> block(frames * kChannels);
    ASSERT_TRUE(split_mixer->mix(at, block).has_value());
    split.insert(split.end(), block.begin(), block.end());
    at += static_cast<double>(frames) / kRate;
  }

  ASSERT_EQ(split.size(), whole.size());
  for (std::size_t i = 0; i < whole.size(); ++i) EXPECT_FLOAT_EQ(split[i], whole[i]) << "at " << i;
}

TEST(AudioMixer, AnAnimatedEffectSweepsOverTheClip) {
  // A gain rising 24 dB across four seconds. Measured near each end, because
  // what is being asserted is that the automation is read at all — the exact
  // curve is `eval_keyframes`, which has its own tests.
  Project p = one_clip_project();
  core::AudioClipEffect sweep;
  sweep.type = "gain";
  sweep.keyframes["gain"] = {{.t = 0.0, .v = -24.0}, {.t = 4.0, .v = 0.0}};
  p.tracks[0].clips[0].audio_effects = {sweep};

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  const double quiet = rms_of(mix_span(*mixer, 0.2, 0.3));
  mixer->reset();
  const double loud = rms_of(mix_span(*mixer, 3.6, 0.3));

  EXPECT_GT(loud, quiet * 4.0) << "quiet " << quiet << ", loud " << loud;
}

TEST(AudioMixer, AnAnimatedEffectSoundsTheSameWhateverTheBlockSize) {
  // The guarantee this mixer already made, extended to automation. The preview
  // and the exporter ask for different buffer sizes, and a chain retuned once
  // per call would follow a different curve in each — the export would not be
  // what was heard. The retune grid is aligned to the timeline for this reason,
  // and this is the test that says so.
  const auto project_with_sweep = [] {
    Project p = one_clip_project();
    core::AudioClipEffect sweep;
    sweep.type = "lowpass";
    sweep.keyframes["freq"] = {{.t = 0.0, .v = 800.0}, {.t = 4.0, .v = 12000.0}};
    p.tracks[0].clips[0].audio_effects = {sweep};
    return p;
  };

  auto whole_mixer = mixer_for(project_with_sweep());
  auto split_mixer = mixer_for(project_with_sweep());
  ASSERT_NE(whole_mixer, nullptr);
  ASSERT_NE(split_mixer, nullptr);

  const auto whole = mix_span(*whole_mixer, 0.0, 1.0);

  std::vector<float> split;
  double at = 0.0;
  for (const std::size_t frames : {801u, 12000u, 5u, 35194u}) {
    std::vector<float> block(frames * kChannels);
    ASSERT_TRUE(split_mixer->mix(at, block).has_value());
    split.insert(split.end(), block.begin(), block.end());
    at += static_cast<double>(frames) / kRate;
  }

  ASSERT_EQ(split.size(), whole.size());
  for (std::size_t i = 0; i < whole.size(); ++i) EXPECT_FLOAT_EQ(split[i], whole[i]) << "at " << i;
}

TEST(AudioMixer, TheOutputBlockMustBeWholeFrames) {
  auto mixer = mixer_for(one_clip_project());
  ASSERT_NE(mixer, nullptr);
  std::vector<float> odd(2 * kChannels + 1);
  EXPECT_FALSE(mixer->mix(0.0, odd).has_value());
}

TEST(AudioMixer, MixingOverwritesWhateverTheBufferHeld) {
  // The caller reuses its scratch, so a mixer that added to it would accumulate
  // the previous block on every call.
  auto mixer = mixer_for(Project{});
  ASSERT_NE(mixer, nullptr);
  std::vector<float> block(128 * kChannels, 7.0f);
  ASSERT_TRUE(mixer->mix(0.0, block).has_value());
  for (const float sample : block) EXPECT_FLOAT_EQ(sample, 0.0f);
}

TEST(AudioMixer, FlushDrainsTheLimiter) {
  auto mixer = mixer_for(one_clip_project());
  ASSERT_NE(mixer, nullptr);
  ASSERT_GT(mixer->latency_frames(), 0u);

  const auto samples = mix_span(*mixer, 1.0, 1.0);
  ASSERT_GT(rms_of(samples), 0.1);

  std::vector<float> tail(mixer->latency_frames() * kChannels);
  mixer->flush(tail);
  // The tail is real audio, not the silence a missing flush would leave.
  EXPECT_GT(rms_of(tail), 0.1);
}

TEST(AudioMixer, AMissingSourceIsRecordedRatherThanFatal) {
  // An export should complete with a hole rather than fail at the last step.
  Media absent;
  absent.id = "gone";
  absent.path = "d:/no/such/file.mp4";
  absent.duration = 10.0;
  absent.audio_stream_count = 1;

  Project p;
  p.media = {absent};
  p.tracks = {audio_track("a1", {audio_clip("c", "gone", 0.0, 5.0)})};

  const auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  ASSERT_EQ(mixer->missing_media().size(), 1u);
  EXPECT_EQ(mixer->missing_media()[0], "gone");
  EXPECT_TRUE(mixer->silent());
}

// ------------------------------------------------- multi-stream footage --
//
// The reference capture carries four separate audio streams — desktop, mic and
// two application sources — and the spec flags getting the wrong one as a past
// bug. A clip stores the stream *ordinal*, since that is what survives a file
// being remuxed, and the media layer maps it onto libav's absolute index; if
// that mapping slips, a project silently plays the wrong audio.

constexpr const char* kMultiStreamClip = "Replay 07-23-2026 10PM-59-02.mkv";

[[nodiscard]] std::string multi_stream_path() {
  const char* dir = std::getenv("CUTLINE_TEST_MEDIA_DIR");
  if (dir == nullptr) return {};
  const std::filesystem::path candidate = std::filesystem::path(dir) / kMultiStreamClip;
  std::error_code ec;
  return std::filesystem::exists(candidate, ec) ? candidate.string() : std::string{};
}

TEST(AudioMixerFootage, EachAudioStreamOrdinalSelectsDifferentAudio) {
  const std::string path = multi_stream_path();
  if (path.empty()) GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to the reference footage";

  const auto mix_of_stream = [&path](int ordinal) {
    Media m;
    m.id = "m";
    m.path = path;
    m.duration = 598.0;
    m.has_video = true;
    m.audio_stream_count = 4;

    Project p;
    p.media = {m};
    p.tracks = {audio_track("a1", {audio_clip("c", "m", 0.0, 20.0, 30.0)})};
    p.tracks[0].clips[0].audio_stream = ordinal;

    auto mixer = AudioMixer::create(p, {.sample_rate = kRate, .channels = kChannels});
    EXPECT_TRUE(mixer.has_value()) << (mixer ? "" : mixer.error());
    if (!mixer) return std::vector<float>{};

    std::vector<float> out(static_cast<std::size_t>(kRate) * 2 * kChannels);
    EXPECT_TRUE((*mixer)->mix(2.0, out).has_value());
    return out;
  };

  const auto first = mix_of_stream(0);
  const auto second = mix_of_stream(1);
  ASSERT_EQ(first.size(), second.size());
  ASSERT_FALSE(first.empty());

  // Not merely different by a sample or two: two genuinely different captures.
  std::size_t differing = 0;
  for (std::size_t i = 0; i < first.size(); ++i) {
    if (std::abs(first[i] - second[i]) > 1e-4f) ++differing;
  }
  EXPECT_GT(differing, first.size() / 10)
      << "streams 0 and 1 decoded to nearly the same audio, so the ordinal was "
         "probably ignored";
}

TEST(AudioMixerFootage, AnOrdinalPastTheLastStreamIsReportedRatherThanGuessed) {
  // Falling back to stream 0 would be worse than silence: the project would
  // play confidently wrong audio.
  const std::string path = multi_stream_path();
  if (path.empty()) GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to the reference footage";

  Media m;
  m.id = "m";
  m.path = path;
  m.duration = 598.0;
  m.audio_stream_count = 4;

  Project p;
  p.media = {m};
  p.tracks = {audio_track("a1", {audio_clip("c", "m", 0.0, 5.0)})};
  p.tracks[0].clips[0].audio_stream = 9;

  const auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  EXPECT_EQ(mixer->missing_media().size(), 1u);
  EXPECT_TRUE(mixer->silent());
}

TEST(AudioMixer, TheSettingsComeBackAsGiven) {
  const auto mixer = mixer_for(Project{});
  ASSERT_NE(mixer, nullptr);
  EXPECT_EQ(mixer->settings().sample_rate, kRate);
  EXPECT_EQ(mixer->settings().channels, kChannels);
}

// ------------------------------------------------------- a source too long --

/// How loud the long tone is during the ten seconds beginning at `t`.
///
/// Stepping the amplitude every ten seconds is what makes *where* the mixer
/// read observable at all. A tone of one level throughout would sound the same
/// whichever part of the file a window happened to land on, which is precisely
/// the mistake worth catching.
[[nodiscard]] double long_tone_level(double t) {
  const int block = static_cast<int>(t / 10.0);
  return 0.05 * static_cast<double>(block + 1);
}

constexpr double kLongToneSeconds = 140.0;

/// A tone longer than a mixer will hold whole, so the windowing runs.
class LongToneSource {
 public:
  [[nodiscard]] static const std::string& path() {
    static const LongToneSource instance;
    return instance.path_;
  }

 private:
  LongToneSource() {
    path_ = scratch_path("cutline_mixer_long_tone");

    media::VideoEncodeSettings video;
    video.width = 32;
    video.height = 32;
    video.fps = 5;
    video.preference = media::EncoderPreference::Software;

    media::AudioEncodeSettings audio;
    audio.enabled = true;
    audio.sample_rate = kRate;
    audio.channels = kChannels;

    auto writer = media::MediaWriter::create(path_, video, audio);
    if (!writer) {
      path_.clear();
      return;
    }

    const std::vector<std::uint8_t> frame(32 * 32 * 4, 128);
    std::vector<float> second(static_cast<std::size_t>(kRate) * kChannels);
    for (int s = 0; s < static_cast<int>(kLongToneSeconds); ++s) {
      const auto level = static_cast<float>(long_tone_level(s));
      for (std::size_t i = 0; i < static_cast<std::size_t>(kRate); ++i) {
        const double phase = 2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kRate;
        const auto value = static_cast<float>(std::sin(phase)) * level;
        second[i * kChannels] = value;
        second[i * kChannels + 1] = value;
      }
      for (int f = 0; f < 5; ++f) {
        if (!writer.value()->write_frame(frame)) {
          path_.clear();
          return;
        }
      }
      if (!writer.value()->write_audio(second)) {
        path_.clear();
        return;
      }
    }
    if (!writer.value()->finish()) path_.clear();
  }

  std::string path_;
};

/// The loudest sample in the second half of a one-second mix at `at`.
///
/// The second half because the master limiter has look-ahead: the first
/// milliseconds after a reset are its delay line filling, and they are quiet
/// for a reason that has nothing to do with where the audio was read from.
[[nodiscard]] float level_at(AudioMixer& mixer, double at) {
  mixer.reset();
  std::vector<float> out(static_cast<std::size_t>(kRate) * kChannels, 0.0f);
  if (!mixer.mix(at, out)) return -1.0f;

  float peak = 0.0f;
  for (std::size_t i = out.size() / 2; i < out.size(); ++i) peak = std::max(peak, std::abs(out[i]));
  return peak;
}

class WithLongTone : public ::testing::Test {
 protected:
  void SetUp() override {
    if (LongToneSource::path().empty()) GTEST_SKIP() << "could not write a long tone";
    media_.id = "long";
    media_.path = LongToneSource::path();
    media_.duration = kLongToneSeconds;
    media_.has_video = true;
    media_.audio_stream_count = 1;
  }

  [[nodiscard]] Project whole_source_project() const {
    Project p;
    p.media = {media_};
    p.tracks = {audio_track("a1", {audio_clip("c", "long", 0.0, kLongToneSeconds)})};
    return p;
  }

  Media media_;
};

// The claim the whole thing rests on: a source too long to hold is still read
// from the right place. Three positions, three amplitudes, and the last is far
// past anything the first window covered — so it can only be right if the
// window was refilled around where the mix had reached.
TEST_F(WithLongTone, ReadsTheRightPartOfASourceTooLongToHold) {
  auto mixer = AudioMixer::create(whole_source_project(),
                                  {.sample_rate = kRate, .channels = kChannels});
  ASSERT_TRUE(mixer.has_value()) << mixer.error();

  for (const double at : {5.0, 65.0, 135.0}) {
    const float measured = level_at(**mixer, at);
    const auto expected = static_cast<float>(long_tone_level(at));
    ASSERT_GE(measured, 0.0f) << "mixing failed at " << at;
    // Generous: this went through a lossy encoder and back, and what is being
    // asserted is which ten seconds were read rather than the codec's accuracy.
    EXPECT_NEAR(measured, expected, expected * 0.35f)
        << "at " << at << "s the mixer read audio from somewhere else";
  }
}

// Walking forward across a window's edge has to be seamless. The join is where
// a refill happens, and a gap there would be a moment of silence in the middle
// of a clip that nothing else would catch.
TEST_F(WithLongTone, WalkingPastAWindowEdgeStaysContinuous) {
  auto mixer = AudioMixer::create(whole_source_project(),
                                  {.sample_rate = kRate, .channels = kChannels});
  ASSERT_TRUE(mixer.has_value()) << mixer.error();

  // A second at a time from well inside the first window to well past its end,
  // which is 75 seconds in.
  (*mixer)->reset();
  std::vector<float> out(static_cast<std::size_t>(kRate) * kChannels, 0.0f);
  for (int second = 60; second < 100; ++second) {
    ASSERT_TRUE((*mixer)->mix(static_cast<double>(second), out));

    float peak = 0.0f;
    for (const float sample : out) peak = std::max(peak, std::abs(sample));
    const auto expected = static_cast<float>(long_tone_level(second));
    EXPECT_GT(peak, expected * 0.5f) << "second " << second << " came out silent or nearly so";
  }
}

// Nothing about the answer may depend on how the caller split its blocks, which
// is exactly what a window refilled mid-block could break.
TEST_F(WithLongTone, TheBlockSizeDoesNotChangeTheSamples) {
  auto whole = AudioMixer::create(whole_source_project(),
                                  {.sample_rate = kRate, .channels = kChannels});
  auto split = AudioMixer::create(whole_source_project(),
                                  {.sample_rate = kRate, .channels = kChannels});
  ASSERT_TRUE(whole.has_value());
  ASSERT_TRUE(split.has_value());

  // Across the first window's edge, so a refill happens inside the run.
  constexpr double kFrom = 70.0;
  const std::size_t frames = static_cast<std::size_t>(kRate) * 10;

  std::vector<float> one(frames * kChannels, 0.0f);
  ASSERT_TRUE((*whole)->mix(kFrom, one));

  std::vector<float> many(frames * kChannels, 0.0f);
  constexpr std::size_t kChunk = 4096;
  for (std::size_t at = 0; at < frames; at += kChunk) {
    const std::size_t take = std::min(kChunk, frames - at);
    ASSERT_TRUE((*split)->mix(kFrom + static_cast<double>(at) / kRate,
                              std::span(many).subspan(at * kChannels, take * kChannels)));
  }

  for (std::size_t i = 0; i < one.size(); ++i) {
    ASSERT_FLOAT_EQ(many[i], one[i]) << "sample " << i << " depends on the block size";
  }
}


// ------------------------------------------------------- submixes and sends --

/// One lane of tone, poured into a bus rather than straight at the master.
[[nodiscard]] Project bus_project() {
  Project p = one_clip_project();
  p = core::add_submix_track(std::move(p), "Dialogue");
  p = core::set_track_output(std::move(p), "a1", p.tracks.back().id);
  return p;
}

TEST(Submixes, GoingThroughABusSoundsExactlyTheSameAsNotHavingOne) {
  // The property that makes everything below safe to build: a bus that does
  // nothing does nothing. A project gains a submix and its mix is unchanged.
  auto direct = mixer_for(one_clip_project());
  auto through = mixer_for(bus_project());
  ASSERT_NE(direct, nullptr);
  ASSERT_NE(through, nullptr);

  const std::vector<float> a = mix_span(*direct, 1.0, 0.5);
  const std::vector<float> b = mix_span(*through, 1.0, 0.5);
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    ASSERT_FLOAT_EQ(a[i], b[i]) << "sample " << i << " changed by being routed";
  }
}

TEST(Submixes, TheBusFaderRidesEverythingOnIt) {
  Project p = bus_project();
  p.tracks[1].gain = 0.5;

  auto plain = mixer_for(bus_project());
  auto ridden = mixer_for(p);
  ASSERT_NE(plain, nullptr);
  ASSERT_NE(ridden, nullptr);

  const double loud = rms_of(mix_span(*plain, 1.0, 0.5));
  const double quiet = rms_of(mix_span(*ridden, 1.0, 0.5));
  EXPECT_NEAR(quiet, loud * 0.5, loud * 0.05);
}

TEST(Submixes, AStackOnTheBusProcessesTheWholeGroup) {
  // The reason submixes exist: one compressor across the dialogue rather than
  // one per clip. A gain stands in for it, being the effect whose result can be
  // stated exactly.
  Project p = bus_project();
  core::AudioClipEffect trim;
  trim.type = "gain";
  trim.params["gain"] = -12.0;
  p.tracks[1].audio_effects = {trim};

  auto plain = mixer_for(bus_project());
  auto processed = mixer_for(p);
  ASSERT_NE(plain, nullptr);
  ASSERT_NE(processed, nullptr);

  const double loud = rms_of(mix_span(*plain, 1.0, 0.5));
  const double quiet = rms_of(mix_span(*processed, 1.0, 0.5));
  EXPECT_NEAR(quiet, loud * std::pow(10.0, -12.0 / 20.0), loud * 0.05);
}

TEST(Submixes, TheBusMetersWhatWasPouredIntoIt) {
  auto mixer = mixer_for(bus_project());
  ASSERT_NE(mixer, nullptr);
  (void)mix_span(*mixer, 1.0, 0.5);

  EXPECT_GT(mixer->track_levels(1).channels[0].peak_db, kSilenceDb)
      << "a bus has no clips on it, and a meter that only ever read clips would stay dark";
}

TEST(Submixes, MutingTheBusSilencesEverythingThroughIt) {
  Project p = bus_project();
  p.tracks[1].muted = true;

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  EXPECT_LT(rms_of(mix_span(*mixer, 1.0, 0.5)), 1e-4)
      << "the plan drops clips on a muted lane; a bus has none to drop";
}

TEST(Submixes, SoloingTheBusKeepsWhatFeedsItAndDropsWhatDoesNot) {
  Project p = bus_project();
  p.tracks.push_back(audio_track("a2", {audio_clip("c2", "m", 0.0, 5.0)}));
  p.tracks[1].solo = true;

  auto mixer = mixer_for(p);
  ASSERT_NE(mixer, nullptr);
  (void)mix_span(*mixer, 1.0, 0.5);

  EXPECT_GT(mixer->track_levels(0).channels[0].peak_db, kSilenceDb) << "a1 feeds the soloed bus";
  EXPECT_LE(mixer->track_levels(2).channels[0].peak_db, kSilenceDb) << "a2 goes to the master";
}

TEST(Sends, ASendIsACopyRatherThanADiversion) {
  Project p = one_clip_project();
  p = core::add_submix_track(std::move(p), "Reverb");
  // Half rather than unity, which is not fussiness about levels: the tone is at
  // 0.5, so sending a full copy of it puts the sum at full scale and the master
  // limiter — correctly — takes some of it back. The arithmetic being checked
  // here is the routing's, so it is kept below the ceiling.
  p = core::set_send(std::move(p), "a1", p.tracks.back().id, 0.5);

  auto dry = mixer_for(one_clip_project());
  auto sent = mixer_for(p);
  ASSERT_NE(dry, nullptr);
  ASSERT_NE(sent, nullptr);

  const double alone = rms_of(mix_span(*dry, 1.0, 0.5));
  const double both = rms_of(mix_span(*sent, 1.0, 0.5));
  EXPECT_NEAR(both, alone * 1.5, alone * 0.05)
      << "the track still feeds the master, and the bus feeds it half as much again";
}

TEST(Sends, APostFaderSendFollowsTheFaderAndAPreFaderOneDoesNot) {
  const auto bus_level = [](bool pre_fader, double fader) {
    Project p = one_clip_project();
    p = core::add_submix_track(std::move(p), "Reverb");
    p = core::set_send(std::move(p), "a1", p.tracks.back().id, 1.0, pre_fader);
    auto mixer = mixer_for(p);
    EXPECT_NE(mixer, nullptr);
    if (mixer == nullptr) return 0.0;
    mixer->set_track_gain(0, fader);
    (void)mix_span(*mixer, 1.0, 0.5);
    return mixer->track_levels(1).channels[0].peak_db;
  };

  const double post_up = bus_level(false, 1.0);
  const double post_down = bus_level(false, 0.25);
  EXPECT_LT(post_down, post_up - 6.0) << "pulling a track down takes its reverb down with it";

  const double pre_up = bus_level(true, 1.0);
  const double pre_down = bus_level(true, 0.25);
  EXPECT_NEAR(pre_down, pre_up, 0.5) << "a pre-fader send is tapped before the fader";
}

}  // namespace
}  // namespace cutline::engine
