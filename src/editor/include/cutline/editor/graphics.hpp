#pragma once

/// Making and editing graphics: Premiere's Essential Graphics.
///
/// A graphic is generated media: nothing on disk, its layers in the project.
/// That makes it a source an editor creates rather than imports, and the reason
/// this is not part of `import.hpp` — there is no file, no probe, and no pool to
/// deduplicate against. Two titles with the same words are two titles.
///
/// The layer operations work on a `Graphic` value and hand back a changed one;
/// putting it back into the project is `set_graphic`, one undo step however
/// many layers changed. A caller reads what it has, changes what it means to
/// change, and sets it back.

#include "cutline/core/graphic.hpp"
#include "cutline/core/model.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// How long a new graphic lasts, in seconds.
///
/// A graphic has no inherent duration, so something has to choose. Five seconds
/// is long enough to read and short enough to trim rather than having to.
inline constexpr double kDefaultTitleLength = 5.0;

// ------------------------------------------------------------ graphics --

/// What a new title is: one text layer, white and centred, as Premiere's Type
/// tool makes.
[[nodiscard]] core::Graphic default_title();

/// Adds a graphic to the project's media pool. Its id comes back through `id`
/// when one is wanted, since a caller that has just made a graphic almost
/// always wants to place or select it.
[[nodiscard]] core::Project add_graphic(core::Project project, core::Graphic graphic,
                                        std::string* id = nullptr,
                                        double length = kDefaultTitleLength);

/// Adds a graphic and puts it on the timeline at `at`: on the track named, or
/// as Premiere places one, on the first track above everything showing there --
/// over the picture rather than over a clip of it -- with a track added at the
/// top when there is no room.
[[nodiscard]] core::Project add_graphic_at(core::Project project, core::Graphic graphic, double at,
                                           std::string_view video_track_id = {},
                                           std::string* clip_id = nullptr,
                                           double length = kDefaultTitleLength);

/// The graphic a media is, or null.
[[nodiscard]] const core::Graphic* graphic_of(const core::Project& project,
                                              std::string_view media_id) noexcept;
/// The graphic a clip shows, or null when the clip is not a graphic's.
[[nodiscard]] const core::Graphic* clip_graphic(const core::Project& project,
                                                std::string_view clip_id) noexcept;

/// Replaces a graphic. Unchanged when the media is not a graphic or already is
/// that one, so the session can skip the undo entry. The project panel's name
/// follows the first text layer's words unless it has been renamed.
[[nodiscard]] core::Project set_graphic(core::Project project, std::string_view media_id,
                                        core::Graphic graphic);

// -------------------------------------------------------------- layers --

/// New layers as Premiere's tools make them. Placed in the middle of the frame;
/// `add_layer` gives them their id and their name.
[[nodiscard]] core::GraphicLayer text_layer(std::string text = "New Text Layer");
[[nodiscard]] core::GraphicLayer shape_layer(core::GraphicKind kind);
[[nodiscard]] core::GraphicLayer image_layer(std::string path);

/// Puts a layer on top of the others, with an id no other layer of the graphic
/// has, and named as Premiere names one -- the text itself, or "Shape 01" --
/// unless it has a name already.
[[nodiscard]] core::Graphic add_layer(core::Graphic graphic, core::GraphicLayer layer,
                                      std::string* id = nullptr);

[[nodiscard]] core::Graphic remove_layer(core::Graphic graphic, std::string_view id);

/// A copy of a layer just above it, as Premiere's Duplicate.
[[nodiscard]] core::Graphic duplicate_layer(core::Graphic graphic, std::string_view id,
                                            std::string* copy_id = nullptr);

/// Moves a layer to a place in the list, top first. Pins that named it still
/// name it.
[[nodiscard]] core::Graphic move_layer(core::Graphic graphic, std::string_view id,
                                       std::size_t to);

/// Replaces the layer with the same id.
[[nodiscard]] core::Graphic set_layer(core::Graphic graphic, core::GraphicLayer layer);

/// What a layer is called in the list: its own name, or for a text layer that
/// has none, its first line.
[[nodiscard]] std::string layer_label(const core::GraphicLayer& layer);

// ---------------------------------------------------------- keyframes --

/// Sets or adds a keyframe on a layer's property at `t` seconds into the clip.
[[nodiscard]] core::Graphic set_layer_keyframe(core::Graphic graphic, std::string_view id,
                                               std::string_view property, double t, double value);
[[nodiscard]] core::Graphic remove_layer_keyframe(core::Graphic graphic, std::string_view id,
                                                  std::string_view property, double t);
/// Takes a property's keyframes away, leaving it at its value at `t`.
[[nodiscard]] core::Graphic clear_layer_keyframes(core::Graphic graphic, std::string_view id,
                                                  std::string_view property, double t);
/// Sets a property, through its keyframes when it has any: Premiere's rule
/// that changing an animated value at the playhead keys it there.
[[nodiscard]] core::Graphic set_layer_value(core::Graphic graphic, std::string_view id,
                                            std::string_view property, double t, double value);

// ----------------------------------------------------- responsive design --

/// Which edges of a layer to pin.
struct PinEdges {
  bool left = false;
  bool top = false;
  bool right = false;
  bool bottom = false;
};

/// Pins a layer's edges to another layer's, or to the frame's when `to` is
/// empty, keeping the distances they have now: Premiere's Responsive Design,
/// Position. Refused when it would make a loop -- a layer pinned, however
/// indirectly, to itself.
[[nodiscard]] std::expected<core::Graphic, std::string> pin_layer(
    core::Graphic graphic, std::string_view id, std::string_view to, PinEdges edges,
    double canvas_w, double canvas_h, const core::LayerMeasurer& measure);

// ------------------------------------------------------------ alignment --

enum class Align { Left, CentreX, Right, Top, CentreY, Bottom };

/// Lines layers up as Premiere's Align and Transform does: one layer against the
/// frame, several against the box round all of them.
[[nodiscard]] core::Graphic align_layers(core::Graphic graphic,
                                         const std::vector<std::string>& ids, Align align,
                                         double canvas_w, double canvas_h,
                                         const core::LayerMeasurer& measure);

// -------------------------------------------------------- master styles --

/// Applies a master style to a layer: it takes the style's look and follows it
/// from then on.
[[nodiscard]] core::Graphic apply_master_style(core::Graphic graphic, std::string_view id,
                                               const core::GraphicMasterStyle& style);
/// Makes a master style from a layer's look, as Premiere's Create Master Text
/// Style, and links the layer to it.
[[nodiscard]] core::Project create_master_style(core::Project project, std::string_view media_id,
                                                std::string_view layer_id, std::string name);
/// Changes a master style, and every layer following it in every graphic.
[[nodiscard]] core::Project set_master_style(core::Project project, core::GraphicMasterStyle style);
/// Whether a layer's look still matches the master style it follows.
[[nodiscard]] bool follows_master(const core::Project& project,
                                  const core::GraphicLayer& layer) noexcept;

}  // namespace cutline::editor
