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

/// A layer's visual effects, already resolved to plain numbers.
///
/// These run on *coded* values -- after the YUV matrix, before the transfer
/// function -- because that is the space FFmpeg's filters are defined in, and
/// the spec names those fragments as each effect's authoritative behaviour.
/// Applying a 200% contrast to linear light instead would be a large visible
/// difference, not a subtle one. Compositing stays linear; only the effect
/// maths is not.
///
/// Defaults are neutral, so a default-constructed instance draws the layer
/// untouched.
struct LayerEffects {
  float brightness = 0.0f;   ///< luma offset, -1..1
  float contrast = 1.0f;     ///< multiplier about the midpoint
  float saturation = 1.0f;   ///< chroma multiplier
  float hue_degrees = 0.0f;  ///< chroma rotation
  bool invert = false;

  /// Vignette angle in radians. Zero leaves the edges alone.
  float vignette = 0.0f;

  /// Fractions cut from each edge. The layer keeps its size; what is cut goes
  /// transparent rather than the picture shrinking.
  float crop_left = 0.0f;
  float crop_top = 0.0f;
  float crop_right = 0.0f;
  float crop_bottom = 0.0f;

  bool chroma_key = false;
  float chroma_similarity = 0.3f;
  float chroma_blend = 0.1f;
  /// sRGB-coded, not linear. Defaults to #00d000, the registry green.
  Color chroma_color{0.0f, 208.0f / 255.0f, 0.0f, 1.0f};

  friend bool operator==(const LayerEffects&, const LayerEffects&) = default;
};

/// One thing to draw. A null `frame` draws `color` as a solid, which is what a
/// colour matte is.
struct Layer {
  const FrameView* frame = nullptr;
  Color color;
  Quad quad;
  float opacity = 1.0f;
  BlendMode blend = BlendMode::Normal;
  bool flip_x = false;
  bool flip_y = false;
  LayerEffects effects;
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
