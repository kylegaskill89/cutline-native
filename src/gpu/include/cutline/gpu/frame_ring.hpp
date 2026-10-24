#pragma once

/// Finished pictures handed from a render thread to the thread that shows them.
///
/// The compositor has one display target, and that is fine while one thread
/// renders and draws. It stops being fine the moment rendering moves off the
/// interface thread, for two reasons that are each enough on their own:
///
///   - The render thread wants to be *ahead* of what is on screen. Playback
///     renders the next few frames while the current one is showing, so there
///     have to be somewhere for them to wait.
///   - The interface samples whatever texture it was handed, whenever it next
///     paints. A render thread writing that same texture meanwhile is a picture
///     half of one frame and half of the next, and resizing it is freeing
///     memory that is about to be read.
///
/// So the render thread copies each finished picture into a slot of its own
/// (`store`), and the interface copies the one it wants to show into a texture
/// that only it ever touches (`show`). The second copy is the whole trick: it is
/// submitted on the same queue just before the interface paints, so the slot is
/// free again the moment that copy is done, and the texture the interface
/// samples changes only when the interface itself changes it. A 4K picture is
/// thirty-three megabytes, which the card copies in a fraction of a millisecond.

#include "cutline/gpu/compositor.hpp"
#include "cutline/gpu/device.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <string>

namespace cutline::gpu {

class FrameRing {
 public:
  /// `slots` is how many finished pictures can wait at once. Each is a canvas
  /// of display-encoded pixels, so this is memory on the card and not free:
  /// eight at 4K is about a quarter of a gigabyte.
  ///
  /// `producer` is the queue the render thread's compositor records onto, and
  /// `store` copies on the same one, so the copy follows the frame it copies.
  [[nodiscard]] static std::expected<std::unique_ptr<FrameRing>, std::string> create(
      std::shared_ptr<Device> device, std::size_t slots,
      std::shared_ptr<Queue> producer = nullptr);

  FrameRing(const FrameRing&) = delete;
  FrameRing& operator=(const FrameRing&) = delete;
  ~FrameRing();

  [[nodiscard]] std::size_t capacity() const noexcept;

  // ------------------------------------------------- the rendering thread --

  /// Whether a slot is free to `store` into without waiting.
  [[nodiscard]] bool has_room() const;

  /// Copies the compositor's display target into a free slot and returns the
  /// slot. Run the display pass first — `Compositor::display_texture` — since
  /// this copies what is there.
  ///
  /// Fails rather than waits when every slot is taken: a render thread that
  /// blocked here would be blocked on the interface, which is the dependency
  /// all of this exists to remove. Ask `has_room` first.
  [[nodiscard]] std::expected<std::size_t, std::string> store(Compositor& from);

  /// Gives a slot back without showing it. A frame rendered ahead that the
  /// playhead has already passed, or one from a project that has since changed.
  void discard(std::size_t slot);

  // ----------------------------------------------- the interface's thread --

  /// Copies a stored slot into the texture the interface draws from, frees the
  /// slot, and returns that texture.
  ///
  /// The texture is the same one every time until the size changes, which is
  /// what lets a painter wrap it once and keep the wrapper. Its generation
  /// moves when it is rebuilt.
  [[nodiscard]] std::expected<SceneTexture, std::string> show(std::size_t slot);

  /// The pixels of what `show` last put up, for tests: the only way to say the
  /// picture arrived is to look at it.
  [[nodiscard]] std::expected<Image, std::string> read_shown();

  struct Impl;

 private:
  explicit FrameRing(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::gpu
