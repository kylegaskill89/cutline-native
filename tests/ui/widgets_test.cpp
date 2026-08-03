/// The controls, checked for the things that are easy to get wrong and hard to
/// see: that a button is sized around its label rather than a constant, that
/// the theme's metrics actually reach the layout, that a click cancels if you
/// slide off before releasing, and that a panel's contents land under its
/// header instead of behind it.

#include "cutline/ui/widgets.hpp"

#include "cutline/ui/controls.hpp"
#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

namespace cutline::ui {
namespace {

const RecordingPainter& measurer() {
  static const RecordingPainter shared;
  return shared;
}

[[nodiscard]] LayoutContext context_for(const Theme& theme) {
  return LayoutContext{theme, measurer()};
}

[[nodiscard]] LayoutContext flat_context() { return context_for(default_theme()); }

[[nodiscard]] MouseEvent press(double x, double y) {
  return MouseEvent{.x = x, .y = y, .button = MouseButton::Left};
}

// ------------------------------------------------------------------- label --

TEST(Label, WidthFollowsTheText) {
  const LayoutContext context = flat_context();
  Label short_label("Cut");
  Label long_label("Ripple delete");

  EXPECT_GT(long_label.sizing(Axis::Horizontal, context).basis,
            short_label.sizing(Axis::Horizontal, context).basis);
}

TEST(Label, HeightIsALineOfItsFont) {
  const LayoutContext context = flat_context();
  const Metrics& metrics = context.metrics();

  Label label("Timecode");
  EXPECT_DOUBLE_EQ(label.sizing(Axis::Vertical, context).basis,
                   metrics.font_size * metrics.line_height);

  label.set_small(true);
  EXPECT_DOUBLE_EQ(label.sizing(Axis::Vertical, context).basis,
                   metrics.small_font_size * metrics.line_height);
}

TEST(Label, TextShrinksRatherThanForcingAPanelWider) {
  // A long name in a narrow inspector should be clipped, not push the panel
  // out past the window.
  const LayoutItem item = Label("a very long clip name").sizing(Axis::Horizontal, flat_context());
  EXPECT_GT(item.shrink, 0.0);
  EXPECT_DOUBLE_EQ(item.min, 0.0);
}

TEST(Label, DrawsItsTextInTheThemesColour) {
  Label label("Export");
  label.arrange(Rect{0.0, 0.0, 100.0, 20.0}, flat_context());

  RecordingPainter painter;
  label.paint(painter, default_theme());

  const DrawCall* call = painter.first(DrawCall::Kind::Text);
  ASSERT_NE(call, nullptr);
  ASSERT_TRUE(call->run.has_value());
  EXPECT_EQ(call->run->text, "Export");
  EXPECT_EQ(call->run->color, default_theme().style(Part::Panel).text);
}

TEST(Label, EmptyTextDrawsNothing) {
  Label label;
  label.arrange(Rect{0.0, 0.0, 100.0, 20.0}, flat_context());

  RecordingPainter painter;
  label.paint(painter, default_theme());
  EXPECT_TRUE(painter.calls().empty());
}

// ------------------------------------------------------------------ button --

TEST(Button, IsWiderThanItsLabel) {
  const LayoutContext context = flat_context();
  const Button button("Export Frame");

  const double label = measurer().measure("Export Frame", context.metrics().font_size, false);
  const double width = button.sizing(Axis::Horizontal, context).basis;

  EXPECT_GT(width, label);
  EXPECT_DOUBLE_EQ(width, label + 2.0 * context.metrics().padding_x);
}

TEST(Button, IsAsTallAsTheThemeSaysAControlIs) {
  const LayoutContext context = flat_context();
  EXPECT_DOUBLE_EQ(Button("Cut").sizing(Axis::Vertical, context).basis,
                   context.metrics().control_height);
}

TEST(Button, AnIconButtonIsSquare) {
  const LayoutContext context = flat_context();
  const Button icon;

  EXPECT_DOUBLE_EQ(icon.sizing(Axis::Horizontal, context).basis,
                   icon.sizing(Axis::Vertical, context).basis);
}

TEST(Button, AOneCharacterButtonIsNotASliver) {
  const LayoutContext context = flat_context();
  const LayoutItem item = Button("x").sizing(Axis::Horizontal, context);
  EXPECT_GE(item.basis, context.metrics().control_height);
}

TEST(Button, TheThemeDecidesHowBigItIs) {
  // The whole reason metrics live on the theme. A bevelled control needs room
  // for its bevel, and no widget should know which theme is in use.
  const Theme& flat = default_theme();
  const Theme& xp = *built_in_theme("xp");
  ASSERT_NE(flat.metrics, xp.metrics) << "these themes have nothing to tell apart";

  const Button button("Render");
  const double flat_height = button.sizing(Axis::Vertical, context_for(flat)).basis;
  const double xp_height = button.sizing(Axis::Vertical, context_for(xp)).basis;

  EXPECT_DOUBLE_EQ(flat_height, flat.metrics.control_height);
  EXPECT_DOUBLE_EQ(xp_height, xp.metrics.control_height);
}

/// A button in a host, so clicks go through the real routing.
struct Clicked {
  Clicked() {
    host = std::make_unique<WidgetHost>(std::make_unique<Widget>());
    button = &host->root().emplace<Button>("Go", [this] { ++count; });
    host->resize(Rect{0.0, 0.0, 200.0, 100.0}, flat_context());
    button->arrange(Rect{0.0, 0.0, 80.0, 24.0}, flat_context());
  }

