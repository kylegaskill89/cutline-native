#pragma once

/// Premiere's Morph Cut, as it is drawn: a dissolve in which each picture is
/// first bent partway towards the other along the motion found between them,
/// so that halfway through both show the head halfway between where it was
/// and where it ends up.
///
/// The motion is found in the background, a field for every frame of the
/// transition from the outgoing picture to the incoming one, and kept beside
/// Warp Stabilizer's analyses under a name made of what was analysed: both
/// clips' sources and parts, and the transition's length. So the project
/// records nothing, and trimming either side or changing the length makes the
/// name change and the looking run again, as Premiere analyses again after any
/// change to the transition. Until it has, the transition is a plain dissolve.
///
/// The bending is Warp Stabilizer's mesh pass, run on each side before its
/// effects: the outgoing picture drawn from where its points were, the
/// incoming one from where they are going.

#include "cutline/core/model.hpp"
#include "cutline/core/segments.hpp"
#include "cutline/render/effect_passes.hpp"
#include "cutline/track/morph.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cutline::render {

/// A Morph Cut in the open sequence: the two sides as they are drawn, which
/// is what the looking needs to draw them alone.
struct MorphJoin {
  core::VideoSeg out;
  core::VideoSeg in;
  /// The window it morphs across, on the timeline.
  double start = 0.0;
  double end = 0.0;
  std::string key;
};

/// Every Morph Cut in the open sequence, top track first. The segments point
/// into `project`.
[[nodiscard]] std::vector<MorphJoin> morph_cuts(const core::Project& project);

/// The one at the out-edge of `clip_id`, if it has one.
[[nodiscard]] std::optional<MorphJoin> morph_cut_of(const core::Project& project,
                                                    std::string_view clip_id);

/// The name of the analysis a Morph Cut needs. Empty when either side has
/// nothing to analyse.
[[nodiscard]] std::string morph_key(const core::Project& project, const core::MorphSide& side);
[[nodiscard]] std::filesystem::path morph_analysis_path(const std::string& key);
[[nodiscard]] bool has_morph_analysis(const std::string& key);

/// How many frames the looking makes a field for, and the moment of each: the
/// middle of each of the sequence's frames across the window.
[[nodiscard]] int morph_frames(const core::Project& project, double start, double end);
[[nodiscard]] double morph_frame_time(const core::Project& project, double start, int frame);

/// The analysis, read once and kept until its file changes.
[[nodiscard]] std::shared_ptr<const track::MorphAnalysis> morph_analysis(const std::string& key);

/// The mesh pass that bends `seg`'s picture at timeline `t`, when it is one
/// side of a Morph Cut that has been analysed.
[[nodiscard]] std::optional<EffectPass> plan_morph_cut(const core::Project& project,
                                                       const core::VideoSeg& seg, double t);

}  // namespace cutline::render
