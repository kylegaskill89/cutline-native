#include "cutline/core/routing.hpp"

#include "cutline/core/id.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cutline::core {
namespace {

Track* mutable_track(Project& p, std::string_view id) noexcept {
  for (Track& t : p.sequence().tracks) {
    if (t.id == id) return &t;
  }
  return nullptr;
}

/// Everywhere a track's signal goes in one step: its output and its sends.
///
/// One function so that "reaches" and the loop check cannot disagree about
/// whether a send counts. It has to count — a send into a bus that feeds you
/// back is as much a loop as an output into one, and it is the easier of the
/// two to build by accident.
void steps_from(const Track& t, std::vector<std::string_view>& into) {
  into.clear();
  if (!t.output.empty()) into.push_back(t.output);
  for (const Send& s : t.sends) {
    if (!s.to.empty()) into.push_back(s.to);
  }
}

}  // namespace

const Track* track_with_id(const Project& p, std::string_view id) noexcept {
  if (id.empty()) return nullptr;
  for (const Track& t : p.sequence().tracks) {
    if (t.id == id) return &t;
  }
  return nullptr;
}

bool has_submixes(const Project& p) noexcept {
  return std::ranges::any_of(p.sequence().tracks, [](const Track& t) { return t.submix; });
}

bool reaches(const Project& p, std::string_view from_id, std::string_view to_id) {
  if (from_id.empty() || to_id.empty()) return false;
  if (from_id == to_id) return true;

  // A walk over the routes, with a seen set. The seen set is not an
  // optimisation: this is asked *about* projects that may already contain a
  // loop, and without it the walk is what never returns.
  std::unordered_set<std::string> seen{std::string(from_id)};
  std::vector<std::string_view> pending{from_id};
  std::vector<std::string_view> steps;
  while (!pending.empty()) {
    const std::string_view id = pending.back();
    pending.pop_back();
    const Track* t = track_with_id(p, id);
    if (t == nullptr) continue;
    steps_from(*t, steps);
    for (const std::string_view step : steps) {
      if (step == to_id) return true;
      if (seen.insert(std::string(step)).second) pending.push_back(step);
    }
  }
  return false;
}

bool can_route(const Project& p, std::string_view from_id, std::string_view to_id) {
  if (from_id.empty() || to_id.empty()) return false;
  if (from_id == to_id) return false;
  const Track* from = track_with_id(p, from_id);
  const Track* to = track_with_id(p, to_id);
  if (from == nullptr || to == nullptr) return false;
  if (from->kind != TrackKind::Audio || to->kind != TrackKind::Audio) return false;
  // Only a submix can be fed. An ordinary track has clips on it and no way to
  // hear anything else.
  if (!to->submix) return false;
  // The loop check, asked from the far end: `to` must not already reach `from`.
  return !reaches(p, to_id, from_id);
}

const Track* routed_output(const Project& p, const Track& t) {
  if (t.output.empty()) return nullptr;
  const Track* out = track_with_id(p, t.output);
  if (out == nullptr || !out->submix || out->id == t.id) return nullptr;
  // A route that closes a loop is read as the master. It cannot be built
  // through the interface, and a file can say anything.
  if (reaches(p, out->id, t.id)) return nullptr;
  return out;
}

