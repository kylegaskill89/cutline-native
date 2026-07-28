#pragma once

/// The libav* headers, included in one place with their warnings silenced.
///
/// FFmpeg's headers contain narrowing conversions that MSVC reports at /W4, and
/// the build treats warnings as errors. `/external:W0` is supposed to cover
/// third-party includes but does not reach these, so the suppression is done
/// here explicitly and narrowly: the pragma covers only what is between the
/// push and the pop, leaving our own code held to the full warning level.
///
/// This header is deliberately not in the public interface's spirit — nothing
/// outside the media layer and its tools should include it. It lives under
/// `include/` only because the debug tools need it too.

// MSVC: warning level 0 for everything in this region.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
