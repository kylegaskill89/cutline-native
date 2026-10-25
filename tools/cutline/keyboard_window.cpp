/// Premiere's Keyboard Shortcuts window.
///
/// A preset to start from, a drawn keyboard that shows what is on each key
/// under the modifiers chosen, and every command in a list that can be
/// searched, with its keys beside it and a way to add one or clear them. A
/// key pressed for a command that another already has moves it, and the
/// window says so. Every change is saved as it is made.

#include "app.hpp"

#include "cutline/ui/keyboard_view.hpp"

namespace cutline::tool {
namespace {

using cutline::editor::Action;
using cutline::editor::Chord;

/// Waits for the next key and reports it as a chord.
///
/// Wants text, which is what makes the window hand it every key first --
/// Ctrl+Z, Delete, a bare letter -- rather than running what they are bound
/// to. A modifier on its own is not a chord and is waited past; Escape on its
/// own gives up.
class ChordCapture : public Label {
 public:
  ChordCapture(std::function<void(Chord)> on_chord, std::function<void()> on_cancel)
      : Label("Press a key..."), on_chord_(std::move(on_chord)), on_cancel_(std::move(on_cancel)) {
    set_focusable(true);
  }

  [[nodiscard]] bool wants_text() const noexcept override { return true; }

  bool on_key_down(const KeyEvent& event) override {
    if (event.key == Key::None) return true;
    if (event.key == Key::Escape && event.modifiers.none()) {
      if (on_cancel_) on_cancel_();
      return true;
    }
    if (on_chord_) {
      on_chord_(Chord{.key = event.key, .control = event.modifiers.control,
                      .shift = event.modifiers.shift, .alt = event.modifiers.alt});
    }
    return true;
  }

