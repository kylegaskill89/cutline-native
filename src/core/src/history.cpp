#include "cutline/core/history.hpp"

#include <map>
#include <string>
#include <utility>

namespace cutline::core {

namespace {

/// Drops the oldest snapshots until no more than `limit` are left. A limit of
/// zero means no limit at all, which is what a `History` built without one used
/// to mean and what nothing in the application asks for.
template <typename Stack>
void trim(Stack& stack, std::size_t limit) {
  if (limit == 0 || stack.size() <= limit) return;
  stack.erase(stack.begin(), stack.begin() + static_cast<std::ptrdiff_t>(stack.size() - limit));
}

}  // namespace

History::Snapshot History::store(Project project, const Snapshot* beside) {
  // What the neighbour holds, by where it lives. Looked up by id rather than by
  // position, because inserting a track moves every one below it.
  std::map<std::pair<std::string, std::string>, const std::shared_ptr<const Track>*> held;
  if (beside != nullptr) {
    for (std::size_t s = 0; s < beside->rest.sequences.size() && s < beside->tracks.size(); ++s) {
      for (const std::shared_ptr<const Track>& track : beside->tracks[s]) {
        held.emplace(std::pair{beside->rest.sequences[s].id, track->id}, &track);
      }
    }
  }

  Snapshot snapshot;
  snapshot.tracks.resize(project.sequences.size());
  for (std::size_t s = 0; s < project.sequences.size(); ++s) {
    Sequence& sequence = project.sequences[s];
    std::vector<std::shared_ptr<const Track>>& kept = snapshot.tracks[s];
    kept.reserve(sequence.tracks.size());
    for (Track& track : sequence.tracks) {
      const auto found = held.find(std::pair{sequence.id, track.id});
      if (found != held.end() && **found->second == track) {
        kept.push_back(*found->second);
      } else {
        kept.push_back(std::make_shared<const Track>(std::move(track)));
      }
    }
    sequence.tracks.clear();
  }
  snapshot.rest = std::move(project);
  return snapshot;
}

Project History::restore(const Snapshot& snapshot) {
  Project project = snapshot.rest;
  for (std::size_t s = 0; s < project.sequences.size() && s < snapshot.tracks.size(); ++s) {
    std::vector<Track>& tracks = project.sequences[s].tracks;
    tracks.reserve(snapshot.tracks[s].size());
    for (const std::shared_ptr<const Track>& track : snapshot.tracks[s]) tracks.push_back(*track);
  }
  return project;
}

void History::push(Project snapshot) {
  undo_.push_back(store(std::move(snapshot), undo_.empty() ? nullptr : &undo_.back()));
  trim(undo_, limit_);
  redo_.clear();
}

void History::set_limit(std::size_t limit) {
  limit_ = limit;
  trim(undo_, limit_);
}

std::optional<Project> History::undo(Project current) {
  if (undo_.empty()) return std::nullopt;
  // Stored beside the snapshot it is about to replace, which is the one it
  // differs from by a single edit.
  Snapshot stored = store(std::move(current), &undo_.back());
  Project previous = restore(undo_.back());
  undo_.pop_back();
  redo_.push_back(std::move(stored));
  return previous;
}

std::optional<Project> History::redo(Project current) {
  if (redo_.empty()) return std::nullopt;
  Snapshot stored = store(std::move(current), &redo_.back());
  Project next = restore(redo_.back());
  redo_.pop_back();
  undo_.push_back(std::move(stored));
  return next;
}

void History::clear() noexcept {
  undo_.clear();
  redo_.clear();
}

}  // namespace cutline::core
