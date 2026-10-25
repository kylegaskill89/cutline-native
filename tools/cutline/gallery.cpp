#include "gallery.hpp"

#include "cutline/ui/controls.hpp"
#include "cutline/ui/dock_view.hpp"
#include "cutline/ui/skia_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"
#include "cutline/ui/widgets.hpp"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <print>
#include <string>
#include <utility>
#include <vector>

namespace cutline::tool {
namespace {

using ui::Axis;
using ui::Box;
using ui::LayoutContext;
using ui::MouseButton;
using ui::MouseEvent;
using ui::Rect;
using ui::Theme;
using ui::Widget;
using ui::WidgetHost;

/// One control in one state: a window of its own, so its state can be put
/// there with the same events a person would send.
struct Cell {
  std::string caption;
  double width = 150.0;
  double height = 40.0;
  /// Builds the control and returns it; the cell's host owns the root.
  std::function<std::unique_ptr<Widget>()> make;
  /// Puts the control into its state, after the first layout.
  std::function<void(WidgetHost&, Widget&, const LayoutContext&)> pose;
};

struct Row {
  std::string title;
  std::vector<Cell> cells;
};

[[nodiscard]] MouseEvent at_centre(const Widget& widget, MouseButton button = MouseButton::Left) {
  const Rect r = widget.bounds();
  return MouseEvent{.x = r.x + r.width * 0.5, .y = r.y + r.height * 0.5, .button = button};
}

void hover(WidgetHost& host, Widget& widget) { host.mouse_move(at_centre(widget)); }
void press(WidgetHost& host, Widget& widget) {
  host.mouse_move(at_centre(widget));
  host.mouse_down(at_centre(widget));
}
void focus(WidgetHost& host, Widget& widget) {
  host.show_keyboard_cues();
  host.set_focus(&widget);
}

/// The control alone in a row, at its own size, so a cell shows the control
/// and not the control stretched to the cell.
template <typename T, typename... Args>
[[nodiscard]] std::function<std::unique_ptr<Widget>()> alone(std::function<void(T&)> setup,
                                                            Args... args) {
  return [setup, args...]() -> std::unique_ptr<Widget> {
    // In a row in a column, each with a spring after it, so the control is as
    // wide and as tall as it asks to be rather than as big as its cell.
    auto column = std::make_unique<Box>(Axis::Vertical);
    auto& row = column->emplace<Box>(Axis::Horizontal);
    auto& control = row.emplace<T>(args...);
    if (setup) setup(control);
    row.emplace<ui::Spacer>();
    column->emplace<ui::Spacer>();
    return column;
  };
}

/// The first control of type `T` in a tree.
template <typename T>
[[nodiscard]] T* find(Widget& widget) {
  if (auto* hit = dynamic_cast<T*>(&widget); hit != nullptr) return hit;
  for (const std::unique_ptr<Widget>& child : widget.children()) {
    if (T* hit = find<T>(*child); hit != nullptr) return hit;
  }
  return nullptr;
}

template <typename T>
[[nodiscard]] std::function<void(WidgetHost&, Widget&, const LayoutContext&)> posing(
    std::function<void(WidgetHost&, T&)> pose) {
  return [pose](WidgetHost& host, Widget& root, const LayoutContext&) {
    if (T* control = find<T>(root); control != nullptr) pose(host, *control);
  };
}

[[nodiscard]] std::vector<Row> rows() {
  using ui::Button;
  using ui::Checkbox;
  std::vector<Row> out;

  // Buttons, in each of the states the captures show.
  {
    Row row{.title = "Button"};
    const auto button = [](std::function<void(Button&)> setup = {}) {
      return alone<Button>(std::move(setup), std::string("Cancel"));
    };
    row.cells.push_back({.caption = "rest", .make = button()});
    row.cells.push_back({.caption = "hover", .make = button(),
                         .pose = posing<Button>([](WidgetHost& h, Button& b) { hover(h, b); })});
    row.cells.push_back({.caption = "pressed", .make = button(),
                         .pose = posing<Button>([](WidgetHost& h, Button& b) { press(h, b); })});
    row.cells.push_back({.caption = "default",
                         .make = button([](Button& b) { b.set_default(true); })});
    row.cells.push_back({.caption = "focused", .make = button(),
                         .pose = posing<Button>([](WidgetHost& h, Button& b) { focus(h, b); })});
    row.cells.push_back({.caption = "disabled",
                         .make = button([](Button& b) { b.set_enabled(false); })});
    out.push_back(std::move(row));
  }

  {
    Row row{.title = "Check box"};
    const auto box = [](bool checked, std::function<void(Checkbox&)> setup = {}) {
      return alone<Checkbox>(std::move(setup), std::string("Match case"), checked);
    };
    row.cells.push_back({.caption = "off", .make = box(false)});
    row.cells.push_back({.caption = "on", .make = box(true)});
    row.cells.push_back({.caption = "hover", .make = box(false),
                         .pose = posing<Checkbox>([](WidgetHost& h, Checkbox& c) { hover(h, c); })});
    row.cells.push_back({.caption = "pressed", .make = box(false),
                         .pose = posing<Checkbox>([](WidgetHost& h, Checkbox& c) { press(h, c); })});
    row.cells.push_back({.caption = "disabled",
                         .make = box(true, [](Checkbox& c) { c.set_enabled(false); })});
    out.push_back(std::move(row));
  }

  {
    Row row{.title = "Radio"};
    const auto radio = [](std::function<void(ui::RadioGroup&)> setup = {}) {
      return alone<ui::RadioGroup>(std::move(setup), std::vector<std::string>{"Up", "Down"},
                                   std::size_t{1});
    };
    row.cells.push_back({.caption = "rest", .width = 200, .make = radio()});
    row.cells.push_back({.caption = "hover", .width = 200, .make = radio(),
                         .pose = posing<ui::RadioGroup>([](WidgetHost& h, ui::RadioGroup& r) {
                           const Rect b = r.bounds();
                           h.mouse_move(MouseEvent{.x = b.x + 6.0, .y = b.y + b.height * 0.5});
                         })});
    out.push_back(std::move(row));
  }

  {
    Row row{.title = "Text and lists"};
    row.cells.push_back({.caption = "text", .width = 200,
                         .make = alone<ui::TextField>({}, std::string("zzz"))});
    row.cells.push_back({.caption = "text, focused", .width = 200,
                         .make = alone<ui::TextField>({}, std::string("zzz")),
                         .pose = posing<ui::TextField>(
                             [](WidgetHost& h, ui::TextField& t) { h.set_focus(&t); })});
    row.cells.push_back({.caption = "dropdown", .width = 200,
                         .make = alone<ui::Dropdown>({}, std::vector<std::string>{"Desktop", "Icon"},
                                                     std::size_t{0})});
    row.cells.push_back({.caption = "dropdown, hover", .width = 200,
                         .make = alone<ui::Dropdown>({}, std::vector<std::string>{"Desktop", "Icon"},
                                                     std::size_t{0}),
                         .pose = posing<ui::Dropdown>(
                             [](WidgetHost& h, ui::Dropdown& d) { hover(h, d); })});
    out.push_back(std::move(row));
  }

  {
    Row row{.title = "Slider and progress"};
    const auto slider = [] {
      return [] {
        auto box = std::make_unique<Box>(Axis::Horizontal);
        box->emplace<ui::Slider>(ui::ValueRange{.minimum = 0.0, .maximum = 1.0}, 0.6);
        return std::unique_ptr<Widget>(std::move(box));
      };
    };
    row.cells.push_back({.caption = "slider", .width = 220, .make = slider()});
    row.cells.push_back({.caption = "slider, hover", .width = 220, .make = slider(),
                         .pose = posing<ui::Slider>([](WidgetHost& h, ui::Slider& s) {
                           const Rect b = s.bounds();
                           h.mouse_move(MouseEvent{.x = b.x + b.width * 0.6, .y = b.y + b.height * 0.5});
                         })});
    row.cells.push_back({.caption = "progress 30%", .width = 220,
                         .make = [] {
                           auto box = std::make_unique<Box>(Axis::Horizontal);
                           box->emplace<ui::ProgressBar>(0.3);
                           return std::unique_ptr<Widget>(std::move(box));
                         }});
    row.cells.push_back({.caption = "progress 80%", .width = 220,
                         .make = [] {
                           auto box = std::make_unique<Box>(Axis::Horizontal);
                           box->emplace<ui::ProgressBar>(0.8);
                           return std::unique_ptr<Widget>(std::move(box));
                         }});
    out.push_back(std::move(row));
  }

  {
    Row row{.title = "Tabs"};
    const auto tabs = [] {
      return [] {
        auto strip = std::make_unique<ui::TabStrip>();
        strip->set_tabs({{.id = "a", .title = "Speed"}, {.id = "b", .title = "Hardware"},
                         {.id = "c", .title = "Effects"}},
                        0);
        return std::unique_ptr<Widget>(std::move(strip));
      };
    };
    row.cells.push_back({.caption = "first showing", .width = 330, .make = tabs()});
    row.cells.push_back({.caption = "second under the pointer", .width = 330, .make = tabs(),
                         .pose = posing<ui::TabStrip>([](WidgetHost& h, ui::TabStrip& t) {
                           const Rect r = t.tab_rect(1);
                           h.mouse_move(MouseEvent{.x = r.x + r.width * 0.5, .y = r.y + r.height * 0.5});
                         })});
    out.push_back(std::move(row));
  }

  {
    Row row{.title = "Title bar"};
    const auto bar = [](bool active) {
      return [active] {
        auto title = std::make_unique<ui::TitleBar>("Untitled - Notepad");
        title->emplace<ui::CaptionButton>(ui::CaptionButton::Kind::Minimise);
        title->emplace<ui::CaptionButton>(ui::CaptionButton::Kind::Maximise);
        title->emplace<ui::CaptionButton>(ui::CaptionButton::Kind::Close);
        title->set_enabled(active);
        return std::unique_ptr<Widget>(std::move(title));
      };
    };
    row.cells.push_back({.caption = "active", .width = 420, .height = 36, .make = bar(true)});
    row.cells.push_back({.caption = "close under the pointer", .width = 420, .height = 36,
                         .make = bar(true),
                         .pose = [](WidgetHost& h, Widget& root, const LayoutContext&) {
                           Widget* last = root.children().empty()
                                              ? nullptr
                                              : root.children().back().get();
                           if (last != nullptr) hover(h, *last);
                         }});
    out.push_back(std::move(row));
  }

  {
    Row row{.title = "Menu"};
    const auto menu = [] {
      return [] { return std::make_unique<Widget>(); };
    };
    const auto open = [](bool point_at_submenu) {
      return [point_at_submenu](WidgetHost& h, Widget&, const LayoutContext& context) {
        using ui::MenuEntry;
        MenuEntry ticked{.label = "Status Bar", .checked = true};
        MenuEntry greyed{.label = "Paste", .shortcut = "Ctrl+V", .enabled = false};
        MenuEntry more{.label = "New",
                       .submenu = {MenuEntry{.label = "Folder"}, MenuEntry{.label = "Shortcut"}}};
        auto list = std::make_unique<ui::MenuList>(std::vector<MenuEntry>{
            MenuEntry{.label = "Undo", .shortcut = "Ctrl+Z"}, MenuEntry::divider(),
            MenuEntry{.label = "Cut", .shortcut = "Ctrl+X"}, greyed, MenuEntry::divider(), ticked,
            more});
        ui::MenuList* shown = list.get();
        h.open_popup(std::move(list), Rect{4.0, 4.0, 0.0, 0.0});
        (void)h.update_layout(context);
        const std::size_t row = point_at_submenu ? 6 : 2;
        const Rect r = shown->row_rect(row);
        h.mouse_move(MouseEvent{.x = r.x + 20.0, .y = r.y + r.height * 0.5});
      };
    };
    row.cells.push_back({.caption = "a row lit", .width = 320, .height = 230, .make = menu(),
                         .pose = open(false)});
    row.cells.push_back({.caption = "a submenu open", .width = 420, .height = 230, .make = menu(),
                         .pose = open(true)});
    row.cells.push_back({.caption = "tooltip", .width = 160, .height = 60, .make = menu(),
                         .pose = [](WidgetHost& h, Widget&, const LayoutContext&) {
                           h.mouse_move(MouseEvent{.x = 10.0, .y = 6.0});
                           h.show_tooltip("Close");
                         }});
    out.push_back(std::move(row));
  }
  return out;
}

}  // namespace

int write_gallery(std::string_view dir) {
  std::error_code ignored;
  std::filesystem::create_directories(dir, ignored);
  int failures = 0;

  constexpr double kMargin = 12.0;
  constexpr double kCaption = 16.0;
  constexpr double kWidth = 1280.0;

  for (const Theme& theme : ui::built_in_themes()) {
    std::vector<Row> sheet = rows();

    // Laid out first, so the surface can be made the height the rows need.
    struct Placed {
      std::unique_ptr<WidgetHost> host;
      Rect at;
      std::string caption;
      std::function<void(WidgetHost&, Widget&, const LayoutContext&)> pose;
    };
    std::vector<Placed> placed;
    std::vector<std::pair<std::string, double>> titles;
    double y = kMargin;
    for (Row& row : sheet) {
      titles.emplace_back(row.title, y);
      y += kCaption + 4.0;
      double x = kMargin;
      double tallest = 0.0;
      for (Cell& cell : row.cells) {
        if (x + cell.width > kWidth - kMargin) {
          x = kMargin;
          y += tallest + kCaption + 10.0;
          tallest = 0.0;
        }
        placed.push_back(Placed{.host = std::make_unique<WidgetHost>(cell.make()),
                                .at = Rect{x, y + kCaption, cell.width, cell.height},
                                .caption = cell.caption,
                                .pose = std::move(cell.pose)});
        x += cell.width + 10.0;
        tallest = std::max(tallest, cell.height);
      }
      y += tallest + kCaption + 18.0;
    }

    const int height = static_cast<int>(y + kMargin);
    const sk_sp<SkSurface> surface =
        SkSurfaces::Raster(SkImageInfo::MakeN32Premul(static_cast<int>(kWidth), height));
    if (surface == nullptr) {
      ++failures;
      continue;
    }
    SkCanvas* canvas = surface->getCanvas();
    const std::unique_ptr<ui::SkiaPainter> painter = ui::SkiaPainter::create(canvas);
    const LayoutContext context{theme, *painter};

    painter->set_typeface(theme.metrics.font_family);
    painter->set_text_smoothing(theme.metrics.text_smoothing);
    ui::paint_surface(*painter, Rect{0.0, 0.0, kWidth, static_cast<double>(height)},
                      theme.style(ui::Part::Window));
    const ui::SurfaceStyle& words = theme.style(ui::Part::Window);
    for (const auto& [title, top] : titles) {
      painter->text(ui::text_run(Rect{kMargin, top, kWidth, kCaption}, title, words,
                                 theme.metrics.font_size, ui::TextAlign::Left, true));
    }

    for (Placed& cell : placed) {
      WidgetHost& host = *cell.host;
      // A clock that stands still until it is moved: every fade starts at
      // nothing on its first frame, so the cell is drawn once to start them
      // and again a minute later, when each has long arrived.
      auto now = std::make_shared<double>(0.0);
      host.animator().set_clock([now] { return *now; });
      host.resize(Rect{0.0, 0.0, cell.at.width, cell.at.height}, context);
      if (cell.pose) {
        cell.pose(host, host.root(), context);
        (void)host.update_layout(context);
      }

      painter->text(ui::text_run(Rect{cell.at.x, cell.at.y - kCaption, cell.at.width, kCaption},
                                 cell.caption, words, theme.metrics.small_font_size,
                                 ui::TextAlign::Left, false));
      canvas->save();
      canvas->translate(static_cast<float>(cell.at.x), static_cast<float>(cell.at.y));
      canvas->clipRect(SkRect::MakeWH(static_cast<float>(cell.at.width),
                                      static_cast<float>(cell.at.height)));
      host.paint(*painter, theme);
      *now = 60.0;
      (void)host.update_layout(context);
      ui::paint_surface(*painter, Rect{0.0, 0.0, cell.at.width, cell.at.height},
                        theme.style(ui::Part::Window));
      host.paint(*painter, theme);
      canvas->restore();
    }

    SkPixmap pixels;
    const std::string path = std::format("{}/{}.png", dir, theme.id);
    SkFILEWStream file(path.c_str());
    if (!surface->peekPixels(&pixels) || !SkPngEncoder::Encode(&file, pixels, {})) {
      std::println("{}: could not write {}", theme.id, path);
      ++failures;
    }
  }
  return failures;
}

}  // namespace cutline::tool
