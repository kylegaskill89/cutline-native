/// Subclips, and what Scene Edit Detection does with the cuts it finds.
///
/// What matters about a subclip is what Premiere's does: it places its own
/// span, a restricted one cannot be trimmed, slipped or marked past that span
/// while a soft one can, it is filed and saved as an entry of its own, and a
/// subclip of a subclip still belongs to the footage. What matters about
/// Scene Edit Detection's edit is that each option does its one thing at
/// every cut, to the linked audio too, and nothing outside the clip.

#include "cutline/core/subclips.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/scene_edits.hpp"
#include "cutline/core/serialize.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace cutline::core {
namespace {

/// Ten seconds of footage with a picture and a sound, not yet placed.
[[nodiscard]] Project with_footage() {
  Project p = empty_project();
  Media m;
  m.id = "m1";
  m.path = "D:/footage.mp4";
  m.name = "footage.mp4";
  m.duration = 10.0;
  m.has_video = true;
  m.audio_stream_count = 1;
  m.markers = {Marker{.id = "early", .time = 1.0}, Marker{.id = "inside", .time = 3.0}};
  p.media = {m};
  return p;
}

[[nodiscard]] const Media& media(const Project& p, std::string_view id) {
  return *std::ranges::find(p.media, id, &Media::id);
}

/// The one clip on the first video track.
[[nodiscard]] const Clip& picture(const Project& p) {
  for (const Track& t : p.sequence().tracks) {
    if (t.kind == TrackKind::Video && !t.clips.empty()) return t.clips.front();
  }
  static const Clip none;
  return none;
}

[[nodiscard]] std::vector<const Clip*> clips_of(const Project& p, TrackKind kind) {
  std::vector<const Clip*> out;
  for (const Track& t : p.sequence().tracks) {
    if (t.kind != kind) continue;
    for (const Clip& c : t.clips) out.push_back(&c);
  }
  std::ranges::sort(out, {}, [](const Clip* c) { return c->start; });
  return out;
}

[[nodiscard]] Project placed_subclip(bool restrict_trims, std::string* id) {
  Project p = make_subclip(with_footage(), "m1", "Shot", 2.0, 5.0, restrict_trims, {}, id);
  return place_media(std::move(p), *id, 0.0, {}, source_range(p, *id));
}

}  // namespace

TEST(Subclips, ASubclipIsAnEntryOfItsOwnOverTheSameFootage) {
  std::string id;
  const Project p = make_subclip(with_footage(), "m1", "Shot", 2.0, 5.0, false, {}, &id);
  ASSERT_EQ(p.media.size(), 2u);
  const Media& sub = media(p, id);
  EXPECT_NE(id, "m1");
  EXPECT_EQ(sub.name, "Shot");
  EXPECT_EQ(sub.path, "D:/footage.mp4");
  ASSERT_TRUE(sub.subclip.has_value());
  EXPECT_EQ(sub.subclip->master, "m1");
  EXPECT_DOUBLE_EQ(sub.subclip->start, 2.0);
  EXPECT_DOUBLE_EQ(sub.subclip->end, 5.0);
}

TEST(Subclips, OnlyTheSourcesMarkersInsideTheSpanComeWithIt) {
  std::string id;
  const Project p = make_subclip(with_footage(), "m1", "Shot", 2.0, 5.0, false, {}, &id);
  ASSERT_EQ(media(p, id).markers.size(), 1u);
  EXPECT_EQ(media(p, id).markers.front().id, "inside");
}

TEST(Subclips, ASubclipPlacesItsOwnSpan) {
  for (const bool restrict_trims : {false, true}) {
    std::string id;
    const Project p = placed_subclip(restrict_trims, &id);
    EXPECT_DOUBLE_EQ(picture(p).source_in, 2.0) << restrict_trims;
    EXPECT_DOUBLE_EQ(picture(p).source_out, 5.0) << restrict_trims;
  }
}

