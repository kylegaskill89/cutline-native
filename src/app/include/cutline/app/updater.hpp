#pragma once

/// Fetching an update, on a thread that is not the one drawing.
///
/// The half of updating that touches the network. What it means — whether a
/// release is newer, whether a manifest can be trusted, whether a download is
/// the file it claims to be — is `editor::updates`, and is decided from text
/// with no sockets anywhere near it.
///
/// Nothing here installs anything. It fetches, it verifies, and it hands back a
/// path. Running the installer is the application's decision and the user's,
/// and it happens where the "are you sure" is.

#include "cutline/editor/updates.hpp"

#include <atomic>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace cutline::app {

/// Where the manifest lives.
///
/// GitHub redirects `/releases/latest/download/<asset>` to whatever the newest
/// release is, so this URL never has to change and nothing has to enumerate
/// releases or parse the API. The manifest itself is a few hundred bytes.
inline constexpr std::string_view kUpdateManifestUrl =
    "https://github.com/kylegaskill89/cutline-native/releases/latest/download/latest.json";

/// An update check or download in flight.
///
/// A worker with a notification, the same shape the waveform and thumbnail
/// caches use: the interface asks, carries on drawing, and is told. The one
/// difference is that this is a single job rather than a cache, so there is no
/// keying and no eviction — just a state anybody can read.
class Updater {
 public:
  enum class State {
    Idle,
    Checking,
    /// Finished, and there is nothing newer. Worth distinguishing from Idle:
    /// somebody who pressed the button is owed an answer, and "no news" looks
    /// exactly like "nothing happened".
    UpToDate,
    Available,
    Downloading,
    /// Downloaded and verified. `installer()` is the file.
    Ready,
    Failed,
  };

  Updater();
  ~Updater();

  Updater(const Updater&) = delete;
  Updater& operator=(const Updater&) = delete;

  /// Called from the worker when the state changes, so a message can be posted
  /// and the frame loop woken. Runs on the worker's thread: do the one thing
  /// that is safe from there and leave the rest to the message handler.
  void set_on_change(std::function<void()> on_change);

  /// Asks whether there is something newer than `current`. Returns at once.
  void check(editor::Version current, std::string url = std::string(kUpdateManifestUrl));

  /// Fetches the installer the last check found, into the temporary directory,
  /// and verifies its digest before reporting it ready.
  ///
  /// A download whose digest does not match is deleted rather than kept. There
  /// is no case where a file that failed that check is worth having on disk,
  /// and one left lying about beside a real one is an invitation.
  void download();

  [[nodiscard]] State state() const noexcept;
  /// 0 to 1 while downloading. Stays at 0 when the server sends no length,
  /// which is a bar that does not move rather than one that lies.
  [[nodiscard]] double progress() const noexcept;
  /// What the last check found. Only meaningful from `Available` onwards.
  [[nodiscard]] editor::Release found() const;
  /// The verified installer, once `Ready`.
  [[nodiscard]] std::filesystem::path installer() const;
  /// Why it failed, in a sentence somebody can act on.
  [[nodiscard]] std::string error() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Fetches a URL into memory. Exposed for the tool that publishes releases and
/// for anything else that needs one small file over https.
///
/// Follows redirects, which is the whole reason the manifest URL can be a
/// permanent one. Refuses anything that is not https, and anything larger than
/// `limit` — a check for a few hundred bytes should not be able to be answered
/// with a gigabyte.
[[nodiscard]] std::expected<std::string, std::string> fetch_text(std::string_view url,
                                                                 std::size_t limit = 1 << 20);

}  // namespace cutline::app
