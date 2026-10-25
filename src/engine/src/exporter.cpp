#include "cutline/engine/exporter.hpp"

#include "cutline/core/query.hpp"
#include "cutline/engine/audio_mixer.hpp"
#include "cutline/engine/frame_renderer.hpp"
#include "cutline/media/packet_reader.hpp"
#include "cutline/render/plan.hpp"

#include "cutline/gpu/compositor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace cutline::engine {
namespace {

/// One frame on its way to the file: where its picture is coming back to, or
/// the source frame it is copied from, and the sound that goes with it, a block
/// per stream.
struct Delivery {
  std::size_t slot = 0;
  std::optional<std::vector<std::uint8_t>> copied;
  std::vector<std::vector<float>> audio;
};

/// The output an export is writing, for deciding whether a source frame is
/// already it.
struct Shape {
  media::VideoCodec codec = media::VideoCodec::H264;
  int width = 0;
  int height = 0;
  double fps = 0.0;
  core::WorkingColour colour = core::WorkingColour::Rec709;
};

/// The source frame an output frame is, unchanged, when it is one.
struct Untouched {
  const core::Media* media = nullptr;
  double source_time = 0.0;
};

/// Whether the frame at `t` is exactly one source frame, untouched.
///
/// Read from the renderer's own plan, so "untouched" means what the renderer
/// would draw rather than a second reading of the clip: one layer and nothing
/// else -- a transition, a title, an adjustment layer above all make two --
/// filling the frame exactly, opaque, unrotated, blended normally, from a clip
/// at its own speed with no effect on it, of a source the frame's size and in
/// the colour being written. Conservative on purpose. A frame copied that
/// should have been rendered is a wrong picture in a delivered file; a frame
/// rendered that could have been copied is only slower.
[[nodiscard]] std::optional<Untouched> untouched_at(const core::Project& project, double t,
                                                   const Shape& shape) {
  const std::vector<render::PlannedLayer> layers = render::plan_frame(project, t);
  if (layers.size() != 1) return std::nullopt;
  const render::PlannedLayer& layer = layers.front();
  if (layer.content != render::LayerContent::Video || layer.clip == nullptr ||
      layer.media == nullptr) {
    return std::nullopt;
  }
  if (layer.alpha < 1.0 - 1e-9 || layer.blend != core::BlendMode::Normal ||
      layer.anti_flicker != 0.0) {
    return std::nullopt;
  }
  constexpr double kPixel = 1e-3;
  const core::LayerBox& box = layer.box;
  if (std::abs(box.center_x - shape.width / 2.0) > kPixel ||
      std::abs(box.center_y - shape.height / 2.0) > kPixel ||
      std::abs(box.width - shape.width) > kPixel || std::abs(box.height - shape.height) > kPixel ||
      box.rotation_deg != 0.0) {
    return std::nullopt;
  }
  const core::Clip& clip = *layer.clip;
  if (clip.speed != 1.0 || clip.reverse || clip.hold.has_value()) return std::nullopt;
  if (std::ranges::any_of(clip.effects, &core::ClipEffect::enabled)) return std::nullopt;
  const core::Media& media = *layer.media;
  // A conformed source plays its frames at another rate; a copy would not.
  if (media.assumed_fps.has_value()) return std::nullopt;
  if (media.colour_override.value_or(media.colour_space.value_or(core::WorkingColour::Rec709)) !=
      shape.colour) {
    return std::nullopt;
  }
  if (media.width != shape.width || media.height != shape.height) return std::nullopt;
  // On a frame of the source, not between two. The renderer would show the
  // one before; the copy would take the nearer, and they could differ.
  const double frames = layer.source_time * shape.fps;
  if (std::abs(frames - std::round(frames)) > 1e-3) return std::nullopt;
  return Untouched{.media = &media, .source_time = layer.source_time};
}

/// Whether a source's frames can stand in the file being written: the same
/// codec and profile, the same size and rate, whole frames.
[[nodiscard]] bool fits(const media::PacketStream& stream, const Shape& shape) {
  return stream.codec == shape.codec && stream.export_profile && stream.width == shape.width &&
         stream.height == shape.height && std::abs(stream.fps - shape.fps) < 1e-3 &&
         stream.progressive;
}

/// The encoding side of an export, on a thread of its own.
///
/// An export was render, wait for the card, read back 33 MB of RGBA, convert
/// on the processor, encode -- each in turn, 26 ms a 4K frame, 0.6x realtime.
/// Now the card converts while it still has the frame and copies back a third
/// as much, and this thread collects each frame and encodes it while the next
/// is being decoded and composited. The compositor's slots are what bound how
/// far rendering runs ahead.
class Encoding {
 public:
  Encoding(gpu::Compositor& compositor, media::MediaWriter& writer)
      : compositor_(compositor), writer_(writer), thread_([this] { run(); }) {}

