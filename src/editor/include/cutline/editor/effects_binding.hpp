#pragma once

/// What the Effect Controls panel shows for a clip's effect stack.
///
/// The same shape as `inspector.hpp`, and for the same reason: the core has
/// stack operations and the interface has sliders, and neither should learn
/// about the other. This describes a clip's effects as data, so the panel is a
/// loop over a list rather than a form that has to be remembered whenever an
/// effect is added.
///
/// Only the *description* lives here. Removing, reordering, toggling and
/// setting a parameter are already core operations taking a clip id and an
/// index, and the panel calls those directly. Adding is the exception: "add a
/// blur" means "add a blur with the values a new blur should have", and that
/// needs the catalogue.
///
/// Values are in the units the effect stores, which the registry defines as the
/// units a person reads — percent, degrees, pixels. Unlike the transform, there
/// is no conversion to do.

#include "cutline/core/model.hpp"
#include "cutline/render/effect_catalog.hpp"
#include "cutline/ui/controls.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// One numeric parameter of one effect on one clip.
struct EffectParamRow {
  /// As stored, and as the core operations take it.
  std::string key;
  std::string name;
  ui::ValueRange range;
  double value = 0.0;
  /// What a reset returns to; the value a newly added effect was given.
  double fallback = 0.0;
  std::string suffix;
  /// Worth a checkbox rather than a slider.
  bool toggle = false;
  /// Animated, so `value` is what the keyframes evaluate to rather than the
  /// stored one — which is ignored entirely while a parameter is animated.
  bool animated = false;
  /// Animated, and one of the keyframes is at the time asked about. What the
  /// keyframe marker draws as filled rather than hollow.
  bool keyed_here = false;
  /// The curve the whole parameter animates along. One per parameter rather
  /// than one per keyframe, as the reference had it. Meaningless unless
  /// `animated`.
  core::Interp interp = core::Interp::Linear;

  friend bool operator==(const EffectParamRow&, const EffectParamRow&) = default;
};

struct EffectColorRow {
  std::string key;
  std::string name;
  /// Hex, as stored. The catalogue's default when the effect has none set.
  std::string value;

  friend bool operator==(const EffectColorRow&, const EffectColorRow&) = default;
};

/// The mask on one effect, as a panel reads it.
///
/// Values in display units, like everything else here: the model keeps
/// fractions of the layer and this shows percentages, which is what the numbers
/// on a mask read as everywhere else.
struct EffectMaskRow {
  core::MaskShape shape = core::MaskShape::None;
  double x = 50.0;
  double y = 50.0;
  double width = 25.0;
  double height = 25.0;
  double rotation = 0.0;
  double feather = 0.0;
  double opacity = 100.0;
  bool inverted = false;

  friend bool operator==(const EffectMaskRow&, const EffectMaskRow&) = default;
};

/// The shapes a mask can be, in the order a control should offer them, and what
/// each is called on screen.
[[nodiscard]] std::span<const core::MaskShape> mask_shapes() noexcept;
[[nodiscard]] std::string_view mask_shape_name(core::MaskShape shape) noexcept;

/// One effect in the stack, in stack order.
struct EffectRow {
  /// Position in the clip's stack, which is what every core operation takes.
  /// Not stable across a removal or a move, so it is read again after either.
  std::size_t index = 0;
  std::string type;
  /// The catalogue's name, or the raw type for an effect this build does not
  /// know — a project written by a newer version should still open and still
  /// show that something is there.
  std::string name;
  bool enabled = true;
  /// True when the type is not in the catalogue, so the panel can say so
  /// rather than drawing an effect with no parameters and no explanation.
  bool unknown = false;
  std::vector<EffectParamRow> params;
  std::vector<EffectColorRow> colors;
  /// Where the effect applies. A shape of `None` is everywhere, and is what
  /// almost every effect says.
  EffectMaskRow mask;

  friend bool operator==(const EffectRow&, const EffectRow&) = default;
};

