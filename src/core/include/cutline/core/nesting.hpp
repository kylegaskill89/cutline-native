#pragma once

/// A sequence inside a sequence.
///
/// Premiere's Nest, and what it is for: an effect applied to a group of clips
/// *as one picture* rather than to each of them. Six shots and a colour cast
/// over all of them is one adjustment on a nest and six identical stacks
/// without one — and the second is not the same picture anyway, since each
/// clip would be graded before it was composited rather than after.
///
/// The arrangement is Premiere's too. A sequence appears in the pool beside the
/// footage, and a nest is an ordinary clip of it — so trimming, effects, a
/// transform, a transition and a label all work on one without anything being
/// told nests exist. See `Media::sequence_id`.
///
/// **It cannot be flattened.** A nest is a compositing boundary: its clips are
/// composited together and the result is what the outer clip's effects run on.
/// Splicing the inner layers into the outer list would apply the nest's effects
/// to each of them separately, which is precisely the thing nesting exists to
/// avoid.

#include "cutline/core/model.hpp"

#include <span>
#include <string>
#include <string_view>

namespace cutline::core {

/// The sequence a clip shows, or null when it shows a file.
[[nodiscard]] const Sequence* nested_sequence(const Project& p,
                                              const Clip& clip) noexcept;

/// Whether `inner` is `outer`, or is nested inside it at any depth.
///
/// What stops a sequence being put inside itself. Directly is the obvious case
/// and the easy one; the case that matters is A inside B inside A, which no
/// check at the moment of nesting would catch without walking.
///
/// Cycles are refused rather than detected later because there is no sensible
/// behaviour for one: rendering it recurses until the stack runs out, and any
/// depth limit is a number that turns a wrong project into a differently wrong
/// project.
[[nodiscard]] bool sequence_contains(const Project& p, std::string_view outer,
                                     std::string_view inner);

/// How long a sequence runs, which is what a nest's pool entry reports.
[[nodiscard]] double sequence_duration(const Sequence& s) noexcept;

/// Brings the pool's entries for sequences back into agreement with them.
///
/// A nest's pool entry mirrors its sequence — the name, the length, the canvas
/// — and every one of those changes when somebody edits inside it. Rather than
/// hunting for the places that could, this reconciles the lot, and the session
/// runs it after every edit.
///
/// Only sequences that are *used* get an entry. A project with three cuts and
/// no nesting should not grow three pool rows nobody asked for; the entry is
/// made when the nest is, and it is left alone afterwards even if the last clip
/// of it is deleted, because the source is still in the pool the way a piece of
/// footage is once imported.
[[nodiscard]] Project sync_nested_media(Project p);

/// Replaces the named clips with a single clip of a new sequence holding them.
///
/// Premiere's Nest. The new sequence takes the canvas and rate of the one being
/// cut in, because that is what the nest will be composited into; its clips
/// keep their tracks and their offsets from the earliest of them, so what was
/// on V2 stays above what was on V1.
///
/// The nest lands on the track and at the time the earliest clip was, and it
/// runs from the start of the earliest to the end of the latest — a gap between
/// two nested clips is part of what was nested, and closing it would move
/// footage nobody asked to move.
///
/// Returns the project unchanged when there is nothing to nest, or when doing
/// it would put a sequence inside itself.
[[nodiscard]] Project nest_clips(Project p, std::span<const std::string> clip_ids,
                                 std::string name = {}, std::string* made_id = nullptr);

}  // namespace cutline::core
