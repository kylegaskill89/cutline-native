#pragma once

/// Premiere's Morph Cut, the looking half: where each part of one picture has
/// gone in another, everywhere at once.
///
/// A jump cut in an interview is the same shot either side, with the head a
/// little elsewhere. What hides it is knowing, for every point of the outgoing
/// frame, where that point is in the incoming one; then both frames can be
/// drawn partway towards each other and dissolved, and the head moves rather
/// than jumping. Premiere adds a face tracker to that. This has none, and
/// leans instead on the field being smooth: the background stays put, a head
/// moves as one, and nothing tears in between.
///
/// The method is the usual dense one. The two pictures are matched from
/// coarse to fine, so a head that has moved a tenth of the frame is found as
/// readily as one that has hardly moved; at each size the incoming picture is
/// drawn back along the field found so far and the difference left is solved
/// for in a small window around every pixel, then the field is smoothed. What
/// is kept is the field at the corners of a coarse grid, which is what the
/// renderer's mesh is drawn from.

#include "cutline/track/tracker.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace cutline::track {

/// The grid the field is kept at: corners, so one more each way than cells.
inline constexpr int kMorphColumns = 32;
inline constexpr int kMorphRows = 18;

/// Where each grid corner of one picture is in another, as a move in
/// fractions of the frame, row after row: (columns + 1) × (rows + 1) of them.
struct FlowField {
  int columns = kMorphColumns;
  int rows = kMorphRows;
  std::vector<Point> moves;

  /// The move at a point of the frame, in fractions, read bilinearly between
  /// the corners.
  [[nodiscard]] Point at(double x, double y) const noexcept;
};

/// How `from` has to move to become `to`. The two are the same size.
[[nodiscard]] FlowField dense_flow(const Image& from, const Image& to,
                                   int columns = kMorphColumns, int rows = kMorphRows);

/// What looking at a Morph Cut found: one field per frame of the transition,
/// from the outgoing picture to the incoming one.
struct MorphAnalysis {
  /// Frames a second, the sequence's: one field per frame drawn.
  double fps = 30.0;
  std::vector<FlowField> frames;
};

[[nodiscard]] bool save_morph(const MorphAnalysis& analysis, const std::filesystem::path& path);
[[nodiscard]] std::optional<MorphAnalysis> load_morph(const std::filesystem::path& path);

}  // namespace cutline::track