/// A clip's visual effect stack, as it stands at clip-local time `local_t`.
///
/// The time is what an animated parameter is read at, so the panel shows what
/// the picture is actually doing at the playhead rather than a stored number
/// the keyframes are overriding.
///
/// Empty when the clip is not there, which is also what an empty selection
/// produces.
[[nodiscard]] std::vector<EffectRow> clip_effects(const core::Project& project,
                                                  std::string_view clip_id,
                                                  double local_t = 0.0);

/// An effect that can be added, for the menu that offers them.
struct EffectChoice {
  std::string type;
  std::string name;
  std::string category;

  friend bool operator==(const EffectChoice&, const EffectChoice&) = default;
};

[[nodiscard]] std::vector<EffectChoice> addable_effects();

/// Adds an effect with the values a new one should have.
///
/// Returns the project unchanged when the type is not in the catalogue or the
/// clip is not there, like everything else that edits, so the session can skip
/// the undo entry.
[[nodiscard]] core::Project add_effect(core::Project project, std::string_view clip_id,
                                       std::string_view type);

// ------------------------------------------------------------------- mask --

/// Sets where one effect applies, taking display units.
///
/// Choosing a shape for an effect that had none gives it a mask covering the
/// middle of the layer rather than one of no size: a mask nobody can see is
/// indistinguishable from the control not working, which is the same reason a
/// freshly added vignette is visible.
[[nodiscard]] core::Project set_effect_mask(core::Project project, std::string_view clip_id,
                                            std::size_t index, const EffectMaskRow& mask);

/// Turns the mask off, leaving the effect applying everywhere.
[[nodiscard]] core::Project clear_effect_mask(core::Project project, std::string_view clip_id,
                                              std::size_t index);

// -------------------------------------------------------------- keyframes --

/// Sets a parameter: a keyframe at `local_t` when it is animated, the stored
/// value when it is not.
///
/// One entry point rather than two, because which of them a drag means is not
/// the slider's business — it is a property of the parameter being dragged, and
/// asking every caller to check would eventually get it wrong somewhere.
[[nodiscard]] core::Project set_effect_parameter(core::Project project,
                                                 std::string_view clip_id, std::size_t index,
                                                 std::string_view key, double value,
                                                 double local_t = 0.0);

/// Turns animation on or off — Premiere's stopwatch.
///
/// On, the value the parameter has now becomes its first keyframe at `local_t`,
/// so switching it on changes nothing about the picture. Off, every keyframe is
/// dropped and the value at `local_t` is kept as the static one, so switching
/// it off does not either. Anything else loses work silently.
[[nodiscard]] core::Project set_effect_parameter_animated(core::Project project,
                                                          std::string_view clip_id,
                                                          std::size_t index,
                                                          std::string_view key, bool animated,
                                                          double local_t);

/// Adds a keyframe at `local_t` holding the current value, or removes the one
/// already there. Does nothing when the parameter is not animated: the first
/// keyframe is the stopwatch's job.
[[nodiscard]] core::Project toggle_effect_keyframe(core::Project project,
                                                   std::string_view clip_id, std::size_t index,
                                                   std::string_view key, double local_t);

/// Sets the curve the whole parameter animates along. Does nothing when it is
/// not animated: there are no keyframes to set it on.
[[nodiscard]] core::Project set_effect_parameter_interp(core::Project project,
                                                        std::string_view clip_id,
                                                        std::size_t index, std::string_view key,
                                                        core::Interp mode);

// ------------------------------------------------------------ audio stack --
//
// The same rows, built from the audio registry instead of the video catalogue,
// so a panel showing an audio clip's effects is the same loop over the same
// struct. Two differences, both in what is absent rather than what is here:
//
//   - **No colours.** Nothing in the audio registry takes one.
//   - **Keyframes, but retuned at a control rate.** `AudioClipEffect` carries the
//     same per-parameter keyframe map `ClipEffect` does. The one difference is
//     under the panel rather than in it: a filter carries state that assumes its
//     own coefficients, so the chain is re-read on a fixed grid of frames rather
//     than per sample. Nothing here has to know that.
//
// The registry already exists, in `cutline::audio`: each effect declares its
// parameters once with ranges, steps, defaults and units, exactly as the video
// catalogue does. There was nothing to write here but the join.