  std::unique_ptr<WidgetHost> host;
  Button* button = nullptr;
  int count = 0;
};

TEST(Button, FiresOnTheWayBackUp) {
  Clicked test;
  test.host->mouse_down(press(40.0, 12.0));
  EXPECT_EQ(test.count, 0) << "a button should not fire on the way down";

  test.host->mouse_up(press(40.0, 12.0));
  EXPECT_EQ(test.count, 1);
}

TEST(Button, SlidingOffBeforeReleasingCancelsTheClick) {
  // Only works because the press captured the pointer, so the release still
  // arrives at the button and it can see that it happened elsewhere.
  Clicked test;
  test.host->mouse_down(press(40.0, 12.0));
  test.host->mouse_move(press(400.0, 12.0));
  test.host->mouse_up(press(400.0, 12.0));

  EXPECT_EQ(test.count, 0);
}

TEST(Button, LooksPressedWhileHeld) {
  Clicked test;
  test.host->mouse_down(press(40.0, 12.0));
  EXPECT_EQ(test.button->state(), State::Pressed);

  RecordingPainter painter;
  test.button->paint(painter, default_theme());
  const DrawCall* fill = painter.first(DrawCall::Kind::Fill);
  ASSERT_NE(fill, nullptr);
  EXPECT_EQ(fill->fill, default_theme().style(Part::Button, State::Pressed).fill);
}

TEST(Button, TakesTheKeyboardAndAnswersSpace) {
  Clicked test;
  ASSERT_TRUE(test.host->set_focus(test.button));

  EXPECT_TRUE(test.host->key_down(KeyEvent{.key = Key::Space}));
  EXPECT_TRUE(test.host->key_down(KeyEvent{.key = Key::Enter}));
  EXPECT_EQ(test.count, 2);
}

TEST(Button, IgnoresAShortcutThatMerelyContainsItsKey) {
  // Control-Space is somebody else's binding; a focused button must not eat it.
  Clicked test;
  test.host->set_focus(test.button);

  EXPECT_FALSE(
      test.host->key_down(KeyEvent{.key = Key::Space, .modifiers = Modifiers{.control = true}}));
  EXPECT_EQ(test.count, 0);
}

TEST(Button, ClickingItFocusesIt) {
  Clicked test;
  test.host->mouse_down(press(40.0, 12.0));
  EXPECT_EQ(test.host->focused(), test.button);
}

// --------------------------------------------------------------------- box --

TEST(Box, LaysChildrenOutWithTheThemesSpacing) {
  const LayoutContext context = flat_context();
  Box row(Axis::Horizontal);
  auto& first = row.emplace<Button>("One");
  auto& second = row.emplace<Button>("Two");

  row.arrange(Rect{0.0, 0.0, 400.0, 40.0}, context);

  EXPECT_DOUBLE_EQ(first.bounds().x, 0.0);
  EXPECT_DOUBLE_EQ(second.bounds().x, first.bounds().right() + context.metrics().spacing);
}

TEST(Box, ASpacerPushesWhatFollowsToTheEnd) {
  Box row(Axis::Horizontal);
  row.emplace<Button>("Left");
  row.emplace<Spacer>();
  auto& last = row.emplace<Button>("Right");

  row.arrange(Rect{0.0, 0.0, 400.0, 40.0}, flat_context());
  EXPECT_DOUBLE_EQ(last.bounds().right(), 400.0);
}

TEST(Box, AHiddenChildTakesNoRoom) {
  const LayoutContext context = flat_context();
  Box row(Axis::Horizontal);
  auto& hidden = row.emplace<Button>("Gone");
  auto& shown = row.emplace<Button>("Here");
  hidden.set_visible(false);

  row.arrange(Rect{0.0, 0.0, 400.0, 40.0}, context);
  EXPECT_DOUBLE_EQ(shown.bounds().x, 0.0) << "it left a gap where the hidden child was";
}

TEST(Box, AColumnStacksDownwards) {
  Box column(Axis::Vertical);
  auto& top = column.emplace<Button>("Top");
  auto& bottom = column.emplace<Button>("Bottom");

  column.arrange(Rect{0.0, 0.0, 200.0, 400.0}, flat_context());
  EXPECT_LT(top.bounds().y, bottom.bounds().y);
  EXPECT_DOUBLE_EQ(top.bounds().width, 200.0) << "stretch is the default across the axis";
}

TEST(Box, SizingSumsAlongTheAxisAndMaxesAcross) {
  const LayoutContext context = flat_context();
  Box row(Axis::Horizontal);
  auto& first = row.emplace<Button>("One");
  auto& second = row.emplace<Button>("Two");

  const double widths = first.sizing(Axis::Horizontal, context).basis +
                        second.sizing(Axis::Horizontal, context).basis;

  EXPECT_DOUBLE_EQ(row.sizing(Axis::Horizontal, context).basis,
                   widths + context.metrics().spacing);
  EXPECT_DOUBLE_EQ(row.sizing(Axis::Vertical, context).basis,
                   context.metrics().control_height);
}

TEST(Box, SpacingCanBeOverriddenPerBox) {
  Box row(Axis::Horizontal);
  row.set_spacing(0.0);
  auto& first = row.emplace<Button>("One");
  auto& second = row.emplace<Button>("Two");

  row.arrange(Rect{0.0, 0.0, 400.0, 40.0}, flat_context());
  EXPECT_DOUBLE_EQ(second.bounds().x, first.bounds().right());
}

TEST(Box, NestedBoxesArrangeTheirOwnChildren) {
  const LayoutContext context = flat_context();
  Box column(Axis::Vertical);
  auto& row = column.emplace<Box>(Axis::Horizontal);
  auto& button = row.emplace<Button>("Deep");

  column.arrange(Rect{10.0, 20.0, 300.0, 200.0}, context);
  EXPECT_DOUBLE_EQ(button.bounds().x, 10.0) << "the innermost child never got placed";
  EXPECT_DOUBLE_EQ(button.bounds().y, 20.0);
}

TEST(Box, AnEmptyBoxIsHarmless) {
  Box row(Axis::Horizontal);
  row.arrange(Rect{0.0, 0.0, 100.0, 20.0}, flat_context());
  EXPECT_DOUBLE_EQ(row.sizing(Axis::Horizontal, flat_context()).basis, 0.0);
}

// ------------------------------------------------------------------- panel --

TEST(Panel, ContentsSitUnderTheHeaderAndInsideThePadding) {
  const LayoutContext context = flat_context();
  const Metrics& metrics = context.metrics();

  Panel panel("Effect Controls");
  auto& body = panel.emplace<Button>("Reset");
  panel.arrange(Rect{0.0, 0.0, 300.0, 400.0}, context);

  EXPECT_DOUBLE_EQ(panel.header().height, metrics.panel_header_height);
  EXPECT_DOUBLE_EQ(body.bounds().y, metrics.panel_header_height + metrics.panel_padding)
      << "the contents are behind the header";
  EXPECT_DOUBLE_EQ(body.bounds().x, metrics.panel_padding);
}

TEST(Panel, WithNoTitleThereIsNoHeader) {
  const LayoutContext context = flat_context();
  Panel panel;
  auto& body = panel.emplace<Button>("Reset");
  panel.arrange(Rect{0.0, 0.0, 300.0, 400.0}, context);

  EXPECT_TRUE(panel.header().empty());
  EXPECT_DOUBLE_EQ(body.bounds().y, context.metrics().panel_padding);
}

TEST(Panel, DrawsItsHeaderAndClipsItsContents) {
  Panel panel("Project");
  panel.emplace<Button>("Import");
  panel.arrange(Rect{0.0, 0.0, 300.0, 400.0}, flat_context());

  RecordingPainter painter;
  panel.paint(painter, default_theme());

  EXPECT_TRUE(painter.clips_balanced());
  EXPECT_NE(painter.first(DrawCall::Kind::PushClip), nullptr);

  const DrawCall* title = painter.first(DrawCall::Kind::Text);
  ASSERT_NE(title, nullptr);
  EXPECT_EQ(title->run->text, "Project");
  // The header is painted before the contents are clipped in.
  EXPECT_LT(painter.index_of(DrawCall::Kind::Text), painter.index_of(DrawCall::Kind::PushClip));
}

TEST(Panel, AskingForMoreRoomThanItHasDoesNotInvertTheBody) {
  // A panel dragged shorter than its own header must come out empty rather
  // than with a negative body that propagates into every child.
  const LayoutContext context = flat_context();
  Panel panel("Squeezed");
  auto& body = panel.emplace<Button>("Reset");

  panel.arrange(Rect{0.0, 0.0, 300.0, 4.0}, context);

  EXPECT_DOUBLE_EQ(panel.header().height, 4.0);
  EXPECT_GE(body.bounds().height, 0.0);
  EXPECT_GE(body.bounds().width, 0.0);
}

TEST(Panel, KnowsHowMuchRoomItWants) {
  const LayoutContext context = flat_context();
  const Metrics& metrics = context.metrics();

  Panel panel("Inspector");
  panel.emplace<Button>("Reset");

  const double wanted = panel.sizing(Axis::Vertical, context).basis;
  EXPECT_DOUBLE_EQ(wanted, metrics.control_height + metrics.panel_header_height +
                               2.0 * metrics.panel_padding);
}

// --------------------------------------------------------------- caption --

TEST(TitleBar, IsAsTallAsTheThemesCaption) {
  const LayoutContext context = flat_context();
  const TitleBar bar("Cutline");
  EXPECT_DOUBLE_EQ(bar.sizing(Axis::Vertical, context).basis,
                   context.metrics().title_bar_height);
}

TEST(TitleBar, PutsItsButtonsAtTheTrailingEdge) {
  const LayoutContext context = flat_context();
  TitleBar bar("Cutline");
  auto& close = bar.emplace<CaptionButton>(CaptionButton::Kind::Close);
  bar.arrange(Rect{0.0, 0.0, 600.0, 30.0}, context);

  EXPECT_DOUBLE_EQ(close.bounds().right(), 600.0);
  EXPECT_DOUBLE_EQ(close.bounds().height, 30.0);
}

TEST(TitleBar, DrawsItsSurfaceAndItsTitle) {
  TitleBar bar("Cutline");
  bar.arrange(Rect{0.0, 0.0, 600.0, 30.0}, flat_context());

  RecordingPainter painter;
  bar.paint(painter, default_theme());

  const DrawCall* fill = painter.first(DrawCall::Kind::Fill);
  ASSERT_NE(fill, nullptr);
  EXPECT_EQ(fill->fill, default_theme().style(Part::TitleBar).fill);

  const DrawCall* text = painter.first(DrawCall::Kind::Text);
  ASSERT_NE(text, nullptr);
  EXPECT_EQ(text->run->text, "Cutline");
}

TEST(CaptionButton, DrawsItsGlyphFromPrimitivesRatherThanAFont) {
  // A caption font is not something that can be relied on to exist, and a
  // close button rendering as a missing-glyph box would be worse than one
  // drawn by hand.
  CaptionButton close(CaptionButton::Kind::Close);
  close.arrange(Rect{0.0, 0.0, 48.0, 30.0}, flat_context());

  RecordingPainter painter;
  close.paint(painter, default_theme());

  EXPECT_EQ(painter.count(DrawCall::Kind::Line), 2u) << "a cross is two strokes";
  EXPECT_EQ(painter.count(DrawCall::Kind::Text), 0u) << "it should not need a font";
}

TEST(CaptionButton, EachKindLooksDifferent) {
  const auto shape_of = [](CaptionButton::Kind kind) {
    CaptionButton button(kind);
    button.arrange(Rect{0.0, 0.0, 48.0, 30.0}, flat_context());
    RecordingPainter painter;
    button.paint(painter, default_theme());
    return std::pair{painter.count(DrawCall::Kind::Line),
                     painter.count(DrawCall::Kind::Stroke)};
  };

  EXPECT_NE(shape_of(CaptionButton::Kind::Minimise), shape_of(CaptionButton::Kind::Close));
  EXPECT_NE(shape_of(CaptionButton::Kind::Maximise), shape_of(CaptionButton::Kind::Restore));
  EXPECT_NE(shape_of(CaptionButton::Kind::Minimise), shape_of(CaptionButton::Kind::Maximise));
}

TEST(CaptionButton, IsNotInTheTabOrder) {
  // Closing the window because focus happened to be resting on the close
  // button when Space was pressed would be unforgivable.
  const CaptionButton close(CaptionButton::Kind::Close);
  EXPECT_FALSE(close.focusable());
}

TEST(CaptionButton, IsWiderThanItIsTall) {
  const LayoutContext context = flat_context();
  const CaptionButton close(CaptionButton::Kind::Close);
  EXPECT_GT(close.sizing(Axis::Horizontal, context).basis,
            close.sizing(Axis::Vertical, context).basis);
}

TEST(CaptionButton, StillClicks) {
  int closed = 0;
  WidgetHost host(std::make_unique<TitleBar>("Cutline"));
  auto& close =
      host.root().emplace<CaptionButton>(CaptionButton::Kind::Close, [&closed] { ++closed; });
  host.resize(Rect{0.0, 0.0, 600.0, 30.0}, flat_context());

  const MouseEvent on_close = press(close.bounds().x + 4.0, 15.0);
  host.mouse_down(on_close);
  host.mouse_up(on_close);
  EXPECT_EQ(closed, 1);
}

TEST(CaptionButton, TheBarItselfIsWhatAnswersADragOfTheWindow) {
  // The platform layer asks the tree which parts move the window. Anything
  // sitting on the caption must answer as itself, or the buttons stop working
  // the moment the caption becomes draggable.
  WidgetHost host(std::make_unique<TitleBar>("Cutline"));
  auto& bar = static_cast<TitleBar&>(host.root());
  auto& close = bar.emplace<CaptionButton>(CaptionButton::Kind::Close);
  host.resize(Rect{0.0, 0.0, 600.0, 30.0}, flat_context());

  EXPECT_EQ(host.root().at(20.0, 15.0), &bar) << "the empty caption should drag the window";
  EXPECT_EQ(host.root().at(close.bounds().x + 4.0, 15.0), &close);
}

// --------------------------------------------------------- put together --

TEST(Toolbar, ATypicalRowOfControlsLandsWhereItShould) {
  // The shape most of the interface is: a titled panel holding a toolbar with
  // the far-right button pushed over by a spacer.
  const LayoutContext context = flat_context();

  WidgetHost host(std::make_unique<Panel>("Timeline"));
  auto& panel = static_cast<Panel&>(host.root());
  auto& toolbar = panel.emplace<Box>(Axis::Horizontal);

  int exported = 0;
  auto& cut = toolbar.emplace<Button>("Cut");
  toolbar.emplace<Spacer>();
  auto& exp = toolbar.emplace<Button>("Export", [&exported] { ++exported; });

  host.resize(Rect{0.0, 0.0, 600.0, 300.0}, context);

  const double padding = context.metrics().panel_padding;
  EXPECT_DOUBLE_EQ(exp.bounds().right(), 600.0 - padding);
  // The toolbar is as tall as its controls, not as tall as the panel.
  EXPECT_DOUBLE_EQ(toolbar.bounds().height, context.metrics().control_height);

  const MouseEvent on_export =
      press(exp.bounds().x + 5.0, exp.bounds().y + exp.bounds().height / 2.0);
  host.mouse_down(on_export);
  host.mouse_up(on_export);
  EXPECT_EQ(exported, 1);

  // And Tab reaches both buttons but not the spacer or the panel.
  host.set_focus(nullptr);
  EXPECT_TRUE(host.focus_next());
  EXPECT_EQ(host.focused(), &cut);
  EXPECT_TRUE(host.focus_next());
  EXPECT_EQ(host.focused(), &exp);
  EXPECT_TRUE(host.focus_next());
  EXPECT_EQ(host.focused(), &cut) << "Tab should wrap past the spacer, not stop on it";
}

TEST(Label, ClipsItsTextWhenItHasBeenSqueezed) {
  // A label declares itself shrinkable so it gives way before a panel is forced
  // wider — and for a long time nothing made the second half of that true, so a
  // squeezed one simply drew across whatever was beside it. A parameter row
  // came out reading "Opacity100.0%".
  Label label("A title too long for the room it was given");
  label.arrange(Rect{0.0, 0.0, 20.0, 20.0}, flat_context());

  RecordingPainter painter;
  label.paint(painter, default_theme());
  EXPECT_TRUE(painter.clips_balanced());
  EXPECT_EQ(painter.count(DrawCall::Kind::PushClip), 1u);
}

TEST(Label, DoesNotClipWhenItFits) {
  // A clip costs a save and a restore, and nearly every label has its room.
  Label label("ok");
  label.arrange(Rect{0.0, 0.0, 400.0, 20.0}, flat_context());

  RecordingPainter painter;
  label.paint(painter, default_theme());
  EXPECT_EQ(painter.count(DrawCall::Kind::PushClip), 0u);
}

// ----------------------------------------------------------------- grab row --

struct Grabbed {
  Grabbed() {
    host = std::make_unique<WidgetHost>(std::make_unique<Widget>());
    row = &host->root().emplace<GrabRow>(Axis::Horizontal);
    box = &row->emplace<Checkbox>("On", false);
    host->resize(Rect{0.0, 0.0, 400.0, 200.0}, flat_context());
    row->arrange(Rect{0.0, 0.0, 300.0, 24.0}, flat_context());
  }

