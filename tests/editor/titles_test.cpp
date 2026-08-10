/// Making and editing titles: the one source an editor creates rather than
/// imports.

#include "cutline/editor/titles.hpp"

#include "cutline/core/id.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <string>

namespace cutline::editor {
namespace {

using core::Project;
using core::TextSpec;
using core::Track;
using core::TrackKind;

[[nodiscard]] Project with_tracks() {
  Project project;
  project.sequence().fps = 30.0;
  project.sequence().tracks = {Track{.id = "v2", .kind = TrackKind::Video},
                    Track{.id = "v1", .kind = TrackKind::Video},
                    Track{.id = "a1", .kind = TrackKind::Audio}};
  return project;
}

[[nodiscard]] TextSpec saying(std::string content) {
  TextSpec spec;
  spec.content = std::move(content);
  return spec;
}

TEST(Titles, ANewTitleIsGeneratedMediaWithText) {
  std::string id;
  const Project p = add_title(with_tracks(), saying("Chapter One"), &id);

  ASSERT_EQ(p.media.size(), 1u);
  EXPECT_FALSE(id.empty());
  EXPECT_EQ(p.media.front().id, id);
  EXPECT_TRUE(p.media.front().is_text);
  EXPECT_TRUE(p.media.front().path.empty()) << "a title is not a file";
  ASSERT_TRUE(p.media.front().text.has_value());
  EXPECT_EQ(p.media.front().text->content, "Chapter One");
}

TEST(Titles, ATitleIsNamedAfterWhatItSays) {
  const Project p = add_title(with_tracks(), saying("Chapter One"));
  EXPECT_EQ(p.media.front().name, "Chapter One");
}

TEST(Titles, AMultiLineTitleIsNamedAfterItsFirstLine) {
  const Project p = add_title(with_tracks(), saying("Cutline\nnative"));
  EXPECT_EQ(p.media.front().name, "Cutline");
}

TEST(Titles, AnEmptyTitleStillHasAName) {
  const Project p = add_title(with_tracks(), saying(""));
  EXPECT_EQ(p.media.front().name, "Title") << "a nameless entry cannot be found in a list";
}

TEST(Titles, TwoTitlesSayingTheSameThingAreTwoTitles) {
  // Unlike a file, which is deduplicated against the pool: there is no identity
  // here beyond the words, and two cards reading "Chapter One" are two cards.
  Project p = add_title(with_tracks(), saying("Chapter One"));
  p = add_title(std::move(p), saying("Chapter One"));

  ASSERT_EQ(p.media.size(), 2u);
  EXPECT_NE(p.media[0].id, p.media[1].id);
}

TEST(Titles, ATitleHasALengthSoItCanBePlaced) {
  const Project p = add_title(with_tracks(), saying("Hello"));
  EXPECT_GT(p.media.front().duration, 0.0);
}

TEST(Titles, PlacingPutsItOnATrackAndReportsTheClip) {
  std::string clip_id;
  const Project p = add_title_at(with_tracks(), saying("Hello"), 2.0, {}, &clip_id);

  ASSERT_FALSE(clip_id.empty()) << "a caller has just made this and wants to select it";
  const core::Clip* clip = core::find_clip(p, clip_id);
  ASSERT_NE(clip, nullptr);
  EXPECT_DOUBLE_EQ(clip->start, 2.0);
  EXPECT_EQ(clip->kind, TrackKind::Video);
  EXPECT_GT(core::clip_duration(*clip), 0.0);
}

TEST(Titles, PlacingHonoursTheTrackItIsGiven) {
  std::string clip_id;
  const Project p = add_title_at(with_tracks(), saying("Hello"), 0.0, "v1", &clip_id);

  const auto& lower = p.sequence().tracks[1];
  ASSERT_EQ(lower.id, "v1");
  ASSERT_EQ(lower.clips.size(), 1u);
  EXPECT_EQ(lower.clips.front().id, clip_id);
}

TEST(Titles, TheSpecComesBackForAMediaAndForAClip) {
  std::string clip_id;
  const Project p = add_title_at(with_tracks(), saying("Hello"), 0.0, {}, &clip_id);

  const core::Clip* clip = core::find_clip(p, clip_id);
  ASSERT_NE(clip, nullptr);

  const TextSpec* by_media = title_spec(p, clip->media_id);
  ASSERT_NE(by_media, nullptr);
  EXPECT_EQ(by_media->content, "Hello");

  const TextSpec* by_clip = clip_title_spec(p, clip_id);
  ASSERT_EQ(by_clip, by_media) << "both routes should reach the same spec";
}

TEST(Titles, AnythingThatIsNotATitleHasNoSpec) {
  Project p = with_tracks();
  p.media = {core::Media{.id = "m1", .path = "wide.mp4", .has_video = true}};

  EXPECT_EQ(title_spec(p, "m1"), nullptr);
  EXPECT_EQ(title_spec(p, "nope"), nullptr);
  EXPECT_EQ(clip_title_spec(p, "nope"), nullptr);
}

TEST(Titles, EditingReplacesTheTextAndTheName) {
  std::string id;
  Project p = add_title(with_tracks(), saying("First"), &id);
  p = set_title_spec(std::move(p), id, saying("Second"));

  ASSERT_NE(title_spec(p, id), nullptr);
  EXPECT_EQ(title_spec(p, id)->content, "Second");
  EXPECT_EQ(p.media.front().name, "Second") << "the name follows the words";
}

TEST(Titles, EditingKeepsANameSomebodyChose) {
  std::string id;
  Project p = add_title(with_tracks(), saying("First"), &id);
  p.media.front().name = "Opening card";

  p = set_title_spec(std::move(p), id, saying("Second"));
  EXPECT_EQ(p.media.front().name, "Opening card")
      << "a name that was not the text must survive an edit to the text";
}

TEST(Titles, EditingKeepsEveryOtherPartOfTheSpec) {
  std::string id;
  Project p = add_title(with_tracks(), saying("First"), &id);

  TextSpec styled = *title_spec(p, id);
  styled.content = "Second";
  styled.color = "#ff0000";
  styled.font_size = 42.0;
  styled.shadow = true;
  p = set_title_spec(std::move(p), id, styled);

  const TextSpec* now = title_spec(p, id);
  ASSERT_NE(now, nullptr);
  EXPECT_EQ(now->color, "#ff0000");
  EXPECT_DOUBLE_EQ(now->font_size, 42.0);
  EXPECT_TRUE(now->shadow);
}

TEST(Titles, SettingWhatIsAlreadyThereChangesNothing) {
  // Which is what lets the session skip the undo entry.
  std::string id;
  const Project before = add_title(with_tracks(), saying("Same"), &id);
  EXPECT_EQ(set_title_spec(before, id, saying("Same")), before);
}

TEST(Titles, EditingSomethingThatIsNotATitleChangesNothing) {
  Project p = with_tracks();
  p.media = {core::Media{.id = "m1", .path = "wide.mp4", .has_video = true}};

  EXPECT_EQ(set_title_spec(p, "m1", saying("Hello")), p);
  EXPECT_EQ(set_title_spec(p, "nope", saying("Hello")), p);
}

TEST(Titles, ANewTitleSaysSomething) {
  // An empty card is indistinguishable from a broken one, so the default has
  // words in it — and they are the model's own rather than a second answer.
  const TextSpec spec = default_title_spec();
  EXPECT_FALSE(spec.content.empty());
  EXPECT_GT(spec.font_size, 0.0);
}

}  // namespace
}  // namespace cutline::editor
