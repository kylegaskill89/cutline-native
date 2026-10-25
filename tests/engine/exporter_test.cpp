/// Exports, checked by reading the file back.
///
/// The export path had no test of its own until the output size became
/// something a person could choose: until then the only thing that could go
/// wrong was the encoder, which has its own tests. A size that the dialog
/// offers and the exporter ignores is exactly the sort of failure that looks
/// fine on screen, so it is checked against the file rather than the settings.
///
/// A colour matte stands in for footage, so these run without any. The ones
/// about audio need a real source — nothing generated makes a sound — and skip
/// unless CUTLINE_TEST_MEDIA_DIR points at the reference footage.

#include "cutline/engine/exporter.hpp"

#include "cutline/core/model.hpp"
#include "cutline/media/decoder.hpp"
#include "cutline/media/encoder.hpp"
#include "cutline/media/packet_reader.hpp"
#include "cutline/media/probe.hpp"

#include <gtest/gtest.h>

#include "cutline/media/av_headers.hpp"

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <process.h>
#include <numbers>
#include <memory>
#include <string>
#include <vector>

namespace cutline::engine {
namespace {

using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

constexpr int kWidth = 128;
constexpr int kHeight = 64;

std::shared_ptr<gpu::Device> shared_device() {
  static std::shared_ptr<gpu::Device> device = [] {
    auto created = gpu::Device::create({.allow_software = true});
    return created ? *created : nullptr;
  }();
  return device;
}

/// A scratch path that cleans itself up.
///
/// The process id is in the name, not only the counter. ctest runs the suite as
/// many processes at once and the counter restarts at one in each of them, so
/// two exports were writing the same file and reading each other's — which
/// looked like an exporter that sometimes wrote the wrong size.
class TempFile {
 public:
  explicit TempFile(std::string suffix)
      : path_(std::filesystem::temp_directory_path() /
              ("cutline_export_" + std::to_string(_getpid()) + "_" +
               std::to_string(++counter_) + suffix)) {}

  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  [[nodiscard]] std::string string() const { return path_.string(); }

 private:
  std::filesystem::path path_;
  static inline int counter_ = 0;
};

/// Half a second of a red matte, which is long enough to be a file and short
/// enough that a software encoder is not the slowest thing in the suite.
[[nodiscard]] Project matte_project() {
  Media m;
  m.id = "m";
  m.is_color = true;
  m.color = "#ff0000";

  Clip c;
  c.id = "c";
  c.media_id = "m";
  c.start = 0.0;
  c.source_in = 0.0;
  c.source_out = 0.5;

  Track t;
  t.id = "v1";
  t.kind = TrackKind::Video;
  t.clips = {std::move(c)};

  Project p;
  p.sequence().canvas_w = kWidth;
  p.sequence().canvas_h = kHeight;
  p.sequence().fps = 30.0;
  p.media = {std::move(m)};
  p.sequence().tracks = {std::move(t)};
  return p;
}

/// A second of a 440 Hz tone in a real file, written once and shared.
///
/// A generated matte has no audio at all, so an audible clip needs something an
/// exporter can actually decode. Encoding it per test would dominate the runtime
/// of the file for no extra coverage.
class ToneSource {
 public:
  [[nodiscard]] static const std::string& path() {
    static const ToneSource instance;
    return instance.path_;
  }

 private:
  ToneSource() {
    path_ = (std::filesystem::temp_directory_path() /
             ("cutline_export_tone_" + std::to_string(_getpid()) + ".mp4"))
                .string();

    media::VideoEncodeSettings video;
    video.width = 64;
    video.height = 64;
    video.fps = 10;
    video.preference = media::EncoderPreference::Software;

    media::AudioEncodeSettings audio;
    audio.enabled = true;
    audio.sample_rate = 48000;
    audio.channels = 2;

    auto writer = media::MediaWriter::create(path_, video, audio);
    if (!writer) return;

    std::vector<float> samples(48000 * 2);
    for (std::size_t i = 0; i < 48000; ++i) {
      const double phase = 2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / 48000.0;
      const auto value = static_cast<float>(std::sin(phase) * 0.5);
      samples[i * 2] = value;
      samples[i * 2 + 1] = value;
    }

    const std::vector<std::uint8_t> frame(64 * 64 * 4, 128);
    for (int f = 0; f < 10; ++f) {
      if (!writer.value()->write_frame(frame)) return;
    }
    if (!writer.value()->write_audio(samples)) return;
    (void)writer.value()->finish();
  }

