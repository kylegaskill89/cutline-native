#include "cutline/editor/render_previews.hpp"

#include "cutline/core/query.hpp"
#include "cutline/core/serialize.hpp"
#include "cutline/render/warp_stabilizer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <set>
#include <system_error>

namespace cutline::editor {
namespace {

/// Edits closer than this are one edit: a frame at 240 fps is four
/// milliseconds, and nothing finer is a cut anybody made.
constexpr double kSameMoment = 1e-4;

/// The effects that work on neighbouring pixels rather than one at a time.
/// What makes a piece red rather than yellow.
constexpr std::array kSpatialEffects{"blur",     "directionalblur", "radialblur", "sharpen",
                                     "noise",    "distort",         "dropshadow", "chromakey",
                                     "trackmatte", "ultrakey",      "transform", "warpstabilizer"};

/// More layers than this over one moment is red as well.
constexpr std::size_t kLightLayers = 3;

[[nodiscard]] bool has_keyframes(const core::Clip& clip) {
  return std::ranges::any_of(clip.keyframes, [](const auto& list) { return !list.empty(); });
}

[[nodiscard]] const core::Media* media_of(const core::Project& project, const core::Clip& clip) {
  const auto found = std::ranges::find(project.media, clip.media_id, &core::Media::id);
  return found == project.media.end() ? nullptr : &*found;
}

[[nodiscard]] bool heavy_effects(const core::Clip& clip) {
  return std::ranges::any_of(clip.effects, [](const core::ClipEffect& effect) {
    if (!effect.enabled) return false;
    if (std::ranges::find(kSpatialEffects, effect.type) != kSpatialEffects.end()) return true;
    // Lumetri's sharpen, secondary and LUTs read more than their pixel.
    if (effect.type == "lumetri") {
      const auto value = [&effect](const char* key) {
        const auto found = effect.params.find(key);
        return found == effect.params.end() ? 0.0 : found->second;
      };
      return value("sharpen") != 0.0 || value("show_mask") != 0.0 || !effect.files.empty();
    }
    return false;
  });
}

/// Whether one clip, alone over a moment, plays straight from its source.
[[nodiscard]] bool plain(const core::Project& project, const core::Clip& clip) {
  const core::Media* media = media_of(project, clip);
  if (media == nullptr || media->path.empty() || !media->has_video) return false;
  if (media->is_image || media->is_graphic || media->is_color || media->is_adjustment ||
      !media->sequence_id.empty()) {
    return false;
  }
  const core::Sequence& sequence = project.sequence();
  if (media->width.value_or(0) != sequence.canvas_w ||
      media->height.value_or(0) != sequence.canvas_h) {
    return false;
  }
  return std::ranges::none_of(clip.effects, [](const core::ClipEffect& e) { return e.enabled; }) &&
         clip.opacity >= 1.0 && clip.transform == core::Transform{} && clip.speed == 1.0 &&
         !clip.reverse && !clip.hold.has_value() && !has_keyframes(clip) &&
         clip.blend == core::BlendMode::Normal;
}

/// The clips over one moment of the picture: visible video tracks, enabled
/// clips covering it.
[[nodiscard]] std::vector<const core::Clip*> layers_at(const core::Project& project, double t) {
  std::vector<const core::Clip*> out;
  for (const core::Track& track : project.sequence().tracks) {
    if (track.kind != core::TrackKind::Video || track.hidden) continue;
    for (const core::Clip& clip : track.clips) {
      if (clip.disabled) continue;
      if (clip.start <= t && t < core::clip_end(clip)) out.push_back(&clip);
    }
  }
  return out;
}

/// Whether a transition straddles the moment, on any visible video track.
[[nodiscard]] bool in_transition(const core::Project& project, double t) {
  for (const core::Track& track : project.sequence().tracks) {
    if (track.kind != core::TrackKind::Video || track.hidden) continue;
    for (const core::Clip& clip : track.clips) {
      if (!clip.transition_out.has_value() || clip.disabled) continue;
      const double cut = core::clip_end(clip);
      const double half = clip.transition_out->duration / 2.0;
      if (cut - half <= t && t < cut + half) return true;
    }
  }
  return false;
}

/// FNV-1a, 64 bits, as text: what a piece is called.
[[nodiscard]] std::string fingerprint(std::string_view bytes) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const char c : bytes) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }
  return std::format("{:016x}", hash);
}

