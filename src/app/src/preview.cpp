#include "cutline/app/preview.hpp"

#include "cutline/engine/frame_renderer.hpp"
#include "cutline/gpu/compositor.hpp"
#include "cutline/gpu/device.hpp"
#include "cutline/media/probe.hpp"

#include <filesystem>
#include <utility>

namespace cutline::app {

std::expected<editor::MediaSource, std::string> probe_source(std::string_view path) {
  const auto info = media::probe(path);
  if (!info.has_value()) return std::unexpected(info.error());

  // libavformat is permissive enough to open things that are not media at all
  // â€” it will happily identify a text file as some raw format â€” and without
  // this a mistyped name imports as a clip with no streams and no duration,
  // which then sits in the browser doing nothing and explaining nothing.
  if (!info->has_video() && info->audio.empty()) {
    return std::unexpected(std::string(path) + " has no video or audio in it");
  }

  const std::filesystem::path file{path};

  editor::MediaSource source;
  source.path = file.string();
  source.name = file.filename().string();
  source.duration = info->duration;
  source.has_video = info->has_video();
  source.audio_stream_count = static_cast<int>(info->audio.size());

  // A still is decided by extension rather than by what the decoder makes of
  // it. libavformat reports a PNG as a one-frame video, and taking that at
  // face value places a clip one frame long.
  source.is_image = editor::looks_like_image(file);
  source.is_animated = source.is_image && file.extension() == ".gif";

  if (const media::VideoStreamInfo* video = info->primary_video(); video != nullptr) {
    source.width = video->width;
    source.height = video->height;
    if (video->fps > 0.0) source.fps = video->fps;
  }
  return source;
}

// ----------------------------------------------------------------- preview --

struct ProjectPreview::Impl {
  std::shared_ptr<gpu::Device> device;
  std::unique_ptr<engine::FrameRenderer> renderer;
  gpu::Image frame;
  int width = 0;
  int height = 0;
};

ProjectPreview::ProjectPreview(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ProjectPreview::~ProjectPreview() = default;

std::expected<std::unique_ptr<ProjectPreview>, std::string> ProjectPreview::create(
    int canvas_width, int canvas_height, std::shared_ptr<gpu::Device> device) {
  if (canvas_width <= 0 || canvas_height <= 0) {
    return std::unexpected("a preview needs a canvas with some size to it");
  }

  if (device == nullptr) {
    auto made = gpu::Device::create();
    if (!made.has_value()) return std::unexpected(made.error());
    device = std::move(*made);
  }

  auto renderer = engine::FrameRenderer::create(device, canvas_width, canvas_height);
  if (!renderer.has_value()) return std::unexpected(renderer.error());

  auto impl = std::make_unique<Impl>();
  impl->device = std::move(device);
  impl->renderer = std::move(*renderer);
  impl->width = canvas_width;
  impl->height = canvas_height;

  return std::unique_ptr<ProjectPreview>(new ProjectPreview(std::move(impl)));
}

std::expected<void, std::string> ProjectPreview::resize(int width, int height) {
  if (width <= 0 || height <= 0) return std::unexpected("a canvas needs some size to it");
  if (width == impl_->width && height == impl_->height) return {};

  // The renderer keeps its decoders. This used to build a whole new one, on the
  // grounds that it owns its compositor "and every decoder it has open, and
  // those are all sized to the old canvas" â€” which is true of the compositor and
  // false of the decoders. A decoder is sized to its *source*: its surface pool
  // comes from the media's own width and height, and nothing in it knows what
  // canvas the result will be drawn into.
  //
  // So throwing them away bought nothing and cost a seek from the nearest
  // keyframe on every open source. Changing the preview quality mid-playback
  // stalled for over a second â€” 1,435 ms at 1/2 on a 4K60 capture â€” and that is
  // the one control somebody reaches for *because* playback is already
  // struggling under a stack of effects. The worst possible moment to freeze.
  //
  // `Compositor::resize` rebuilds only the canvas-sized targets, and it waits
  // for the GPU to go idle before freeing them â€” which is the wait the old code
  // had to do by hand, because what it could not do was wait on behalf of a
  // renderer being destroyed whole. That crash is still guarded; it is guarded
  // in the one place that owns the textures.
  if (const auto ok = impl_->renderer->resize(width, height); !ok.has_value()) {
    return std::unexpected(ok.error());
  }

  impl_->width = width;
  impl_->height = height;
  impl_->frame = {};
  return {};
}

std::expected<ui::ImageView, std::string> ProjectPreview::frame_at(const core::Project& project,
                                                                   double t) {
  // The sequence may have been resized under us â€” opening a different project,
  // most obviously â€” and rendering at the old size would letterbox wrongly.
  if (const auto matched = resize(project.canvas_w, project.canvas_h); !matched.has_value()) {
    return std::unexpected(matched.error());
  }

  // The preview is the one thing that asks for proxies, and it asks every
  // frame from the project's own setting â€” so turning them on or off takes
  // effect at once rather than at whatever moment something remembers to push
  // it. Export never asks, which is what keeps the small copy out of the file
  // it writes.
  impl_->renderer->set_use_proxies(project.use_proxies);

  if (const auto rendered = impl_->renderer->render(project, t); !rendered.has_value()) {
    return std::unexpected(rendered.error());
  }

  auto image = impl_->renderer->read_back();
  if (!image.has_value()) return std::unexpected(image.error());

  impl_->frame = std::move(*image);
  if (impl_->frame.empty()) return ui::ImageView{};

  return ui::ImageView{.pixels = impl_->frame.pixels.data(),
                       .width = impl_->frame.width,
                       .height = impl_->frame.height};
}

std::expected<ui::TextureView, std::string> ProjectPreview::texture_at(
    const core::Project& project, double t) {
  if (const auto matched = resize(project.canvas_w, project.canvas_h); !matched.has_value()) {
    return std::unexpected(matched.error());
  }

  // The preview is the one thing that asks for proxies, and it asks every
  // frame from the project's own setting â€” so turning them on or off takes
  // effect at once rather than at whatever moment something remembers to push
  // it. Export never asks, which is what keeps the small copy out of the file
  // it writes.
  impl_->renderer->set_use_proxies(project.use_proxies);

  if (const auto rendered = impl_->renderer->render(project, t); !rendered.has_value()) {
    return std::unexpected(rendered.error());
  }

  auto scene = impl_->renderer->compositor().display_texture();
  if (!scene.has_value()) return std::unexpected(scene.error());

  return ui::TextureView{.texture = scene->resource,
                         .width = scene->width,
                         .height = scene->height,
                         .format = scene->format,
                         .state = scene->state,
                         .generation = scene->generation};
}

const std::shared_ptr<gpu::Device>& ProjectPreview::device() const noexcept {
  return impl_->device;
}

engine::FrameRenderer::DecodeStats ProjectPreview::decode_stats() const noexcept {
  return impl_->renderer->decode_stats();
}

int ProjectPreview::width() const noexcept { return impl_->width; }
int ProjectPreview::height() const noexcept { return impl_->height; }

const std::vector<std::string>& ProjectPreview::missing_media() const noexcept {
  return impl_->renderer->missing_media();
}

void ProjectPreview::release_sources() { impl_->renderer->release_sources(); }

}  // namespace cutline::app
