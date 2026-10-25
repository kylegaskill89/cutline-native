#include "cutline/ui/dialog.hpp"

#include <algorithm>

namespace cutline::ui {

namespace {

/// The narrowest a dialog button is drawn: Windows' 75 dialog units, which is
/// what made OK and Cancel the same width on every dialog of the period.
constexpr double kMinButtonWidth = 75.0;

}  // namespace

// -------------------------------------------------------------------- form --

FormRow::FormRow(std::string label) : Box(Axis::Horizontal) {
  label_ = &emplace<Label>(std::move(label));
}

Form::Form() : Box(Axis::Vertical) {}

FormRow& Form::row(std::string label) { return emplace<FormRow>(std::move(label)); }

void Form::align(const LayoutContext& context) const {
  std::vector<Label*> labels;
  for (const std::unique_ptr<Widget>& child : children()) {
    if (auto* line = dynamic_cast<FormRow*>(child.get()); line != nullptr && line->visible()) {
      labels.push_back(&line->label());
    }
  }
  double widest = 0.0;
  for (Label* label : labels) {
    label->set_min_width(0.0);
    widest = std::max(widest, label->sizing(Axis::Horizontal, context).basis);
  }
  for (Label* label : labels) label->set_min_width(widest);
}

LayoutItem Form::sizing(Axis axis, const LayoutContext& context) const {
  // Aligned before being measured as well as before being laid out, or the
  // first answer about its width would be for ragged labels.
  if (axis == Axis::Horizontal) align(context);
  return Box::sizing(axis, context);
}

void Form::layout(const LayoutContext& context) {
  align(context);
  Box::layout(context);
}

// ------------------------------------------------------------------ dialog --

Dialog::Dialog(std::string title) : Panel(std::move(title)) {
  body_ = &emplace<Form>();

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
    // The beep Windows gave an answer it would not take.
    if (WidgetHost* owner = host(); owner != nullptr) owner->play(Sound::Refused);
    return;
  }
  // Answered, so whatever it said before is gone.
  if (error_->visible()) {
    error_->set_visible(false);
    invalidate_layout();
  }
  if (closes_on_accept_) close();
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

Dialog* find_dialog(Widget& root) noexcept {
  if (auto* dialog = dynamic_cast<Dialog*>(&root); dialog != nullptr) return dialog;
  for (const std::unique_ptr<Widget>& child : root.children()) {
    if (Dialog* found = find_dialog(*child); found != nullptr) return found;
  }
  return nullptr;
}

}  // namespace cutline::ui
