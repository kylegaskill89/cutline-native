/// Deciding whether there is a newer version, and whether what arrived is what
/// was promised.
///
/// Worth more care than most of this codebase, because what is on the other end
/// of these decisions is an executable that will be run on somebody's machine.
/// Most of these are about what gets *refused*.

#include "cutline/editor/updates.hpp"

#include "cutline/core/version.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::editor {
namespace {

[[nodiscard]] std::vector<std::uint8_t> bytes_of(std::string_view text) {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

// ---------------------------------------------------------------- versions --

TEST(Version, ReadsTheOrdinaryForm) {
  const auto parsed = parse_version("1.2.3");
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed->major, 1);
  EXPECT_EQ(parsed->minor, 2);
  EXPECT_EQ(parsed->patch, 3);
}

// A git tag spells it with a v, and the tag is what a release is named after,
// so both forms arrive and both mean the same thing.
TEST(Version, ReadsATagAsWell) {
  EXPECT_EQ(parse_version("v0.4.1").value(), (Version{0, 4, 1}));
  EXPECT_EQ(parse_version("V0.4.1").value(), (Version{0, 4, 1}));
}

TEST(Version, RefusesAnythingItCannotBeSureOf) {
  for (const std::string_view bad : {"", "v", "1", "1.2", "1.2.3.4", "1.2.x", "one.two.three",
                                     "1.2.3-beta", " 1.2.3", "1.2.3 ", "-1.2.3"}) {
    EXPECT_FALSE(parse_version(bad).has_value()) << bad;
  }
}

TEST(Version, ComparesPartByPart) {
  EXPECT_LT((Version{0, 9, 9}), (Version{1, 0, 0}));
  EXPECT_LT((Version{1, 0, 9}), (Version{1, 1, 0}));
  EXPECT_LT((Version{1, 1, 1}), (Version{1, 1, 2}));
  // Not as text: "10" is bigger than "9" however it sorts.
  EXPECT_LT((Version{1, 9, 0}), (Version{1, 10, 0}));
}

TEST(Version, PrintsWhatItRead) {
  EXPECT_EQ(parse_version("2.10.0")->to_string(), "2.10.0");
}

TEST(Version, TheBuildKnowsItsOwn) {
  const auto parsed = parse_version(core::kVersion);
  ASSERT_TRUE(parsed.has_value()) << core::kVersion;
  EXPECT_EQ(parsed->major, core::kVersionMajor);
  EXPECT_EQ(parsed->minor, core::kVersionMinor);
  EXPECT_EQ(parsed->patch, core::kVersionPatch);
}

// Strictly newer. Equal is not an update, and older is somebody's mirror being
// out of date rather than an invitation to downgrade.
TEST(UpdateAvailable, OnlyForSomethingStrictlyNewer) {
  EXPECT_TRUE(update_available(Version{0, 1, 0}, Version{0, 2, 0}));
  EXPECT_FALSE(update_available(Version{0, 2, 0}, Version{0, 2, 0}));
  EXPECT_FALSE(update_available(Version{0, 3, 0}, Version{0, 2, 0}));
}

// ---------------------------------------------------------------- manifests --

constexpr std::string_view kDigest =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

[[nodiscard]] std::string manifest(std::string_view version, std::string_view installer,
                                   std::string_view digest = kDigest) {
  return std::string(R"({"version":")") + std::string(version) + R"(","installer":")" +
         std::string(installer) + R"(","sha256":")" + std::string(digest) +
         R"(","notes":"Fixed things."})";
}

TEST(ReleaseManifest, ReadsAWellFormedOne) {
  const auto release = parse_release_manifest(
      manifest("1.4.0", "https://example.com/Cutline-1.4.0-Setup.exe"));
  ASSERT_TRUE(release.has_value()) << release.error();

  EXPECT_EQ(release->version, (Version{1, 4, 0}));
  EXPECT_EQ(release->installer, "https://example.com/Cutline-1.4.0-Setup.exe");
  EXPECT_EQ(release->sha256, kDigest);
  EXPECT_EQ(release->notes, "Fixed things.");
}