[[nodiscard]] std::vector<EffectRow> clip_audio_effects(const core::Project& project,
                                                        std::string_view clip_id,
                                                        double local_t = 0.0);

[[nodiscard]] std::vector<EffectChoice> addable_audio_effects();

/// Adds an audio effect with the values a new one should have — every parameter
/// at its registry default, so a freshly added filter is the neutral one until
/// somebody moves a slider.
[[nodiscard]] core::Project add_audio_effect(core::Project project, std::string_view clip_id,
                                             std::string_view type);

/// The same four operations the visual stack has, on the audio one.
///
/// Sound is retuned on a fixed grid rather than per sample — see
/// `audio::EffectChain::retune`. That is invisible from here: as far as the
/// panel is concerned an audio parameter animates exactly as a visual one does.
[[nodiscard]] core::Project set_audio_effect_parameter(core::Project project,
                                                       std::string_view clip_id,
                                                       std::size_t index, std::string_view key,
                                                       double value, double local_t = 0.0);

[[nodiscard]] core::Project set_audio_effect_parameter_animated(core::Project project,
                                                                std::string_view clip_id,
                                                                std::size_t index,
                                                                std::string_view key,
                                                                bool animated, double local_t);

[[nodiscard]] core::Project toggle_audio_effect_keyframe(core::Project project,
                                                         std::string_view clip_id,
                                                         std::size_t index, std::string_view key,
                                                         double local_t);

[[nodiscard]] core::Project set_audio_effect_parameter_interp(core::Project project,
                                                              std::string_view clip_id,
                                                              std::size_t index,
                                                              std::string_view key,
                                                              core::Interp mode);

/// Puts every parameter of one effect back to its catalogue default.
///
/// Colours too, and any keyframes with them: a reset that left an animation
/// running would put the sliders back and change nothing about the picture.
/// Returns the project unchanged when the effect is not there or the registry
/// does not know its type.
[[nodiscard]] core::Project reset_effect(core::Project project, std::string_view clip_id,
                                         std::size_t index);

/// The same for an entry in the audio stack, which has no colours. Keyframes
/// are cleared with it, for the same reason they are on the visual side.
[[nodiscard]] core::Project reset_audio_effect(core::Project project, std::string_view clip_id,
                                               std::size_t index);

// ---------------------------------------------------------------- library --

/// One entry in the effects library: everything that can be applied to a clip,
/// in one list, whatever kind of thing it is.
///
/// Premiere's Effects panel holds video effects, audio effects and transitions
/// together, and the person reaching for one does not think of them as three
/// catalogues. Which of the three an entry is lives in its `id` rather than in
/// a field, so the browser showing it stays a list of names and folders and
/// never learns the difference.
struct LibraryEntry {
  /// `video:blur`, `audio:eq`, `transition:dissolve`. Round-tripped through
  /// `apply_library_entry`, which is the only thing that takes it apart.
  std::string id;
  std::string name;
  /// The folder it sits under, as a path: `Video Effects/Colour`. The browser
  /// makes the tree out of these, so nesting one deeper needs nothing but a
  /// longer string.
  std::string folder;

  friend bool operator==(const LibraryEntry&, const LibraryEntry&) = default;
};

/// The whole library, in the order a panel should show it: video effects by
/// category, then audio effects, then transitions, then whatever has been saved
/// as a preset.
///
/// Presets are passed in rather than read from disk here, because this layer
/// has no business deciding when a settings file is loaded — and because a test
/// wants to hand it a set without writing one.
[[nodiscard]] std::vector<LibraryEntry> effect_library(
    std::span<const std::string> preset_names = {});

