#pragma once

/// Premiere's Interpret Footage: playing a source at a rate the file did not
/// claim.
///
/// Footage shot fast to be shown slow is the reason this exists. Conforming
/// 60 fps to 24 makes the source two and a half times longer and **every frame
/// of it a real one** — which is not what a 40% retime gives you, since that
/// has to either invent the frames in between or show each one two and a half
/// times. The two features look alike in the timeline and are different
/// pictures.
///
/// The other difference is where it lives. A retime is a property of one clip;
/// a conform is a property of the *source*, so every clip cut from it is
/// conformed, including the ones placed tomorrow.
///
/// **What is deliberately not here.** Premiere's dialogue also offers alpha
/// interpretation and audio channel layout. Decoded video in this application
/// is NV12 or YUV 4:2:0 and carries no alpha at all — the only thing in the
/// pipeline with an alpha channel is a rasterised title — so "ignore alpha" and
/// "invert alpha" would be switches on something that does not exist. Channel
/// layout is `Clip::channel_map`, which is built and is per clip, where a
/// camera's two microphones actually need deciding.

#include "cutline/core/model.hpp"

#include <optional>
#include <string_view>

namespace cutline::core {

/// How many seconds of *file* are consumed per second of source time.
///
/// One unless the source is conformed. Below one for footage shot fast and
/// shown slow, which is the direction anybody reaches for this in: at 60
/// conformed to 24 it is 0.4, so ten seconds of source reads four seconds of
/// file.
///
/// This is the one number the whole feature is: the renderer multiplies by it
/// on the way to the decoder, and everything above stays in source seconds.
[[nodiscard]] double conform_speed(const Media& m) noexcept;

/// Whether this source is played at a rate the file did not claim.
[[nodiscard]] bool is_conformed(const Media& m) noexcept;

/// The rate the source is *played* at: the override when there is one, the
/// file's own rate otherwise, and nothing when neither is known.
[[nodiscard]] std::optional<double> playback_fps(const Media& m) noexcept;

/// Whether a source can be conformed at all.
///
/// It cannot without a rate to conform *from*: a file whose frame rate never
/// probed has no ratio to form, and guessing one would be inventing the very
/// number the feature is about. Generated media and stills have no frames to
/// re-time either.
[[nodiscard]] bool can_interpret(const Media& m) noexcept;

/// Sets or clears a source's assumed rate. Nothing clears it.
///
/// **Clips already cut from it keep their frames.** Their source ranges are
/// scaled, so a clip showing frames 100 to 200 still shows frames 100 to 200 —
/// at the new rate, which makes it longer or shorter on the timeline. Leaving
/// the ranges alone would have every existing clip jump to different footage,
/// which is not what changing a playback rate means.
///
/// **And the sequence ripples to fit.** Every clip that changed length moves
/// what follows it along, on every track a sync lock holds together — the same
/// rule a retime with the ripple box ticked uses, because it is the same
/// problem. Without it, conforming a source already in a cut leaves its clips
/// overlapping their neighbours.
///
/// Returns the project unchanged when the media is not there, cannot be
/// interpreted, or is already at this rate.
[[nodiscard]] Project interpret_media(Project p, std::string_view media_id,
                                      std::optional<double> assumed_fps);

}  // namespace cutline::core