  std::string path_;
};

/// The matte, plus `tracks` audio tracks each holding one clip of the tone.
[[nodiscard]] Project matte_with_audio_tracks(int tracks) {
  Project p = matte_project();

  Media sound;
  sound.id = "a";
  sound.path = ToneSource::path();
  sound.duration = 1.0;
  sound.has_video = true;
  sound.audio_stream_count = 1;
  p.media.push_back(std::move(sound));

  for (int i = 0; i < tracks; ++i) {
    Clip c;
    c.id = "ac" + std::to_string(i);
    c.media_id = "a";
    c.kind = TrackKind::Audio;
    c.start = 0.0;
    c.source_in = 0.0;
    c.source_out = 0.5;

    Track t;
    t.id = "a" + std::to_string(i + 1);
    t.kind = TrackKind::Audio;
    t.clips = {std::move(c)};
    p.sequence().tracks.push_back(std::move(t));
  }
  return p;
}

class ExportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    device_ = shared_device();
    if (!device_) GTEST_SKIP() << "no Direct3D 12 device available";
  }

  /// Exports and probes in one step, since every test here asks the same
  /// question of the result: what actually landed in the file.
  [[nodiscard]] std::expected<media::MediaInfo, std::string> write(
      const Project& project, ExportSettings settings, const TempFile& file) {
    settings.path = file.string();
    if (auto result = export_project(device_, project, settings); !result) {
      return std::unexpected(result.error());
    }
    return media::probe(file.string());
  }

  std::shared_ptr<gpu::Device> device_;
};

// ----------------------------------------------------------------- audio --

TEST_F(ExportTest, AMixdownIsOneStreamHoweverManyTracksThereAre) {
  const TempFile file(".mp4");
  const auto info = write(matte_with_audio_tracks(3),
                          ExportSettings{.audio = true, .separate_audio = false}, file);

  ASSERT_TRUE(info.has_value()) << info.error();
  EXPECT_EQ(info->audio.size(), 1u) << "a mixdown is one stream by definition";
}

TEST_F(ExportTest, SeparateAudioWritesAStreamPerTrack) {
  // What anybody sending a cut on for a mix needs: a stereo mixdown cannot be
  // unpicked back into the tracks it came from.
  const TempFile file(".mp4");
  const auto info = write(matte_with_audio_tracks(3),
                          ExportSettings{.audio = true, .separate_audio = true}, file);

  ASSERT_TRUE(info.has_value()) << info.error();
  EXPECT_EQ(info->audio.size(), 3u);
}

TEST_F(ExportTest, ATrackWithNothingOnItGetsNoStream) {
  // An empty stem is a file somebody has to open to discover is empty.
  Project p = matte_with_audio_tracks(2);
  Track bare;
  bare.id = "a3";
  bare.kind = TrackKind::Audio;
  p.sequence().tracks.push_back(std::move(bare));

  const TempFile file(".mp4");
  const auto info =
      write(p, ExportSettings{.audio = true, .separate_audio = true}, file);

  ASSERT_TRUE(info.has_value()) << info.error();
  EXPECT_EQ(info->audio.size(), 2u);
}

TEST_F(ExportTest, SeparateAudioWithNothingAudibleWritesNoStreamAtAll) {
  const TempFile file(".mp4");
  const auto info =
      write(matte_project(), ExportSettings{.audio = true, .separate_audio = true}, file);

  ASSERT_TRUE(info.has_value()) << info.error();
  EXPECT_TRUE(info->audio.empty());
  EXPECT_TRUE(info->has_video()) << "and the picture still got written";
}

