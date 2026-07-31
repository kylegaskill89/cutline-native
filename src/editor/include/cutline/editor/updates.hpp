#pragma once

/// Knowing whether there is a newer version, and what to do about it.
///
/// The half that has no network in it. Whether a release is newer, whether a
/// manifest is one this build understands, and whether a download is the file
/// it claims to be are all decidable from text and are all tested here — which
/// matters more than usual, because the thing on the other end of this decision
/// is an executable that will be run on the user's machine.
///
/// Fetching lives in `app::updater`, above the media layer, because that is
/// where the threads and the sockets are.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// A three-part version, compared the way anybody would expect.
///
/// No pre-release tags and no build metadata: a video editor's releases are
/// numbers, and half of semantic versioning exists to describe libraries.
/// Anything unparseable is refused rather than guessed at — a manifest that
/// cannot be read must not be treated as newer than what is running.
struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;

  [[nodiscard]] std::string to_string() const;

  friend auto operator<=>(const Version&, const Version&) = default;
  friend bool operator==(const Version&, const Version&) = default;
};

/// Parses "1.2.3", with or without a leading "v". Nothing for anything else.
[[nodiscard]] std::expected<Version, std::string> parse_version(std::string_view text);

/// What a release manifest says.
struct Release {
  Version version;
  /// Where the installer is. Required, and required to be https — see
  /// `parse_release_manifest`.
  std::string installer;
  /// The installer's SHA-256, lower-case hex. Required.
  std::string sha256;
  /// What changed. Shown to the user before anything is downloaded.
  std::string notes;
};

/// Reads the manifest published beside a release.
///
/// Rejects rather than tolerates. This decides whether to download and run an
/// executable, so every field it needs must be present and well-formed:
///
///  * the URL must be **https**, because a plain-http installer can be replaced
///    in transit by anybody on the path;
///  * the digest must be present and the right length, because an unverified
///    download is a promise nobody made;
///  * the version must parse, because a version that does not is not newer.
///
/// A manifest written by a future version with extra fields still reads: the
/// ones named here are what is required, and the rest are ignored.
[[nodiscard]] std::expected<Release, std::string> parse_release_manifest(std::string_view json);

/// Whether `latest` is worth offering to somebody running `current`.
///
/// Strictly newer. Equal is not an update, and older is somebody's mirror being
/// out of date rather than an invitation to downgrade.
[[nodiscard]] bool update_available(const Version& current, const Version& latest) noexcept;

/// The digest of some bytes, as lower-case hex. Pure so the comparison can be
/// tested against a known answer rather than against whatever the machine
/// happened to download.
[[nodiscard]] std::string sha256_hex(const std::vector<std::uint8_t>& bytes);

/// Whether a download is the file the manifest described. Case-insensitive,
/// because hex is written both ways and neither is wrong.
[[nodiscard]] bool digest_matches(std::string_view expected, std::string_view actual) noexcept;

}  // namespace cutline::editor