// A plain-http installer can be replaced in transit by anybody on the path
// between here and the server, and this one is about to be run.
TEST(ReleaseManifest, RefusesAnInstallerThatIsNotOverHttps) {
  EXPECT_FALSE(
      parse_release_manifest(manifest("1.4.0", "http://example.com/Setup.exe")).has_value());
  EXPECT_FALSE(
      parse_release_manifest(manifest("1.4.0", "file:///C:/Setup.exe")).has_value());
  EXPECT_FALSE(parse_release_manifest(manifest("1.4.0", "//example.com/Setup.exe")).has_value());
}

TEST(ReleaseManifest, RefusesADigestThatIsNotOne) {
  const std::string_view url = "https://example.com/Setup.exe";
  EXPECT_FALSE(parse_release_manifest(manifest("1.4.0", url, "")).has_value());
  EXPECT_FALSE(parse_release_manifest(manifest("1.4.0", url, "abc123")).has_value());
  // The right length and not hex.
  EXPECT_FALSE(parse_release_manifest(manifest("1.4.0", url, std::string(64, 'z'))).has_value());
}

TEST(ReleaseManifest, RefusesOneWithSomethingMissing) {
  EXPECT_FALSE(parse_release_manifest("{}").has_value());
  EXPECT_FALSE(parse_release_manifest(R"({"version":"1.0.0"})").has_value());
  EXPECT_FALSE(
      parse_release_manifest(R"({"installer":"https://example.com/x.exe"})").has_value());
}

TEST(ReleaseManifest, RefusesSomethingThatIsNotAManifestAtAll) {
  EXPECT_FALSE(parse_release_manifest("").has_value());
  EXPECT_FALSE(parse_release_manifest("not json").has_value());
  EXPECT_FALSE(parse_release_manifest("[1,2,3]").has_value());
  // A 404 page, which is what a wrong URL actually returns.
  EXPECT_FALSE(parse_release_manifest("<!doctype html><title>Not Found</title>").has_value());
}

// A manifest written by a future version with extra fields still reads. The
// named fields are what is required; the rest is somebody else's business.
TEST(ReleaseManifest, IgnoresFieldsItDoesNotKnow) {
  const auto release = parse_release_manifest(
      R"({"version":"2.0.0","installer":"https://example.com/x.exe",
          "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "channel":"beta","minimum_windows":11})");
  ASSERT_TRUE(release.has_value()) << release.error();
  EXPECT_EQ(release->version, (Version{2, 0, 0}));
}

// Notes are the one optional field: a release with none is a release, and
// refusing an update over a missing paragraph would be absurd.
TEST(ReleaseManifest, AReleaseWithNoNotesIsStillARelease) {
  const auto release = parse_release_manifest(
      R"({"version":"2.0.0","installer":"https://example.com/x.exe",
          "sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"})");
  ASSERT_TRUE(release.has_value()) << release.error();
  EXPECT_TRUE(release->notes.empty());
}

// ----------------------------------------------------------------- digests --

// The published vectors. If these pass, the digest is the one the rest of the
// world computes, which is the only property that matters here.
TEST(Digest, MatchesTheKnownAnswers) {
  EXPECT_EQ(sha256_hex({}),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256_hex(bytes_of("abc")),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(sha256_hex(bytes_of("The quick brown fox jumps over the lazy dog")),
            "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

TEST(Digest, ADifferentByteIsADifferentDigest) {
  EXPECT_NE(sha256_hex(bytes_of("abc")), sha256_hex(bytes_of("abd")));
}

TEST(Digest, ComparisonIgnoresTheCaseOfTheHex) {
  EXPECT_TRUE(digest_matches("ABCD", "abcd"));
  EXPECT_TRUE(digest_matches("aBcD", "AbCd"));
}

TEST(Digest, NothingMatchesNothing) {
  // An empty expectation is a manifest that said nothing, and a file that
  // could not be hashed is an empty answer. Neither is a match.
  EXPECT_FALSE(digest_matches("", ""));
  EXPECT_FALSE(digest_matches(std::string(kDigest), ""));
  EXPECT_FALSE(digest_matches("", std::string(kDigest)));
}

TEST(Digest, ADifferentLengthIsNotAMatch) {
  EXPECT_FALSE(digest_matches("abcd", "abcde"));
}

}  // namespace
}  // namespace cutline::editor