TEST_F(ExportTest, EveryStemIsTheSameLengthAsTheMixdownWouldBe) {
  // Fed in step, so a stem set drops into a session lined up with the picture
  // rather than each stem starting where its own first clip does.
  const TempFile mixed_file(".mp4");
  const auto mixed = write(matte_with_audio_tracks(2),
                           ExportSettings{.audio = true, .separate_audio = false}, mixed_file);
  ASSERT_TRUE(mixed.has_value()) << mixed.error();
  ASSERT_EQ(mixed->audio.size(), 1u);

  const TempFile stems_file(".mp4");
  const auto stems = write(matte_with_audio_tracks(2),
                           ExportSettings{.audio = true, .separate_audio = true}, stems_file);
  ASSERT_TRUE(stems.has_value()) << stems.error();
  ASSERT_EQ(stems->audio.size(), 2u);

  EXPECT_NEAR(stems->duration, mixed->duration, 0.05);
}

// ------------------------------------------------------------- resolution --

TEST_F(ExportTest, TheFileHoldsTheColourTheProjectShows) {
  // The frame is converted for the encoder on the card now, not by
  // `sws_scale` on the processor, so the colour in the file is what has to be
  // checked: pure red, BT.709 at studio range, is Y 63, Cb 102, Cr 240.
  const TempFile file(".mp4");
  ExportSettings settings;
  settings.audio = false;
  settings.path = file.string();
  auto exported = export_project(device_, matte_project(), settings);
  ASSERT_TRUE(exported.has_value()) << exported.error();

  auto decoder = media::VideoDecoder::open(file.string(),
                                           {.preferred = media::Acceleration::Software});
  ASSERT_TRUE(decoder.has_value()) << decoder.error();
  const auto got = (*decoder)->next_frame();
  ASSERT_TRUE(got.has_value() && *got);
  const AVFrame* frame = (*decoder)->frame();
  const int x = frame->width / 2;
  const int y = frame->height / 2;
  const int luma = frame->data[0][static_cast<std::size_t>(y) * frame->linesize[0] + x];
  int cb = 0;
  int cr = 0;
  if (frame->format == AV_PIX_FMT_NV12) {
    const std::uint8_t* row =
        frame->data[1] + static_cast<std::size_t>(y / 2) * frame->linesize[1];
    cb = row[(x / 2) * 2];
    cr = row[(x / 2) * 2 + 1];
  } else {
    cb = frame->data[1][static_cast<std::size_t>(y / 2) * frame->linesize[1] + x / 2];
    cr = frame->data[2][static_cast<std::size_t>(y / 2) * frame->linesize[2] + x / 2];
  }
  EXPECT_NEAR(luma, 63, 2);
  EXPECT_NEAR(cb, 102, 2);
  EXPECT_NEAR(cr, 240, 2);
}

TEST_F(ExportTest, CancellingPartWayStopsPromptlyAndSaysSo) {
  // Frames are encoded on a thread of their own and up to three are on their
  // way back from the card at any moment, so stopping has to give every one of
  // them back and stop the thread, rather than leave either waiting.
  const TempFile file(".mp4");
  ExportSettings settings;
  settings.audio = false;
  settings.path = file.string();
  int seen = 0;
  auto exported = export_project(device_, matte_project(), settings,
                                 [&](const ExportProgress&) { return ++seen < 4; });
  ASSERT_FALSE(exported.has_value());
  EXPECT_EQ(exported.error(), "export cancelled");
  EXPECT_EQ(seen, 4);
}

TEST_F(ExportTest, WritesTheProjectsOwnCanvasByDefault) {
  const TempFile file(".mp4");
  const auto info = write(matte_project(), {}, file);
  ASSERT_TRUE(info.has_value()) << info.error();

  const media::VideoStreamInfo* video = info->primary_video();
  ASSERT_NE(video, nullptr);
  EXPECT_EQ(video->width, kWidth);
  EXPECT_EQ(video->height, kHeight);
}

