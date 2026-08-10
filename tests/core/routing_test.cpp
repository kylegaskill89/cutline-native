#include "cutline/core/routing.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/serialize.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace cutline::core {
namespace {

/// A project with one video track and two audio tracks, which is the ordinary
/// arrangement and the one where the two numberings disagree.
Project sequence() {
  Project p;
  Track v;
  v.id = "v1";
  v.kind = TrackKind::Video;
  p.sequence().tracks.push_back(v);
  for (const char* id : {"a1", "a2"}) {
    Track a;
    a.id = id;
    a.kind = TrackKind::Audio;
    p.sequence().tracks.push_back(a);
  }
  return p;
}

/// The id of the submix `add_submix_track` just appended.
const std::string& last_id(const Project& p) { return p.sequence().tracks.back().id; }

// ------------------------------------------------------------- the routes --

TEST(Submix, ANewOneIsAnAudioTrackWithNoClips) {
  const Project p = add_submix_track(sequence(), "Dialogue");
  const Track& bus = p.sequence().tracks.back();
  EXPECT_TRUE(bus.submix);
  EXPECT_EQ(bus.kind, TrackKind::Audio) << "a bus has a fader, a stack and a meter like any lane";
  EXPECT_EQ(bus.label, "Dialogue");
  EXPECT_TRUE(bus.clips.empty());
  EXPECT_TRUE(has_submixes(p));
}

TEST(Submix, AProjectWithoutOneSaysSo) {
  EXPECT_FALSE(has_submixes(sequence()));
}

TEST(TrackOutput, NothingIsTheMaster) {
  Project p = add_submix_track(sequence(), "Dialogue");
  const std::string bus = last_id(p);
  p = set_track_output(std::move(p), "a1", bus);
  ASSERT_EQ(p.sequence().tracks[1].output, bus);

  p = set_track_output(std::move(p), "a1", "");
  EXPECT_TRUE(p.sequence().tracks[1].output.empty());
  EXPECT_EQ(routed_output(p, p.sequence().tracks[1]), nullptr) << "empty means the master";
}

TEST(TrackOutput, OnlyASubmixCanBeFed) {
  Project p = set_track_output(sequence(), "a1", "a2");
  EXPECT_TRUE(p.sequence().tracks[1].output.empty()) << "an ordinary lane has clips, not an input";
}

TEST(TrackOutput, ATrackCannotFeedItself) {
  Project p = sequence();
  p.sequence().tracks[1].submix = true;
  p = set_track_output(std::move(p), "a1", "a1");
  EXPECT_TRUE(p.sequence().tracks[1].output.empty());
}

TEST(TrackOutput, ARouteThatWouldCloseALoopIsRefused) {
  Project p = add_submix_track(add_submix_track(sequence(), "One"), "Two");
  const std::string one = p.sequence().tracks[3].id;
  const std::string two = p.sequence().tracks[4].id;

  p = set_track_output(std::move(p), one, two);
  ASSERT_EQ(p.sequence().tracks[3].output, two);

  p = set_track_output(std::move(p), two, one);
  EXPECT_TRUE(p.sequence().tracks[4].output.empty()) << "two already feeds one";
}

TEST(TrackOutput, ASubmixThatHasGoneIsReadAsTheMaster) {
  Project p = add_submix_track(sequence(), "Dialogue");
  p = set_track_output(std::move(p), "a1", last_id(p));
  p.sequence().tracks.pop_back();  // the bus is deleted underneath the route

  EXPECT_FALSE(p.sequence().tracks[1].output.empty()) << "the route is kept, so undo can bring it back";
  EXPECT_EQ(routed_output(p, p.sequence().tracks[1]), nullptr) << "but it is heard on the master meanwhile";
}

TEST(TrackOutput, ALoopArrivingInAFileIsReadAsTheMaster) {
  // Not buildable through `set_track_output`, and a file can say anything.
  Project p = add_submix_track(add_submix_track(sequence(), "One"), "Two");
  p.sequence().tracks[3].output = p.sequence().tracks[4].id;
  p.sequence().tracks[4].output = p.sequence().tracks[3].id;

  EXPECT_EQ(routed_output(p, p.sequence().tracks[3]), nullptr);
  EXPECT_EQ(routed_output(p, p.sequence().tracks[4]), nullptr);
}

// -------------------------------------------------------------------sends --

TEST(Sends, OneIsAddedAndThenChangedRatherThanDoubled) {
  Project p = add_submix_track(sequence(), "Reverb");
  const std::string bus = last_id(p);

  p = set_send(std::move(p), "a1", bus, 0.25);
  ASSERT_EQ(p.sequence().tracks[1].sends.size(), 1u);
  EXPECT_DOUBLE_EQ(p.sequence().tracks[1].sends[0].level, 0.25);
  EXPECT_FALSE(p.sequence().tracks[1].sends[0].pre_fader) << "post-fader is Premiere's default";

  p = set_send(std::move(p), "a1", bus, 0.5, true);
  ASSERT_EQ(p.sequence().tracks[1].sends.size(), 1u) << "the same destination is one send, not two";
  EXPECT_DOUBLE_EQ(p.sequence().tracks[1].sends[0].level, 0.5);
  EXPECT_TRUE(p.sequence().tracks[1].sends[0].pre_fader);

  p = remove_send(std::move(p), "a1", bus);
  EXPECT_TRUE(p.sequence().tracks[1].sends.empty());
}

TEST(Sends, ASendCountsAsARouteWhenLoopsAreChecked) {
  Project p = add_submix_track(add_submix_track(sequence(), "One"), "Two");
  const std::string one = p.sequence().tracks[3].id;
  const std::string two = p.sequence().tracks[4].id;

  p = set_send(std::move(p), one, two, 0.5);
  ASSERT_EQ(p.sequence().tracks[3].sends.size(), 1u);

  EXPECT_FALSE(can_route(p, two, one)) << "a send round the other way closes the loop just as an "
                                          "output would, and is easier to build by accident";
  p = set_track_output(std::move(p), two, one);
  EXPECT_TRUE(p.sequence().tracks[4].output.empty());
}

TEST(Sends, ATrackCanFeedTwoBusesAtOnce) {
  Project p = add_submix_track(add_submix_track(sequence(), "Reverb"), "Delay");
  const std::string reverb = p.sequence().tracks[3].id;
  const std::string delay = p.sequence().tracks[4].id;

  p = set_send(std::move(p), "a1", reverb, 0.2);
  p = set_send(std::move(p), "a1", delay, 0.1);
  EXPECT_EQ(p.sequence().tracks[1].sends.size(), 2u);
}

// -------------------------------------------------------------- bus order --

TEST(BusRoutes, WithoutSubmixesTheOrderIsTheOrderItAlwaysWas) {
  const std::vector<BusRoute> routes = bus_routes(sequence());
  ASSERT_EQ(routes.size(), 2u);
  EXPECT_EQ(routes[0].lane, 0);
  EXPECT_EQ(routes[1].lane, 1);
  EXPECT_EQ(routes[0].output_lane, -1) << "-1 is the master";
  EXPECT_FALSE(routes[0].submix);
}

TEST(BusRoutes, LanesCountAudioTracksAndTrackIndicesCountAll) {
  const std::vector<BusRoute> routes = bus_routes(sequence());
  ASSERT_EQ(routes.size(), 2u);
  // The confusion that once left a meter dark: the video track is index 0 and
  // no lane at all.
  EXPECT_EQ(routes[0].lane, 0);
  EXPECT_EQ(routes[0].track_index, 1);
  EXPECT_EQ(routes[1].lane, 1);
  EXPECT_EQ(routes[1].track_index, 2);
}

TEST(BusRoutes, ABusComesAfterEverythingThatFeedsIt) {
  Project p = add_submix_track(sequence(), "Dialogue");
  const std::string bus = last_id(p);
  p = set_track_output(std::move(p), "a1", bus);
  p = set_track_output(std::move(p), "a2", bus);

  const std::vector<BusRoute> routes = bus_routes(p);
  ASSERT_EQ(routes.size(), 3u);
  EXPECT_EQ(routes[0].lane, 0);
  EXPECT_EQ(routes[1].lane, 1);
  EXPECT_EQ(routes[2].lane, 2) << "the bus is run last, once both lanes have poured into it";
  EXPECT_EQ(routes[0].output_lane, 2);
  EXPECT_EQ(routes[1].output_lane, 2);
  EXPECT_EQ(routes[2].output_lane, -1);
  EXPECT_TRUE(routes[2].submix);
}

TEST(BusRoutes, ABusFeedingABusIsOrderedTheWholeWayDown) {
  // The submix is added first, so lane order and feed order disagree — which is
  // the case that tells the two apart.
  Project p = add_submix_track(sequence(), "Stems");
  const std::string stems = p.sequence().tracks[3].id;
  p = add_submix_track(std::move(p), "Dialogue");
  const std::string dialogue = p.sequence().tracks[4].id;

  p = set_track_output(std::move(p), "a1", dialogue);
  p = set_track_output(std::move(p), dialogue, stems);

  const std::vector<BusRoute> routes = bus_routes(p);
  ASSERT_EQ(routes.size(), 4u);
  const auto position_of = [&](int lane) {
    for (std::size_t i = 0; i < routes.size(); ++i) {
      if (routes[i].lane == lane) return static_cast<int>(i);
    }
    return -1;
  };
  EXPECT_LT(position_of(0), position_of(3)) << "a1 before the dialogue bus";
  EXPECT_LT(position_of(3), position_of(2)) << "the dialogue bus before the stems bus";
}

TEST(BusRoutes, ASendIsResolvedToALaneAndKeepsItsLevel) {
  Project p = add_submix_track(sequence(), "Reverb");
  p = set_send(std::move(p), "a1", last_id(p), 0.3, true);

  const std::vector<BusRoute> routes = bus_routes(p);
  ASSERT_EQ(routes.size(), 3u);
  ASSERT_EQ(routes[0].sends.size(), 1u);
  EXPECT_EQ(routes[0].sends[0].to_lane, 2);
  EXPECT_DOUBLE_EQ(routes[0].sends[0].level, 0.3);
  EXPECT_TRUE(routes[0].sends[0].pre_fader);
}

TEST(BusRoutes, ASendToNowhereIsDropped) {
  Project p = sequence();
  p.sequence().tracks[1].sends.push_back(Send{.to = "gone", .level = 0.5});
  const std::vector<BusRoute> routes = bus_routes(p);
  ASSERT_EQ(routes.size(), 2u);
  EXPECT_TRUE(routes[0].sends.empty());
}

TEST(BusRoutes, EveryLaneComesBackEvenWhenAFileIsLooped) {
  Project p = add_submix_track(add_submix_track(sequence(), "One"), "Two");
  p.sequence().tracks[3].output = p.sequence().tracks[4].id;
  p.sequence().tracks[4].output = p.sequence().tracks[3].id;

  const std::vector<BusRoute> routes = bus_routes(p);
  EXPECT_EQ(routes.size(), 4u) << "a bus heard on the master beats a bus never heard";
}

// -------------------------------------------------------------- mute, solo --

TEST(BusAudibility, SoloingABusSolosWhatFeedsIt) {
  Project p = add_submix_track(sequence(), "Dialogue");
  const std::string bus = last_id(p);
  p = set_track_output(std::move(p), "a1", bus);
  p.sequence().tracks.back().solo = true;

  EXPECT_TRUE(is_track_audible(p, p.sequence().tracks[1])) << "a1 feeds the soloed bus";
  EXPECT_FALSE(is_track_audible(p, p.sequence().tracks[2])) << "a2 goes straight to the master";
  EXPECT_TRUE(is_track_audible(p, p.sequence().tracks.back()));
}

TEST(BusAudibility, ASendIntoASoloedBusIsEnough) {
  Project p = add_submix_track(sequence(), "Reverb");
  p = set_send(std::move(p), "a1", last_id(p), 0.3);
  p.sequence().tracks.back().solo = true;

  EXPECT_TRUE(is_track_audible(p, p.sequence().tracks[1]));
}

TEST(BusAudibility, AMutedBusIsNotHeardHoweverItIsFed) {
  Project p = add_submix_track(sequence(), "Dialogue");
  p.sequence().tracks.back().muted = true;
  EXPECT_FALSE(is_track_audible(p, p.sequence().tracks.back()));
}

// ------------------------------------------------------------- no clips on it --

TEST(Submix, NothingCanBePlacedOnABus) {
  Project p = sequence();
  Media m;
  m.id = "m1";
  m.path = "D:/footage/take.wav";
  m.duration = 10.0;
  m.audio_stream_count = 1;
  p.media = {m};
  // The bus first in the track list, so a placement that simply took the first
  // audio track would take this one.
  p = add_submix_track(std::move(p), "Dialogue");
  std::rotate(p.sequence().tracks.begin() + 1, p.sequence().tracks.end() - 1, p.sequence().tracks.end());
  ASSERT_TRUE(p.sequence().tracks[1].submix);

  p = place_media(std::move(p), "m1", 0.0);
  EXPECT_TRUE(p.sequence().tracks[1].clips.empty()) << "a bus is fed by tracks, not by what is dropped on it";
  EXPECT_FALSE(p.sequence().tracks[2].clips.empty()) << "it went to the first lane that can hold clips";
}

// ------------------------------------------------------------------- file --

TEST(RoutingFile, RoutingSurvivesARoundTrip) {
  Project p = add_submix_track(sequence(), "Reverb");
  const std::string bus = last_id(p);
  p = set_track_output(std::move(p), "a2", bus);
  p = set_send(std::move(p), "a1", bus, 0.35, true);

  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  const Project& read = loaded->project;
  EXPECT_TRUE(read.sequence().tracks.back().submix);
  EXPECT_EQ(read.sequence().tracks.back().label, "Reverb");
  EXPECT_EQ(read.sequence().tracks[2].output, bus);
  ASSERT_EQ(read.sequence().tracks[1].sends.size(), 1u);
  EXPECT_EQ(read.sequence().tracks[1].sends[0].to, bus);
  EXPECT_DOUBLE_EQ(read.sequence().tracks[1].sends[0].level, 0.35);
  EXPECT_TRUE(read.sequence().tracks[1].sends[0].pre_fader);
}

TEST(RoutingFile, ATrackWithNoRoutingWritesNoneOfIt) {
  const std::string written = to_json(sequence());
  EXPECT_EQ(written.find("submix"), std::string::npos);
  EXPECT_EQ(written.find("sends"), std::string::npos);
  EXPECT_EQ(written.find("\"output\""), std::string::npos);
}

TEST(RoutingFile, ASendNamingNothingIsNotReadBack) {
  Project p = sequence();
  p.sequence().tracks[1].sends.push_back(Send{.to = "", .level = 0.5});
  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_TRUE(loaded->project.sequence().tracks[1].sends.empty());
}

}  // namespace
}  // namespace cutline::core