TEST(Subclips, ARestrictedSubclipCannotBeTrimmedPastItsSpan) {
  std::string id;
  Project p = placed_subclip(true, &id);
  p = set_clip_edge(std::move(p), picture(p).id, ClipEdge::Out, 9.0);
  EXPECT_DOUBLE_EQ(picture(p).source_out, 5.0);
  p = ripple_trim_edge(std::move(p), picture(p).id, ClipEdge::In, -4.0);
  EXPECT_DOUBLE_EQ(picture(p).source_in, 2.0);
}

TEST(Subclips, ASoftSubclipTrimsIntoTheRestOfTheFootage) {
  std::string id;
  Project p = placed_subclip(false, &id);
  p = set_clip_edge(std::move(p), picture(p).id, ClipEdge::Out, 5.0);
  EXPECT_DOUBLE_EQ(picture(p).source_out, 7.0);
}

TEST(Subclips, ARestrictedSubclipCannotBeSlippedPastItsSpan) {
  std::string id;
  Project p = placed_subclip(true, &id);
  p = set_clip_edge(std::move(p), picture(p).id, ClipEdge::Out, 2.0);  // source 2..4
  p = slip_clip(std::move(p), picture(p).id, 5.0);
  EXPECT_DOUBLE_EQ(picture(p).source_in, 3.0);
  EXPECT_DOUBLE_EQ(picture(p).source_out, 5.0);
  EXPECT_DOUBLE_EQ(source_handles(p, picture(p)).tail, 0.0);
  EXPECT_DOUBLE_EQ(source_handles(p, picture(p)).head, 1.0);
}

TEST(Subclips, MarksOnARestrictedSubclipStayInsideItsSpan) {
  std::string id;
  Project p = make_subclip(with_footage(), "m1", "Shot", 2.0, 5.0, true, {}, &id);
  p = set_source_in_point(std::move(p), id, 0.5);
  p = set_source_out_point(std::move(p), id, 9.0);
  EXPECT_EQ(media(p, id).in_point, 2.0);
  EXPECT_EQ(media(p, id).out_point, 5.0);
}

TEST(Subclips, ASubclipOfASubclipBelongsToTheFootage) {
  std::string first;
  std::string second;
  Project p = make_subclip(with_footage(), "m1", "Shot", 2.0, 5.0, false, {}, &first);
  p = make_subclip(std::move(p), first, "Part", 3.0, 4.0, false, {}, &second);
  ASSERT_TRUE(media(p, second).subclip.has_value());
  EXPECT_EQ(media(p, second).subclip->master, "m1");
}

TEST(Subclips, OnlyFootageHasSubclips) {
  Project p = with_footage();
  p.media.front().is_image = true;
  const Project before = p;
  EXPECT_EQ(make_subclip(p, "m1", "Shot", 0.0, 1.0, false), before);
  p = with_footage();
  p.media.front().sequence_id = "seq";
  EXPECT_FALSE(can_make_subclip(p.media.front()));
  // Nor a span shorter than a frame.
  EXPECT_EQ(make_subclip(with_footage(), "m1", "Shot", 2.0, 2.0, false).media.size(), 1u);
}

TEST(Subclips, EditingOneMovesItsSpanAndItsMarks) {
  std::string id;
  Project p = make_subclip(with_footage(), "m1", "Shot", 2.0, 5.0, false, {}, &id);
  p = edit_subclip(std::move(p), id, 4.0, 8.0, false);
  EXPECT_DOUBLE_EQ(media(p, id).subclip->start, 4.0);
  EXPECT_DOUBLE_EQ(media(p, id).subclip->end, 8.0);
  EXPECT_EQ(media(p, id).in_point, 4.0);
  EXPECT_EQ(media(p, id).out_point, 8.0);
}

TEST(Subclips, ConvertedToAMasterClipItReachesTheWholeFootage) {
  std::string id;
  Project p = placed_subclip(true, &id);
  p = convert_to_master_clip(std::move(p), id);
  EXPECT_FALSE(media(p, id).subclip.has_value());
  p = set_clip_edge(std::move(p), picture(p).id, ClipEdge::Out, 8.0);
  EXPECT_DOUBLE_EQ(picture(p).source_out, 10.0);
}

