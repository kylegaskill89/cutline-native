#include "cutline/core/history.hpp"

#include <utility>

namespace cutline::core {

void History::push(Project snapshot) {
  undo_.push_back(std::move(snapshot));
  if (limit_ > 0 && undo_.size() > limit_) undo_.erase(undo_.begin());
  redo_.clear();
}

std::optional<Project> History::undo(Project current) {
  if (undo_.empty()) return std::nullopt;
  redo_.push_back(std::move(current));
  Project previous = std::move(undo_.back());
  undo_.pop_back();
  return previous;
}

std::optional<Project> History::redo(Project current) {
  if (redo_.empty()) return std::nullopt;
  undo_.push_back(std::move(current));
  Project next = std::move(redo_.back());
  redo_.pop_back();
  return next;
}

void History::clear() noexcept {
  undo_.clear();
  redo_.clear();
}

}  // namespace cutline::core