 private:
  std::function<void(Chord)> on_chord_;
  std::function<void()> on_cancel_;
};

/// Every chord an action has, as a menu writes them, comma-separated.
[[nodiscard]] std::string chords_text(const App& app, std::string_view action) {
  std::string text;
  for (const Chord& chord : app.keys.chords(action)) {
    if (!text.empty()) text += ", ";
    text += cutline::editor::to_text(chord);
  }
  return text;
}

/// Whether a command is one the search is asking about: by its name, its
/// heading or one of its keys, ignoring case.
[[nodiscard]] bool matches(const App& app, const Action& action, std::string_view search) {
  if (search.empty()) return true;
  const auto lower = [](std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
  };
  const std::string wanted = lower(search);
  return lower(action.label).find(wanted) != std::string::npos ||
         lower(action.category).find(wanted) != std::string::npos ||
         lower(chords_text(app, action.id)).find(wanted) != std::string::npos;
}

void after_change(App& app) {
  save_keys(app);
  app.keyboard_stale = true;
  // The tooltips name keys, and the panels that hold them are built once.
  app.dock_stale = true;
  mark_dirty(app);
}

}  // namespace

/// The window's tree: the preset, the drawing, the search and the list.
[[nodiscard]] std::unique_ptr<Widget> build_keyboard_interface(App& app, Shell* shell) {
  auto root = std::make_unique<Box>(Axis::Vertical);
  root->set_spacing(0.0);

  auto& caption = root->emplace<TitleBar>("Keyboard Shortcuts");
  if (shell != nullptr) shell->title_bar = &caption;
  caption.emplace<CaptionButton>(CaptionButton::Kind::Close, [&app] { close_keyboard_dialog(app); });

  // Nothing to cancel: every change is saved as it is made.
  auto& dialog = root->emplace<cutline::ui::Dialog>("");
  dialog.set_accept("Close", [] { return std::optional<std::string>{}; });
  dialog.set_cancel("");
  dialog.set_on_close([&app] { close_keyboard_dialog(app); });

  auto& preset_row = dialog.row("Keyboard Layout Preset");
  std::vector<std::string> names;
  std::size_t chosen = 0;
  for (std::size_t i = 0; i < cutline::editor::presets().size(); ++i) {
    names.emplace_back(cutline::editor::presets()[i]);
    if (cutline::editor::presets()[i] == app.keys.preset) chosen = i;
  }
  auto& preset = preset_row.emplace<Dropdown>(std::move(names), chosen);
  preset.set_on_change([&app](std::size_t index) {
    // A preset is a fresh start: the person's changes were over the old one.
    app.keys = cutline::editor::preset_keymap(cutline::editor::presets()[index]);
    app.keyboard_note = "Using the " + app.keys.preset + " keys.";
    after_change(app);
  });
  app.keyboard_custom = &preset_row.emplace<Label>("");
  app.keyboard_custom->set_small(true);
  preset_row.emplace<Button>("Reset to Preset", [&app] {
    app.keys = cutline::editor::preset_keymap(app.keys.preset);
    app.keyboard_note = "Back to the " + app.keys.preset + " keys.";
    after_change(app);
  });

  auto& keyboard = dialog.body().emplace<cutline::ui::KeyboardView>();
  keyboard.set_modifiers(app.keyboard_modifiers);
  keyboard.set_on_modifiers([&app](Modifiers modifiers) {
    app.keyboard_modifiers = modifiers;
    app.keyboard_stale = true;
    mark_dirty(app);
  });
  keyboard.set_on_choose([&app](Key key) {
    // The key's chord under the modifiers shown, as the search: the list then
    // shows what is on it, or says nothing is.
    const Chord chord{.key = key, .control = app.keyboard_modifiers.control,
                      .shift = app.keyboard_modifiers.shift, .alt = app.keyboard_modifiers.alt};
    app.keyboard_search = cutline::editor::to_text(chord);
    if (app.keyboard_search_field != nullptr) app.keyboard_search_field->set_text(app.keyboard_search);
    const Action* holder = app.keys.holder(chord);
    app.keyboard_note = holder == nullptr ? app.keyboard_search + " is free."
                                          : app.keyboard_search + " runs " +
                                                std::string(holder->label) + ".";
    app.keyboard_stale = true;
    mark_dirty(app);
  });
  app.keyboard_view = &keyboard;

  auto& search_row = dialog.row("Search");
  auto& search = search_row.emplace<TextField>(app.keyboard_search);
  search.set_placeholder("A command, or a key such as Ctrl+K");
  search.set_on_change([&app](const std::string& text) {
    app.keyboard_search = text;
    app.keyboard_stale = true;
    mark_dirty(app);
  });
  app.keyboard_search_field = &search;

  app.keyboard_note_label = &dialog.body().emplace<Label>("");
  app.keyboard_note_label->set_small(true);

  auto& scroller = dialog.body().emplace<ScrollView>(Axis::Vertical);
  auto list = std::make_unique<Box>(Axis::Vertical);
  list->set_fills_cross(true);
  app.keyboard_list = list.get();
  scroller.set_content(std::move(list));
  return root;
}

void refresh_keyboard_dialog(App& app) {
  if (app.keyboard_window == nullptr || app.keyboard_list == nullptr) return;

  // The drawing: what each key does under the modifiers shown.
  if (app.keyboard_view != nullptr) {
    std::map<Key, cutline::ui::KeyboardView::Bound> bound;
    for (const Action& action : cutline::editor::actions()) {
      for (const Chord& chord : app.keys.chords(action.id)) {
        if (chord.control != app.keyboard_modifiers.control ||
            chord.shift != app.keyboard_modifiers.shift ||
            chord.alt != app.keyboard_modifiers.alt) {
          continue;
        }
        bound[chord.key] = {.label = std::string(action.label),
                            .panel = action.scope == cutline::editor::Scope::Fallback};
      }
    }
    app.keyboard_view->set_bound(std::move(bound));
    app.keyboard_view->set_modifiers(app.keyboard_modifiers);
  }
  // Hidden while they have nothing to say, rather than taking up a line.
  if (app.keyboard_custom != nullptr) {
    const bool custom = cutline::editor::is_custom(app.keys);
    app.keyboard_custom->set_text(custom ? "(Custom)" : "");
    app.keyboard_custom->set_visible(custom);
  }
  if (app.keyboard_note_label != nullptr) {
    app.keyboard_note_label->set_text(app.keyboard_note);
    app.keyboard_note_label->set_visible(!app.keyboard_note.empty());
  }

  // The list, rebuilt in place: every command the search finds, under its
  // heading, with its keys and the two things that can be done to them.
  Box& list = *app.keyboard_list;
  list.clear_children();
  Widget* capture = nullptr;
  std::string_view heading;
  for (const Action& action : cutline::editor::actions()) {
    if (!matches(app, action, app.keyboard_search)) continue;
    if (action.category != heading) {
      heading = action.category;
      list.emplace<Label>(std::string(heading)).set_heading(true);
    }
    auto& row = list.emplace<Box>(Axis::Horizontal);
    row.set_spacing(Space::Normal);
    row.emplace<Label>(std::string(action.label));
    row.emplace<Spacer>();
    const std::string id(action.id);
    if (app.keyboard_capturing == id) {
      auto& waiting = row.emplace<ChordCapture>(
          [&app, id](Chord chord) {
            const Action* lost = cutline::editor::assign(app.keys, id, chord);
            const Action* mine = cutline::editor::find_action(id);
            const std::string keys = cutline::editor::to_text(chord);
            app.keyboard_note =
                lost != nullptr
                    ? keys + " was " + std::string(lost->label) + ", and now runs " +
                          std::string(mine->label) + "."
                    : std::string(mine->label) + " is now on " + keys + ".";
            app.keyboard_capturing.clear();
            after_change(app);
          },
          [&app] {
            app.keyboard_capturing.clear();
            app.keyboard_stale = true;
            mark_dirty(app);
          });
      capture = &waiting;
    } else {
      const std::string keys = chords_text(app, action.id);
      row.emplace<Label>(keys).set_visible(!keys.empty());
      row.emplace<Button>("Add...", [&app, id] {
        app.keyboard_capturing = id;
        app.keyboard_note = "Press the key for it, or Esc to leave it as it is.";
        app.keyboard_stale = true;
        mark_dirty(app);
      });
    }
    auto& clear = row.emplace<Button>("Clear", [&app, id] {
      const Action* action = cutline::editor::find_action(id);
      if (action == nullptr) return;
      app.keys.bindings[id].clear();
      app.keyboard_note = std::string(action->label) + " has no key now.";
      after_change(app);
    });
    clear.set_enabled(!app.keys.chords(action.id).empty());
  }
  if (list.children().empty()) {
    list.emplace<Label>("No command matches \"" + app.keyboard_search + "\".");
  }

  WidgetHost* host = app.keyboard_window->host.get();
  if (host != nullptr) {
    host->request_layout();
    // The key goes straight to the row waiting for it.
    if (capture != nullptr) host->set_focus(capture);
  }
  app.keyboard_window->dirty = true;
}

void open_keyboard_dialog(App& app) {
  if (app.keyboard_window != nullptr) {
    SetForegroundWindow(app.keyboard_window->window);
    return;
  }

  auto shell = std::make_unique<Shell>();
  shell->app = &app;
  shell->is_dialog = true;
  app.keyboard_capturing.clear();
  app.keyboard_note.clear();
  shell->host = std::make_unique<WidgetHost>(build_keyboard_interface(app, shell.get()));
  give_sounds(app, *shell->host);

  RECT owner{};
  GetWindowRect(app.main.window, &owner);
  const auto [units_wide, units_tall] = dialog_size(app, "Keyboard Shortcuts", 820, 660);
  shell->scale = app.main.scale;
  const int width = to_pixels(units_wide, shell->scale);
  const int height = to_pixels(units_tall, shell->scale);
  const int x = owner.left + ((owner.right - owner.left) - width) / 2;
  const int y = owner.top + ((owner.bottom - owner.top) - height) / 2;

  const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | kWindowExStyle, kWindowClass,
                                      L"Keyboard Shortcuts", WS_POPUP | WS_THICKFRAME, x, y,
                                      width, height, app.main.window, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
  shell->size_key = "Keyboard Shortcuts";
  if (window == nullptr) return;
  shell->window = window;
  SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(shell.get()));
  const MARGINS shadow{0, 0, 1, 0};
  DwmExtendFrameIntoClientArea(window, &shadow);
  SetWindowPos(window, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

  app.keyboard_window = std::move(shell);
  refresh_keyboard_dialog(app);

  // Modal, as Premiere's is, and for the reason Preferences is.
  EnableWindow(app.main.window, FALSE);
  ShowWindow(window, SW_SHOW);
  SetForegroundWindow(window);
}

void close_keyboard_dialog(App& app) {
  if (app.keyboard_window == nullptr) return;
  app.keyboard_view = nullptr;
  app.keyboard_list = nullptr;
  app.keyboard_custom = nullptr;
  app.keyboard_note_label = nullptr;
  app.keyboard_search_field = nullptr;
  app.keyboard_capturing.clear();

  const HWND window = app.keyboard_window->window;
  remember_dialog_size(app, *app.keyboard_window);
  app.keyboard_window.reset();
  EnableWindow(app.main.window, TRUE);
  SetForegroundWindow(app.main.window);
  if (window != nullptr) DestroyWindow(window);
  mark_dirty(app);
}

}  // namespace cutline::tool