TEST_F(ExportTest, ARequestedSizeIsTheSizeWritten) {
  const TempFile file(".mp4");
  const auto info = write(matte_project(), {.width = kWidth / 2, .height = kHeight / 2}, file);
  ASSERT_TRUE(info.has_value()) << info.error();

  const media::VideoStreamInfo* video = info->primary_video();
  ASSERT_NE(video, nullptr);
  EXPECT_EQ(video->width, kWidth / 2);
  EXPECT_EQ(video->height, kHeight / 2);
}

TEST_F(ExportTest, AnOddSizeIsRoundedDownRatherThanRefused) {
  const TempFile file(".mp4");
  // Both codecs encode in even-sized blocks. The encoder rounds down to reach
  // one regardless; the exporter has to agree with it, or it would render
  // frames of a size the encoder is not expecting.
  const auto info = write(matte_project(), {.width = 65, .height = 33}, file);
  ASSERT_TRUE(info.has_value()) << info.error();

  const media::VideoStreamInfo* video = info->primary_video();
  ASSERT_NE(video, nullptr);
  EXPECT_EQ(video->width, 64);
  EXPECT_EQ(video->height, 32);
}

TEST_F(ExportTest, ASizeTooSmallToRoundToIsRefused) {
  const TempFile file(".mp4");
  ExportSettings settings{.width = 1, .height = 1};
  settings.path = file.string();

  const auto result = export_project(device_, matte_project(), settings);
  ASSERT_FALSE(result.has_value()) << "one pixel rounds down to none";
  EXPECT_NE(result.error().find("two pixels"), std::string::npos) << result.error();
}

TEST_F(ExportTest, TheFrameRateIsTheProjectsUnlessAskedOtherwise) {
  const TempFile file(".mp4");
  const auto info = write(matte_project(), {.fps = 15.0}, file);
  ASSERT_TRUE(info.has_value()) << info.error();

  const media::VideoStreamInfo* video = info->primary_video();
  ASSERT_NE(video, nullptr);
  EXPECT_NEAR(video->fps, 15.0, 0.01);
}

// ------------------------------------------------------------------ audio --

/// Audio needs a real source: a colour matte is silent, and a silent project
/// deliberately produces a file with no audio stream at all.
class ExportAudioTest : public ExportTest {
 protected:
  void SetUp() override {
    ExportTest::SetUp();
    const char* dir = std::getenv("CUTLINE_TEST_MEDIA_DIR");
    if (dir == nullptr) {
      GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to a directory containing Boiler.mp4";
    }
    const std::filesystem::path source = std::filesystem::path(dir) / "Boiler.mp4";
    if (!std::filesystem::exists(source)) {
      GTEST_SKIP() << "no Boiler.mp4 in CUTLINE_TEST_MEDIA_DIR";
    }
    source_ = source.string();
  }

  [[nodiscard]] Project audible_project() const {
    Project p = matte_project();

    Media m;
    m.id = "sound";
    m.path = source_;
    m.duration = 60.0;
    m.has_video = true;
    m.audio_stream_count = 1;
    p.media.push_back(std::move(m));

    Clip c;
    c.id = "a";
    c.media_id = "sound";
    c.kind = TrackKind::Audio;
    c.audio_stream = 0;
    c.start = 0.0;
    c.source_in = 0.0;
    c.source_out = 0.5;

    Track t;
    t.id = "a1";
    t.kind = TrackKind::Audio;
    t.clips = {std::move(c)};
    p.sequence().tracks.push_back(std::move(t));
    return p;
  }

  std::string source_;
};

TEST_F(ExportAudioTest, StereoByDefault) {
  const TempFile file(".mp4");
  const auto info = write(audible_project(), {}, file);
  ASSERT_TRUE(info.has_value()) << info.error();

  ASSERT_EQ(info->audio.size(), 1u);
  EXPECT_EQ(info->audio.front().channels, 2);
}