  Encoding(const Encoding&) = delete;
  Encoding& operator=(const Encoding&) = delete;

  ~Encoding() { stop(); }

  void send(Delivery delivery) {
    {
      const std::lock_guard<std::mutex> held(lock_);
      queue_.push_back(std::move(delivery));
    }
    wake_.notify_all();
  }

  /// Everything sent, written; then the thread is done. Returns the first
  /// thing that went wrong, if anything did.
  [[nodiscard]] std::expected<void, std::string> finish() {
    stop();
    if (failure_) return std::unexpected(*failure_);
    return {};
  }

  /// Whether something has gone wrong, so the renderer stops making frames
  /// nobody will write.
  [[nodiscard]] std::optional<std::string> failure() const {
    const std::lock_guard<std::mutex> held(lock_);
    return failure_;
  }

  /// Gives up without writing what is left, handing back every slot so the
  /// compositor can be torn down.
  void abandon() {
    {
      const std::lock_guard<std::mutex> held(lock_);
      abandoned_ = true;
    }
    stop();
  }

 private:
  void stop() {
    {
      const std::lock_guard<std::mutex> held(lock_);
      closing_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  void run() {
    while (true) {
      Delivery delivery;
      bool skip = false;
      {
        std::unique_lock<std::mutex> held(lock_);
        wake_.wait(held, [&] { return closing_ || !queue_.empty(); });
        if (queue_.empty()) return;
        delivery = std::move(queue_.front());
        queue_.pop_front();
        skip = abandoned_ || failure_.has_value();
      }
      std::expected<void, std::string> written;
      // Collected even when it will not be written, because collecting is what
      // gives the slot back.
      const auto write = [&](const gpu::YuvPlanes& planes) {
        if (skip) return;
        written = writer_.write_frame(
            media::PlanarFrame{.ten_bit = planes.layout == gpu::YuvLayout::P010,
                               .luma = planes.luma,
                               .luma_stride = planes.luma_stride,
                               .chroma = planes.chroma,
                               .chroma_stride = planes.chroma_stride});
      };
      if (delivery.copied.has_value()) {
        if (skip) continue;
        written = writer_.write_packet(*delivery.copied);
      } else {
        auto collected = compositor_.finish_yuv_read_back(delivery.slot, write);
        if (skip) continue;
        if (!collected) written = std::unexpected(collected.error());
      }
      for (std::size_t i = 0; written && i < delivery.audio.size(); ++i) {
        written = writer_.write_audio(delivery.audio[i], static_cast<int>(i));
      }
      if (!written) {
        const std::lock_guard<std::mutex> held(lock_);
        if (!failure_) failure_ = written.error();
      }
    }
  }

  gpu::Compositor& compositor_;
  media::MediaWriter& writer_;
  mutable std::mutex lock_;
  std::condition_variable wake_;
  std::deque<Delivery> queue_;
  bool closing_ = false;
  bool abandoned_ = false;
  std::optional<std::string> failure_;
  // Last, so everything it reads exists before it starts.
  std::thread thread_;
};

}  // namespace

std::expected<ExportResult, std::string> export_project(
    const std::shared_ptr<gpu::Device>& device, const core::Project& project,
    const ExportSettings& settings, const ProgressCallback& on_progress) {
  if (!device) return std::unexpected("an export needs a device");
  if (settings.path.empty()) return std::unexpected("an export needs an output path");

  const double fps = settings.fps > 0.0 ? settings.fps : project.sequence().fps;
  if (fps <= 0.0) return std::unexpected("the output frame rate must be positive");

  const double timeline = core::timeline_duration(project);
  const double start = std::max(0.0, settings.start);
  const double duration =
      settings.duration > 0.0 ? settings.duration : std::max(0.0, timeline - start);

  if (duration <= 0.0) {
    return std::unexpected("there is nothing to export: the timeline is empty");
  }

  // Round rather than truncate, so a timeline that is a whole number of frames
  // long does not lose its last one to floating-point error.
  const auto total = static_cast<std::int64_t>(std::llround(duration * fps));
  if (total <= 0) return std::unexpected("the export range is shorter than one frame");

  // Rounded down to even, because both codecs encode in even-sized blocks and
  // the encoder rounds to reach one. Doing it here as well is what keeps the
  // frames the renderer produces the size the encoder is expecting.
  const int width = (settings.width > 0 ? settings.width : project.sequence().canvas_w) & ~1;
  const int height = (settings.height > 0 ? settings.height : project.sequence().canvas_h) & ~1;
  if (width <= 0 || height <= 0) {
    return std::unexpected("the output size must be at least two pixels each way");
  }

  // The renderer takes its canvas from the project it is handed, so exporting
  // at another resolution is exporting a project whose canvas is that size.
  // A copy, because the caller's project is not the export's to change.
  core::Project resized;
  const core::Project* source = &project;
  if (width != project.sequence().canvas_w || height != project.sequence().canvas_h) {
    resized = project;
    resized.sequence().canvas_w = width;
    resized.sequence().canvas_h = height;
    source = &resized;
  }

  auto renderer = FrameRenderer::create(device, width, height);
  if (!renderer) return std::unexpected(renderer.error());

  // What the file is written in, which is the sequence's unless the export
  // says otherwise.
  const core::WorkingColour colour = settings.colour.value_or(project.sequence().colour);
  const bool hdr = colour != core::WorkingColour::Rec709;
  if (hdr && settings.codec != media::VideoCodec::Hevc &&
      settings.codec != media::VideoCodec::ProRes) {
    return std::unexpected("HDR is written as HEVC or ProRes; H.264 and DNxHR HQ have no HDR "
                           "here, as in Premiere");
  }
  // ProRes 422 HQ is ten bits whatever was asked; DNxHR HQ eight.
  int bit_depth = settings.bit_depth != 0 ? settings.bit_depth : (hdr ? 10 : 8);
  if (settings.codec == media::VideoCodec::ProRes) bit_depth = 10;
  if (settings.codec == media::VideoCodec::DnxHr) bit_depth = 8;

  media::VideoEncodeSettings encode;
  encode.width = width;
  encode.height = height;
  encode.fps = fps;
  encode.codec = settings.codec;
  encode.preference = settings.preference;
  encode.bitrate = settings.bitrate;
  encode.quality = settings.quality;
  encode.bit_depth = bit_depth;
  encode.colour = colour == core::WorkingColour::Rec2100Pq    ? media::VideoColour::Bt2100Pq
                  : colour == core::WorkingColour::Rec2100Hlg ? media::VideoColour::Bt2100Hlg
                                                              : media::VideoColour::Bt709;
  if (colour == core::WorkingColour::Rec2100Pq) {
    encode.hdr = settings.hdr.value_or(media::HdrMetadata{});
  }
  const gpu::YuvLayout layout = bit_depth > 8 ? gpu::YuvLayout::P010 : gpu::YuvLayout::Nv12;
  const auto output = static_cast<gpu::WorkingColour>(colour);

  // Built before the writer, because whether the file gets an audio stream at
  // all depends on whether anything is audible — and a container records its
  // streams in the header, so that has to be known up front.
  // One mixer for a mixdown, one per audio track when the tracks are to be kept
  // apart. A stem set is what anybody sending a cut on for a mix needs, since a
  // stereo mixdown cannot be unpicked back into the tracks it came from.
  //
  // A track with nothing on it produces a silent mixer and gets no stream: an
  // empty stem is a file somebody has to open to discover is empty. So the
  // streams follow the order of the tracks that have something on them.
  std::vector<std::unique_ptr<AudioMixer>> mixers;
  if (settings.audio) {
    const auto add = [&](std::optional<int> track) -> std::expected<void, std::string> {
      auto built = AudioMixer::create(*source, {.sample_rate = settings.audio_sample_rate,
                                                .channels = settings.audio_channels,
                                                .only_track = track});
      if (!built) return std::unexpected(built.error());
      if (!(*built)->silent()) mixers.push_back(std::move(*built));
      return {};
    };

    if (settings.separate_audio) {
      int index = 0;
      for (const core::Track& track : source->sequence().tracks) {
        if (track.kind != core::TrackKind::Audio) continue;
        if (auto ok = add(index); !ok) return std::unexpected(ok.error());
        ++index;
      }
    } else if (auto ok = add(std::nullopt); !ok) {
      return std::unexpected(ok.error());
    }
  }

  media::AudioEncodeSettings encode_audio;
  encode_audio.enabled = !mixers.empty();
  encode_audio.sample_rate = settings.audio_sample_rate;
  encode_audio.channels = settings.audio_channels;
  encode_audio.bitrate = settings.audio_bitrate;
  encode_audio.streams = static_cast<int>(mixers.size());

  auto writer = media::MediaWriter::create(settings.path, encode, encode_audio);
  if (!writer) return std::unexpected(writer.error());

  ExportResult result;
  result.encoder = (*writer)->encoder_name();
  if ((*writer)->has_audio()) result.audio_encoder = (*writer)->audio_encoder_name();

  // Deduplicated: a source missing for one frame is missing for all of them,
  // and a per-frame list would be thousands of identical lines.
  std::set<std::string> missing;
  for (const std::unique_ptr<AudioMixer>& mixer : mixers) {
    for (const std::string& id : mixer->missing_media()) missing.insert(id);
  }

  // Audio is written a frame's worth at a time, interleaved with the video, so
  // the muxer never has to buffer one stream while it waits for the other.
  const auto channels = static_cast<std::size_t>(settings.audio_channels);
  const double rate = static_cast<double>(settings.audio_sample_rate);
  std::vector<float> audio_block;
  std::int64_t audio_fed = 0;
  const auto audio_total = static_cast<std::int64_t>(std::llround(duration * rate));

  const auto began = std::chrono::steady_clock::now();

  // Prime the limiter. Its look-ahead delays output by a few milliseconds, and
  // the first block out is that much silence; feeding it and throwing the
  // silence away is what keeps audio aligned to picture instead of trailing it.
  for (const std::unique_ptr<AudioMixer>& mixer : mixers) {
    const auto latency = std::min(static_cast<std::int64_t>(mixer->latency_frames()),
                                  audio_total);
    if (latency > 0) {
      audio_block.assign(static_cast<std::size_t>(latency) * channels, 0.0f);
      if (auto ok = mixer->mix(start, audio_block); !ok) return std::unexpected(ok.error());
      // Every stream is the same length and fed in step, so one count serves
      // them all — they differ in what they carry, not in how much.
      audio_fed = latency;
    }
  }

  // Smart rendering, for the intermediates. One reader per source, opened the
  // first time a frame of it could be copied; null for a source whose frames
  // cannot stand in this file, so it is not opened again to find that out.
  const bool smart = settings.smart_render && media::is_intermediate(settings.codec);
  const Shape shape{.codec = settings.codec, .width = width, .height = height, .fps = fps,
                    .colour = colour};
  std::map<std::string, std::unique_ptr<media::PacketReader>> readers;
  const auto copy_of = [&](double t) -> std::optional<std::vector<std::uint8_t>> {
    if (!smart) return std::nullopt;
    const std::optional<Untouched> found = untouched_at(*source, t, shape);
    if (!found.has_value()) return std::nullopt;
    auto [entry, fresh] = readers.try_emplace(found->media->id);
    if (fresh) {
      auto opened = media::PacketReader::open(found->media->path);
      if (opened.has_value() && fits((*opened)->stream(), shape)) entry->second = std::move(*opened);
    }
    if (!entry->second) return std::nullopt;
    // A frame that cannot be read is rendered instead: the copy is a saving,
    // never a reason for an export to fail.
    auto bytes = entry->second->frame_at(found->source_time);
    if (!bytes.has_value()) return std::nullopt;
    return std::move(*bytes);
  };

  Encoding encoding((*renderer)->compositor(), **writer);
  // Anything that leaves early leaves the encoder stopped and the slots given
  // back, before the renderer that owns them goes.
  struct Abandon {
    Encoding* encoding;
    ~Abandon() {
      if (encoding != nullptr) encoding->abandon();
    }
  } abandon{&encoding};

  for (std::int64_t frame = 0; frame < total; ++frame) {
    if (auto failed = encoding.failure()) {
      return std::unexpected(std::format("frame {}: {}", frame, *failed));
    }

    // Time is computed from the frame index rather than accumulated, so a
    // rounding error cannot drift over a long timeline.
    const double t = start + static_cast<double>(frame) / fps;

    Delivery delivery;
    if (auto copied = copy_of(t); copied.has_value()) {
      delivery.copied = std::move(*copied);
      ++result.copied_frames;
    } else {
      if (auto ok = (*renderer)->render(*source, t); !ok) {
        return std::unexpected(std::format("frame {}: {}", frame, ok.error()));
      }
      for (const std::string& id : (*renderer)->missing_media()) missing.insert(id);

      const auto slot = (*renderer)->compositor().begin_yuv_read_back(layout, output);
      if (!slot) return std::unexpected(std::format("frame {}: {}", frame, slot.error()));
      delivery.slot = *slot;
    }
    if (!mixers.empty()) {
      // How much audio this frame of video should be accompanied by, computed
      // from the frame index rather than accumulated so the two stay locked
      // together over a long timeline.
      const auto want = std::min(
          audio_total,
          static_cast<std::int64_t>(std::llround(static_cast<double>(frame + 1) / fps * rate)));
      if (const std::int64_t need = want - audio_fed; need > 0) {
        const double at = start + static_cast<double>(audio_fed) / rate;
        for (const std::unique_ptr<AudioMixer>& mixer : mixers) {
          std::vector<float> block(static_cast<std::size_t>(need) * channels, 0.0f);
          if (auto ok = mixer->mix(at, block); !ok) {
            return std::unexpected(std::format("frame {}: {}", frame, ok.error()));
          }
          delivery.audio.push_back(std::move(block));
        }
        audio_fed = want;
      }
    }
    encoding.send(std::move(delivery));

    if (on_progress) {
      const ExportProgress progress{.frame = frame + 1, .total = total, .timeline_seconds = t};
      if (!on_progress(progress)) {
        // Cancelled. The file is deliberately left unfinalised, so it is
        // obviously incomplete rather than a short but valid export.
        return std::unexpected("export cancelled");
      }
    }
  }

  abandon.encoding = nullptr;
  if (auto ok = encoding.finish(); !ok) return std::unexpected(ok.error());

  // The last few milliseconds are still inside the limiter's look-ahead;
  // without this they are simply lost off the end of the timeline.
  for (std::size_t i = 0; i < mixers.size(); ++i) {
    if (mixers[i]->latency_frames() == 0) continue;
    audio_block.assign(mixers[i]->latency_frames() * channels, 0.0f);
    mixers[i]->flush(audio_block);
    if (auto ok = (*writer)->write_audio(audio_block, static_cast<int>(i)); !ok) {
      return std::unexpected(ok.error());
    }
  }

  if (auto ok = (*writer)->finish(); !ok) return std::unexpected(ok.error());

  const auto ended = std::chrono::steady_clock::now();
  result.audio_frames = (*writer)->audio_frame_count();
  result.frames = total;
  result.duration_seconds = static_cast<double>(total) / fps;
  result.elapsed_seconds = std::chrono::duration<double>(ended - began).count();
  result.missing_media.assign(missing.begin(), missing.end());
  return result;
}

}  // namespace cutline::engine
