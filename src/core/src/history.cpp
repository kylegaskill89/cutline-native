#include "cutline/core/history.hpp"

#include <utility>

namespace cutline::core {

namespace {

/// Drops the oldest snapshots until no more than `limit` are left. A limit of
/// zero means no limit at all, which is what a `History` built without one used
/// to mean and what nothing in the application asks for.
void trim(std::vector<Project>& stack, std::size_t limit) {
  if (limit == 0 || stack.size() <= limit) return;
  stack.erase(stack.begin(), stack.begin() + static_cast<std::ptrdiff_t>(stack.size() - limit));
}

}  // namespace

void History::push(Project snapshot) {
  undo_.push_back(std::move(snapshot));
  trim(undo_, limit_);
  redo_.clear();
}

void History::set_limit(std::size_t limit) {
  limit_ = limit;
  trim(undo_, limit_);
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