TEST_F(ExportAudioTest, OneChannelMixesDownToMono) {
  const TempFile file(".mp4");
  const auto info = write(audible_project(), {.audio_channels = 1}, file);
  ASSERT_TRUE(info.has_value()) << info.error();

  ASSERT_EQ(info->audio.size(), 1u);
  EXPECT_EQ(info->audio.front().channels, 1);
}

TEST_F(ExportAudioTest, AudioTurnedOffLeavesNoStream) {
  const TempFile file(".mp4");
  const auto info = write(audible_project(), {.audio = false}, file);
  ASSERT_TRUE(info.has_value()) << info.error();
  EXPECT_TRUE(info->audio.empty());
}


// --------------------------------------------------------------------- HDR --

/// The committed PQ patches, 203 nits on the left and 1,000 on the right,
/// filling a canvas big enough for a hardware encoder.
[[nodiscard]] Project pq_patches(core::WorkingColour colour) {
  Project p;
  p.sequence().canvas_w = 256;
  p.sequence().canvas_h = 128;
  p.sequence().fps = 24.0;
  p.sequence().colour = colour;
  Media m;
  m.id = "pq";
  m.path = (std::filesystem::path(CUTLINE_MEDIA_FIXTURES) / "pq.mkv").generic_string();
  m.has_video = true;
  m.duration = 0.25;
  m.width = 64;
  m.height = 32;
  p.media = {m};
  Track t;
  t.id = "v1";
  t.kind = TrackKind::Video;
  Clip c;
  c.id = "c";
  c.media_id = "pq";
  c.source_out = 0.25;
  t.clips = {c};
  p.sequence().tracks = {t};
  return p;
}

/// The luma code at a point of the first frame of a 10-bit file.
[[nodiscard]] int first_frame_luma_10(const std::string& path, int x, int y) {
  auto decoder = media::VideoDecoder::open(path, {.preferred = media::Acceleration::Software});
  if (!decoder) return -1;
  const auto got = (*decoder)->next_frame();
  if (!got || !*got) return -1;
  const AVFrame* frame = (*decoder)->frame();
  std::uint16_t value = 0;
  std::memcpy(&value, frame->data[0] + static_cast<std::size_t>(y) * frame->linesize[0] + x * 2, 2);
  return frame->format == AV_PIX_FMT_P010LE ? value >> 6 : value;
}

// An HDR sequence exports as HDR without being asked: ten bits, tagged PQ in
// Rec.2020, the mastering display in the container, and the light the
// sequence held still in the file.
TEST_F(ExportTest, APqSequenceWritesAnHdr10File) {
  const TempFile file(".mp4");
  ExportSettings settings;
  settings.audio = false;
  settings.path = file.string();
  settings.codec = media::VideoCodec::Hevc;
  settings.quality = 0;
  auto exported = export_project(device_, pq_patches(core::WorkingColour::Rec2100Pq), settings);
  if (!exported && exported.error().find("no 10-bit") != std::string::npos) {
    GTEST_SKIP() << "no 10-bit HEVC encoder on this machine";
  }
  ASSERT_TRUE(exported.has_value()) << exported.error();

  const auto info = media::probe(file.string());
  ASSERT_TRUE(info.has_value()) << info.error();
  ASSERT_FALSE(info->video.empty());
  EXPECT_EQ(info->video[0].color.bits_per_component, 10);
  EXPECT_EQ(info->video[0].color.transfer, media::TransferCharacteristic::Smpte2084);
  EXPECT_EQ(info->video[0].color.primaries, media::ColorPrimaries::Bt2020);

  // 203 nits is PQ code 573, and it is still 573.
  EXPECT_NEAR(first_frame_luma_10(file.string(), 40, 64), 573, 3);
  EXPECT_NEAR(first_frame_luma_10(file.string(), 216, 64), 723, 3);

  AVFormatContext* raw = nullptr;
  ASSERT_EQ(avformat_open_input(&raw, file.string().c_str(), nullptr, nullptr), 0);
  ASSERT_GE(avformat_find_stream_info(raw, nullptr), 0);
  const AVCodecParameters* stream = raw->streams[0]->codecpar;
  EXPECT_NE(av_packet_side_data_get(stream->coded_side_data, stream->nb_coded_side_data,
                                    AV_PKT_DATA_MASTERING_DISPLAY_METADATA),
            nullptr);
  avformat_close_input(&raw);
}

