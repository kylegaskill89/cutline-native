#include "cutline/media/packet_reader.hpp"

#include "av_common.hpp"

#include <format>
#include <optional>

namespace cutline::media {

using detail::av_error_string;

struct PacketReader::Impl {
  detail::FormatContext format;
  detail::Packet packet;
  int index = -1;
  AVRational time_base{1, 1};
  PacketStream stream;
  /// When the last frame handed out was shown, so the next one in order is
  /// read on rather than sought, which is how an export asks for them.
  std::optional<double> last;
};

PacketReader::PacketReader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PacketReader::~PacketReader() = default;

std::expected<std::unique_ptr<PacketReader>, std::string> PacketReader::open(
    std::string_view path) {
  const std::string path_string(path);
  detail::quiet_av_logging();

  auto impl = std::make_unique<Impl>();
  AVFormatContext* raw = nullptr;
  if (const int rc = avformat_open_input(&raw, path_string.c_str(), nullptr, nullptr); rc < 0) {
    return std::unexpected(std::format("cannot open {}: {}", path_string, av_error_string(rc)));
  }
  impl->format.reset(raw);
  // Read far enough to know the profile, which for DNxHR is in the frames
  // rather than the container.
  if (const int rc = avformat_find_stream_info(impl->format.get(), nullptr); rc < 0) {
    return std::unexpected(
        std::format("cannot read streams in {}: {}", path_string, av_error_string(rc)));
  }
  impl->index = detail::best_footage_stream(impl->format.get(), nullptr);
  if (impl->index < 0) return std::unexpected(path_string + " has no picture");

  const AVStream* stream = impl->format->streams[impl->index];
  const AVCodecParameters* parameters = stream->codecpar;
  impl->time_base = stream->time_base;
  PacketStream& info = impl->stream;
  if (parameters->codec_id == AV_CODEC_ID_PRORES) {
    info.codec = VideoCodec::ProRes;
    // The four-character code is the profile in a .mov, and is what a player
    // trusts; the profile number is what FFmpeg makes of it.
    info.export_profile = parameters->profile == AV_PROFILE_PRORES_HQ ||
                          parameters->codec_tag == MKTAG('a', 'p', 'c', 'h');
  } else if (parameters->codec_id == AV_CODEC_ID_DNXHD) {
    info.codec = VideoCodec::DnxHr;
    info.export_profile = parameters->profile == AV_PROFILE_DNXHR_HQ;
  }
  info.width = parameters->width;
  info.height = parameters->height;
  const AVRational rate =
      stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
  info.fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0.0;
  // Unknown is what a .mov with no field atom says, and every decoder here
  // reads such a file as whole frames.
  info.progressive = parameters->field_order == AV_FIELD_PROGRESSIVE ||
                     parameters->field_order == AV_FIELD_UNKNOWN;

  impl->packet.reset(av_packet_alloc());
  if (!impl->packet) return std::unexpected("out of memory");
  return std::unique_ptr<PacketReader>(new PacketReader(std::move(impl)));
}

const PacketStream& PacketReader::stream() const noexcept { return impl_->stream; }

std::expected<std::vector<std::uint8_t>, std::string> PacketReader::frame_at(double time) {
  Impl& d = *impl_;
  if (d.stream.fps <= 0.0) return std::unexpected("the stream has no frame rate");
  const double frame = 1.0 / d.stream.fps;
  const double half = frame / 2.0;
  const double base = av_q2d(d.time_base);

  // In order is read on; anything else is sought. Backward from half a frame
  // early, so the frame that covers the moment is not passed over.
  if (!d.last.has_value() || time <= *d.last || time - *d.last > 1.5 * frame) {
    const auto target = static_cast<std::int64_t>((time - half) / base);
    if (const int rc = av_seek_frame(d.format.get(), d.index, target, AVSEEK_FLAG_BACKWARD);
        rc < 0) {
      return std::unexpected(std::format("cannot seek to {:.3f}s: {}", time, av_error_string(rc)));
    }
  }

  while (true) {
    const int rc = av_read_frame(d.format.get(), d.packet.get());
    if (rc < 0) {
      d.last.reset();
      return std::unexpected(std::format("no frame at {:.3f}s: {}", time, av_error_string(rc)));
    }
    if (d.packet->stream_index != d.index) {
      av_packet_unref(d.packet.get());
      continue;
    }
    const std::int64_t stamp = d.packet->pts != AV_NOPTS_VALUE ? d.packet->pts : d.packet->dts;
    const double shown = static_cast<double>(stamp) * base;
    if (shown < time - half) {
      av_packet_unref(d.packet.get());
      continue;
    }
    if (shown > time + half || (d.packet->flags & AV_PKT_FLAG_KEY) == 0) {
      av_packet_unref(d.packet.get());
      d.last.reset();
      return std::unexpected(std::format("no frame of its own at {:.3f}s", time));
    }
    std::vector<std::uint8_t> bytes(d.packet->data, d.packet->data + d.packet->size);
    av_packet_unref(d.packet.get());
    d.last = shown;
    return bytes;
  }
}

}  // namespace cutline::media
