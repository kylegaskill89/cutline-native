#pragma once

/// Premiere's Auto Reframe effect: the clip made to fill a frame of another
/// shape, with the window it shows following what matters in the shot.
///
/// Not a pass. A pass works inside the layer's own frame, and filling a
/// vertical sequence with a wide shot is a matter of where the layer sits on
/// the sequence's frame and how large it is — Motion's business — so the
/// effect moves and scales the layer's box, after Motion, as the plan places
/// it. Reframe Offset, Reframe Scale and Reframe Rotation then adjust what was
/// worked out, as Premiere's do.
///
/// The looking is done once, in the background, and kept in a file of its
/// own as Warp Stabilizer's is (see `warp_stabilizer.hpp`): named for the
/// clip's source, which part of it, and the shape of the window, so nothing
/// is written into the project and undo has nothing to undo. Motion Tracking
/// only changes how the path is smoothed, so it does not need another look.

#include "cutline/core/layout.hpp"
#include "cutline/core/model.hpp"
#include "cutline/track/reframe.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace cutline::render {

/// The clip's first Auto Reframe that is switched on, or null.
[[nodiscard]] const core::ClipEffect* auto_reframe_of(const core::Clip& clip) noexcept;

/// How much of the clip's frame the sequence's frame shows once the clip is
/// scaled to fill it, as shares of its width and height.
struct ReframeWindow {
  double width = 1.0;
  double height = 1.0;
};
[[nodiscard]] ReframeWindow reframe_window(const core::Project& project, const core::Clip& clip);

/// The name of the analysis the clip's Auto Reframe needs; empty when there
/// is nothing to analyse.
[[nodiscard]] std::string reframe_key(const core::Project& project, const core::Clip& clip);
[[nodiscard]] std::filesystem::path reframe_analysis_path(const std::string& key);
[[nodiscard]] bool has_reframe_analysis(const std::string& key);

[[nodiscard]] track::ReframeMotion reframe_motion(const core::ClipEffect& effect) noexcept;

/// Where the window is centred `local_t` seconds into the clip, as fractions
/// of its frame; nothing until it has been analysed.
[[nodiscard]] std::optional<track::Point> reframe_centre(const std::string& key,
                                                         track::ReframeMotion motion,
                                                         double local_t);

/// The layer's box once the clip's Auto Reframe has had its way, at timeline
/// time `t`: scaled to fill the sequence's frame, moved so the window sits
/// over the subject but never past the frame's edge, then offset, scaled and
/// turned as its controls say. Unchanged for a clip without one. Before the
/// clip has been analysed the window is the middle of the frame.
void apply_auto_reframe(const core::Project& project, const core::Clip& clip,
                        const core::Media* media, double t, core::LayerBox& box);

}  // namespace cutline::render
