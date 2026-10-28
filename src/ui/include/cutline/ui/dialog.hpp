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

/// One labelled row of a form: the label, then whatever goes beside it.
class FormRow : public Box {
 public:
  explicit FormRow(std::string label);

  [[nodiscard]] Label& label() noexcept { return *label_; }

 private:
  Label* label_ = nullptr;
};

/// A column of labelled rows whose labels all take the widest one's width, so
/// the controls beside them start in one line -- which is how every settings
/// dialog on the platform reads, Premiere's included.
///
/// Before this each row was a label, a spring and a control, so the controls
/// lined up on the right edge instead; on a page wider than its rows that put
/// a gap down the middle with the labels stranded on one side of it.
///
/// The rows are found when the form is laid out rather than remembered as
/// they are added, because a settings page is cleared and rebuilt in place
/// and remembered rows would outlive it.
class Form : public Box {
 public:
  Form();

  /// A row with this label, for whatever goes beside it.
  FormRow& row(std::string label);

  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;

 private:
  /// Gives every row's label the widest one's width.
  void align(const LayoutContext& context) const;
};

class Dialog : public Panel {
 public:
  /// What accepting does. Nothing back means it is done and the dialog closes;
  /// a message means it is not, and the message is shown.
  using Accept = std::function<std::optional<std::string>()>;

  explicit Dialog(std::string title);

  /// Where the dialog's own content goes, above the buttons.
  [[nodiscard]] Form& body() noexcept { return *body_; }

  /// A labelled row in the body, lined up with the others.
  FormRow& row(std::string label) { return body_->row(std::move(label)); }

  /// The accepting button's text and what it does. "OK" for a question,
  /// "Close" for a dialog that edits as it goes and has nothing to undo. No
  /// text means no button, for a window that only says how far something has
  /// got and has Cancel.
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

  /// Whether accepting closes the dialog. Export does not: its button starts a
  /// render and the window stays to show how it is going.
  void set_closes_on_accept(bool closes) noexcept { closes_on_accept_ = closes; }

  /// How the dialog is taken away -- closing the popup it is in, or the window.
  void set_on_close(std::function<void()> close) { on_close_ = std::move(close); }

  /// Enter, or the accepting button.
  void accept();
  /// Escape, or the cancelling button.
  void cancel();

  /// What is wrong with the answer, or empty.
  [[nodiscard]] const std::string& error() const noexcept;


 private:
  void close();

  Form* body_ = nullptr;
  Label* error_ = nullptr;
  Box* buttons_ = nullptr;
  Box* extras_ = nullptr;
  Button* accept_button_ = nullptr;
  Button* cancel_button_ = nullptr;
  Accept accept_;
  std::function<void()> cancel_;
  std::function<void()> on_close_;
  bool closes_on_accept_ = true;
};

/// The dialog in a tree, if it has one: what Enter and Escape answer to in a
/// window that is a dialog.
[[nodiscard]] Dialog* find_dialog(Widget& root) noexcept;

}  // namespace cutline::ui