  std::unique_ptr<WidgetHost> host;
  GrabRow* row = nullptr;
  Checkbox* box = nullptr;
};

TEST(GrabRow, APressThatDoesNotMoveIsNotADrag) {
  Grabbed test;
  int drops = 0;
  test.row->set_on_drop([&](double, double) { ++drops; });

  test.host->mouse_down(MouseEvent{.x = 200.0, .y = 12.0, .button = MouseButton::Left});
  test.host->mouse_up(MouseEvent{.x = 200.0, .y = 12.0, .button = MouseButton::Left});
  EXPECT_EQ(drops, 0);
}

TEST(GrabRow, MovingPastTheThresholdBeginsADrag) {
  Grabbed test;
  int drags = 0;
  double last_y = 0.0;
  test.row->set_on_drag([&](double, double y) {
    ++drags;
    last_y = y;
  });

  test.host->mouse_down(MouseEvent{.x = 200.0, .y = 12.0, .button = MouseButton::Left});
  test.host->mouse_move(MouseEvent{.x = 200.0, .y = 12.0 + GrabRow::kDragThreshold - 1.0});
  EXPECT_EQ(drags, 0) << "a wobble began a drag";

  test.host->mouse_move(MouseEvent{.x = 200.0, .y = 90.0});
  EXPECT_EQ(drags, 1);
  EXPECT_DOUBLE_EQ(last_y, 90.0);
  EXPECT_TRUE(test.row->dragging());
}

TEST(GrabRow, TheDragKeepsArrivingBelowTheRow) {
  // A reorder always leaves the row it started on, so the capture a handled
  // press takes is the whole point.
  Grabbed test;
  double last_y = 0.0;
  test.row->set_on_drag([&](double, double y) { last_y = y; });

  test.host->mouse_down(MouseEvent{.x = 200.0, .y = 12.0, .button = MouseButton::Left});
  ASSERT_EQ(test.host->captured(), test.row);
  test.host->mouse_move(MouseEvent{.x = 200.0, .y = 500.0});
  EXPECT_DOUBLE_EQ(last_y, 500.0);
}

TEST(GrabRow, AControlInsideItKeepsItsOwnBehaviour) {
  // A press on the checkbox is the checkbox's; the row only sees what nothing
  // inside it wanted.
  Grabbed test;
  int drags = 0;
  test.row->set_on_drag([&](double, double) { ++drags; });

  const Rect box = test.box->bounds();
  test.host->mouse_down(MouseEvent{
      .x = box.x + 2.0, .y = box.y + box.height / 2.0, .button = MouseButton::Left});
  test.host->mouse_move(MouseEvent{.x = box.x + 2.0, .y = box.y + 90.0});
  EXPECT_EQ(drags, 0);
}

TEST(GrabRow, ARightClickAsksForAMenu) {
  Grabbed test;
  int menus = 0;
  test.row->set_on_context_menu([&](double, double) { ++menus; });

  test.host->mouse_down(MouseEvent{.x = 200.0, .y = 12.0, .button = MouseButton::Right});
  EXPECT_EQ(menus, 1);
}

TEST(GrabRow, ARightClickWithNoMenuIsNotSwallowed) {
  Grabbed test;
  EXPECT_FALSE(test.host->mouse_down(
      MouseEvent{.x = 200.0, .y = 12.0, .button = MouseButton::Right}));
}

TEST(GrabRow, DrawsAnInsertionLineWhenItIsTheTarget) {
  // A line across the top rather than a lit row: the question a reorder asks is
  // where the card goes, and "onto this one" is not a place.
  Grabbed test;
  RecordingPainter quiet;
  test.row->paint(quiet, default_theme());
  const std::size_t before = quiet.count(DrawCall::Kind::Line);

  test.row->set_selected(true);
  RecordingPainter painter;
  test.row->paint(painter, default_theme());
  EXPECT_EQ(painter.count(DrawCall::Kind::Line), before + 1);
}

// -------------------------------------------------------------- cursors --

TEST(Cursor, AWidgetWithNothingToSayLetsTheQuestionThrough) {
  // `Arrow` means "nothing to say" rather than "an arrow". Without that rule a
  // label lying across a timeline would blank out the timeline's own answer.
  auto owned = std::make_unique<Splitter>(Axis::Horizontal);
  Splitter* splitter = owned.get();
  splitter->emplace<Panel>();
  splitter->emplace<Panel>();

  WidgetHost host(std::move(owned));
  host.resize(Rect{0.0, 0.0, 400.0, 200.0}, flat_context());

  // Over a pane, which has nothing to say — and neither has the splitter away
  // from its dividers.
  host.mouse_move(MouseEvent{.x = 40.0, .y = 100.0});
  EXPECT_EQ(host.cursor(), Cursor::Arrow);

  // Over the divider, where the splitter does.
  host.mouse_move(MouseEvent{.x = 200.0, .y = 100.0});
  EXPECT_EQ(host.cursor(), Cursor::ResizeWE);
}

TEST(Cursor, ASplitterSaysWhichWayItsDividersGo) {
  auto owned = std::make_unique<Splitter>(Axis::Vertical);
  Splitter* splitter = owned.get();
  splitter->emplace<Panel>();
  splitter->emplace<Panel>();

  WidgetHost host(std::move(owned));
  host.resize(Rect{0.0, 0.0, 400.0, 200.0}, flat_context());

  host.mouse_move(MouseEvent{.x = 200.0, .y = 100.0});
  EXPECT_EQ(host.cursor(), Cursor::ResizeNS);
}

TEST(Cursor, WithThePointerOffTheWindowThereIsNoCursorToAskAbout) {
  WidgetHost host(std::make_unique<Panel>());
  host.resize(Rect{0.0, 0.0, 100.0, 100.0}, flat_context());
  EXPECT_EQ(host.cursor(), Cursor::Arrow);
}

}  // namespace
}  // namespace cutline::ui