// The same sequence written to an SDR file: tone mapped, eight bits, BT.709,
// with 1,000 nits at white and 203 nits kept below it.
TEST_F(ExportTest, AnHdrSequenceCanBeWrittenToAnSdrFile) {
  const TempFile file(".mp4");
  ExportSettings settings;
  settings.audio = false;
  settings.path = file.string();
  settings.quality = 0;
  settings.colour = core::WorkingColour::Rec709;
  auto exported = export_project(device_, pq_patches(core::WorkingColour::Rec2100Pq), settings);
  ASSERT_TRUE(exported.has_value()) << exported.error();

  auto decoder = media::VideoDecoder::open(file.string(), {.preferred = media::Acceleration::Software});
  ASSERT_TRUE(decoder.has_value()) << decoder.error();
  ASSERT_TRUE((*decoder)->next_frame().value_or(false));
  const AVFrame* frame = (*decoder)->frame();
  const int low = frame->data[0][static_cast<std::size_t>(64) * frame->linesize[0] + 40];
  const int high = frame->data[0][static_cast<std::size_t>(64) * frame->linesize[0] + 216];
  EXPECT_NEAR(high, 235, 3);
  EXPECT_LT(low, high - 8);
}

// An HDR sequence handed on as ProRes keeps its light: ten bits, PQ, and the
// same codes the HEVC file holds.
TEST_F(ExportTest, AnHdrSequenceCanBeHandedOnAsProRes) {
  const TempFile file(".mov");
  ExportSettings settings;
  settings.audio = false;
  settings.path = file.string();
  settings.codec = media::VideoCodec::ProRes;
  auto exported = export_project(device_, pq_patches(core::WorkingColour::Rec2100Pq), settings);
  ASSERT_TRUE(exported.has_value()) << exported.error();
  EXPECT_EQ(exported->encoder, "prores_ks");

  const auto info = media::probe(file.string());
  ASSERT_TRUE(info.has_value()) << info.error();
  EXPECT_EQ(info->video[0].codec, "prores");
  EXPECT_EQ(info->video[0].color.transfer, media::TransferCharacteristic::Smpte2084);
  EXPECT_NEAR(first_frame_luma_10(file.string(), 40, 64), 573, 3);
}

TEST_F(ExportTest, HdrIsNotWrittenAsH264) {
  const TempFile file(".mp4");
  ExportSettings settings;
  settings.audio = false;
  settings.path = file.string();
  settings.codec = media::VideoCodec::H264;
  EXPECT_FALSE(export_project(device_, pq_patches(core::WorkingColour::Rec2100Pq), settings)
                   .has_value());
}

// ------------------------------------------------------- smart rendering --

constexpr int kSmartWidth = 256;
constexpr int kSmartHeight = 144;
constexpr int kSmartFrames = 12;
constexpr double kSmartFps = 24.0;

/// An intermediate source, every frame a different grey, so a frame copied
/// from it can be told from any other frame of it.
[[nodiscard]] bool write_intermediate(const TempFile& file, media::VideoCodec codec) {
  media::VideoEncodeSettings video;
  video.width = kSmartWidth;
  video.height = kSmartHeight;
  video.fps = kSmartFps;
  video.codec = codec;
  video.bit_depth = codec == media::VideoCodec::ProRes ? 10 : 8;
  auto writer = media::MediaWriter::create(file.string(), video);
  if (!writer) return false;
  for (int f = 0; f < kSmartFrames; ++f) {
    const std::vector<std::uint8_t> frame(kSmartWidth * kSmartHeight * 4,
                                          static_cast<std::uint8_t>(20 + f * 16));
    if (!writer.value()->write_frame(frame)) return false;
  }
  return writer.value()->finish().has_value();
}

