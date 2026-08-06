#pragma once

/// Making a proxy: a small copy of a file that stands in for it while editing.
///
/// A proxy is the same footage — the same length, the same frames at the same
/// times, differing only in what a frame costs. Everything here exists to keep
/// that true, because a clip cutting against a proxy is not told it has one, and
/// a copy that runs even slightly short or slow would move every cut made
/// against it.
///
/// Video only, deliberately. Audio is decoded from the original wherever it is
/// wanted, because decoding audio is cheap enough that a smaller copy would buy
/// nothing, and a proxy with no audio stream cannot become the file somebody
/// hears by accident.

#include "cutline/media/encoder.hpp"

#include <expected>
#include <functional>
#include <string>
#include <string_view>

namespace cutline::media {

/// How tall a proxy is written. Width follows the source's aspect.
///
/// Around a quarter of the pixels of 1080p and a sixteenth of 4K, which is the
/// point: the machines that need proxies need the frame to be *much* cheaper,
/// not a little. Tall enough to still judge focus on, which is the thing a proxy
/// too small to see stops being useful for.
inline constexpr int kProxyHeight = 540;

/// Quality for the proxy encode, in the encoder's own scale.
///
/// Looser than an export, on purpose. A proxy is looked at and then thrown away,
/// and every step tighter is time spent making a file nobody keeps.
inline constexpr int kProxyQuality = 26;

/// How many decoding threads a proxy encode may use.
///
/// The same reasoning as the filmstrips: this runs behind an interface somebody
/// is using, and libav left to itself takes every core. A proxy that takes twice
/// as long in the background is a far better trade than a machine that feels
/// like it is falling over while it runs.
inline constexpr int kProxyThreads = 2;

struct ProxyOptions {
  int height = kProxyHeight;
  VideoCodec codec = VideoCodec::H264;
  int quality = kProxyQuality;
  int threads = kProxyThreads;

  /// Called as the encode advances, with how much of the source has been
  /// written, from 0 to 1. Returning false stops the encode.
  ///
  /// Progress and cancellation are one callback because they happen at the same
  /// moment and in the same place — anything asking to be told how far along it
  /// is has, by then, already earned the right to say "stop".
  std::function<bool(double)> on_progress;
};

enum class ProxyResult {
  Written,
  /// `on_progress` asked to stop. The partial file is removed, because a
  /// half-written proxy that survives is one that gets attached and cuts short.
  Cancelled,
};

/// Writes a proxy for `source` at `destination`, replacing whatever is there.
///
/// Frames are held to the source's frame rate rather than emitted one for one:
/// a variable-rate source is written as constant-rate by repeating and dropping,
/// so a frame at ten seconds in the proxy is the frame at ten seconds in the
/// original. Emitting each source frame in turn would be simpler and would make
/// a proxy that runs at the wrong speed the moment a camera dropped a frame.
[[nodiscard]] std::expected<ProxyResult, std::string> write_proxy(std::string_view source,
                                                                  std::string_view destination,
                                                                  const ProxyOptions& options = {});

/// Where a proxy for `source` goes: in `folder` when there is one, and
/// otherwise alongside the footage in a `Proxies` folder. Either way it takes
/// the source's name and an `.mp4` extension.
///
/// Beside the footage by default, because footage outlives projects — the same
/// card cut twice should not be transcoded twice — and because a drive fast
/// enough to hold the original is fast enough to read the proxy from. A folder
/// is offered because that is often not true: a card mounted read-only has
/// nowhere beside it to put anything, and a slow drive is exactly the case
/// proxies exist for.
///
/// Names collide in a chosen folder where they could not beside the footage —
/// two cards both holding `A001.MXF` are one file there — so a proxy written
/// somewhere else carries a short digest of the source's path.
[[nodiscard]] std::string default_proxy_path(std::string_view source,
                                             std::string_view folder = {});

}  // namespace cutline::media
