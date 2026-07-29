/// Named arrangements, and getting them back off disk.
///
/// The interesting parts are the ones that only matter once time has passed: a
/// file written by a build with different panels, a workspace dragged about and
/// then switched away from, a name that no longer exists. Those are what a
/// round trip through a file is actually for, and none of them are visible the
/// day the format is written.

#include "cutline/editor/workspace.hpp"

#include "cutline/ui/dock.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace cutline::editor {
namespace {

/// The panels this "build" has.
const std::vector<ui::PanelId>& known_panels() {
  static const std::vector<ui::PanelId> panels{"project", "effects", "monitor", "timeline"};
  return panels;
}

[[nodiscard]] std::vector<ui::PanelId> sorted(std::vector<ui::PanelId> panels) {
  std::ranges::sort(panels);
  return panels;
}

/// A directory that cleans up after itself.
class Scratch {
 public:
  Scratch() {
    dir_ = std::filesystem::temp_directory_path() /
           ("cutline-workspaces-" + std::to_string(::testing::UnitTest::GetInstance()
                                                       ->random_seed()) +
            "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::create_directories(dir_);
  }
  ~Scratch() {
    std::error_code ignored;
    std::filesystem::remove_all(dir_, ignored);
  }

  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  [[nodiscard]] std::filesystem::path file(std::string name) const { return dir_ / name; }

 private:
  std::filesystem::path dir_;
};

// ------------------------------------------------------------- the built-ins --

TEST(Workspaces, TheBuiltInsAllShowEveryPanel) {
  // A built-in that forgot a panel would leave it unreachable, because there is
  // no menu to open one from.
  for (const Workspace& workspace : built_in_workspaces()) {
    EXPECT_EQ(sorted(ui::panels_in(workspace.layout)), sorted(known_panels()))
        << workspace.name;
  }
}

TEST(Workspaces, TheBuiltInsAreDifferentArrangements) {
  const std::vector<Workspace> all = built_in_workspaces();
  ASSERT_GE(all.size(), 3u);
  for (std::size_t i = 0; i < all.size(); ++i) {
    for (std::size_t j = i + 1; j < all.size(); ++j) {
      EXPECT_NE(all[i].layout, all[j].layout) << all[i].name << " and " << all[j].name;
      EXPECT_NE(all[i].name, all[j].name);
    }
  }
}

TEST(Workspaces, TheDefaultsStartOnTheFirstOne) {
  const Workspaces workspaces = default_workspaces();
  EXPECT_EQ(workspaces.active, "Editing");
  ASSERT_NE(workspaces.current(), nullptr);
  EXPECT_EQ(*workspaces.current(), workspaces.named.front().layout);
}

// --------------------------------------------------------------- switching --

TEST(Workspaces, SwitchingChangesWhatIsShowing) {
  Workspaces workspaces = default_workspaces();
  ASSERT_TRUE(activate_workspace(workspaces, "Colour"));

  ASSERT_NE(workspaces.current(), nullptr);
  EXPECT_EQ(*workspaces.current(),
            std::ranges::find(workspaces.named, "Colour", &Workspace::name)->layout);
}

TEST(Workspaces, SwitchingToOneThatIsNotThereDoesNothing) {
  Workspaces workspaces = default_workspaces();
  const Workspaces before = workspaces;

  EXPECT_FALSE(activate_workspace(workspaces, "Nowhere"));
  EXPECT_EQ(workspaces, before);
}

TEST(Workspaces, AnArrangementSurvivesSwitchingAwayAndBack) {
  // The behaviour people complain about when it is missing: dragging a panel,
  // glancing at another workspace, and coming back to find the drag undone.
  Workspaces workspaces = default_workspaces();
  ASSERT_TRUE(ui::dock_panel(*workspaces.current(), "effects", "timeline", ui::DockSide::Centre));
  const ui::DockLayout arranged = *workspaces.current();

  ASSERT_TRUE(activate_workspace(workspaces, "Audio"));
  ASSERT_TRUE(activate_workspace(workspaces, "Editing"));

  EXPECT_EQ(*workspaces.current(), arranged);
}

// ----------------------------------------------------------------- resetting --

TEST(Workspaces, ResettingPutsABuiltInBackAsItWasDefined) {
  Workspaces workspaces = default_workspaces();
  ASSERT_TRUE(ui::dock_panel(*workspaces.current(), "effects", "timeline", ui::DockSide::Centre));
  ASSERT_NE(*workspaces.current(), built_in_workspaces().front().layout);

  ASSERT_TRUE(reset_workspace(workspaces, "Editing"));
  EXPECT_EQ(*workspaces.current(), built_in_workspaces().front().layout);
}

TEST(Workspaces, ResettingAnUntouchedOneReportsNothingChanged) {
  Workspaces workspaces = default_workspaces();
  EXPECT_FALSE(reset_workspace(workspaces, "Editing"));
}

TEST(Workspaces, OneTheUserMadeUpHasNothingToResetTo) {
  // Inventing a definition for it would throw their arrangement away for
  // nothing.
  Workspaces workspaces = default_workspaces();
  ASSERT_TRUE(add_workspace(workspaces, "Mine"));
  ASSERT_TRUE(ui::dock_panel(*workspaces.current(), "effects", "timeline", ui::DockSide::Centre));
  const ui::DockLayout arranged = *workspaces.current();

  EXPECT_FALSE(reset_workspace(workspaces, "Mine"));
  EXPECT_EQ(*workspaces.current(), arranged);
}

// ------------------------------------------------------- adding and removing --

TEST(Workspaces, AddingCopiesWhatIsOnScreenAndSwitchesToIt) {
  Workspaces workspaces = default_workspaces();
  ASSERT_TRUE(activate_workspace(workspaces, "Audio"));
  const ui::DockLayout showing = *workspaces.current();

  ASSERT_TRUE(add_workspace(workspaces, "Mine"));
  EXPECT_EQ(workspaces.active, "Mine");
  EXPECT_EQ(*workspaces.current(), showing);
  EXPECT_EQ(workspaces.named.size(), 4u);
}

TEST(Workspaces, AddingOverAnExistingNameReplacesIt) {
  Workspaces workspaces = default_workspaces();
  ASSERT_TRUE(add_workspace(workspaces, "Mine"));
  ASSERT_TRUE(activate_workspace(workspaces, "Audio"));
  const ui::DockLayout audio = *workspaces.current();

  ASSERT_TRUE(add_workspace(workspaces, "Mine"));
  EXPECT_EQ(workspaces.named.size(), 4u) << "a second workspace called Mine was made";
  EXPECT_EQ(*workspaces.current(), audio);
}

TEST(Workspaces, AnUnnamedWorkspaceIsRefused) {
  Workspaces workspaces = default_workspaces();
  const Workspaces before = workspaces;
  EXPECT_FALSE(add_workspace(workspaces, ""));
  EXPECT_EQ(workspaces, before);
}

TEST(Workspaces, ABuiltInCannotBeRemoved) {
  // There is no other way to get one back.
  Workspaces workspaces = default_workspaces();
  EXPECT_FALSE(remove_workspace(workspaces, "Editing"));
  EXPECT_EQ(workspaces.named.size(), 3u);
}

TEST(Workspaces, RemovingTheActiveOneMovesToAnother) {
  Workspaces workspaces = default_workspaces();
  ASSERT_TRUE(add_workspace(workspaces, "Mine"));
  ASSERT_EQ(workspaces.active, "Mine");

  ASSERT_TRUE(remove_workspace(workspaces, "Mine"));
  EXPECT_NE(workspaces.active, "Mine");
  EXPECT_NE(workspaces.current(), nullptr);
}

TEST(Workspaces, TheLastOneCannotBeRemoved) {
  Workspaces workspaces;
  workspaces.named.push_back(Workspace{.name = "Only"});
  workspaces.active = "Only";
  EXPECT_FALSE(remove_workspace(workspaces, "Only"));
}

// ---------------------------------------------------------------- settling --

TEST(Workspaces, SettlingDropsPanelsThisBuildDoesNotHave) {
  // A file written by a version that had a panel since removed. Left alone it
  // would show a tab nothing can fill.
  Workspaces workspaces = default_workspaces();
  ASSERT_TRUE(ui::open_panel(*workspaces.current(), "scopes"));
  ASSERT_TRUE(ui::contains_panel(workspaces.current()->root, "scopes"));

  settle(workspaces, known_panels());
  EXPECT_FALSE(ui::contains_panel(workspaces.current()->root, "scopes"));
}

TEST(Workspaces, SettlingOpensPanelsTheFileNeverHeardOf) {
  // A panel added since the file was written. With no menu to open one from,
  // leaving it out makes it unreachable.
  Workspaces workspaces = default_workspaces();
  ASSERT_TRUE(ui::close_panel(*workspaces.current(), "effects"));

  settle(workspaces, known_panels());
  EXPECT_TRUE(ui::contains_panel(workspaces.current()->root, "effects"));
}

TEST(Workspaces, SettlingFixesAnActiveNameThatIsNotThere) {
  Workspaces workspaces = default_workspaces();
  workspaces.active = "Nowhere";

  settle(workspaces, known_panels());
  EXPECT_EQ(workspaces.active, "Editing");
  EXPECT_NE(workspaces.current(), nullptr);
}

TEST(Workspaces, SettlingAnEmptySetGivesTheBuiltInsBack) {
  Workspaces workspaces;
  settle(workspaces, known_panels());

  EXPECT_FALSE(workspaces.named.empty());
  EXPECT_NE(workspaces.current(), nullptr);
}

// -------------------------------------------------------------- round trips --

TEST(WorkspaceFile, ArrangementsSurviveARoundTrip) {
  Workspaces workspaces = default_workspaces();
  ASSERT_TRUE(activate_workspace(workspaces, "Colour"));
  ASSERT_TRUE(ui::dock_panel(*workspaces.current(), "project", "monitor", ui::DockSide::Bottom));

  const auto read = workspaces_from_json(to_json(workspaces));
  ASSERT_TRUE(read.has_value()) << read.error();
  EXPECT_EQ(*read, workspaces);
}

TEST(WorkspaceFile, FloatingWindowsSurviveARoundTrip) {
  Workspaces workspaces = default_workspaces();
  ASSERT_TRUE(
      ui::float_panel(*workspaces.current(), "effects", ui::Rect{120.0, 80.0, 520.0, 380.0}));

  const auto read = workspaces_from_json(to_json(workspaces));
  ASSERT_TRUE(read.has_value()) << read.error();

  const ui::DockLayout* layout = read->current();
  ASSERT_NE(layout, nullptr);
  ASSERT_EQ(layout->floating.size(), 1u);
  EXPECT_EQ(layout->floating[0].bounds, (ui::Rect{120.0, 80.0, 520.0, 380.0}));
  EXPECT_EQ(layout->floating[0].id, workspaces.current()->floating[0].id);
}

TEST(WorkspaceFile, WhichTabIsShowingSurvivesARoundTrip) {
  Workspaces workspaces = default_workspaces();
  ASSERT_TRUE(ui::activate_panel(*workspaces.current(), "effects"));

  const auto read = workspaces_from_json(to_json(workspaces));
  ASSERT_TRUE(read.has_value());

  const ui::DockNode* group = ui::group_of(read->current()->root, "effects");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->active_panel(), std::optional<ui::PanelId>{"effects"});
}

TEST(WorkspaceFile, RubbishIsRefusedRatherThanGuessedAt) {
  EXPECT_FALSE(workspaces_from_json("not json at all").has_value());
  EXPECT_FALSE(workspaces_from_json("[1, 2, 3]").has_value());
}

TEST(WorkspaceFile, AFileFromTheFutureIsRefused) {
  // Half-reading it would silently throw away whatever it knew that this build
  // does not.
  const std::string text = R"({"version": 99, "workspaces": []})";
  const auto read = workspaces_from_json(text);
  ASSERT_FALSE(read.has_value());
  EXPECT_NE(read.error().find("newer"), std::string::npos);
}

TEST(WorkspaceFile, AMalformedLayoutComesBackNormalisedRatherThanBroken) {
  // The whole reason `normalise` is idempotent: this is a document of unknown
  // provenance and it has to be safe to load.
  const std::string text = R"({
    "version": 1,
    "active": "Odd",
    "workspaces": [{
      "name": "Odd",
      "layout": {
        "root": {
          "split": "horizontal",
          "fractions": [9, 9, 9],
          "children": [
            {"tabs": [], "active": 4},
            {"tabs": ["project"], "active": 7},
            {"split": "horizontal", "children": [{"tabs": ["timeline"]}]}
          ]
        }
      }
    }]
  })";

  const auto read = workspaces_from_json(text);
  ASSERT_TRUE(read.has_value()) << read.error();

  const ui::DockLayout* layout = read->current();
  ASSERT_NE(layout, nullptr);
  EXPECT_EQ(sorted(ui::panels_in(*layout)), (std::vector<ui::PanelId>{"project", "timeline"}));

  // The empty group went, the one-child split collapsed, and the fractions add
  // up again.
  ASSERT_TRUE(layout->root.is_split());
  EXPECT_EQ(layout->root.children.size(), 2u);
  double total = 0.0;
  for (const double share : layout->root.fractions) total += share;
  EXPECT_NEAR(total, 1.0, 1e-9);
}