/// The source's second half and then its first, as two clips: a cut, so a
/// copy has to follow the edit rather than the file.
[[nodiscard]] Project swapped_halves(const std::string& path) {
  Media m;
  m.id = "s";
  m.path = path;
  m.has_video = true;
  m.width = kSmartWidth;
  m.height = kSmartHeight;
  m.fps = kSmartFps;
  m.duration = kSmartFrames / kSmartFps;

  const double half = kSmartFrames / 2 / kSmartFps;
  Clip second;
  second.id = "second";
  second.media_id = "s";
  second.start = 0.0;
  second.source_in = half;
  second.source_out = 2.0 * half;
  Clip first = second;
  first.id = "first";
  first.start = half;
  first.source_in = 0.0;
  first.source_out = half;

  Track t;
  t.id = "v1";
  t.kind = TrackKind::Video;
  t.clips = {std::move(second), std::move(first)};

  Project p;
  p.sequence().canvas_w = kSmartWidth;
  p.sequence().canvas_h = kSmartHeight;
  p.sequence().fps = kSmartFps;
  p.media = {std::move(m)};
  p.sequence().tracks = {std::move(t)};
  return p;
}

/// Every compressed frame of a file, in order.
[[nodiscard]] std::vector<std::vector<std::uint8_t>> packets_of(const std::string& path) {
  std::vector<std::vector<std::uint8_t>> out;
  auto reader = media::PacketReader::open(path);
  if (!reader) return out;
  for (int f = 0; f < kSmartFrames; ++f) {
    auto bytes = (*reader)->frame_at(f / kSmartFps);
    if (!bytes) break;
    out.push_back(std::move(*bytes));
  }
  return out;
}

class SmartRenderTest : public ExportTest,
                        public ::testing::WithParamInterface<media::VideoCodec> {
 protected:
  void SetUp() override {
    ExportTest::SetUp();
    if (IsSkipped()) return;
    ASSERT_TRUE(write_intermediate(source_, GetParam()));
    source_packets_ = packets_of(source_.string());
    ASSERT_EQ(source_packets_.size(), static_cast<std::size_t>(kSmartFrames));
  }

  [[nodiscard]] std::expected<ExportResult, std::string> render(const Project& project,
                                                                const TempFile& file,
                                                                ExportSettings settings = {}) {
    settings.audio = false;
    settings.path = file.string();
    settings.codec = GetParam();
    return export_project(device_, project, settings);
  }

  TempFile source_{".mov"};
  std::vector<std::vector<std::uint8_t>> source_packets_;
};

TEST_P(SmartRenderTest, AnUntouchedCutIsCopiedFrameForFrame) {
  const TempFile file(".mov");
  const auto result = render(swapped_halves(source_.string()), file);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->copied_frames, kSmartFrames);

  // Byte for byte, and following the edit: the source's second half first.
  const auto written = packets_of(file.string());
  ASSERT_EQ(written.size(), static_cast<std::size_t>(kSmartFrames));
  for (int f = 0; f < kSmartFrames; ++f) {
    const int from = (f + kSmartFrames / 2) % kSmartFrames;
    EXPECT_EQ(written[f], source_packets_[from]) << "frame " << f;
  }
}

