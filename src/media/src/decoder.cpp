#include "cutline/media/decoder.hpp"

#include "av_common.hpp"

// Pulls in d3d12.h, so it gets the same warning treatment as the rest of libav.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
extern "C" {
#include <libavutil/hwcontext_d3d12va.h>
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <format>
#include <utility>

namespace cutline::media {
namespace {

using detail::av_error_string;

[[nodiscard]] double rational_to_double(AVRational r) noexcept {
  return r.den == 0 ? 0.0 : static_cast<double>(r.num) / static_cast<double>(r.den);
}

/// Chosen by libavcodec when a decoder offers several output formats. Returning
/// the hardware format keeps frames on the GPU.
///
/// Falling through to libav's own choice matters: returning a hardware format
/// we have no device for would fail at the first frame, long after `open`
/// reported success.
AVPixelFormat pick_hardware_format(AVCodecContext* ctx, const AVPixelFormat* formats) {
  const auto wanted = static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(ctx->opaque));
  for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
    if (*f == wanted) return *f;
  }
  return avcodec_default_get_format(ctx, formats);
}

/// The pixel format this codec hands back when decoded through `type`, or
/// AV_PIX_FMT_NONE when it cannot be. Asking up front is what lets `open`
/// report the acceleration it actually got rather than the one it hoped for.
[[nodiscard]] AVPixelFormat hardware_pixel_format(const AVCodec* codec,
                                                  AVHWDeviceType type) noexcept {
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
    if (config == nullptr) return AV_PIX_FMT_NONE;
    if (config->device_type == type &&
        (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
      return config->pix_fmt;
    }
  }
}

/// Builds a D3D12VA device context around an existing `ID3D12Device`.
///
/// `av_hwdevice_ctx_create` would make its own device, and frames on a device
/// the compositor does not own have to be shared or copied to be drawn.
/// Allocating the context by hand and filling in the device is what keeps the
/// decoder and the compositor on the same one.
[[nodiscard]] AVBufferRef* d3d12_context_for(void* device) {
  if (device == nullptr) return nullptr;

  AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D12VA);
  if (ref == nullptr) return nullptr;

  auto* context = reinterpret_cast<AVHWDeviceContext*>(ref->data);
  auto* d3d12 = static_cast<AVD3D12VADeviceContext*>(context->hwctx);
  d3d12->device = static_cast<ID3D12Device*>(device);
  // The context releases this on teardown whether or not it allocated it, so
  // the reference has to be ours to give away.
  d3d12->device->AddRef();

  if (av_hwdevice_ctx_init(ref) < 0) {
    av_buffer_unref(&ref);
    return nullptr;
  }
  return ref;
}

}  // namespace

std::string_view to_string(Acceleration acceleration) noexcept {
  switch (acceleration) {
    case Acceleration::D3D12Va:
      return "d3d12va";
    case Acceleration::D3D11Va:
      return "d3d11va";
    default:
      return "software";
  }
}

struct VideoDecoder::Impl {
  AVFormatContext* format = nullptr;
  AVCodecContext* codec = nullptr;
  AVBufferRef* hw_device = nullptr;
  AVFrame* decoded = nullptr;
  AVPacket* packet = nullptr;

  int stream_index = -1;
  AVRational time_base{1, 1};
  VideoStreamInfo info;
  Acceleration acceleration = Acceleration::Software;

  double timestamp = 0.0;
  bool have_frame = false;
  bool draining = false;
  bool finished = false;

  ~Impl() {
    if (decoded != nullptr) av_frame_free(&decoded);
    if (packet != nullptr) av_packet_free(&packet);
    if (codec != nullptr) avcodec_free_context(&codec);
    if (hw_device != nullptr) av_buffer_unref(&hw_device);
    if (format != nullptr) avformat_close_input(&format);
  }
};

VideoDecoder::VideoDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VideoDecoder::~VideoDecoder() = default;

