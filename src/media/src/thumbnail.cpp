#include "cutline/media/thumbnail.hpp"

#include "av_common.hpp"

#include "cutline/media/decoder.hpp"
#include "cutline/media/probe.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace cutline::media {
namespace {

using namespace detail;

/// Scales one decoded frame to a packed RGBA thumbnail, reusing the scaler
/// while the source format holds.
struct Shrinker {
  Scaler scaler;
  AVPixelFormat source = AV_PIX_FMT_NONE;
  int width = 0;
  int height = 0;

  [[nodiscard]] std::expected<Thumbnail, std::string> operator()(const AVFrame* frame,
                                                                 double timestamp) {
    const auto format = static_cast<AVPixelFormat>(frame->format);
    // A source can change format mid-stream, so the scaler is rebuilt when it
    // does rather than quietly producing garbage.
    if (!scaler || source != format) {
      scaler.reset(sws_getContext(frame->width, frame->height, format, width, height,
                                  AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr));
      if (!scaler) return std::unexpected("cannot create a scaler for this pixel format");
      source = format;
    }

    Thumbnail thumbnail;
    thumbnail.width = width;
    thumbnail.height = height;
    thumbnail.timestamp = timestamp;
    thumbnail.rgba.resize(static_cast<std::size_t>(width) * height * 4);

    std::uint8_t* destination[4] = {thumbnail.rgba.data(), nullptr, nullptr, nullptr};
    int stride[4] = {width * 4, 0, 0, 0};
    sws_scale(scaler.get(), frame->data, frame->linesize, 0, frame->height, destination, stride);
    return thumbnail;
  }
};

}  // namespace

std::expected<std::vector<Thumbnail>, std::string> extract_thumbnails(std::string_view path,
                                                                     int count,
                                                                     ThumbnailOptions options) {
  if (count <= 0) return std::vector<Thumbnail>{};
  if (options.height <= 0) return std::unexpected("thumbnail height must be positive");

  detail::quiet_av_logging();
  // On the card's own decoder where it will, on a device kept for work in the
  // background so a filmstrip never queues in front of a frame being played.
  // Only the frames kept come back to the processor, a dozen of several
  // hundred decoded. The thread count is for when it falls back to software.
  auto opened = VideoDecoder::open(
      path, {.preferred = options.hardware ? Acceleration::D3D11Va : Acceleration::Software,
             .threads = std::max(0, options.threads),
             .d3d11_device = kBackgroundDecodingDevice});
  if (!opened) return std::unexpected(opened.error());
  VideoDecoder& decoder = **opened;

  const VideoStreamInfo& stream = decoder.stream();
  if (stream.width <= 0 || stream.height <= 0) {
    return std::unexpected("video stream has no usable dimensions");
  }
  Shrinker shrink;
  shrink.height = options.height;
  shrink.width = std::max(1, static_cast<int>(std::lround(static_cast<double>(options.height) *
                                                          stream.width / stream.height)));

  double end = options.end;
  const double start = std::max(0.0, options.start);
  if (!(end > start)) {
    auto info = probe(path);
    if (!info) return std::unexpected(info.error());
    end = std::max(start, info->duration);
  }
  const double span = std::max(0.0, end - start);

  Frame download(av_frame_alloc());
  if (!download) return std::unexpected("out of memory allocating a frame");

  std::vector<Thumbnail> thumbnails;
  thumbnails.reserve(static_cast<std::size_t>(count));

  // Opening decodes a first frame to prove the decoder works, so decoding has
  // already got somewhere before anything is asked for.
  bool have = decoder.frame() != nullptr;
  for (int i = 0; i < count; ++i) {
    // Sampled at bucket centres, so a filmstrip represents the whole span
    // rather than clustering at its start.
    const double fraction = count == 1 ? 0.5 : (i + 0.5) / count;
    const double target = start + span * fraction;

    // Seek only to go forward past pictures that need not be decoded, or to go
    // back. When the keyframe for this target is no further on than decoding
    // already is, the seek would decode the same pictures a second time.
    const double at = have ? decoder.timestamp() : -1.0;
    const auto keyframe = decoder.keyframe_at_or_before(target);
    const bool behind = target + 1e-6 < at;
    const bool skips_ahead = keyframe.has_value() ? *keyframe > at + 1e-6 : target > at + 1.0;
    if (!have || behind || skips_ahead) {
      if (auto sought = decoder.seek(target); !sought) return std::unexpected(sought.error());
      have = false;
    }

    while (!have || decoder.timestamp() + 1e-6 < target) {
      auto next = decoder.next_frame();
      if (!next) return std::unexpected(next.error());
      have = *next;
      if (!have) break;
    }
    if (!have) break;  // ran off the end of the stream

    const AVFrame* frame = decoder.frame();
    if (frame->hw_frames_ctx != nullptr) {
      av_frame_unref(download.get());
      if (const int rc = av_hwframe_transfer_data(download.get(), frame, 0); rc < 0) {
        return std::unexpected(std::format("cannot read a frame back: {}", av_error_string(rc)));
      }
      frame = download.get();
    }
    auto thumbnail = shrink(frame, target);
    if (!thumbnail) return std::unexpected(thumbnail.error());
    thumbnails.push_back(std::move(*thumbnail));
  }

  return thumbnails;
}

}  // namespace cutline::media
