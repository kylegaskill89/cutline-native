#pragma once

/// Drawing a stack of layers into linear light.
///
/// The compositor is the one place pixels are combined, and both preview and
/// export go through it. That is the point: the old app composited with a
/// canvas for preview and an FFmpeg filtergraph for export, and keeping the two
/// agreeing was constant work. Here there is nothing to keep in agreement.
///
/// Everything blends in *linear* light at 16-bit float. This is a deliberate
/// divergence from the reference, which blended in gamma-encoded sRGB because
/// that is what a 2D canvas does. Linear is the physically correct answer — a
/// 50% cross-dissolve now passes through the true midpoint rather than a
/// too-dark one — so old and new projects will not match pixel for pixel across
/// a dissolve or a partial opacity. That was accepted when the float pipeline
/// was chosen.
///
/// Geometry arrives already resolved: `cutline::core` decides where a clip
/// lands and how opaque it is. The types here deliberately do not reuse the
/// core model's, for the same reason `FrameView` carries no libav types — the
/// renderer should not know what a keyframe or a transition is.

#include "cutline/gpu/device.hpp"
#include "cutline/gpu/frame_view.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace cutline::gpu {

/// Straight (non-premultiplied) alpha, in linear light.
struct Color {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;

  /// Converts an sRGB-encoded colour — what a hex string in a project file
  /// means — into the linear values the compositor works in.
  [[nodiscard]] static Color from_srgb(float r, float g, float b, float a = 1.0f) noexcept;
};

/// How a layer combines with what is already beneath it. Matches the model's
/// blend modes one for one.
enum class BlendMode {
  Normal,
  Add,
  Screen,
  Multiply,
  Overlay,
  Darken,
  Lighten,
  Difference,
};

/// A draw rectangle in canvas pixels, centred and rotated clockwise about its
/// centre. Mirrors `core::LayerBox`, which is what produces it.
struct Quad {
  float center_x = 0.0f;
  float center_y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
  float rotation_deg = 0.0f;
};

/// What one effect pass does. Matches `render::EffectPassKind` value for value,
/// and the shader branches on it directly.
enum class PassKind {
  Color,
  Invert,
  Vignette,
  Crop,
  ChromaKey,
  Flip,
  Blur,
  Levels,
  Balance,
  Tint,
  Sharpen,
  Posterize,
  Threshold,
  DirectionalBlur,
  RadialBlur,
  Distort,
  Noise,
};

/// How many floats a pass carries. Eight, shared by every kind, because only
/// one pass runs at a time — which is what lets the catalogue grow without the
/// root constants growing with it.
inline constexpr std::size_t kPassValues = 8;

/// Where one effect applies. A shape of zero is everywhere.
///
/// Mirrors `render::PassMask`, and carries the rotation already resolved to a
/// cosine and a sine because the shader would otherwise do the trigonometry per
/// pixel.
struct PassMask {
  float shape = 0.0f;
  float x = 0.5f;
  float y = 0.5f;
  float width = 0.25f;
  float height = 0.25f;
  float cos_rotation = 1.0f;
  float sin_rotation = 0.0f;
  float feather = 0.0f;
  float opacity = 1.0f;
  float inverted = 0.0f;

  /// A free-drawn path's corners, each an offset from the mask's centre in
  /// fractions of the layer. Empty for every other shape, which is every mask
  /// that fits in the root constants.
  std::vector<std::array<float, 2>> points;

  friend bool operator==(const PassMask&, const PassMask&) = default;
};

/// One effect, ready to run.
///
/// The values mean what the kind says, packed by `render::effect_passes`. The
/// compositor does not interpret them at all beyond the blur, whose two draws
/// it has to expand: everything else goes to the shader as it arrives.
///
/// Effects run on *coded* values — after the YUV matrix, before the transfer
/// function — because that is the space FFmpeg's filters are defined in, and
/// the spec names those fragments as each effect's authoritative behaviour.
/// Applying a 200% contrast to linear light instead would be a large visible
/// difference, not a subtle one. Compositing stays linear; only the effect
/// maths is not.
struct EffectPass {
  PassKind kind = PassKind::Color;
  std::array<float, kPassValues> values{};
  PassMask mask;

  friend bool operator==(const EffectPass&, const EffectPass&) = default;
};

/// One thing to draw. A null `frame` draws `color` as a solid, which is what a
/// colour matte is.
struct Layer {
  const FrameView* frame = nullptr;
  Color color;

  /// A linear gradient across the quad, running from `color` to
  /// `gradient_color` at `gradient_angle_deg` — 0 is left to right, 90 is top
  /// to bottom. Only meaningful for a solid layer.
  ///
  /// The interpolation happens on *coded* values, not linear ones, for the same
  /// reason the effects do: a gradient between two hex colours is specified the
  /// way a canvas draws it, and interpolating in linear light would put the
  /// midpoint somewhere the author did not choose.
  bool gradient = false;
  Color gradient_color;
  float gradient_angle_deg = 0.0f;

