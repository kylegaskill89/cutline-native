#include "cutline/core/time.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace cutline::core {
namespace {

TEST(FrameDuration, IsTheReciprocalOfFps) {
  EXPECT_DOUBLE_EQ(frame_duration(30.0), 1.0 / 30.0);
  EXPECT_DOUBLE_EQ(frame_duration(60.0), 1.0 / 60.0);
}

TEST(FrameDuration, ClampsFpsToAtLeastOne) {
  EXPECT_DOUBLE_EQ(frame_duration(0.0), 1.0);
  EXPECT_DOUBLE_EQ(frame_duration(-5.0), 1.0);
}

TEST(SnapToFrame, SnapsToTheNearestBoundary) {
  EXPECT_DOUBLE_EQ(snap_to_frame(0.51, 30.0), 0.5);
  EXPECT_DOUBLE_EQ(snap_to_frame(0.0, 30.0), 0.0);
  EXPECT_DOUBLE_EQ(snap_to_frame(2.0, 30.0), 2.0);
}

// Ties round toward +infinity, matching JavaScript's Math.round rather than
// std::round's round-half-away-from-zero.
TEST(SnapToFrame, TiesRoundUpward) {
  EXPECT_DOUBLE_EQ(snap_to_frame(1.0 / 60.0, 30.0), 1.0 / 30.0);
  EXPECT_DOUBLE_EQ(snap_to_frame(-1.0 / 60.0, 30.0), 0.0);
}

TEST(SecondsToTimestamp, FormatsHoursMinutesAndFractionalSeconds) {
  EXPECT_EQ(seconds_to_timestamp(0.0), "00:00:00.00");
  EXPECT_EQ(seconds_to_timestamp(5.2), "00:00:05.20");
  EXPECT_EQ(seconds_to_timestamp(61.0), "00:01:01.00");
  EXPECT_EQ(seconds_to_timestamp(3661.5), "01:01:01.50");
}

TEST(SecondsToTimestamp, ClampsNegativeAndNonFiniteInput) {
  EXPECT_EQ(seconds_to_timestamp(-10.0), "00:00:00.00");
  EXPECT_EQ(seconds_to_timestamp(std::numeric_limits<double>::quiet_NaN()), "00:00:00.00");
  EXPECT_EQ(seconds_to_timestamp(std::numeric_limits<double>::infinity()), "00:00:00.00");
}

TEST(SecondsToTimecode, CountsWholeFramesWithinTheSecond) {
  EXPECT_EQ(seconds_to_timecode(0.0, 30.0), "00:00:00:00");
  EXPECT_EQ(seconds_to_timecode(0.5, 30.0), "00:00:00:15");
  EXPECT_EQ(seconds_to_timecode(1.0, 30.0), "00:00:01:00");
  EXPECT_EQ(seconds_to_timecode(1.5, 30.0), "00:00:01:15");
  EXPECT_EQ(seconds_to_timecode(3600.0, 30.0), "01:00:00:00");
}

TEST(SecondsToTimecode, RoundsToTheNearestFrameFirst) {
  // 0.49 of a frame short of frame 30 still reads as frame 30.
  EXPECT_EQ(seconds_to_timecode(1.0 - 0.49 / 30.0, 30.0), "00:00:01:00");
  EXPECT_EQ(seconds_to_timecode(1.0 - 0.51 / 30.0, 30.0), "00:00:00:29");
}

TEST(SecondsToTimecode, HandlesDegenerateFrameRates) {
  EXPECT_EQ(seconds_to_timecode(2.0, 0.0), "00:00:02:00");
  EXPECT_EQ(seconds_to_timecode(2.0, std::numeric_limits<double>::quiet_NaN()),
            "00:00:02:00");
}

TEST(SecondsToTimecode, ClampsNegativeInput) {
  EXPECT_EQ(seconds_to_timecode(-5.0, 30.0), "00:00:00:00");
}

TEST(TimeToSeconds, ParsesEachSupportedShape) {
  EXPECT_DOUBLE_EQ(time_to_seconds("01:02:03.5"), 3723.5);
  EXPECT_DOUBLE_EQ(time_to_seconds("02:03"), 123.0);
  EXPECT_DOUBLE_EQ(time_to_seconds("7.25"), 7.25);
  EXPECT_DOUBLE_EQ(time_to_seconds("00:00:10"), 10.0);
}

TEST(TimeToSeconds, IgnoresSurroundingWhitespace) {
  EXPECT_DOUBLE_EQ(time_to_seconds("  12  "), 12.0);
  EXPECT_DOUBLE_EQ(time_to_seconds(" 01:30 "), 90.0);
}

TEST(TimeToSeconds, MalformedInputYieldsZero) {
  EXPECT_DOUBLE_EQ(time_to_seconds(""), 0.0);
  EXPECT_DOUBLE_EQ(time_to_seconds("   "), 0.0);
  EXPECT_DOUBLE_EQ(time_to_seconds("abc"), 0.0);
  EXPECT_DOUBLE_EQ(time_to_seconds("01:xx:03"), 0.0);
}

// An empty field parses as zero, so "1::2" is a well-formed hour/second pair.
TEST(TimeToSeconds, EmptyFieldsAreZero) {
  EXPECT_DOUBLE_EQ(time_to_seconds("1::2"), 3602.0);
}

TEST(ReadableFileSize, PicksTheLargestFittingUnit) {
  EXPECT_EQ(readable_file_size(0.0), "0.00 B");
  EXPECT_EQ(readable_file_size(512.0), "512.00 B");
  EXPECT_EQ(readable_file_size(1536.0), "1.50 KB");
  EXPECT_EQ(readable_file_size(12.0 * 1024 * 1024), "12.00 MB");
  EXPECT_EQ(readable_file_size(3.0 * 1024 * 1024 * 1024), "3.00 GB");
  EXPECT_EQ(readable_file_size(2.0 * 1024 * 1024 * 1024 * 1024), "2.00 TB");
}

TEST(ReadableFileSize, RejectsNegativeAndNonFiniteInput) {
  EXPECT_EQ(readable_file_size(-1.0), "Unknown Size");
  EXPECT_EQ(readable_file_size(std::numeric_limits<double>::quiet_NaN()), "Unknown Size");
}

// -------------------------------------------------- timecode, read back --

TEST(TimecodeToSeconds, IsTheInverseOfWritingOne) {
  for (const double seconds : {0.0, 1.0, 12.5, 61.0, 3723.0}) {
    const std::string written = seconds_to_timecode(seconds, 30.0);
    const std::optional<double> read = timecode_to_seconds(written, 30.0);
    ASSERT_TRUE(read.has_value()) << written;
    EXPECT_NEAR(*read, seconds, 1e-9) << written;
  }
}

TEST(TimecodeToSeconds, TheLastFieldIsFramesRatherThanHundredths) {
  // The whole reason this is not `time_to_seconds`: at 30 fps, reading
  // "00:00:01:15" as a second and fifteen hundredths is half a second out.
  const std::optional<double> read = timecode_to_seconds("00:00:01:15", 30.0);
  ASSERT_TRUE(read.has_value());
  EXPECT_NEAR(*read, 1.5, 1e-9);
}

TEST(TimecodeToSeconds, FewerFieldsCountFromTheRight) {
  // How anybody types into one of these in a hurry.
  EXPECT_NEAR(*timecode_to_seconds("15", 30.0), 0.5, 1e-9);
  EXPECT_NEAR(*timecode_to_seconds("2:15", 30.0), 2.5, 1e-9);
  EXPECT_NEAR(*timecode_to_seconds("1:00:00", 30.0), 60.0, 1e-9);
}

TEST(TimecodeToSeconds, AFullStopMeansSecondsWereMeant) {
  EXPECT_NEAR(*timecode_to_seconds("1.5", 30.0), 1.5, 1e-9);
}

TEST(TimecodeToSeconds, TheResultLandsOnAFrame) {
  // A hundredth past a second is still frame 30 at thirty a second.
  const std::optional<double> read = timecode_to_seconds("1.01", 30.0);
  ASSERT_TRUE(read.has_value());
  EXPECT_NEAR(*read, 1.0, 1e-9);
}

TEST(TimecodeToSeconds, NonsenseIsNothingRatherThanZero) {
  // Zero would send the playhead to the start of the sequence on a typing
  // mistake, which is a long way from where somebody was.
  EXPECT_FALSE(timecode_to_seconds("", 30.0).has_value());
  EXPECT_FALSE(timecode_to_seconds("half past", 30.0).has_value());
  EXPECT_FALSE(timecode_to_seconds("1:2:3:4:5", 30.0).has_value());
  EXPECT_FALSE(timecode_to_seconds("-5", 30.0).has_value());
}

TEST(TimecodeToSeconds, TheFrameRateIsWhatTheLastFieldMeans) {
  EXPECT_NEAR(*timecode_to_seconds("00:00:00:12", 24.0), 0.5, 1e-9);
  EXPECT_NEAR(*timecode_to_seconds("00:00:00:12", 48.0), 0.25, 1e-9);
}

// -------------------------------------------------------------- drop frame --

TEST(DropFrame, OnlyTheRatesThatCanDriftSupportIt) {
  // At 25 or 30 the timecode already counts real seconds. Offering the choice
  // there would be offering two names for one thing.
  EXPECT_TRUE(supports_drop_frame(30000.0 / 1001.0));
  EXPECT_TRUE(supports_drop_frame(60000.0 / 1001.0));
  EXPECT_TRUE(supports_drop_frame(29.97));
  EXPECT_FALSE(supports_drop_frame(30.0));
  EXPECT_FALSE(supports_drop_frame(25.0));
  EXPECT_FALSE(supports_drop_frame(24000.0 / 1001.0)) << "23.976 has no drop-frame form";
  EXPECT_FALSE(supports_drop_frame(0.0));
}

TEST(DropFrame, EveryFrameGetsItsOwnNumber) {
  // The bug this replaced: counting the frame index at the *nominal* rate meant
  // 29.97 skipped a timecode number about every thousand frames, so the readout
  // stuttered while the picture did not.
  constexpr double fps = 30000.0 / 1001.0;
  std::set<std::string> seen;
  std::string previous;
  for (int frame = 0; frame < 3000; ++frame) {
    const std::string shown = seconds_to_timecode(frame / fps, fps);
    EXPECT_TRUE(seen.insert(shown).second) << "two frames share " << shown;
    previous = shown;
  }
  EXPECT_EQ(seen.size(), 3000u);
}

TEST(DropFrame, NonDropFallsBehindTheClockAndThatIsCorrect) {
  // An hour of 29.97 is 107892 frames, which counted at thirty is four minutes
  // short of a whole hour by three and a half seconds. Drop-frame exists
  // because of exactly this number.
  constexpr double fps = 30000.0 / 1001.0;
  EXPECT_EQ(seconds_to_timecode(3600.0, fps), "00:59:56:12");
}

TEST(DropFrame, DropFrameTracksTheClock) {
  constexpr double fps = 30000.0 / 1001.0;
  EXPECT_EQ(seconds_to_timecode(3600.0, fps, true), "01:00:00;00");
  EXPECT_EQ(seconds_to_timecode(0.0, fps, true), "00:00:00;00");
}

TEST(DropFrame, TheNumbersDroppedAreTheFirstTwoOfNineMinutesInTen) {
  // The rule itself, at the boundary where it bites. One minute in, the count
  // jumps from :29 straight to ;02 — frames 00 and 01 of that minute have no
  // number at all.
  // In frame indices rather than in round seconds: a minute of drop-frame
  // timecode is 1800 frames of the sequence, and 1800/29.97 is 60.06 seconds
  // of clock — writing the test in seconds is how you test the wrong frame.
  constexpr double fps = 30000.0 / 1001.0;
  EXPECT_EQ(seconds_to_timecode(1799 / fps, fps, true), "00:00:59;29");
  EXPECT_EQ(seconds_to_timecode(1800 / fps, fps, true), "00:01:00;02");
}

TEST(DropFrame, TheTenthMinuteKeepsItsNumbers) {
  // The exception that stops the correction overshooting: every tenth minute
  // drops nothing, which is why the drift comes out at zero over ten.
  // Nine minutes have each lost two numbers by here, so ten minutes of
  // timecode is 18000 - 18 frames of sequence.
  constexpr double fps = 30000.0 / 1001.0;
  EXPECT_EQ(seconds_to_timecode(17982 / fps, fps, true), "00:10:00;00");
  EXPECT_EQ(seconds_to_timecode(17981 / fps, fps, true), "00:09:59;29");
}

TEST(DropFrame, TheSeparatorSaysWhichItIs) {
  constexpr double fps = 30000.0 / 1001.0;
  EXPECT_NE(seconds_to_timecode(5.0, fps, true).find(';'), std::string::npos);
  EXPECT_EQ(seconds_to_timecode(5.0, fps, false).find(';'), std::string::npos);
  // And asking for it where it means nothing gives the ordinary form rather
  // than a semicolon that claims something untrue.
  EXPECT_EQ(seconds_to_timecode(5.0, 30.0, true).find(';'), std::string::npos);
}

TEST(DropFrame, ReadingBackGivesTheFrameItCameFrom) {
  // The round trip is what a timecode field lives or dies by: typing back what
  // is displayed has to land on the same frame.
  constexpr double fps = 30000.0 / 1001.0;
  for (const int frame : {0, 1, 29, 30, 1799, 1800, 1801, 17981, 17982, 107891}) {
    const double at = frame / fps;
    for (const bool drop : {false, true}) {
      const std::string shown = seconds_to_timecode(at, fps, drop);
      const auto read = timecode_to_seconds(shown, fps, drop);
      ASSERT_TRUE(read.has_value()) << shown;
      EXPECT_NEAR(*read, at, 1.0 / fps * 0.5) << shown << " read back as a different frame";
    }
  }
}

TEST(DropFrame, EitherSeparatorIsAccepted) {
  // What decides how the digits are read is the setting, not the punctuation.
  constexpr double fps = 30000.0 / 1001.0;
  EXPECT_EQ(timecode_to_seconds("00:01:00;02", fps, true),
            timecode_to_seconds("00:01:00:02", fps, true));
}

TEST(DropFrame, AnIntegerRateIsUntouchedByAnyOfThis) {
  // The change that matters most: nothing about 24, 25, 30 or 60 moved.
  EXPECT_EQ(seconds_to_timecode(3600.0, 30.0), "01:00:00:00");
  EXPECT_EQ(seconds_to_timecode(1.5, 30.0), "00:00:01:15");
  EXPECT_EQ(seconds_to_timecode(0.5, 24.0), "00:00:00:12");
  EXPECT_NEAR(*timecode_to_seconds("00:00:01:15", 30.0), 1.5, 1e-9);
}

}  // namespace
}  // namespace cutline::core