std::expected<std::unique_ptr<VideoDecoder>, std::string> VideoDecoder::open(std::string_view path,
                                                                            Options options) {
  const std::string path_string(path);
  auto impl = std::make_unique<Impl>();

  detail::quiet_av_logging();
  if (const int rc = avformat_open_input(&impl->format, path_string.c_str(), nullptr, nullptr);
      rc < 0) {
    return std::unexpected(std::format("cannot open {}: {}", path_string, av_error_string(rc)));
  }
  if (const int rc = avformat_find_stream_info(impl->format, nullptr); rc < 0) {
    return std::unexpected(
        std::format("cannot read streams in {}: {}", path_string, av_error_string(rc)));
  }

  const AVCodec* codec = nullptr;
  const int index =
      av_find_best_stream(impl->format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  if (index < 0 || codec == nullptr) {
    return std::unexpected(std::format("{} has no decodable video stream", path_string));
  }

  impl->stream_index = index;
  AVStream* stream = impl->format->streams[index];
  impl->time_base = stream->time_base;

  impl->codec = avcodec_alloc_context3(codec);
  if (impl->codec == nullptr) return std::unexpected("out of memory allocating a decoder");
  if (const int rc = avcodec_parameters_to_context(impl->codec, stream->codecpar); rc < 0) {
    return std::unexpected(std::format("cannot configure decoder: {}", av_error_string(rc)));
  }

  // Hardware first, then software. Support varies by codec, driver, and file,
  // so falling back here is routine rather than exceptional — and it is checked
  // before opening, so the reported acceleration is the real one.
  const auto try_hardware = [&](AVHWDeviceType type, Acceleration kind, void* device) {
    if (impl->acceleration != Acceleration::Software) return;

    const AVPixelFormat hw_format = hardware_pixel_format(codec, type);
    if (hw_format == AV_PIX_FMT_NONE) return;

    if (device != nullptr) {
      impl->hw_device = d3d12_context_for(device);
    } else if (av_hwdevice_ctx_create(&impl->hw_device, type, nullptr, nullptr, 0) < 0) {
      impl->hw_device = nullptr;
    }
    if (impl->hw_device == nullptr) return;

    impl->codec->hw_device_ctx = av_buffer_ref(impl->hw_device);
    impl->codec->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(hw_format));
    impl->codec->get_format = pick_hardware_format;
    impl->acceleration = kind;
  };

  if (options.preferred == Acceleration::D3D12Va) {
    try_hardware(AV_HWDEVICE_TYPE_D3D12VA, Acceleration::D3D12Va, options.d3d12_device);
    // Falling back to D3D11 rather than straight to the CPU: it still decodes
    // on the GPU, and a copy across devices beats a software decode.
    try_hardware(AV_HWDEVICE_TYPE_D3D11VA, Acceleration::D3D11Va, nullptr);
  } else if (options.preferred == Acceleration::D3D11Va) {
    try_hardware(AV_HWDEVICE_TYPE_D3D11VA, Acceleration::D3D11Va, nullptr);
  }
  if (impl->acceleration == Acceleration::Software) {
    impl->codec->thread_count = options.threads;
  }

  if (const int rc = avcodec_open2(impl->codec, codec, nullptr); rc < 0) {
    return std::unexpected(std::format("cannot open decoder: {}", av_error_string(rc)));
  }

  impl->decoded = av_frame_alloc();
  impl->packet = av_packet_alloc();
  if (impl->decoded == nullptr || impl->packet == nullptr) {
    return std::unexpected("out of memory allocating decoder buffers");
  }

  const auto pixel_format = static_cast<AVPixelFormat>(stream->codecpar->format);
  const char* pixel_name = av_get_pix_fmt_name(pixel_format);
  impl->info.index = index;
  impl->info.width = stream->codecpar->width;
  impl->info.height = stream->codecpar->height;
  impl->info.fps = rational_to_double(stream->avg_frame_rate);
  impl->info.codec = codec->name != nullptr ? codec->name : "";
  impl->info.pixel_format = pixel_name == nullptr ? "" : pixel_name;

  return std::unique_ptr<VideoDecoder>(new VideoDecoder(std::move(impl)));
}

