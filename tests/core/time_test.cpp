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

}  // namespace
}  // namespace cutline::core