TEST(Subclips, SurviveSaving) {
  std::string id;
  const Project p = make_subclip(with_footage(), "m1", "Shot", 2.0, 5.0, true, {}, &id);
  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(media(loaded->project, id).subclip.has_value());
  EXPECT_EQ(media(loaded->project, id).subclip, media(p, id).subclip);
}

namespace {

/// The footage's seconds 2 to 8 placed at 1, so source and timeline differ.
[[nodiscard]] Project placed() {
  Project p = with_footage();
  return place_media(std::move(p), "m1", 1.0, {}, PlacementRange{2.0, 8.0});
}

}  // namespace

TEST(SceneEdits, CutTheClipAndItsSoundAtEachCut) {
  Project p = placed();
  const std::vector<double> cuts{3.0, 5.5};
  p = apply_scene_edits(std::move(p), picture(p).id, cuts, {.cut = true});
  for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
    const auto pieces = clips_of(p, kind);
    ASSERT_EQ(pieces.size(), 3u);
    EXPECT_DOUBLE_EQ(pieces[0]->start, 1.0);
    EXPECT_DOUBLE_EQ(pieces[1]->start, 3.0);
    EXPECT_DOUBLE_EQ(pieces[2]->start, 5.5);
    EXPECT_DOUBLE_EQ(pieces[1]->source_in, 4.0);
    EXPECT_DOUBLE_EQ(pieces[2]->source_out, 8.0);
  }
}

TEST(SceneEdits, PutAClipMarkerOnTheFootageAtEachCut) {
  Project p = placed();
  const std::vector<double> cuts{3.0, 5.5};
  p = apply_scene_edits(std::move(p), picture(p).id, cuts, {.cut = false, .markers = true});
  EXPECT_EQ(clips_of(p, TrackKind::Video).size(), 1u);
  std::vector<double> times;
  for (const Marker& m : media(p, "m1").markers) times.push_back(m.time);
  EXPECT_EQ(times, (std::vector<double>{1.0, 3.0, 4.0, 6.5}));
  // Again changes nothing: a marker already there is not doubled.
  const Project again = apply_scene_edits(p, picture(p).id, cuts, {.cut = false, .markers = true});
  EXPECT_EQ(media(again, "m1").markers.size(), 4u);
}

TEST(SceneEdits, MakeASubclipForEachShotInABinBesideTheFootage) {
  Project p = placed();
  p.bins.push_back(Bin{.id = "rushes", .name = "Rushes"});
  p.media.front().bin = "rushes";
  const std::vector<double> cuts{3.0, 5.5};
  p = apply_scene_edits(std::move(p), picture(p).id, cuts, {.cut = false, .subclips = true});
  ASSERT_EQ(p.bins.size(), 2u);
  EXPECT_EQ(p.bins.back().name, "footage.mp4");
  EXPECT_EQ(p.bins.back().parent, "rushes");
  std::vector<std::pair<double, double>> spans;
  for (const Media& m : p.media) {
    if (!m.subclip.has_value()) continue;
    EXPECT_EQ(m.bin, p.bins.back().id);
    EXPECT_TRUE(m.subclip->restrict_trims);
    spans.emplace_back(m.subclip->start, m.subclip->end);
  }
  EXPECT_EQ(spans, (std::vector<std::pair<double, double>>{{2.0, 4.0}, {4.0, 6.5}, {6.5, 8.0}}));
  EXPECT_EQ(p.media[1].name, "footage.mp4.Subclip 001");
}

TEST(SceneEdits, IgnoreTimesOutsideTheClipOrAtItsEnds) {
  Project p = placed();
  const Project before = p;
  const std::vector<double> cuts{0.5, 1.0, 7.0, 9.0};
  EXPECT_EQ(apply_scene_edits(p, picture(p).id, cuts, {.cut = true, .markers = true}), before);
}

}  // namespace cutline::core