/// What a piece is called: the sequence's frame and colour, and everything
/// over the piece, each clip placed from the piece's start, written out as a
/// project and fingerprinted. Placed from the piece's start, so moving a whole
/// stretch of the cut keeps what was rendered in it.
[[nodiscard]] std::string key_of(const core::Project& project, double from, double to) {
  core::Project only;
  only.sequences = {core::Sequence{}};
  core::Sequence& sequence = only.sequence();
  const core::Sequence& source = project.sequence();
  sequence.canvas_w = source.canvas_w;
  sequence.canvas_h = source.canvas_h;
  sequence.fps = source.fps;
  sequence.colour = source.colour;
  sequence.graphics_white = source.graphics_white;
  sequence.tone_map = source.tone_map;
  only.use_proxies = false;

  std::set<std::string> media_ids;
  bool nests = false;
  for (const core::Track& track : source.tracks) {
    if (track.kind != core::TrackKind::Video) continue;
    core::Track kept = track;
    kept.clips.clear();
    if (!track.hidden) {
      for (const core::Clip& clip : track.clips) {
        if (clip.disabled || core::clip_end(clip) <= from || clip.start >= to) continue;
        core::Clip placed = clip;
        // A Warp Stabilizer draws the clip as it is until it has been
        // analysed, so which analysis it has is part of what the piece shows.
        for (core::ClipEffect& effect : placed.effects) {
          if (effect.type != "warpstabilizer") continue;
          const std::string analysis = render::analysis_key(project, clip, effect);
          effect.files["analysis"] = render::has_analysis(analysis) ? analysis : std::string{};
        }
        placed.start -= from;
        placed.group_id.reset();
        placed.label_color.clear();
        kept.clips.push_back(std::move(placed));
        media_ids.insert(clip.media_id);
      }
    }
    sequence.tracks.push_back(std::move(kept));
  }
  for (const core::Media& media : project.media) {
    if (!media_ids.contains(media.id)) continue;
    only.media.push_back(media);
    nests = nests || !media.sequence_id.empty();
  }
  // A nest's picture is another sequence, so its contents are part of this.
  if (nests) {
    for (std::size_t i = 0; i < project.sequences.size(); ++i) {
      if (i != project.open) only.sequences.push_back(project.sequences[i]);
    }
  }
  return fingerprint(std::format("{:.6f}|{}", to - from, core::to_json(only, -1)));
}

}  // namespace

std::vector<RenderPiece> render_pieces(const core::Project& project,
                                       const std::filesystem::path& folder) {
  // Every edit: each clip's ends, and the ends of every transition.
  std::vector<double> edges;
  for (const core::Track& track : project.sequence().tracks) {
    if (track.kind != core::TrackKind::Video || track.hidden) continue;
    for (const core::Clip& clip : track.clips) {
      if (clip.disabled) continue;
      const double end = core::clip_end(clip);
      edges.push_back(clip.start);
      edges.push_back(end);
      if (clip.transition_out.has_value()) {
        const double half = clip.transition_out->duration / 2.0;
        edges.push_back(end - half);
        edges.push_back(end + half);
      }
    }
  }
  std::ranges::sort(edges);
  edges.erase(std::unique(edges.begin(), edges.end(),
                          [](double a, double b) { return std::abs(a - b) < kSameMoment; }),
              edges.end());

  std::vector<RenderPiece> pieces;
  for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
    const double from = edges[i];
    const double to = edges[i + 1];
    const double middle = (from + to) / 2.0;
    const std::vector<const core::Clip*> layers = layers_at(project, middle);
    if (layers.empty()) continue;

    RenderPiece piece{.start = from, .end = to};
    const bool transition = in_transition(project, middle);
    if (layers.size() == 1 && !transition && plain(project, *layers.front())) {
      pieces.push_back(piece);
      continue;
    }
    const bool heavy =
        layers.size() > kLightLayers ||
        std::ranges::any_of(layers, [&project](const core::Clip* clip) {
          const core::Media* media = media_of(project, *clip);
          const bool nest = media != nullptr && !media->sequence_id.empty();
          return nest || heavy_effects(*clip) || clip->speed != 1.0 || clip->reverse ||
                 !clip->keyframes[core::anim_prop_index(core::AnimProp::Speed)].empty();
        });
    piece.key = key_of(project, from, to);
    std::error_code error;
    piece.status = std::filesystem::exists(preview_file(folder, piece.key), error)
                       ? RenderStatus::Rendered
                   : heavy ? RenderStatus::Heavy
                           : RenderStatus::Unrendered;
    pieces.push_back(std::move(piece));
  }
  return pieces;
}

std::filesystem::path preview_file(const std::filesystem::path& folder, const std::string& key) {
  return folder / (key + ".mov");
}

std::filesystem::path default_previews_folder() {
  const char* local = std::getenv("LOCALAPPDATA");
  const std::filesystem::path base =
      local == nullptr ? std::filesystem::path{"."} : std::filesystem::path{local};
  return base / "Cutline" / "Preview Files";
}

std::filesystem::path previews_folder(const core::Project& project,
                                      const std::filesystem::path& project_file) {
  switch (project.video_previews.where) {
    case core::ScratchDisk::Where::SameAsProject:
      if (!project_file.empty()) return project_file.parent_path() / "Cutline Video Previews";
      break;
    case core::ScratchDisk::Where::Folder:
      if (!project.video_previews.folder.empty()) return project.video_previews.folder;
      break;
    case core::ScratchDisk::Where::Default: break;
  }
  return default_previews_folder();
}

std::vector<RenderPiece> pieces_to_render(const std::vector<RenderPiece>& pieces, double from,
                                          double to) {
  std::vector<RenderPiece> out;
  for (const RenderPiece& piece : pieces) {
    if (piece.status != RenderStatus::Unrendered && piece.status != RenderStatus::Heavy) continue;
    if (piece.end <= from || piece.start >= to) continue;
    out.push_back(piece);
  }
  return out;
}

std::size_t delete_preview_files(const std::filesystem::path& folder) {
  std::size_t gone = 0;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".mov") continue;
    if (std::filesystem::remove(entry.path(), error)) ++gone;
  }
  return gone;
}

}  // namespace cutline::editor