/// The prefix a preset's id carries, so a caller can tell one from an effect
/// without taking the id apart. `apply_library_entry` refuses these: applying a
/// preset needs the saved set, which this layer does not hold.
inline constexpr std::string_view kPresetPrefix = "preset:";

/// The name inside a preset id, or empty when it is not one.
[[nodiscard]] std::string_view preset_name_of(std::string_view id) noexcept;

/// Applies whatever `id` names to a clip.
///
/// Refuses what does not fit rather than doing something else: a video effect
/// on an audio clip, an audio effect on a picture, a transition on a clip with
/// nothing abutting it. Returns the project unchanged in every one of those
/// cases, like everything else that edits, so the session can skip the entry.
[[nodiscard]] core::Project apply_library_entry(core::Project project,
                                                std::string_view clip_id,
                                                std::string_view id);

/// Whether `id` could be applied to `clip_id` as things stand — what greys the
/// panel's Apply button, and what a double-click checks before doing nothing.
[[nodiscard]] bool library_entry_fits(const core::Project& project, std::string_view clip_id,
                                      std::string_view id);

// ------------------------------------------------------------- copy/paste --

/// One clip's effects, taken off it and held.
///
/// Values, not references: a clipboard that pointed into the project would go
/// stale the moment the clip it came from was trimmed, and undo would make it
/// dangle.
///
/// It remembers what it came *from*, and that is the part that matters. An A/V
/// pair is two linked clips in this model, so selecting a shot selects both,
/// and a paste that treated the two the same would wipe an audio clip's filters
/// every time somebody copied a video look. A clipboard taken off a video clip
/// lands on video clips and passes over the rest.
struct EffectClipboard {
  core::TrackKind kind = core::TrackKind::Video;
  std::vector<core::ClipEffect> video;
  std::vector<core::AudioClipEffect> audio;

  /// True once something has been copied, whether or not it had any effects on
  /// it. "Nothing copied yet" and "copied a clip with a clean stack" are
  /// different: the second is how a stack gets cleared, and the first is a
  /// Paste button that should be greyed out.
  bool filled = false;

  [[nodiscard]] bool empty() const noexcept { return !filled; }
  /// How many effects are held, for a caller that wants to say so.
  [[nodiscard]] std::size_t size() const noexcept { return video.size() + audio.size(); }

  friend bool operator==(const EffectClipboard&, const EffectClipboard&) = default;
};

/// Takes a copy of *one* effect off a stack, keyframes and all.
///
/// The same clipboard the whole-stack copy fills, holding one entry. Pasting is
/// therefore the same operation and has the same meaning: it *replaces* what is
/// on the clip it lands on. Copying one effect to add it to another stack
/// without disturbing what is there is a different operation and does not exist
/// yet.
///
/// Empty when the index names nothing, which leaves whatever was copied before
/// alone rather than clearing it.
[[nodiscard]] EffectClipboard copy_one_effect(const core::Project& project,
                                              std::string_view clip_id, std::size_t index);

/// Takes a copy of what is on `clip_id`, keyframes and all.
///
/// A clip with nothing on it gives a *filled* clipboard holding nothing, which
/// is deliberate: copying a clean clip and pasting is how a stack is cleared,
/// and it is the first thing anybody tries.
[[nodiscard]] EffectClipboard copy_effects(const core::Project& project,
                                           std::string_view clip_id);

/// Puts the clipboard on every clip of its own kind among those named,
/// **replacing** what each already had.
///
/// Replacing rather than appending because of what the gesture means: "make
/// this clip look like that one". Appending makes pasting twice apply
/// everything twice, which is a stack nobody asked for and a fiddle to undo one
/// effect at a time.
///
/// Returns the project unchanged when nothing would alter.
[[nodiscard]] core::Project paste_effects(core::Project project,
                                          std::span<const std::string> clip_ids,
                                          const EffectClipboard& clipboard);

}  // namespace cutline::editor