std::vector<BusRoute> bus_routes(const Project& p) {
  std::vector<BusRoute> routes;
  std::unordered_map<std::string, int> lane_of;

  int lane = 0;
  for (std::size_t i = 0; i < p.sequence().tracks.size(); ++i) {
    const Track& t = p.sequence().tracks[i];
    if (t.kind != TrackKind::Audio) continue;
    BusRoute route;
    route.lane = lane++;
    route.track_index = static_cast<int>(i);
    route.submix = t.submix;
    lane_of.emplace(t.id, route.lane);
    routes.push_back(std::move(route));
  }

  // Resolved in a second pass, because a route may name a track further down
  // the list than the one that names it.
  for (BusRoute& route : routes) {
    const Track& t = p.sequence().tracks[static_cast<std::size_t>(route.track_index)];
    if (const Track* out = routed_output(p, t)) {
      const auto found = lane_of.find(out->id);
      if (found != lane_of.end()) route.output_lane = found->second;
    }
    for (const Send& send : t.sends) {
      if (!can_route(p, t.id, send.to)) continue;
      const auto found = lane_of.find(send.to);
      if (found == lane_of.end()) continue;
      route.sends.push_back(
          SendRoute{.to_lane = found->second, .level = send.level, .pre_fader = send.pre_fader});
    }
  }

  // Feed order. Every route above is already loop-free, so counting how many
  // things feed each bus and repeatedly taking the ones nothing feeds any more
  // terminates with all of them taken.
  std::vector<int> feeders(routes.size(), 0);
  const auto count = [&](int to_lane) {
    if (to_lane >= 0 && static_cast<std::size_t>(to_lane) < feeders.size()) {
      ++feeders[static_cast<std::size_t>(to_lane)];
    }
  };
  for (const BusRoute& route : routes) {
    count(route.output_lane);
    for (const SendRoute& send : route.sends) count(send.to_lane);
  }

  std::vector<BusRoute> ordered;
  ordered.reserve(routes.size());
  std::vector<bool> taken(routes.size(), false);
  for (std::size_t pass = 0; pass < routes.size(); ++pass) {
    // The lowest-numbered lane nothing is still waiting to feed. Lowest rather
    // than any, so a project with no submixes at all comes back in exactly the
    // lane order it has always been mixed in.
    std::size_t pick = routes.size();
    for (std::size_t i = 0; i < routes.size(); ++i) {
      if (!taken[i] && feeders[i] == 0) {
        pick = i;
        break;
      }
    }
    if (pick == routes.size()) break;  // unreachable while the routes are loop-free
    taken[pick] = true;
    const auto uncount = [&](int to_lane) {
      if (to_lane >= 0 && static_cast<std::size_t>(to_lane) < feeders.size()) {
        --feeders[static_cast<std::size_t>(to_lane)];
      }
    };
    uncount(routes[pick].output_lane);
    for (const SendRoute& send : routes[pick].sends) uncount(send.to_lane);
    ordered.push_back(routes[pick]);
  }

  // Anything left is in a loop the checks above should already have broken.
  // Appending it rather than dropping it means the worst case is a bus heard a
  // block late instead of a bus that is never heard.
  for (std::size_t i = 0; i < routes.size(); ++i) {
    if (!taken[i]) ordered.push_back(routes[i]);
  }
  return ordered;
}

Project add_submix_track(Project p, std::string label) {
  Track t;
  t.id = new_id("track");
  t.kind = TrackKind::Audio;
  t.submix = true;
  t.label = std::move(label);
  p.sequence().tracks.push_back(std::move(t));
  return p;
}

Project set_track_output(Project p, std::string_view track_id, std::string_view output_id) {
  Track* t = mutable_track(p, track_id);
  if (t == nullptr || t->kind != TrackKind::Audio) return p;
  if (output_id.empty()) {
    t->output.clear();
    return p;
  }
  if (!can_route(p, track_id, output_id)) return p;
  t->output = std::string(output_id);
  return p;
}

Project set_send(Project p, std::string_view track_id, std::string_view to_id, double level,
                 bool pre_fader) {
  if (!can_route(p, track_id, to_id)) return p;
  Track* t = mutable_track(p, track_id);
  if (t == nullptr) return p;
  const double clamped = std::clamp(level, 0.0, kMaxGain);
  for (Send& send : t->sends) {
    if (send.to == to_id) {
      send.level = clamped;
      send.pre_fader = pre_fader;
      return p;
    }
  }
  t->sends.push_back(
      Send{.to = std::string(to_id), .level = clamped, .pre_fader = pre_fader});
  return p;
}

Project remove_send(Project p, std::string_view track_id, std::string_view to_id) {
  Track* t = mutable_track(p, track_id);
  if (t == nullptr) return p;
  std::erase_if(t->sends, [&](const Send& s) { return s.to == to_id; });
  return p;
}

}  // namespace cutline::core
