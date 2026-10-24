#include "cutline/ui/dialog.hpp"

#include <algorithm>

namespace cutline::ui {

namespace {

/// The narrowest a dialog button is drawn: Windows' 75 dialog units, which is
/// what made OK and Cancel the same width on every dialog of the period.
constexpr double kMinButtonWidth = 75.0;

}  // namespace

Dialog::Dialog(std::string title) : Panel(std::move(title)) {
  body_ = &emplace<Box>(Axis::Vertical);

  error_ = &emplace<Label>();
  error_->set_small(true);
  error_->set_visible(false);

  buttons_ = &emplace<Box>(Axis::Horizontal);
  extras_ = &buttons_->emplace<Box>(Axis::Horizontal);
  // Shown when something is put in it: an empty row is a widget of no size.
  extras_->set_visible(false);
  buttons_->emplace<Spacer>();
  accept_button_ = &buttons_->emplace<Button>("OK", [this] { accept(); });
  accept_button_->set_min_width(kMinButtonWidth);
  accept_button_->set_default(true);
  cancel_button_ = &buttons_->emplace<Button>("Cancel", [this] { cancel(); });
  cancel_button_->set_min_width(kMinButtonWidth);
}

Box& Dialog::row(std::string label) {
  Box& line = body_->emplace<Box>(Axis::Horizontal);
  Label& name = line.emplace<Label>(std::move(label));
  row_labels_.push_back(&name);
  return line;
}

Button& Dialog::add_button(std::string text, std::function<void()> on_click) {
  extras_->set_visible(true);
  Button& button = extras_->emplace<Button>(std::move(text), std::move(on_click));
  button.set_min_width(kMinButtonWidth);
  return button;
}

void Dialog::set_accept(std::string text, Accept accept) {
  accept_button_->set_text(std::move(text));
  accept_ = std::move(accept);
}

void Dialog::set_cancel(std::string text, std::function<void()> cancel) {
  cancel_button_->set_visible(!text.empty());
  cancel_button_->set_text(std::move(text));
  cancel_ = std::move(cancel);
}

void Dialog::accept() {
  std::optional<std::string> wrong;
  if (accept_) wrong = accept_();
  if (wrong.has_value()) {
    error_->set_text(*wrong);
    error_->set_visible(true);
    invalidate_layout();
    return;
  }
  close();
}

void Dialog::cancel() {
  if (cancel_) cancel_();
  close();
}

const std::string& Dialog::error() const noexcept { return error_->text(); }

void Dialog::close() {
  if (on_close_) {
    on_close_();
  } else if (WidgetHost* owner = host(); owner != nullptr) {
    // In a popup unless somebody said otherwise.
    owner->close_popup();
  }
}

void Dialog::layout(const LayoutContext& context) {
  // The labels of every row as wide as the widest, so the controls beside
  // them start in one column.
  double widest = 0.0;
  for (Label* label : row_labels_) {
    label->set_min_width(0.0);
    widest = std::max(widest, label->sizing(Axis::Horizontal, context).basis);
  }
  for (Label* label : row_labels_) label->set_min_width(widest);
  Panel::layout(context);
}

}  // namespace cutline::ui
