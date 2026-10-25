/// The drawn keyboard in the Keyboard Shortcuts window.

#include "cutline/ui/keyboard_view.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"

#include <gtest/gtest.h>

#include <set>

namespace cutline::ui {
namespace {

[[nodiscard]] MouseEvent press_at(const Rect& area) {
  return MouseEvent{.x = area.x + area.width / 2.0, .y = area.y + area.height / 2.0,
                    .button = MouseButton::Left};
}

[[nodiscard]] const KeyboardView::Cap& cap_for(const KeyboardView& view, Key key) {
  for (const KeyboardView::Cap& cap : view.caps()) {
    if (cap.key == key) return cap;
  }
  ADD_FAILURE() << "no cap for " << to_string(key);
  return view.caps().front();
}

struct Drawn {
  Drawn() {
    view.arrange(Rect{0.0, 0.0, 740.0, 260.0},
                 LayoutContext{default_theme(), measurer});
  }
  RecordingPainter measurer;
  KeyboardView view;
};

TEST(KeyboardView, EveryKeyThatCanBeBoundIsOnItOnce) {
  const KeyboardView view;
  std::set<Key> seen;
  for (const KeyboardView::Cap& cap : view.caps()) {
    if (cap.key == Key::None) continue;
    EXPECT_TRUE(seen.insert(cap.key).second) << to_string(cap.key) << " twice";
  }
  for (const Key key : {Key::A, Key::Z, Key::Digit0, Key::Space, Key::Escape, Key::F12,
                        Key::Left, Key::Home, Key::Delete, Key::Backtick, Key::Backslash}) {
    EXPECT_TRUE(seen.contains(key)) << to_string(key);
  }
}

TEST(KeyboardView, NoTwoKeysOverlap) {
  Drawn test;
  const auto& caps = test.view.caps();
  for (std::size_t i = 0; i < caps.size(); ++i) {
    for (std::size_t j = i + 1; j < caps.size(); ++j) {
      const Rect a = test.view.cap_rect(caps[i]);
      const Rect b = test.view.cap_rect(caps[j]);
      const bool overlap = a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
      EXPECT_FALSE(overlap) << caps[i].legend << " and " << caps[j].legend;
    }
  }
}

TEST(KeyboardView, ClickingAKeyChoosesIt) {
  Drawn test;
  Key chosen = Key::None;
  test.view.set_on_choose([&](Key key) { chosen = key; });
  EXPECT_TRUE(test.view.on_mouse_down(press_at(test.view.cap_rect(cap_for(test.view, Key::K)))));
  EXPECT_EQ(chosen, Key::K);
  EXPECT_EQ(test.view.selected(), Key::K);
}

TEST(KeyboardView, ClickingAModifierShowsItsLayer) {
  Drawn test;
  Modifiers shown;
  test.view.set_on_modifiers([&](Modifiers m) { shown = m; });
  for (const KeyboardView::Cap& cap : test.view.caps()) {
    if (cap.modifier == KeyboardView::Modifier::Control) {
      test.view.on_mouse_down(press_at(test.view.cap_rect(cap)));
      break;
    }
  }
  EXPECT_TRUE(shown.control);
  EXPECT_TRUE(test.view.modifiers().control);
}

TEST(KeyboardView, ABoundKeyCarriesItsCommandsName) {
  Drawn test;
  test.view.set_bound({{Key::C, KeyboardView::Bound{.label = "Razor Tool"}}});
  RecordingPainter painter;
  test.view.paint(painter, default_theme());
  bool named = false;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind == DrawCall::Kind::Text && call.run->text == "Razor Tool") named = true;
  }
  EXPECT_TRUE(named);
  const Rect c = test.view.cap_rect(cap_for(test.view, Key::C));
  EXPECT_EQ(test.view.tooltip_at(c.x + 2.0, c.y + 2.0), "C: Razor Tool");
}

}  // namespace
}  // namespace cutline::ui