  Quad quad;
  float opacity = 1.0f;
  BlendMode blend = BlendMode::Normal;

  /// The effect stack, in the order it is applied.
  ///
  /// Empty is the cheap case and the common one: the layer is decoded,
  /// positioned and composited in a single draw. Anything here instead draws
  /// the layer once into a scratch target in its own space, runs each pass over
  /// it, and composites the result — which is what buys the ordering, and what
  /// leaves room for a mask to belong to one effect rather than to the stack.
  ///
  /// Borrowed: the span has to outlive the `compose` call and nothing more.
  std::span<const EffectPass> passes;

  /// An adjustment layer draws nothing of its own: it puts everything already
  /// composited beneath it through `passes`, within its own quad. `opacity`
  /// becomes the strength of that adjustment rather than a transparency, and
  /// `frame`, `color`, and `blend` are ignored.
  bool adjustment = false;
};

/// A composited frame brought back to the CPU: 8-bit RGBA, sRGB-encoded, which
/// is to say exactly what the preview window would have shown. This is what the
/// golden-image tests compare.
struct Image {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> pixels;  ///< row-major RGBA, `width * height * 4`

  [[nodiscard]] bool empty() const noexcept { return pixels.empty(); }
};

/// The same frame left where it already is: on the GPU, display-encoded, ready
/// to be sampled.
///
/// `read_back` exists because a test compares pixels and a still gets written
/// to a file. Neither is true of the picture on screen, and paying a
/// canvas-sized trip to system memory and back for every scrub was always the
/// wrong shape — it was just the only shape available while the interface drew
/// on the CPU. Now that it does not, the frame need never leave the card.
///
/// Untyped for the reason everything here is untyped: `d3d12.h` stays inside
/// this library.
struct SceneTexture {
  void* resource = nullptr;  ///< ID3D12Resource*
  int width = 0;
  int height = 0;
  unsigned format = 0;  ///< DXGI_FORMAT
  /// The state the texture is left in, which is also the state whoever samples
  /// it must declare. Disagreeing about this is not a visible bug so much as a
  /// debug-layer message, and on some drivers not even that until it is a
  /// wrong picture.
  unsigned state = 0;  ///< D3D12_RESOURCE_STATES

  /// Bumped every time the targets are rebuilt.
  ///
  /// Anyone caching something derived from this texture needs it. A rebuild
  /// frees the old target, and Direct3D reuses addresses freely — so the same
  /// pointer can come back, at the same size, referring to different memory.
  /// Comparing the handle would say nothing had changed.
  unsigned generation = 0;

  [[nodiscard]] bool empty() const noexcept {
    return resource == nullptr || width <= 0 || height <= 0;
  }
};

class Compositor {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<Compositor>, std::string> create(
      std::shared_ptr<Device> device, int canvas_width, int canvas_height);

  Compositor(const Compositor&) = delete;
  Compositor& operator=(const Compositor&) = delete;
  ~Compositor();

  /// Draws the layers bottom-first into the linear scene target, over black.
  /// An empty span composites a black frame, which is what an empty timeline
  /// should look like rather than stale pixels.
  [[nodiscard]] std::expected<void, std::string> compose(std::span<const Layer> layers);

  /// Encodes the scene for display and copies it back to system memory.
  /// Slow by nature — this stalls on the GPU — and meant for tests and stills,
  /// not for every frame of an export.
  [[nodiscard]] std::expected<Image, std::string> read_back();

  /// Encodes the scene for display and stops there, handing back the texture
  /// rather than its contents. Nothing is copied.
  ///
  /// The result borrows the compositor's own display target, so it is valid
  /// until the next `compose`/`display_texture` pair overwrites it and until
  /// this compositor is destroyed. That is exactly a preview's lifetime, and
  /// deliberately not something to hold on to.
  ///
  /// This still waits for the GPU, because everything here shares one command
  /// allocator and resetting it under a running command list is not allowed.
  /// What it does not do is copy a canvas to system memory and let the
  /// interface upload it again — which was the expensive half, not the wait.
  [[nodiscard]] std::expected<SceneTexture, std::string> display_texture();

  /// Changes the canvas size, discarding the current scene contents.
  [[nodiscard]] std::expected<void, std::string> resize(int width, int height);

  [[nodiscard]] int width() const noexcept;
  [[nodiscard]] int height() const noexcept;

  /// Internal Direct3D state, so the on-screen presenter can read the scene
  /// target. Defined in an internal header.
  struct Impl;
  [[nodiscard]] Impl& internals() noexcept { return *impl_; }

 private:
  explicit Compositor(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::gpu
