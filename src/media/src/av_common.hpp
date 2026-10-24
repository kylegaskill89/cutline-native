#pragma once

/// Internal helpers shared by the media sources. Not installed and not part of
/// the public interface — libav* types stay out of the headers so the rest of
/// the application never sees them.

#include <memory>
#include <string>

#include "cutline/media/av_headers.hpp"

namespace cutline::media::detail {

/// Quietens libav* to things somebody can act on, once per process.
///
/// It logs to stderr at warning level by default and is chatty about matters
/// nobody can do anything about — "UDTA parsing failed retrying raw" on
/// ordinary camera mp4s, once per open — and this application opens a file per
/// clip, per waveform, per filmstrip and per probe. The result was a console
/// scrolling past faster than a real error could be noticed in it, which is the
/// same as having no log at all.
///
/// Errors are kept. A file that genuinely will not open is worth saying.
inline void quiet_av_logging() noexcept {
  static const bool once = [] {
    av_log_set_level(AV_LOG_ERROR);
    return true;
  }();
  (void)once;
}

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

/// Whether a stream is footage rather than a picture filed alongside it.
///
/// Containers carry cover art as a video stream with the attached-picture
/// disposition: an mp3 with an album sleeve in its ID3 tag, an m4a or a flac
/// from any music library. libav reports those exactly as it reports a camera's
/// picture, so counting every video stream as footage made an mp3 with art a
/// *video* file — placed on a video track, sent to the picture decoder, given a
/// filmstrip of one frame — and the sound it actually holds never arrived as
/// sound.
[[nodiscard]] inline bool is_footage(const AVStream* stream) noexcept {
  return stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
         (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0;
}

/// The video stream worth decoding, or a negative number when there is none.
///
/// `av_find_best_stream` with the cover art taken out of the running. It does
/// not do that itself: art is a stream of one frame and is merely ranked low,
/// so in a file with nothing else it is the "best" and gets chosen.
[[nodiscard]] inline int best_footage_stream(AVFormatContext* format,
                                             const AVCodec** codec) noexcept {
  const int best = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, codec, 0);
  if (best < 0 || is_footage(format->streams[best])) return best;

  // The art won, so it was ranked against footage that has no decoder or
  // against nothing at all. Take the first footage stream that can be decoded.
  for (unsigned i = 0; i < format->nb_streams; ++i) {
    if (!is_footage(format->streams[i])) continue;
    const AVCodec* found = avcodec_find_decoder(format->streams[i]->codecpar->codec_id);
    if (found == nullptr) continue;
    if (codec != nullptr) *codec = found;
    return static_cast<int>(i);
  }
  if (codec != nullptr) *codec = nullptr;
  return AVERROR_STREAM_NOT_FOUND;
}

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
