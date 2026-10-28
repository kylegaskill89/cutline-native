#pragma once

/// Warp Stabilizer's second step: from what the analysis found, the warp that
/// steadies each frame.
///
/// The analysis is corners followed through every frame of the clip. From
/// them the camera's path is worked out frame by frame, as far as the Method
/// allows: a move, a move with a turn and a scale, or a perspective. That path
/// is smoothed, or for No Motion held still, and each frame is warped from
/// where the camera was to where the smoothed path says it should have been.
/// Subspace Warp goes further: the corners' own paths are smoothed, and a mesh
/// over the frame is bent so each corner lands on its smoothed path, which is
/// what steadies a scene with depth in it, where no one transform fits all of
/// it.
///
/// Every method's result is the same thing, a mesh, so the shader has one way
/// to draw it. A rolling shutter's lean and the framing's crop or scale are
/// folded into the mesh too.

#include "cutline/track/tracker.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace cutline::track {

/// What the analysis found: the corners on every frame of the clip, at the
/// size it was analysed at.
struct Analysis {
  int width = 0;
  int height = 0;
  double fps = 0.0;
  std::vector<std::vector<TrackPoint>> frames;

  friend bool operator==(const Analysis&, const Analysis&) = default;
};

[[nodiscard]] bool save_analysis(const Analysis& analysis, const std::filesystem::path& path);
[[nodiscard]] std::optional<Analysis> load_analysis(const std::filesystem::path& path);

// Premiere's menus, in its order.
enum class StabilizeResult { SmoothMotion, NoMotion };
enum class StabilizeMethod { Position, PositionScaleRotation, Perspective, SubspaceWarp };
enum class Framing { StabilizeOnly, StabilizeCrop, StabilizeCropAutoScale, SynthesizeEdges };
enum class RollingShutter { Automatic, Enhanced };

struct StabilizeSettings {
  StabilizeResult result = StabilizeResult::SmoothMotion;
  /// Smoothness, 0.5 being Premiere's 50%.
  double smoothness = 0.5;
  StabilizeMethod method = StabilizeMethod::SubspaceWarp;
  bool preserve_scale = false;
  Framing framing = Framing::StabilizeCropAutoScale;
  /// The most Auto-scale may zoom, 1.5 being 150%.
  double maximum_scale = 1.5;
  /// How much of the frame's edge Auto-scale may leave uncovered, 0 to 1.
  double safe_margin = 0.0;
  double additional_scale = 1.0;
  RollingShutter rolling_shutter = RollingShutter::Automatic;
  /// Crop Less <-> Smooth More, 0 to 1.
  double crop_less_smooth_more = 0.5;

  friend bool operator==(const StabilizeSettings&, const StabilizeSettings&) = default;
};

/// Where each point of a frame is drawn from: a mesh over the output frame,
/// each vertex holding the place in the source it shows, both as fractions of
/// the frame. Between vertices, the place is interpolated.
struct WarpGrid {
  int columns = 0;
  int rows = 0;
  /// (columns + 1) × (rows + 1) of them, row by row from the top.
  std::vector<Point> source;

  /// The source place for the output place (`x`, `y`), fractions.
  [[nodiscard]] Point at(double x, double y) const noexcept;
};

struct Stabilization {
  /// One per analysed frame.
  std::vector<WarpGrid> frames;
  /// What Auto-scale reads: how far the frames were zoomed to fill.
  double auto_scale = 1.0;
  /// For Stabilize, Crop: how much of the frame is left showing about its
  /// centre, the rest cut away. 1 otherwise.
  double visible = 1.0;
  /// The rolling shutter's readout as a share of a frame, as it was found.
  double readout = 0.0;
  /// Where the first frame's scene is on each frame, in the analysis's pixels:
  /// what Synthesize Edges uses to find where a neighbouring frame's picture
  /// lands on this one.
  std::vector<Homography> camera;
  /// The size the analysis was made at.
  int width = 0;
  int height = 0;
};

/// The mesh is this many cells across and down.
inline constexpr int kWarpColumns = 16;
inline constexpr int kWarpRows = 9;

[[nodiscard]] Stabilization stabilize(const Analysis& analysis,
                                      const StabilizeSettings& settings);

}  // namespace cutline::track
