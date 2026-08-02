#pragma once

/// A clip's effect stack as a list of things to *do*, in order.
///
/// The older `resolve_effect_params` folds a whole stack into one flat struct
/// with a field per effect, and the shader applies that fixed set in a fixed
/// order. It served while there were nine effects. It cannot go further, for
/// three reasons that are really one reason:
///
///   - **The stack is not an order.** Two contrasts around a hue accumulate
///     into one multiplier applied once, so moving an effect up or down the
///     stack changes nothing. Premiere applies them in order, and so does
///     anybody's intuition about what a stack is.
///   - **The catalogue cannot grow.** Every effect needs its own permanent
///     field in a struct that arrives as root constants, and the Direct3D root
///     signature holds sixty-four DWORDs. Twenty-eight of them are already
///     effect parameters. A dozen more effects do not fit, and neither does a
///     mask.
///   - **Nothing can be per-effect.** A mask belongs to *one* effect in the
///     stack. There is nowhere to put it when the stack has been flattened.
///
/// A pass is one effect, run over the layer on its own. They share the same
/// small block of parameters because only one of them runs at a time, which is
/// what removes the ceiling: a new effect costs no permanent room at all.
///
/// The values are written by the named makers below rather than by index, so
/// the packing is stated once and the shader's reading of it can be checked
/// against one place.

#include "cutline/core/model.hpp"
#include "cutline/render/effects.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace cutline::render {

/// What one pass does. The shader has a branch per entry and nothing else.
enum class EffectPassKind {
  /// One `eq`/`hue`-style colour operation: brightness, contrast, saturation
  /// and hue rotation together, because a single effect may set several and
  /// they are one matrix when it does.
  Color,
  /// `negate`.
  Invert,
  /// `vignette`.
  Vignette,
  /// `crop`, which cuts to transparent and keeps the frame size.
  Crop,
  /// `chromakey`.
  ChromaKey,
  /// `hflip` / `vflip`.
  Flip,
  /// One axis of a separable Gaussian. A blur is two of these.
  Blur,
};

/// How many floats a pass carries.
///
/// Eight, which is two float4 rows and comfortably more than any effect here
/// needs. The number is a budget rather than a natural size: the point is that
/// it is *fixed and shared*, so adding an effect never widens anything.
inline constexpr std::size_t kPassValues = 8;

/// Where a pass applies, if it is masked.
///
/// A mask belongs to one pass, which is the thing the flat effect struct could
/// not express: once a stack has been folded into one set of numbers there is
/// nothing left for a mask to belong to. Every kind of pass takes one, and the
/// shader applies it the same way for all of them — work out the effect, work
/// out the coverage, and mix between the two. An effect never has to know it is
/// masked.
///
/// In fractions of the layer, like everything else about a pass, so a mask
/// keeps its place when the clip is scaled.
struct PassMask {
  /// 0 for no mask, 1 for an ellipse, 2 for a rectangle — `core::MaskShape` as
  /// the shader reads it.
  float shape = 0.0f;
  float x = 0.5f;
  float y = 0.5f;
  float width = 0.25f;
  float height = 0.25f;
  /// cos and sin of the mask's own rotation, so the shader has no trigonometry
  /// to do per pixel.
  float cos_rotation = 1.0f;
  float sin_rotation = 0.0f;
  float feather = 0.0f;
  float opacity = 1.0f;
  /// 1 when the effect applies outside the shape instead.
  float inverted = 0.0f;

  [[nodiscard]] bool active() const noexcept { return shape > 0.0f; }

  friend bool operator==(const PassMask&, const PassMask&) = default;
};

/// One effect, ready to run.
struct EffectPass {
  EffectPassKind kind = EffectPassKind::Color;
  /// What the pass does, in the units the shader works in. Which entry means
  /// what depends on `kind` — see the makers below, which are the only things
  /// that write them.
  std::array<float, kPassValues> values{};
  PassMask mask;

  friend bool operator==(const EffectPass&, const EffectPass&) = default;
};

/// A mask as the shader wants it. An inactive one leaves the pass unmasked.
[[nodiscard]] PassMask pass_mask(const core::Mask& mask) noexcept;

// ------------------------------------------------------------------ makers --
//
// One per kind, so the packing is written down once. Reading a pass back is the
// shader's job and the tests', and both go through the accessors underneath.

/// `brightness` is a luma offset, `contrast` and `saturation` are multipliers,
/// `hue_radians` rotates the chroma plane.
[[nodiscard]] EffectPass color_pass(float brightness, float contrast, float saturation,
                                    float hue_radians) noexcept;

[[nodiscard]] EffectPass invert_pass() noexcept;

/// `angle` in radians, as `vignette=a` takes it.
[[nodiscard]] EffectPass vignette_pass(float angle) noexcept;

/// Fractions cut from each edge.
[[nodiscard]] EffectPass crop_pass(float left, float top, float right, float bottom) noexcept;

/// `similarity` and `blend` are fractions; `color` is the key, coded.
[[nodiscard]] EffectPass chroma_key_pass(EffectColor color, float similarity,
                                         float blend) noexcept;

[[nodiscard]] EffectPass flip_pass(bool horizontal, bool vertical) noexcept;

/// A Gaussian of `sigma` pixels.
///
/// One pass, not two, even though a separable blur is two draws. How many draws
/// it takes and how wide a tap is are questions about the target's size, which
/// nothing here knows; the compositor expands this into its axes.
[[nodiscard]] EffectPass blur_pass(float sigma) noexcept;

// --------------------------------------------------------------- accessors --

[[nodiscard]] float pass_brightness(const EffectPass& pass) noexcept;
[[nodiscard]] float pass_contrast(const EffectPass& pass) noexcept;
[[nodiscard]] float pass_saturation(const EffectPass& pass) noexcept;
[[nodiscard]] float pass_hue_radians(const EffectPass& pass) noexcept;
[[nodiscard]] float pass_vignette(const EffectPass& pass) noexcept;
[[nodiscard]] std::array<float, 4> pass_crop(const EffectPass& pass) noexcept;
[[nodiscard]] EffectColor pass_key_color(const EffectPass& pass) noexcept;
[[nodiscard]] float pass_similarity(const EffectPass& pass) noexcept;
[[nodiscard]] float pass_blend(const EffectPass& pass) noexcept;
[[nodiscard]] bool pass_flips_x(const EffectPass& pass) noexcept;
[[nodiscard]] bool pass_flips_y(const EffectPass& pass) noexcept;
[[nodiscard]] float pass_sigma(const EffectPass& pass) noexcept;

// ------------------------------------------------------------------- plan --

/// A clip's enabled effects at `local_t`, in stack order, one pass each.
///
/// **In stack order**, which is the behaviour change this brings: two contrasts
/// either side of a hue rotation are now three operations in the order they are
/// written down, where before they folded into one multiplier and a rotation
/// applied in a fixed sequence. Premiere works the way this does, and so does
/// what anybody means by moving an effect up the stack.
///
/// A blur is one pass here and two draws in the compositor: how wide a tap is
/// depends on the target's size, which is not something this layer knows.
///
/// Empty when nothing in the stack does anything, which is what lets a layer
/// with no effects keep drawing in exactly one pass.
[[nodiscard]] std::vector<EffectPass> plan_effect_passes(const core::Clip& clip,
                                                         double local_t);

}  // namespace cutline::render
