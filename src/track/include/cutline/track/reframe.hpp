#pragma once

/// Premiere's Auto Reframe, the looking half: where in each frame of a shot
/// the window of a new shape should sit, and how it should move from frame
/// to frame.
///
/// Premiere decides what matters in a shot with a trained model. This decides
/// it from what draws the eye without one: what moves against the camera's
/// own movement, skin, colour that stands out from the rest of the frame, and
/// detail, with a little pull towards the middle where a camera operator
/// usually put the subject already. The window is then put where it holds the
/// most of that, which for a vertical crop of a wide shot is the question that
/// matters — left or right — and the answer is smoothed through the shot as
/// Motion Tracking asks, never across a cut.

#include "cutline/track/tracker.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace cutline::track {

/// Premiere's Motion Tracking presets, in its order.
enum class ReframeMotion {
  /// Footage with little or no camera movement, an interview: the window
  /// holds still through a shot whenever the subject stays inside it.
  Slower,
  Default,
  /// Fast action: the window keeps up with the subject.
  Faster,
};

/// What looking at a clip found.
struct ReframeAnalysis {
  /// How many frames a second were looked at.
  double fps = 10.0;
  /// The window the new shape shows of the source, as shares of its width
  /// and height: a vertical crop of a wide shot is narrow and full height.
  double window_width = 1.0;
  double window_height = 1.0;
  /// Per frame, where the window is best centred, as fractions of the frame.
  std::vector<Point> subjects;
  /// The frames that begin a new shot, by index, in order.
  std::vector<int> cuts;
};

/// Looks at frames one after another and says where the window is best
/// centred in each.
class SubjectFinder {
 public:
  SubjectFinder(double window_width, double window_height);

  /// The next frame, as 8-bit RGBA. `new_shot` says it begins a shot, so
  /// nothing is taken to have moved since the frame before.
  [[nodiscard]] Point add(std::span<const std::uint8_t> rgba, int width, int height,
                          bool new_shot = false);

 private:
  double window_width_ = 1.0;
  double window_height_ = 1.0;
  int columns_ = 0;
  int rows_ = 0;
  std::vector<float> last_luma_;
  /// Per cell, how much it has changed from frame to frame lately.
  std::vector<float> change_;
  int changes_seen_ = 0;
};

/// The window's centre through the clip, one per analysed frame: each shot's
/// subjects smoothed as `motion` asks, kept where the window stays inside the
/// frame.
[[nodiscard]] std::vector<Point> reframe_path(const ReframeAnalysis& analysis,
                                              ReframeMotion motion);

[[nodiscard]] bool save_reframe(const ReframeAnalysis& analysis,
                                const std::filesystem::path& path);
[[nodiscard]] std::optional<ReframeAnalysis> load_reframe(const std::filesystem::path& path);

}  // namespace cutline::track
