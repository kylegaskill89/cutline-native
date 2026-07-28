/// The mixer: a project in, samples out.
///
/// These generate their own source file rather than reaching for the reference
/// footage, so they run anywhere. That costs an AAC round trip, which is lossy
/// — hence assertions on levels over a window rather than on individual
/// samples. The encoder also prepends priming samples, so nothing here assumes
/// a sample lands at an exact index.

#include "cutline/engine/audio_mixer.hpp"

#include "cutline/media/encoder.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <string>
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
    path_ = (std::filesystem::temp_directory_path() / "cutline_mixer_tone.mp4").string();

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

}  // namespace
}  // namespace cutline::engine