TEST_P(SmartRenderTest, AFrameWithSomethingOverItIsRenderedAndTheRestCopied) {
  // A matte on the track above, over three frames in the middle. Those are a
  // different picture and have to be rendered; the frames either side are
  // still the source's own, and the file holds them all in order.
  Project project = swapped_halves(source_.string());
  Media matte;
  matte.id = "matte";
  matte.is_color = true;
  matte.color = "#00ff00";
  project.media.push_back(matte);
  Clip over;
  over.id = "over";
  over.media_id = "matte";
  over.start = 4 / kSmartFps;
  over.source_in = 0.0;
  over.source_out = 3 / kSmartFps;
  Track above;
  above.id = "v2";
  above.kind = TrackKind::Video;
  above.clips = {over};
  project.sequence().tracks.insert(project.sequence().tracks.begin(), above);

  const TempFile file(".mov");
  const auto result = render(project, file);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->copied_frames, kSmartFrames - 3);

  const auto written = packets_of(file.string());
  ASSERT_EQ(written.size(), static_cast<std::size_t>(kSmartFrames));
  for (int f = 0; f < kSmartFrames; ++f) {
    const int from = (f + kSmartFrames / 2) % kSmartFrames;
    if (f >= 4 && f < 7) {
      EXPECT_NE(written[f], source_packets_[from]) << "frame " << f << " is under the matte";
    } else {
      EXPECT_EQ(written[f], source_packets_[from]) << "frame " << f;
    }
  }
}

TEST_P(SmartRenderTest, AnythingDoneToThePictureIsRendered) {
  const TempFile file(".mov");
  {
    Project faded = swapped_halves(source_.string());
    faded.sequence().tracks[0].clips[0].opacity = 0.5;
    const auto result = render(faded, file);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->copied_frames, kSmartFrames / 2) << "only the faded clip is rendered";
  }
  {
    Project moved = swapped_halves(source_.string());
    for (Clip& clip : moved.sequence().tracks[0].clips) clip.transform.x = 0.6;
    const auto result = render(moved, file);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->copied_frames, 0);
  }
  {
    Project graded = swapped_halves(source_.string());
    for (Clip& clip : graded.sequence().tracks[0].clips) {
      clip.effects.push_back(core::ClipEffect{.type = "brightness"});
    }
    const auto result = render(graded, file);
    ASSERT_TRUE(result.has_value()) << result.error();
    EXPECT_EQ(result->copied_frames, 0);
  }
}

TEST_P(SmartRenderTest, AnotherSizeOrRateIsRendered) {
  const TempFile file(".mov");
  ExportSettings smaller;
  smaller.width = kSmartWidth;
  smaller.height = kSmartHeight * 2;
  auto result = render(swapped_halves(source_.string()), file, smaller);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->copied_frames, 0);

  ExportSettings faster;
  faster.fps = kSmartFps * 2;
  result = render(swapped_halves(source_.string()), file, faster);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->copied_frames, 0);
}

TEST_P(SmartRenderTest, ItCanBeTurnedOff) {
  const TempFile file(".mov");
  ExportSettings settings;
  settings.smart_render = false;
  const auto result = render(swapped_halves(source_.string()), file, settings);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->copied_frames, 0);
}

INSTANTIATE_TEST_SUITE_P(Intermediates, SmartRenderTest,
                         ::testing::Values(media::VideoCodec::ProRes, media::VideoCodec::DnxHr),
                         [](const auto& info) { return std::string(media::to_string(info.param)); });

TEST_F(ExportTest, AnotherFormatsFramesAreNeverCopied) {
  // DNxHR into ProRes: both stand alone, neither is the other.
  const TempFile source(".mov");
  ASSERT_TRUE(write_intermediate(source, media::VideoCodec::DnxHr));
  const TempFile file(".mov");
  ExportSettings settings;
  settings.audio = false;
  settings.path = file.string();
  settings.codec = media::VideoCodec::ProRes;
  const auto result = export_project(device_, swapped_halves(source.string()), settings);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->copied_frames, 0);
}

TEST_F(ExportTest, DeliveryFormatsAreNeverCopiedInto) {
  // H.264 frames depend on one another, so none stands alone to be copied in.
  const TempFile source(".mov");
  ASSERT_TRUE(write_intermediate(source, media::VideoCodec::ProRes));
  const TempFile file(".mp4");
  ExportSettings settings;
  settings.audio = false;
  settings.path = file.string();
  settings.codec = media::VideoCodec::H264;
  const auto result = export_project(device_, swapped_halves(source.string()), settings);
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->copied_frames, 0);
}

}  // namespace
}  // namespace cutline::engine
