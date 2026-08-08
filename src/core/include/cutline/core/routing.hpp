#pragma once

/// Where each track's sound goes: submixes, sends, and the order they have to
/// be mixed in.
///
/// Everything else in the audio path treats a track as a lane that adds into
/// the mix. That is one arrangement out of several, and it is the one that
/// cannot express the thing a dub stage is for: putting one compressor across
/// all the dialogue, one reverb behind six tracks, and one fader on the music
/// bed as a whole. A submix is a track whose input is other tracks; a send is a
/// copy of a track poured into one.
///
/// Pure, like the rest of the core. What a bus *sounds* like is the mixer's
/// business — what this decides is which bus feeds which, in what order they
/// can be run, and what a route means when it names something that is missing
/// or would close a loop.
///
/// **Two numberings, again.** `render::plan_audio` numbers audio tracks 0, 1,
/// 2 and skips the video ones; everything else counts tracks as the project
/// lists them. That difference has already cost one silent meter, so nothing
/// here returns a bare `int`: a `BusRoute` carries both, named.

#include "cutline/core/model.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cutline::core {

/// The track with this id, or null. Any track, of either kind.
[[nodiscard]] const Track* track_with_id(const Project& p, std::string_view id) noexcept;

/// Whether the project has any submix at all.
///
/// Worth asking before doing any of the work below: nearly every project has
/// none, and a mixer that knows that can sum its lanes the way it always did.
[[nodiscard]] bool has_submixes(const Project& p) noexcept;

/// The submix a track actually feeds, or null when it feeds the master.
///
/// Null covers all four ways of meaning the master: an empty `output`, one
/// naming a track that is gone, one naming a track that is not a submix, and
/// one that would close a loop. A route is *read* rather than trusted, so a
/// project that has had a submix deleted underneath it stays audible instead of
/// going quiet.
[[nodiscard]] const Track* routed_output(const Project& p, const Track& t);

/// Whether `from` reaches `to` by following outputs and sends — that is,
/// whether anything put on `from` can end up on `to`.
///
/// Reflexive: a track reaches itself. That is what makes `can_route` a single
/// question rather than a loop check with a special case at one end.
[[nodiscard]] bool reaches(const Project& p, std::string_view from_id, std::string_view to_id);

/// Whether routing `from` into `to` would be legal: `to` is a submix, it is not
/// `from`, and `to` does not already feed `from`.
///
/// Asked by the interface before it offers the route, so a loop is something
/// that cannot be built rather than something the mixer has to survive. The
/// mixer survives it anyway — see `bus_routes` — because a file can say
/// anything.
[[nodiscard]] bool can_route(const Project& p, std::string_view from_id, std::string_view to_id);

/// One resolved send: which bus, how much, and where it is tapped from.
struct SendRoute {
  /// The destination, in the plan's numbering.
  int to_lane = -1;
  double level = 1.0;
  bool pre_fader = false;

  friend bool operator==(const SendRoute&, const SendRoute&) = default;
};

/// One audio track, with its routing resolved against the rest of the project.
struct BusRoute {
  /// The audio-track ordinal — `render::plan_audio`'s numbering, and the
  /// mixer's lane.
  int lane = -1;
  /// The index into `Project::tracks` — everybody else's numbering.
  int track_index = -1;
  /// Where this bus's output goes, as a lane, or -1 for the master.
  int output_lane = -1;
  /// Whether it is a bus fed by other tracks rather than a lane of clips.
  bool submix = false;
  /// Its sends, with unroutable ones already dropped.
  std::vector<SendRoute> sends;

  friend bool operator==(const BusRoute&, const BusRoute&) = default;
};

/// Every audio track, in an order where a bus comes after everything that feeds
/// it.
///
/// That order is the whole point: a submix cannot be run until the tracks
/// pouring into it have been, and getting it wrong does not crash — it plays
/// last block's audio through this block's compressor, one buffer late, which
/// is the kind of fault that sounds like a bad plugin rather than like a bug.
///
/// A project with no submixes comes back in plain lane order, which is the
/// order it has always been mixed in.
///
/// Loops cannot be built through the interface and can still arrive in a file.
/// A route that would close one is dropped — that bus feeds the master instead
/// — so the worst a hand-edited project does is sound wrong in a way you can
/// see, rather than never finishing.
[[nodiscard]] std::vector<BusRoute> bus_routes(const Project& p);

// ------------------------------------------------------------------- edits --

/// Adds an empty submix at the bottom of the track stack.
///
/// Below the audio tracks, which is where Premiere puts them and where they
/// belong for the same reason video tracks are above: the stack reads as the
/// signal flows, sources at the top and the things they feed underneath.
[[nodiscard]] Project add_submix_track(Project p, std::string label = {});

/// Sends a track's output to a submix, or to the master when given nothing.
/// No-op when the route would not be legal — see `can_route`.
[[nodiscard]] Project set_track_output(Project p, std::string_view track_id,
                                       std::string_view output_id);

/// Adds a send, or changes the one already going to that bus. No-op when the
/// route would not be legal.
[[nodiscard]] Project set_send(Project p, std::string_view track_id, std::string_view to_id,
                               double level, bool pre_fader = false);

/// Removes the send from `track_id` to `to_id`, if there is one.
[[nodiscard]] Project remove_send(Project p, std::string_view track_id, std::string_view to_id);

}  // namespace cutline::core
