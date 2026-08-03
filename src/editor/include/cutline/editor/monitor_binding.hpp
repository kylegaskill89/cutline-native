#pragma once

/// Between a clip's transform and the box the monitor draws handles on.
///
/// Two coordinate systems that look alike and are not. The model stores a
/// *scale*, relative to the media's aspect-fit size on the canvas — scale 1
/// means "as large as it goes without distortion", which is what keeps a
/// project's transforms independent of the resolution it is exported at. The
/// overlay works in canvas fractions, because a handle is a place on the frame
/// and knows nothing about what media is under it.
///
/// The conversion between them is one multiplication in each direction, and it
/// lives here because this is the only layer that knows both: `core` has no
/// idea what a widget is, and `ui` has never heard of a clip.

#include "cutline/core/model.hpp"
#include "cutline/ui/monitor.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// The box the handles should be drawn on for `clip_id` at timeline time `t`,
/// with any keyframed transform evaluated.
///
/// Nothing when there is no such clip, when it is an audio clip, or when it
/// contributes no picture — an overlay on something invisible would be handles
/// for moving nothing.
[[nodiscard]] std::optional<ui::MonitorBox> monitor_box(const core::Project& project,
                                                        std::string_view clip_id, double t);

/// Applies a box the handles produced back to the clip.
///
/// Goes through the same parameter setter the inspector's rows use, so a
/// transform that is animated takes keyframes at the playhead and one that is
/// not takes a stored value. A drag on the monitor and a drag on a slider are
/// the same edit, and having them disagree about that would be a good way to
/// lose somebody's animation.
///
/// Returns the project unchanged when it cannot apply, like every other edit.
[[nodiscard]] core::Project apply_monitor_box(core::Project project, std::string_view clip_id,
                                              const ui::MonitorBox& box, double t);

// ------------------------------------------------------------- framing --

/// What a clip should do with a frame it does not match the shape of.
enum class FrameFit {
  /// The whole picture on screen, with bars where the shapes differ. This is
  /// what a placed clip already does — scale 1 *is* the aspect fit — so it is
  /// also how a clip that has been scaled by hand gets back to sensible.
  Fit,
  /// The frame filled, with whatever does not fit falling outside it. The one
  /// people reach for on footage that is nearly the right shape and has bars
  /// nobody wants.
  Fill,
};

/// Scales every named clip to fit or fill the sequence.
///
/// Worth stating plainly, because a note in the gap map had it wrong for weeks:
/// **"scale to frame size" is already what placing a clip does here.** The model
/// stores scale relative to the aspect-fit size, so a 4K clip in a 1080p
/// sequence arrives fitted rather than cropped, and Premiere's command exists
/// because Premiere's default is native pixels. What is genuinely missing is
/// the other half — filling a frame the footage is the wrong shape for — and
/// both are offered together, since neither is any use without the other to get
/// back to.
///
/// Through the same setter the inspector's rows use, so an animated scale takes
/// keyframes at `t` rather than quietly losing its animation.
///
/// Clips with nothing to measure — audio, and anything with no stored size —
/// are passed over rather than scaled by a guess.
[[nodiscard]] core::Project scale_to_frame(core::Project project,
                                           std::span<const std::string> clip_ids, FrameFit fit,
                                           double t);

// ------------------------------------------------------------------ masks --

/// One masked effect on a clip, as the monitor should draw it.
struct MaskOverlayRef {
  /// Which effect in the clip's stack it belongs to.
  std::size_t effect = 0;
  ui::MaskOverlay overlay;

  friend bool operator==(const MaskOverlayRef&, const MaskOverlayRef&) = default;
};

/// Every mask on `clip_id` at time `t`, in canvas fractions.
///
/// A mask is stored in fractions of the **layer**, which is what keeps it in
/// place when the clip is scaled. The monitor works in fractions of the
/// **canvas**, because a shape on the frame is a place on the frame. Turning
/// one into the other needs the clip's transform, and this is the only layer
/// that knows both — the same reason `monitor_box` lives here.
///
/// Empty when there is no such clip, when it draws no picture, or when nothing
/// on it is masked.
[[nodiscard]] std::vector<MaskOverlayRef> mask_overlays(const core::Project& project,
                                                        std::string_view clip_id, double t);

/// Puts a dragged overlay back on the effect it came from.
///
/// The inverse of the above, and it takes the same route the numbers in the
/// panel do, so a drag on the picture and a typed percentage are one edit.
///
/// Returns the project unchanged when the clip or the effect is not there, or
/// when the layer has no size to measure the mask against.
[[nodiscard]] core::Project apply_mask_overlay(core::Project project,
                                               std::string_view clip_id, std::size_t effect,
                                               const ui::MaskOverlay& overlay, double t);

}  // namespace cutline::editor
