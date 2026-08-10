#include "cutline/editor/session.hpp"

#include "cutline/core/query.hpp"
#include "cutline/core/sequences.hpp"
#include "cutline/core/time.hpp"

#include <algorithm>
#include <utility>

namespace cutline::editor {

Session::Session(core::Project project) : project_(std::move(project)), saved_(project_) {}

void Session::mark_saved(std::filesystem::path path) {
  path_ = std::move(path);
  saved_ = project_;
  modified_ = false;
}

std::string Session::document_title() const {
  const std::string name = path_.empty() ? std::string("Untitled") : path_.filename().string();
  return modified_ ? name + " *" : name;
}

void Session::refresh_modified() { modified_ = project_ != saved_; }

bool Session::apply(core::Project next) {
  // Every core operation returns the project unchanged when it cannot apply,
  // so this comparison is what tells a rejected drag from an accepted one. It
  // is also what keeps the undo stack free of entries that undo nothing.
  if (next == project_) return false;

  history_.push(project_);
  project_ = std::move(next);
  prune_selection();
  refresh_modified();
  ++revision_;
  return true;
}

void Session::mark_unsaved() {
  // The comparison is against a snapshot, so the flag cannot simply be set: an
  // edit would recompute it and find the project matching what it was told was
  // on disk. Clearing the snapshot is what makes the difference stick.
  saved_ = core::Project{};
  refresh_modified();
}

void Session::reset(core::Project project, std::filesystem::path path) {
  project_ = std::move(project);
  // The history belongs to the document being closed, not to the new one.
  history_.clear();
  selection_.clear();
  playhead_ = 0.0;
  mark_saved(std::move(path));
  ++revision_;
}

bool Session::undo() {
  std::optional<core::Project> previous = history_.undo(project_);
  if (!previous.has_value()) return false;
  project_ = std::move(*previous);
  // A clip that was undone back out of existence must not stay selected.
  prune_selection();
  // Undoing back to what is on disk means there is nothing to save again,
  // which a latched flag could never work out.
  refresh_modified();
  ++revision_;
  return true;
}

bool Session::redo() {
  std::optional<core::Project> next = history_.redo(project_);
  if (!next.has_value()) return false;
  project_ = std::move(*next);
  prune_selection();
  refresh_modified();
  ++revision_;
  return true;
}

bool Session::is_selected(std::string_view clip_id) const noexcept {
  return std::ranges::find(selection_, clip_id) != selection_.end();
}

void Session::select(std::vector<std::string> clip_ids) {
  selection_ = std::move(clip_ids);
  prune_selection();
  ++revision_;
}

void Session::select_one(std::string clip_id) {
  select(std::vector<std::string>{std::move(clip_id)});
}

void Session::clear_selection() {
  if (selection_.empty()) return;
  selection_.clear();
  ++revision_;
}

std::vector<std::string> Session::selected_group() const {
  std::vector<std::string> out;
  for (const std::string& id : selection_) {
    // Linked clips move and cut together, so an edit acting on one has to act
    // on all of them or the video and its audio drift apart.
    for (std::string& member : core::group_members(project_, id)) {
      if (std::ranges::find(out, member) == out.end()) out.push_back(std::move(member));
    }
  }
  return out;
}

void Session::set_playhead(double seconds) {
  playhead_ = std::max(0.0, core::snap_to_frame(seconds, project_.sequence().fps));
}

bool Session::open_sequence(std::string_view id) {
  const std::size_t at = core::sequence_index(project_, id);
  if (at == std::string::npos || at == project_.open) return at != std::string::npos;

  // Where this one was left, so coming back to a cut finds it as you left it.
  playheads_[project_.sequence().id] = playhead_;

  project_.open = at;

  const auto found = playheads_.find(project_.sequence().id);
  // Through the setter, so the restored position is snapped to the frame grid
  // of the sequence being opened rather than the one being left.
  set_playhead(found == playheads_.end() ? 0.0 : found->second);

  // Nothing selected in this cut is in that one. `prune_selection` drops what
  // is not there, which after a switch is everything.
  prune_selection();

  // Not `apply`: switching is not an edit. The revision still moves, because
  // every view is showing a different sequence now and all of them have to
  // rebuild.
  ++revision_;
  return true;
}

void Session::prune_selection() {
  std::erase_if(selection_, [this](const std::string& id) {
    return core::find_clip(project_, id) == nullptr;
  });
}

}  // namespace cutline::editor
