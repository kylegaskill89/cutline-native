#pragma once

/// Creating and editing the generated media that are not titles: colour mattes
/// and adjustment layers.
///
/// Same shape as `titles.hpp`, and for the same reasons — nothing on disk, no
/// probe, no pool to deduplicate against, and a length that has to be chosen
/// because there is no source to take one from. Two mattes of the same colour
/// are two mattes.
///
/// Kept apart from titles because text carries a whole specification of its own
/// and these carry almost nothing. What they share is `place_media`, and that
/// already lives in the core.
///
/// **An adjustment layer draws nothing.** It is a clip whose effect stack
/// applies to everything composited beneath it within its span, which is why it
/// has a colour field it never uses and why the compositor treats it as a
/// filter rather than a source.

#include "cutline/core/model.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace cutline::editor {

/// How long a new generated clip lasts, in seconds. The same as a title's:
/// long enough to see and short enough to trim rather than having to.
inline constexpr double kDefaultGeneratorLength = 5.0;

/// A colour matte's fill, as the panel reads and writes it.
///
/// One struct for the whole thing because the two halves are not independent —
/// a gradient with only one colour is a solid, and a solid with an angle is a
/// solid. Callers read it, change what they mean, and set it back, like every
/// other value in this layer.
struct MatteFill {
  std::string color = core::kDefaultMatteColor;
  /// Unset for a flat fill. Set makes it a linear ramp from `color` to this.
  std::optional<core::MatteGradient> gradient;

  friend bool operator==(const MatteFill&, const MatteFill&) = default;
};

/// Adds a colour matte to the pool. The id is reported through `id` when one is
/// wanted, since whoever just made one usually wants to place or select it.
[[nodiscard]] core::Project add_color_matte(core::Project project, MatteFill fill = {},
                                            std::string* id = nullptr);

/// Adds one and puts it on the timeline at `at`.
[[nodiscard]] core::Project add_color_matte_at(core::Project project, MatteFill fill, double at,
                                               std::string_view video_track_id = {},
                                               std::string* clip_id = nullptr);

/// Adds an adjustment layer to the pool.
[[nodiscard]] core::Project add_adjustment_layer(core::Project project,
                                                 std::string* id = nullptr);

/// Adds one and puts it on the timeline at `at`.
///
/// On the topmost video track unless one is named — which is where an
/// adjustment layer belongs, since it acts on everything below it.
[[nodiscard]] core::Project add_adjustment_layer_at(core::Project project, double at,
                                                    std::string_view video_track_id = {},
                                                    std::string* clip_id = nullptr);

/// The fill of a colour matte, or nothing when that media is not one.
[[nodiscard]] std::optional<MatteFill> matte_fill(const core::Project& project,
                                                  std::string_view media_id);

/// The same for whatever a clip shows.
[[nodiscard]] std::optional<MatteFill> clip_matte_fill(const core::Project& project,
                                                       std::string_view clip_id);

/// Replaces a matte's fill.
///
/// Returns the project unchanged when the media is not a matte or the fill is
/// the one it already has, so the session can skip the undo entry.
[[nodiscard]] core::Project set_matte_fill(core::Project project, std::string_view media_id,
                                           MatteFill fill);

/// Whether a clip is an adjustment layer — which is worth saying out loud in
/// the panel, since a clip that draws nothing looks like a clip that is broken.
[[nodiscard]] bool clip_is_adjustment(const core::Project& project,
                                      std::string_view clip_id) noexcept;

}  // namespace cutline::editor
