/// One way to ask a question: the dialog framework every dialog is built on.

#include "cutline/ui/dialog.hpp"

#include "cutline/ui/controls.hpp"
#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

namespace cutline::ui {
namespace {

const RecordingPainter& measurer() {
  static const RecordingPainter shared;
  return shared;
}

/// A host with an empty window and the dialog open as a popup in it, the way
/// the application opens one.
struct Fixture {
  explicit Fixture(std::unique_ptr<Dialog> made) {
    dialog = made.get();
    host = std::make_unique<WidgetHost>(std::make_unique<Panel>());
    host->resize(Rect{0.0, 0.0, 800.0, 600.0}, context());
    host->open_popup(std::move(made), Rect{100.0, 100.0, 10.0, 10.0});
    (void)host->update_layout(context());
  }

  [[nodiscard]] static LayoutContext context() { return LayoutContext{default_theme(), measurer()}; }

  void key(Key key) { (void)host->key_down(KeyEvent{.key = key}); (void)host->update_layout(context()); }

  std::unique_ptr<WidgetHost> host;
  Dialog* dialog = nullptr;
};

/// A dialog with one text field and counters for what happened to it.
struct Asked {
  int accepted = 0;
  int cancelled = 0;
  std::optional<std::string> answer;  ///< what accepting says is wrong, if anything
  TextField* field = nullptr;

