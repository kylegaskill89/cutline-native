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
  // — it will happily identify a text file as some raw format — and without
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

  // A whole new renderer rather than a resize: it owns its compositor and every
  // decoder it has open, and those are all sized to the old canvas. Rebuilding
  // costs the open decoders, which is why this returns early when it can.
  auto renderer = engine::FrameRenderer::create(impl_->device, width, height);
  if (!renderer.has_value()) return std::unexpected(renderer.error());

  impl_->renderer = std::move(*renderer);
  impl_->width = width;
  impl_->height = height;
  impl_->frame = {};
  return {};
}

std::expected<ui::ImageView, std::string> ProjectPreview::frame_at(const core::Project& project,
                                                                   double t) {
  // The sequence may have been resized under us — opening a different project,
  // most obviously — and rendering at the old size would letterbox wrongly.
  if (const auto matched = resize(project.canvas_w, project.canvas_h); !matched.has_value()) {
    return std::unexpected(matched.error());
  }

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

int ProjectPreview::width() const noexcept { return impl_->width; }
int ProjectPreview::height() const noexcept { return impl_->height; }

const std::vector<std::string>& ProjectPreview::missing_media() const noexcept {
  return impl_->renderer->missing_media();
}

}  // namespace cutline::app