std::expected<bool, std::string> VideoDecoder::next_frame() {
  Impl& d = *impl_;
  d.have_frame = false;
  if (d.finished) return false;

  while (true) {
    // Take anything the decoder already has before feeding it more. A single
    // packet can yield several frames, and B-frames mean output lags input.
    const int rc = avcodec_receive_frame(d.codec, d.decoded);
    if (rc == 0) {
      const int64_t pts = d.decoded->best_effort_timestamp != AV_NOPTS_VALUE
                              ? d.decoded->best_effort_timestamp
                              : d.decoded->pts;
      d.timestamp = pts == AV_NOPTS_VALUE ? 0.0 : static_cast<double>(pts) * av_q2d(d.time_base);
      d.have_frame = true;
      return true;
    }
    if (rc == AVERROR_EOF) {
      d.finished = true;
      return false;
    }
    if (rc != AVERROR(EAGAIN)) {
      return std::unexpected(std::format("decode failed: {}", av_error_string(rc)));
    }

    if (d.draining) {
      d.finished = true;
      return false;
    }

    // The decoder wants more input.
    const int read = av_read_frame(d.format, d.packet);
    if (read == AVERROR_EOF) {
      // Flush: the decoder still holds reordered frames.
      avcodec_send_packet(d.codec, nullptr);
      d.draining = true;
      continue;
    }
    if (read < 0) {
      return std::unexpected(std::format("read failed: {}", av_error_string(read)));
    }

    if (d.packet->stream_index == d.stream_index) {
      const int sent = avcodec_send_packet(d.codec, d.packet);
      // EAGAIN here would mean the decoder still has output pending, which the
      // receive above has already drained. Treating it as an error rather than
      // dropping the packet keeps a silent decode corruption from being
      // possible at all.
      if (sent < 0) {
        av_packet_unref(d.packet);
        return std::unexpected(std::format("cannot feed decoder: {}", av_error_string(sent)));
      }
    }
    av_packet_unref(d.packet);
  }
}

const AVFrame* VideoDecoder::frame() const noexcept {
  return impl_->have_frame ? impl_->decoded : nullptr;
}

double VideoDecoder::timestamp() const noexcept { return impl_->timestamp; }

std::expected<void, std::string> VideoDecoder::seek(double seconds) {
  Impl& d = *impl_;
  const auto target = static_cast<int64_t>(seconds / av_q2d(d.time_base));

  if (const int rc = av_seek_frame(d.format, d.stream_index, target, AVSEEK_FLAG_BACKWARD);
      rc < 0) {
    return std::unexpected(std::format("seek failed: {}", av_error_string(rc)));
  }

  avcodec_flush_buffers(d.codec);
  d.have_frame = false;
  d.draining = false;
  d.finished = false;
  return {};
}

std::optional<HardwareTexture> VideoDecoder::hardware_texture() const noexcept {
  const AVFrame* frame = impl_->decoded;
  if (frame == nullptr || frame->format != AV_PIX_FMT_D3D12) return std::nullopt;

  // For D3D12VA, data[0] points at the frame descriptor rather than at pixels.
  const auto* descriptor = reinterpret_cast<const AVD3D12VAFrame*>(frame->data[0]);
  if (descriptor == nullptr || descriptor->texture == nullptr) return std::nullopt;

  return HardwareTexture{
      .resource = descriptor->texture,
      .subresource = descriptor->subresource_index,
      .fence = descriptor->sync_ctx.fence,
      .fence_value = descriptor->sync_ctx.fence_value,
  };
}

Acceleration VideoDecoder::acceleration() const noexcept { return impl_->acceleration; }

const VideoStreamInfo& VideoDecoder::stream() const noexcept { return impl_->info; }

}  // namespace cutline::media