TEST(WorkspaceFile, AWorkspaceWithNoNameIsSkipped) {
  // It could never be switched to, so keeping it would only put an unreachable
  // entry in the switcher.
  const std::string text = R"({"version": 1, "workspaces": [{"layout": {}}]})";
  const auto read = workspaces_from_json(text);
  ASSERT_TRUE(read.has_value());
  EXPECT_TRUE(read->named.empty());
}

// ---------------------------------------------------------------- the file --

TEST(WorkspaceFile, WritingThenReadingGivesTheSameThing) {
  const Scratch scratch;
  const std::filesystem::path path = scratch.file("workspaces.json");

  Workspaces workspaces = default_workspaces();
  ASSERT_TRUE(activate_workspace(workspaces, "Audio"));

  ASSERT_TRUE(write_workspaces(path, workspaces).has_value());
  const auto read = read_workspaces(path);
  ASSERT_TRUE(read.has_value()) << read.error();
  EXPECT_EQ(*read, workspaces);
}

TEST(WorkspaceFile, WritingMakesTheDirectoryItNeeds) {
  const Scratch scratch;
  const std::filesystem::path path = scratch.file("nested") / "deeper" / "workspaces.json";

  ASSERT_TRUE(write_workspaces(path, default_workspaces()).has_value());
  EXPECT_TRUE(std::filesystem::exists(path));
}

TEST(WorkspaceFile, NoFileMeansAFreshInstallRatherThanAFailure) {
  const Scratch scratch;
  const auto read = read_workspaces(scratch.file("never-written.json"));
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(*read, default_workspaces());
}

TEST(WorkspaceFile, NothingIsLeftBehindByASave) {
  const Scratch scratch;
  const std::filesystem::path path = scratch.file("workspaces.json");
  ASSERT_TRUE(write_workspaces(path, default_workspaces()).has_value());

  std::filesystem::path staging = path;
  staging += ".saving";
  EXPECT_FALSE(std::filesystem::exists(staging)) << "the staging file was not renamed away";
}

TEST(WorkspaceFile, ThePathIsUnderTheUsersOwnData) {
  const std::filesystem::path path = default_workspace_path();
  EXPECT_EQ(path.filename(), "workspaces.json");
  EXPECT_EQ(path.parent_path().filename(), "Cutline");
}

}  // namespace
}  // namespace cutline::editor