  [[nodiscard]] std::unique_ptr<Dialog> make() {
    auto dialog = std::make_unique<Dialog>("Question");
    field = &dialog->row("Speed").emplace<TextField>("100");
    dialog->set_accept("OK", [this] {
      ++accepted;
      return answer;
    });
    dialog->set_cancel("Cancel", [this] { ++cancelled; });
    return dialog;
  }
};

[[nodiscard]] std::vector<Button*> buttons_of(Widget& widget) {
  std::vector<Button*> found;
  if (auto* button = dynamic_cast<Button*>(&widget); button != nullptr && button->visible()) {
    found.push_back(button);
  }
  for (const auto& child : widget.children()) {
    for (Button* button : buttons_of(*child)) found.push_back(button);
  }
  return found;
}

// The platform's order: at the bottom right, the accepting button first and
// marked as the default.
TEST(Dialog, TheButtonsSitBottomRightAcceptFirst) {
  Asked asked;
  Fixture fixture(asked.make());
  const std::vector<Button*> buttons = buttons_of(*fixture.dialog);
  ASSERT_EQ(buttons.size(), 2u);
  EXPECT_EQ(buttons[0]->text(), "OK");
  EXPECT_EQ(buttons[1]->text(), "Cancel");
  EXPECT_LT(buttons[0]->bounds().x, buttons[1]->bounds().x);
  EXPECT_TRUE(buttons[0]->is_default());
  EXPECT_EQ(buttons[0]->state(), State::Focused);
  // And right-aligned: nothing after Cancel but the dialog's own padding.
  EXPECT_GT(buttons[1]->bounds().x, fixture.dialog->bounds().x + fixture.dialog->bounds().width / 2.0);
  // The same width, as on every dialog of the period.
  EXPECT_DOUBLE_EQ(buttons[0]->bounds().width, buttons[1]->bounds().width);
}

TEST(Dialog, ItOpensWithItsFirstControlInHand) {
  Asked asked;
  Fixture fixture(asked.make());
  EXPECT_EQ(fixture.host->focused(), asked.field);
}

TEST(Dialog, EnterAcceptsAndTheDialogCloses) {
  Asked asked;
  Fixture fixture(asked.make());
  fixture.key(Key::Enter);
  EXPECT_EQ(asked.accepted, 1);
  EXPECT_FALSE(fixture.host->popup_open());
}

// A window that only says how far something has got has Cancel and nothing
// to answer, so it has no OK.
TEST(Dialog, AProgressWindowHasOnlyCancel) {
  int cancelled = 0;
  auto made = std::make_unique<Dialog>("Rendering");
  made->set_accept("", {});
  made->set_cancel("Cancel", [&cancelled] { ++cancelled; });
  Fixture fixture(std::move(made));
  const std::vector<Button*> shown = buttons_of(*fixture.dialog);
  ASSERT_EQ(shown.size(), 1u);
  EXPECT_EQ(shown[0]->text(), "Cancel");
  fixture.key(Key::Escape);
  EXPECT_EQ(cancelled, 1);
}

// A mistyped answer used to close most dialogs having done nothing, which
// looks exactly like the edit having worked.
TEST(Dialog, AWrongAnswerKeepsItOpenAndSaysWhy) {
  Asked asked;
  asked.answer = "Speed has to be a number above zero.";
  Fixture fixture(asked.make());
  fixture.key(Key::Enter);
  EXPECT_EQ(asked.accepted, 1);
  EXPECT_TRUE(fixture.host->popup_open());
  EXPECT_EQ(fixture.dialog->error(), "Speed has to be a number above zero.");
}

TEST(Dialog, AWrongAnswerBeepsAsWindowsDid) {
  Asked asked;
  asked.answer = "Speed has to be a number above zero.";
  Fixture fixture(asked.make());
  std::vector<Sound> heard;
  fixture.host->set_sound_player([&heard](Sound sound) { heard.push_back(sound); });
  fixture.key(Key::Enter);
  EXPECT_EQ(heard, (std::vector<Sound>{Sound::Refused}));

  // A good answer is taken without a sound.
  asked.answer.reset();
  heard.clear();
  fixture.key(Key::Enter);
  EXPECT_TRUE(heard.empty());
}

TEST(Dialog, EscapeCancelsAndTellsTheDialog) {
  Asked asked;
  Fixture fixture(asked.make());
  fixture.key(Key::Escape);
  EXPECT_EQ(asked.cancelled, 1);
  EXPECT_EQ(asked.accepted, 0);
  EXPECT_FALSE(fixture.host->popup_open());
}

TEST(Dialog, TabGoesRoundTheDialogRatherThanBehindIt) {
  Asked asked;
  Fixture fixture(asked.make());
  // The field, OK, Cancel, and back to the field.
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(fixture.host->focus_next());
  EXPECT_EQ(fixture.host->focused(), asked.field);
}

TEST(Dialog, RowLabelsLineUp) {
  auto dialog = std::make_unique<Dialog>("Rows");
  auto& short_row = dialog->row("In");
  TextField& first = short_row.emplace<TextField>("1");
  auto& long_row = dialog->row("A much longer name");
  TextField& second = long_row.emplace<TextField>("2");
  Fixture fixture(std::move(dialog));
  EXPECT_DOUBLE_EQ(first.bounds().x, second.bounds().x);
}

TEST(Form, AWidePageKeepsItsControlsBesideTheirLabels) {
  // The settings pages are wider than their rows. A row of label, spring and
  // control put the control at the far edge with a gap down the middle.
  Form page;
  auto& row = page.row("Height");
  TextField& field = row.emplace<TextField>("540");
  page.arrange(Rect{0.0, 0.0, 900.0, 300.0}, Fixture::context());

  EXPECT_DOUBLE_EQ(field.bounds().x,
                   row.label().bounds().right() + Fixture::context().metrics().spacing);
}

TEST(Form, AFormRebuiltInPlaceLinesUpItsNewRows) {
  Form page;
  page.row("A much longer name than any other").emplace<TextField>("old");
  page.arrange(Rect{0.0, 0.0, 900.0, 300.0}, Fixture::context());

  // What a settings page does when another category is chosen.
  page.clear_children();
  TextField& first = page.row("In").emplace<TextField>("1");
  TextField& second = page.row("Out").emplace<TextField>("2");
  page.arrange(Rect{0.0, 0.0, 900.0, 300.0}, Fixture::context());

  EXPECT_DOUBLE_EQ(first.bounds().x, second.bounds().x);
  EXPECT_LT(first.bounds().x, 300.0) << "the old page's label is still being measured";
}

TEST(Form, AHiddenRowDoesNotWidenTheColumn) {
  Form page;
  TextField& shown = page.row("In").emplace<TextField>("1");
  page.row("A much longer name than any other").set_visible(false);
  page.arrange(Rect{0.0, 0.0, 900.0, 300.0}, Fixture::context());

  const double label = Fixture::context().text.measure("In", Fixture::context().metrics().font_size,
                                                       false);
  EXPECT_DOUBLE_EQ(shown.bounds().x, label + Fixture::context().metrics().spacing);
}

TEST(Dialog, WithNoCancelThereIsOneButton) {
  auto dialog = std::make_unique<Dialog>("Edits as it goes");
  dialog->set_accept("Close", [] { return std::optional<std::string>{}; });
  dialog->set_cancel("");
  Fixture fixture(std::move(dialog));
  const std::vector<Button*> buttons = buttons_of(*fixture.dialog);
  ASSERT_EQ(buttons.size(), 1u);
  EXPECT_EQ(buttons[0]->text(), "Close");
}

}  // namespace
}  // namespace cutline::ui
