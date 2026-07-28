#pragma once

/// Internal helpers shared by the media sources. Not installed and not part of
/// the public interface — libav* types stay out of the headers so the rest of
/// the application never sees them.

#include <memory>
#include <string>

#include "cutline/media/av_headers.hpp"

namespace cutline::media::detail {

[[nodiscard]] inline std::string av_error_string(int code) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, buffer, sizeof(buffer));
  return buffer;
}

// libav* frees through pointer-to-pointer so it can null the caller's handle,
// which does not match unique_ptr's deleter signature; these bridge the two.

struct FormatContextDeleter {
  void operator()(AVFormatContext* ctx) const noexcept {
    if (ctx != nullptr) avformat_close_input(&ctx);
  }
};

/// Output contexts are freed, not closed: `avformat_close_input` tears down a
/// demuxer and would misread a muxer's fields. Closing the I/O here too means an
/// abandoned export leaves an obviously truncated file rather than a leaked
/// handle.
struct OutputFormatContextDeleter {
  void operator()(AVFormatContext* ctx) const noexcept {
    if (ctx == nullptr) return;
    if (ctx->oformat != nullptr && (ctx->oformat->flags & AVFMT_NOFILE) == 0 &&
        ctx->pb != nullptr) {
      avio_closep(&ctx->pb);
    }
    avformat_free_context(ctx);
  }
};

struct CodecContextDeleter {
  void operator()(AVCodecContext* ctx) const noexcept {
    if (ctx != nullptr) avcodec_free_context(&ctx);
  }
};

struct FrameDeleter {
  void operator()(AVFrame* frame) const noexcept {
    if (frame != nullptr) av_frame_free(&frame);
  }
};

struct PacketDeleter {
  void operator()(AVPacket* packet) const noexcept {
    if (packet != nullptr) av_packet_free(&packet);
  }
};

struct SwrDeleter {
  void operator()(SwrContext* ctx) const noexcept {
    if (ctx != nullptr) swr_free(&ctx);
  }
};

struct SwsDeleter {
  void operator()(SwsContext* ctx) const noexcept { sws_freeContext(ctx); }
};

using FormatContext = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using OutputFormatContext = std::unique_ptr<AVFormatContext, OutputFormatContextDeleter>;
using CodecContext = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
using Resampler = std::unique_ptr<SwrContext, SwrDeleter>;
using Scaler = std::unique_ptr<SwsContext, SwsDeleter>;

/// Maps an audio-stream ordinal — the N a clip stores, counting only audio
/// streams — onto libav's index across all streams. Returns -1 when absent.
[[nodiscard]] inline int audio_stream_index(const AVFormatContext* format, int ordinal) noexcept {
  int seen = 0;
  for (unsigned i = 0; i < format->nb_streams; ++i) {
    if (format->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
    if (seen == ordinal) return static_cast<int>(i);
    ++seen;
  }
  return -1;
}

}  // namespace cutline::media::detail
