#pragma once

/// One way to ask a question, for every dialog in the application.
///
/// Each dialog used to be built by hand: a bold label for a title, rows of
/// whatever, and an OK and a Cancel left wherever the row put them. So the
/// buttons sat on the left in some and the order varied, Enter did nothing,
/// Escape threw a dialog away without telling it, and a mistyped number made
/// most of them close silently and do nothing -- which looks exactly like the
/// edit having worked.
///
/// A dialog here has a title, rows whose labels line up, a line for what is
/// wrong, and its buttons at the bottom right in the platform's order: the
/// accepting one first and marked as the default, Cancel after it. Enter
/// accepts and Escape cancels, wherever focus is inside it. Accepting asks the
/// dialog's owner whether the answer is good; when it is not, the dialog stays
/// open and says why, rather than closing on nothing.

#include "cutline/ui/widgets.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cutline::ui {

class Dialog : public Panel {
 public:
  /// What accepting does. Nothing back means it is done and the dialog closes;
  /// a message means it is not, and the message is shown.
  using Accept = std::function<std::optional<std::string>()>;

  explicit Dialog(std::string title);

  /// Where the dialog's own content goes, above the buttons.
  [[nodiscard]] Box& body() noexcept { return *body_; }

  /// A labelled row in the body: the label on the left, at the width of the
  /// widest label in the dialog so the controls line up, and the returned box
  /// for whatever goes beside it.
  Box& row(std::string label);

  /// The accepting button's text and what it does. "OK" for a question,
  /// "Close" for a dialog that edits as it goes and has nothing to undo.
  void set_accept(std::string text, Accept accept);
  /// The cancelling button, and anything to do on the way out besides closing.
  /// No text means no button: a dialog that edits as it goes has nothing to
  /// cancel, and Escape then simply closes it.
  void set_cancel(std::string text, std::function<void()> cancel = {});
  /// Another button, at the left of the row where the platform puts actions
  /// that are not an answer to the question -- a marker's Delete.
  Button& add_button(std::string text, std::function<void()> on_click);
  /// The accepting button, for a dialog that has to say it cannot be pressed.
  [[nodiscard]] Button& accept_button() noexcept { return *accept_button_; }

  /// How the dialog is taken away -- closing the popup it is in, or the window.
  void set_on_close(std::function<void()> close) { on_close_ = std::move(close); }

  /// Enter, or the accepting button.
  void accept();
  /// Escape, or the cancelling button.
  void cancel();

  /// What is wrong with the answer, or empty.
  [[nodiscard]] const std::string& error() const noexcept;

  void layout(const LayoutContext& context) override;

 private:
  void close();

  Box* body_ = nullptr;
  Label* error_ = nullptr;
  Box* buttons_ = nullptr;
  Box* extras_ = nullptr;
  Button* accept_button_ = nullptr;
  Button* cancel_button_ = nullptr;
  std::vector<Label*> row_labels_;
  Accept accept_;
  std::function<void()> cancel_;
  std::function<void()> on_close_;
};

}  // namespace cutline::ui
