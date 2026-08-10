#include "cutline/core/serialize.hpp"

#include "cutline/core/id.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <utility>

namespace cutline::core {

using nlohmann::json;

// Enum spellings are part of the file format, so they are written out rather
// than derived from the enumerator order.
NLOHMANN_JSON_SERIALIZE_ENUM(TrackKind, {
                                            {TrackKind::Video, "video"},
                                            {TrackKind::Audio, "audio"},
                                        })

// Read first, so an unrecognised name in a file written by something newer
// reads as Read rather than as Off — following a curve is the safe answer, and
// silently ignoring somebody's automation is not.
NLOHMANN_JSON_SERIALIZE_ENUM(AutomationMode, {
                                                 {AutomationMode::Read, "read"},
                                                 {AutomationMode::Off, "off"},
                                                 {AutomationMode::Write, "write"},
                                                 {AutomationMode::Latch, "latch"},
                                                 {AutomationMode::Touch, "touch"},
                                             })

// None first, so a role written by something newer reads as an unlabelled clip
// rather than as a wrong label — being told nothing is better than being told
// that the music is dialogue.
NLOHMANN_JSON_SERIALIZE_ENUM(AudioRole, {
                                            {AudioRole::None, "none"},
                                            {AudioRole::Dialogue, "dialogue"},
                                            {AudioRole::Music, "music"},
                                            {AudioRole::Effects, "sfx"},
                                            {AudioRole::Ambience, "ambience"},
                                        })

NLOHMANN_JSON_SERIALIZE_ENUM(BlendMode, {
                                            {BlendMode::Normal, "normal"},
                                            {BlendMode::Add, "add"},
                                            {BlendMode::Screen, "screen"},
                                            {BlendMode::Multiply, "multiply"},
                                            {BlendMode::Overlay, "overlay"},
                                            {BlendMode::Darken, "darken"},
                                            {BlendMode::Lighten, "lighten"},
                                            {BlendMode::Difference, "difference"},
                                        })

NLOHMANN_JSON_SERIALIZE_ENUM(TransitionKind, {
                                                 {TransitionKind::Dissolve, "dissolve"},
                                                 {TransitionKind::DipBlack, "dip-black"},
                                                 {TransitionKind::Push, "push"},
                                                 {TransitionKind::Slide, "slide"},
                                             })

NLOHMANN_JSON_SERIALIZE_ENUM(TextAlign, {
                                            {TextAlign::Left, "left"},
                                            {TextAlign::Center, "center"},
                                            {TextAlign::Right, "right"},
                                        })

NLOHMANN_JSON_SERIALIZE_ENUM(MaskShape, {
                                            {MaskShape::None, "none"},
                                            {MaskShape::Ellipse, "ellipse"},
                                            {MaskShape::Rectangle, "rectangle"},
                                            {MaskShape::Path, "path"},
                                        })

NLOHMANN_JSON_SERIALIZE_ENUM(Interp, {
                                         {Interp::Linear, "linear"},
                                         {Interp::Hold, "hold"},
                                         {Interp::Ease, "ease"},
                                         {Interp::Bezier, "bezier"},
                                     })

namespace {

/// Keyframe lists are stored under property names rather than array positions,
/// so reordering the AnimProp enumerators cannot silently reinterpret a file.
constexpr std::array<const char*, kAnimPropCount> kAnimPropNames{
    "x",        "y",        "scale_x", "scale_y", "rotation",
    "opacity",  "anchor_x", "anchor_y", "pan",     "speed",
};
static_assert(kAnimPropNames.back() != nullptr,
              "every animatable property needs a name in the file, or the last one is read "
              "through an uninitialised pointer");

// ------------------------------------------------------------------ writing --

/// Writes a container only when it has something in it, keeping documents free
/// of empty arrays and objects.
template <typename T>
void put_unless_empty(json& j, const char* key, const T& value) {
  if (!value.empty()) j[key] = value;
}

template <typename T>
void put_if_set(json& j, const char* key, const std::optional<T>& value) {
  if (value.has_value()) j[key] = *value;
}

json write(const Keyframe& k) {
  json j{{"t", k.t}, {"v", k.v}};
  if (k.e != Interp::Linear) j["e"] = k.e;  // linear is the overwhelming default
  // Handles only when they mean something. Every keyframe carries them and
  // almost none has moved them, so writing them always would double the size of
  // an animated project's file to say "untouched" over and over.
  if (k.e == Interp::Bezier) {
    j["h"] = json::array({k.out_x, k.out_y, k.in_x, k.in_y});
  }
  return j;
}

json write(const std::vector<Keyframe>& kfs) {
  json out = json::array();
  for (const Keyframe& k : kfs) out.push_back(write(k));
  return out;
}

json write(const Transform& t) {
  return {{"x", t.x},
          {"y", t.y},
          {"scale_x", t.scale_x},
          {"scale_y", t.scale_y},
          {"rotation", t.rotation},
          {"anchor_x", t.anchor_x},
          {"anchor_y", t.anchor_y},
          {"anti_flicker", t.anti_flicker}};
}

json write(const MatteGradient& g) { return {{"color2", g.color2}, {"angle", g.angle}}; }

json write(const TextSpec& s) {
  json j{{"content", s.content},
         {"font_size", s.font_size},
         {"color", s.color},
         {"font_family", s.font_family},
         {"bold", s.bold},
         {"italic", s.italic},
         {"align", s.align},
         {"stroke_width", s.stroke_width},
         {"shadow", s.shadow}};
  put_if_set(j, "background", s.background);
  put_if_set(j, "stroke_color", s.stroke_color);
  return j;
}

json write(const Mask& m) {
  json j{{"shape", m.shape},     {"x", m.x},
         {"y", m.y},             {"width", m.width},
         {"height", m.height},   {"rotation", m.rotation},
         {"feather", m.feather}, {"opacity", m.opacity},
         {"inverted", m.inverted}};
  // Only for a path. Every mask carries the field and almost none has corners.
  if (!m.points.empty()) {
    json points = json::array();
    for (const MaskPoint& point : m.points) {
      json entry{{"x", point.x}, {"y", point.y}};
      // The handles only when there are any. A sharp corner is the common case
      // and every path written before the pen existed is entirely sharp, so
      // writing four zeroes per point would quadruple the size of the commonest
      // shape to say nothing.
      if (!point.sharp()) {
        entry["in_x"] = point.in_x;
        entry["in_y"] = point.in_y;
        entry["out_x"] = point.out_x;
        entry["out_y"] = point.out_y;
      }
      points.push_back(std::move(entry));
    }
    j["points"] = std::move(points);
  }
  return j;
}

json write(const ClipEffect& e) {
  json j{{"type", e.type}, {"enabled", e.enabled}};
  // Only when there is one. Every effect carries the field and almost none has
  // a shape, so writing it always would put nine keys saying "nothing" onto
  // every entry in every project.
  if (e.mask.active()) j["mask"] = write(e.mask);
  put_unless_empty(j, "params", e.params);
  put_unless_empty(j, "colors", e.colors);
  if (!e.keyframes.empty()) {
    json kfs = json::object();
    for (const auto& [key, list] : e.keyframes) kfs[key] = write(list);
    j["keyframes"] = std::move(kfs);
  }
  return j;
}

json write(const AudioClipEffect& e) {
  json j{{"type", e.type}, {"enabled", e.enabled}};
  put_unless_empty(j, "params", e.params);
  if (!e.keyframes.empty()) {
    json kfs = json::object();
    for (const auto& [key, list] : e.keyframes) kfs[key] = write(list);
    j["keyframes"] = std::move(kfs);
  }
  return j;
}

json write(const Media& m) {
  json j{{"id", m.id},
         {"path", m.path},
         {"name", m.name},
         {"duration", m.duration},
         {"has_video", m.has_video},
         {"audio_stream_count", m.audio_stream_count},
         {"is_image", m.is_image},
         {"is_animated", m.is_animated},
         {"is_text", m.is_text},
         {"is_color", m.is_color},
         {"is_adjustment", m.is_adjustment}};
  if (!m.color.empty()) j["color"] = m.color;
  if (m.text.has_value()) j["text"] = write(*m.text);
  if (m.gradient.has_value()) j["gradient"] = write(*m.gradient);
  put_if_set(j, "width", m.width);
  put_if_set(j, "height", m.height);
  put_if_set(j, "fps", m.fps);
  // Both absent unless the source has been conformed, so nothing written
  // before Interpret Footage existed gains a field and nothing written now
  // troubles a build that predates it.
  put_if_set(j, "assumed_fps", m.assumed_fps);
  put_if_set(j, "file_duration", m.file_duration);
  put_if_set(j, "in_point", m.in_point);
  put_if_set(j, "out_point", m.out_point);
  if (!m.proxy_path.empty()) j["proxy_path"] = m.proxy_path;
  if (!m.bin.empty()) j["bin"] = m.bin;
  if (!m.label_color.empty()) j["label_color"] = m.label_color;
  return j;
}

json write(const Bin& b) {
  json j{{"id", b.id}, {"name", b.name}};
  // Omitted at the top level, which is where most bins are: a key saying
  // "parent: nothing" on every one of them says less than its absence.
  if (!b.parent.empty()) j["parent"] = b.parent;
  return j;
}

json write(const Clip& c) {
  json j{{"id", c.id},
         {"media_id", c.media_id},
         {"kind", c.kind},
         {"audio_stream", c.audio_stream},
         {"source_in", c.source_in},
         {"source_out", c.source_out},
         {"start", c.start},
         {"gain", c.gain},
         {"pan", c.pan},
         {"opacity", c.opacity},
         {"fade_in", c.fade_in},
         {"fade_out", c.fade_out},
         {"speed", c.speed},
         {"reverse", c.reverse},
         {"blend", c.blend},
         {"disabled", c.disabled},
         {"label_color", c.label_color},
         {"transform", write(c.transform)}};
  put_if_set(j, "group_id", c.group_id);
  put_if_set(j, "hold", c.hold);
  put_unless_empty(j, "gain_keyframes", write(c.gain_keyframes));
  put_unless_empty(j, "channel_map", json(c.channel_map));
  // Only when it has one, so a project from before roles existed writes exactly
  // the JSON it always did.
  if (c.role != AudioRole::None) j["role"] = c.role;

  if (c.transition_out.has_value()) {
    j["transition_out"] = json{{"kind", c.transition_out->kind},
                               {"duration", c.transition_out->duration}};
  }

  json keyframes = json::object();
  for (std::size_t i = 0; i < kAnimPropCount; ++i) {
    if (!c.keyframes[i].empty()) keyframes[kAnimPropNames[i]] = write(c.keyframes[i]);
  }
  put_unless_empty(j, "keyframes", keyframes);

  json effects = json::array();
  for (const ClipEffect& e : c.effects) effects.push_back(write(e));
  put_unless_empty(j, "effects", effects);

  json audio_effects = json::array();
  for (const AudioClipEffect& e : c.audio_effects) audio_effects.push_back(write(e));
  put_unless_empty(j, "audio_effects", audio_effects);

  return j;
}

json write(const Track& t) {
  json j{{"id", t.id},
         {"kind", t.kind},
         {"gain", t.gain},
         {"pan", t.pan},
         {"muted", t.muted},
         {"solo", t.solo},
         {"locked", t.locked},
         {"hidden", t.hidden},
         {"targeted", t.targeted},
         {"sync_locked", t.sync_locked}};
  if (!t.label.empty()) j["label"] = t.label;
  put_if_set(j, "height", t.height);
  // Only when there is any, so a project whose faders were set and left reads
  // exactly as it always did.
  put_unless_empty(j, "gain_keyframes", write(t.gain_keyframes));
  put_unless_empty(j, "pan_keyframes", write(t.pan_keyframes));
  // Only when it is not the default, so a track nobody has automated writes
  // exactly the JSON it always did.
  if (t.automation != AutomationMode::Read) j["automation"] = t.automation;

  json track_effects = json::array();
  for (const AudioClipEffect& e : t.audio_effects) track_effects.push_back(write(e));
  put_unless_empty(j, "audio_effects", track_effects);

  // Routing, written only when it says something. A track feeding the master
  // with no sends is every track in nearly every project.
  if (t.submix) j["submix"] = true;
  if (!t.output.empty()) j["output"] = t.output;
  json sends = json::array();
  for (const Send& s : t.sends) {
    json send{{"to", s.to}, {"level", s.level}};
    if (s.pre_fader) send["pre_fader"] = true;
    sends.push_back(std::move(send));
  }
  put_unless_empty(j, "sends", sends);

  json clips = json::array();
  for (const Clip& c : t.clips) clips.push_back(write(c));
  put_unless_empty(j, "clips", clips);
  return j;
}

json write(const Marker& m) {
  json j{{"id", m.id}, {"time", m.time}};
  if (!m.label.empty()) j["label"] = m.label;
  if (!m.color.empty()) j["color"] = m.color;
  if (m.duration > 0.0) j["duration"] = m.duration;
  if (!m.comment.empty()) j["comment"] = m.comment;
  return j;
}

/// One sequence, in the shape a project used to be written in.
///
/// The field names are the ones a single-sequence project has always used, so
/// the sequence a project opens with reads and writes exactly as it did — see
/// `write(const Project&)` for why that is worth keeping.
json write(const Sequence& s) {
  json j{{"canvas_w", s.canvas_w},
         {"canvas_h", s.canvas_h},
         {"fps", s.fps},
         {"master_gain", s.master_gain},
         {"drop_frame", s.drop_frame}};
  if (!s.id.empty()) j["id"] = s.id;
  if (!s.name.empty()) j["name"] = s.name;
  put_unless_empty(j, "master_gain_keyframes", write(s.master_gain_keyframes));
  if (s.master_automation != AutomationMode::Read) j["master_automation"] = s.master_automation;

  json master_effects = json::array();
  for (const AudioClipEffect& e : s.master_effects) master_effects.push_back(write(e));
  put_unless_empty(j, "master_effects", master_effects);

  json tracks = json::array();
  for (const Track& t : s.tracks) tracks.push_back(write(t));
  put_unless_empty(j, "tracks", tracks);

  json markers = json::array();
  for (const Marker& m : s.markers) markers.push_back(write(m));
  put_unless_empty(j, "markers", markers);

  put_if_set(j, "in_point", s.in_point);
  put_if_set(j, "out_point", s.out_point);
  return j;
}

/// A project.
///
/// **The first sequence is written where a project's own fields have always
/// been.** A project with one sequence — which is every project written before
/// there could be two, and most of them after — comes out byte for byte the
/// shape it always did, and an older build can still open it. Only the second
/// and later sequences go in `sequences`, and only then does a file appear that
/// an older build would read as its first sequence alone. Losing the others is
/// a poor outcome; refusing to open the file at all is a worse one.
json write(const Project& p) {
  json j = p.sequences.empty() ? write(Sequence{}) : write(p.sequences.front());
  j["use_proxies"] = p.use_proxies;

  json media = json::array();
  for (const Media& m : p.media) media.push_back(write(m));
  put_unless_empty(j, "media", media);

  json bins = json::array();
  for (const Bin& b : p.bins) bins.push_back(write(b));
  put_unless_empty(j, "bins", bins);

  if (p.sequences.size() > 1) {
    json rest = json::array();
    for (std::size_t i = 1; i < p.sequences.size(); ++i) rest.push_back(write(p.sequences[i]));
    j["sequences"] = std::move(rest);
    j["open"] = p.open;
  }

  return j;
}

// ------------------------------------------------------------------ reading --

/// Reads a field, or leaves the fallback in place when it is absent or null.
/// Absent fields are how a file written by an older build stays readable.
template <typename T>
T read_or(const json& j, const char* key, T fallback) {
  if (!j.is_object()) return fallback;
  const auto it = j.find(key);
  if (it == j.end() || it->is_null()) return fallback;
  return it->get<T>();
}

template <typename T>
std::optional<T> read_optional(const json& j, const char* key) {
  if (!j.is_object()) return std::nullopt;
  const auto it = j.find(key);
  if (it == j.end() || it->is_null()) return std::nullopt;
  return it->get<T>();
}

std::vector<Keyframe> read_keyframes(const json& j) {
  std::vector<Keyframe> out;
  if (!j.is_array()) return out;
  for (const json& k : j) {
    Keyframe frame{
        .t = read_or(k, "t", 0.0),
        .v = read_or(k, "v", 0.0),
        .e = read_or(k, "e", Interp::Linear),
    };
    // Absent in every file written before there were handles, and absent in
    // most written since — the defaults are the cubic that is a straight line,
    // which is what a keyframe without them meant.
    if (const auto handles = k.find("h");
        handles != k.end() && handles->is_array() && handles->size() == 4) {
      frame.out_x = (*handles)[0].get<double>();
      frame.out_y = (*handles)[1].get<double>();
      frame.in_x = (*handles)[2].get<double>();
      frame.in_y = (*handles)[3].get<double>();
    }
    out.push_back(frame);
  }
  return out;
}

Transform read_transform(const json& j) {
  Transform t;
  t.x = read_or(j, "x", t.x);
  t.y = read_or(j, "y", t.y);
  t.scale_x = read_or(j, "scale_x", t.scale_x);
  t.scale_y = read_or(j, "scale_y", t.scale_y);
  t.rotation = read_or(j, "rotation", t.rotation);
  // Absent in every file written before there was an anchor, and the default is
  // the middle of the layer, which is what those files meant.
  t.anchor_x = read_or(j, "anchor_x", t.anchor_x);
  t.anchor_y = read_or(j, "anchor_y", t.anchor_y);
  // Absent in every file written before the filter existed, and zero is what
  // those files meant: no softening at all.
  t.anti_flicker = read_or(j, "anti_flicker", t.anti_flicker);
  return t;
}

TextSpec read_text_spec(const json& j) {
  TextSpec s;
  s.content = read_or(j, "content", s.content);
  s.font_size = read_or(j, "font_size", s.font_size);
  s.color = read_or(j, "color", s.color);
  s.font_family = read_or(j, "font_family", s.font_family);
  s.bold = read_or(j, "bold", s.bold);
  s.italic = read_or(j, "italic", s.italic);
  s.align = read_or(j, "align", s.align);
  s.background = read_optional<std::string>(j, "background");
  s.stroke_color = read_optional<std::string>(j, "stroke_color");
  s.stroke_width = read_or(j, "stroke_width", s.stroke_width);
  s.shadow = read_or(j, "shadow", s.shadow);
  return s;
}

ClipEffect read_clip_effect(const json& j) {
  ClipEffect e;
  e.type = read_or(j, "type", std::string{});
  e.enabled = read_or(j, "enabled", true);
  e.params = read_or(j, "params", std::map<std::string, double>{});
  e.colors = read_or(j, "colors", std::map<std::string, std::string>{});

  const auto kfs = j.find("keyframes");
  if (kfs != j.end() && kfs->is_object()) {
    for (const auto& [key, list] : kfs->items()) e.keyframes[key] = read_keyframes(list);
  }

  if (const auto mask = j.find("mask"); mask != j.end() && mask->is_object()) {
    e.mask.shape = read_or(*mask, "shape", MaskShape::None);
    e.mask.x = read_or(*mask, "x", e.mask.x);
    e.mask.y = read_or(*mask, "y", e.mask.y);
    e.mask.width = read_or(*mask, "width", e.mask.width);
    e.mask.height = read_or(*mask, "height", e.mask.height);
    e.mask.rotation = read_or(*mask, "rotation", e.mask.rotation);
    e.mask.feather = read_or(*mask, "feather", e.mask.feather);
    e.mask.opacity = read_or(*mask, "opacity", e.mask.opacity);
    e.mask.inverted = read_or(*mask, "inverted", e.mask.inverted);
    if (const auto points = mask->find("points");
        points != mask->end() && points->is_array()) {
      for (const auto& point : *points) {
        if (!point.is_object() || e.mask.points.size() >= kMaxMaskPoints) continue;
        // Absent handles read as zero, which is a sharp corner — so a path
        // written before the pen existed comes back as exactly the polygon it
        // was.
        e.mask.points.push_back(MaskPoint{.x = read_or(point, "x", 0.0),
                                          .y = read_or(point, "y", 0.0),
                                          .in_x = read_or(point, "in_x", 0.0),
                                          .in_y = read_or(point, "in_y", 0.0),
                                          .out_x = read_or(point, "out_x", 0.0),
                                          .out_y = read_or(point, "out_y", 0.0)});
      }
    }
  }
  return e;
}

AudioClipEffect read_audio_effect(const json& j) {
  AudioClipEffect e;
  e.type = read_or(j, "type", std::string{});
  e.enabled = read_or(j, "enabled", true);
  e.params = read_or(j, "params", std::map<std::string, double>{});

  const auto kfs = j.find("keyframes");
  if (kfs != j.end() && kfs->is_object()) {
    for (const auto& [key, list] : kfs->items()) e.keyframes[key] = read_keyframes(list);
  }
  return e;
}

Media read_media(const json& j) {
  Media m;
  m.id = read_or(j, "id", std::string{});
  m.path = read_or(j, "path", std::string{});
  m.name = read_or(j, "name", std::string{});
  m.duration = read_or(j, "duration", 0.0);
  m.has_video = read_or(j, "has_video", false);
  m.audio_stream_count = read_or(j, "audio_stream_count", 0);
  m.is_image = read_or(j, "is_image", false);
  m.is_animated = read_or(j, "is_animated", false);
  m.is_text = read_or(j, "is_text", false);
  m.is_color = read_or(j, "is_color", false);
  m.is_adjustment = read_or(j, "is_adjustment", false);
  m.color = read_or(j, "color", std::string{});

  const auto text = j.find("text");
  if (text != j.end() && text->is_object()) m.text = read_text_spec(*text);

  const auto gradient = j.find("gradient");
  if (gradient != j.end() && gradient->is_object()) {
    m.gradient = MatteGradient{
        .color2 = read_or(*gradient, "color2", std::string{}),
        .angle = read_or(*gradient, "angle", 0.0),
    };
  }

  m.width = read_optional<int>(j, "width");
  m.height = read_optional<int>(j, "height");
  m.fps = read_optional<double>(j, "fps");
  m.assumed_fps = read_optional<double>(j, "assumed_fps");
  m.file_duration = read_optional<double>(j, "file_duration");
  m.in_point = read_optional<double>(j, "in_point");
  m.out_point = read_optional<double>(j, "out_point");
  m.proxy_path = read_or(j, "proxy_path", std::string{});
  m.bin = read_or(j, "bin", std::string{});
  m.label_color = read_or(j, "label_color", std::string{});
  return m;
}

Clip read_clip(const json& j) {
  Clip c;
  c.id = read_or(j, "id", std::string{});
  c.media_id = read_or(j, "media_id", std::string{});
  c.kind = read_or(j, "kind", TrackKind::Video);
  c.audio_stream = read_or(j, "audio_stream", 0);
  c.source_in = read_or(j, "source_in", 0.0);
  c.source_out = read_or(j, "source_out", 0.0);
  c.start = read_or(j, "start", 0.0);
  c.group_id = read_optional<std::string>(j, "group_id");
  c.gain = read_or(j, "gain", 1.0);
  c.pan = read_or(j, "pan", 0.0);
  c.opacity = read_or(j, "opacity", 1.0);
  c.fade_in = read_or(j, "fade_in", 0.0);
  c.fade_out = read_or(j, "fade_out", 0.0);
  c.speed = read_or(j, "speed", 1.0);
  c.reverse = read_or(j, "reverse", false);
  c.blend = read_or(j, "blend", BlendMode::Normal);
  c.disabled = read_or(j, "disabled", false);
  c.label_color = read_or(j, "label_color", std::string{});
  c.hold = read_optional<double>(j, "hold");

  const auto transform = j.find("transform");
  if (transform != j.end() && transform->is_object()) c.transform = read_transform(*transform);

  const auto gain_keyframes = j.find("gain_keyframes");
  if (gain_keyframes != j.end()) c.gain_keyframes = read_keyframes(*gain_keyframes);

  const auto channel_map = j.find("channel_map");
  if (channel_map != j.end() && channel_map->is_array()) {
    for (const json& entry : *channel_map) {
      if (entry.is_number_integer()) c.channel_map.push_back(entry.get<int>());
    }
  }

  c.role = read_or(j, "role", AudioRole::None);

  const auto transition = j.find("transition_out");
  if (transition != j.end() && transition->is_object()) {
    c.transition_out = Transition{
        .kind = read_or(*transition, "kind", TransitionKind::Dissolve),
        .duration = read_or(*transition, "duration", 0.0),
    };
  }

  const auto keyframes = j.find("keyframes");
  if (keyframes != j.end() && keyframes->is_object()) {
    for (std::size_t i = 0; i < kAnimPropCount; ++i) {
      const auto list = keyframes->find(kAnimPropNames[i]);
      if (list != keyframes->end()) c.keyframes[i] = read_keyframes(*list);
    }
  }

  const auto effects = j.find("effects");
  if (effects != j.end() && effects->is_array()) {
    for (const json& e : *effects) c.effects.push_back(read_clip_effect(e));
  }

  const auto audio_effects = j.find("audio_effects");
  if (audio_effects != j.end() && audio_effects->is_array()) {
    for (const json& e : *audio_effects) c.audio_effects.push_back(read_audio_effect(e));
  }

  return c;
}

Track read_track(const json& j) {
  Track t;
  t.id = read_or(j, "id", std::string{});
  t.kind = read_or(j, "kind", TrackKind::Video);
  t.label = read_or(j, "label", std::string{});
  // Unity and centred in every file written before there was a track mixer,
  // which is what those files meant.
  t.gain = read_or(j, "gain", 1.0);
  t.pan = read_or(j, "pan", 0.0);
  t.muted = read_or(j, "muted", false);
  t.solo = read_or(j, "solo", false);
  t.locked = read_or(j, "locked", false);
  t.hidden = read_or(j, "hidden", false);
  // On by default, which is what a file written before this existed means and
  // what Premiere does with a fresh track.
  t.targeted = read_or(j, "targeted", false);
  t.sync_locked = read_or(j, "sync_locked", true);
  t.height = read_optional<double>(j, "height");

  // Absent is a fader that was set and left, which is every project written
  // before automation existed.
  const auto track_gain_keys = j.find("gain_keyframes");
  if (track_gain_keys != j.end()) t.gain_keyframes = read_keyframes(*track_gain_keys);
  const auto track_pan_keys = j.find("pan_keyframes");
  if (track_pan_keys != j.end()) t.pan_keyframes = read_keyframes(*track_pan_keys);
  t.automation = read_or(j, "automation", AutomationMode::Read);

  const auto track_effects = j.find("audio_effects");
  if (track_effects != j.end() && track_effects->is_array()) {
    for (const json& e : *track_effects) t.audio_effects.push_back(read_audio_effect(e));
  }

  t.submix = read_or(j, "submix", false);
  t.output = read_or(j, "output", std::string{});
  const auto sends = j.find("sends");
  if (sends != j.end() && sends->is_array()) {
    for (const json& s : *sends) {
      if (!s.is_object()) continue;
      Send send;
      send.to = read_or(s, "to", std::string{});
      send.level = read_or(s, "level", 1.0);
      send.pre_fader = read_or(s, "pre_fader", false);
      // A send to nowhere is not a send. Read back rather than kept, because
      // the alternative is a row in the interface that names nothing.
      if (!send.to.empty()) t.sends.push_back(std::move(send));
    }
  }

  const auto clips = j.find("clips");
  if (clips != j.end() && clips->is_array()) {
    for (const json& c : *clips) t.clips.push_back(read_clip(c));
  }
  return t;
}

/// One sequence, from the shape `write(const Sequence&)` puts it in — which is
/// also the shape a whole project used to be, so this reads an old file's
/// top-level fields without knowing that is what it is doing.
Sequence read_sequence(const json& j) {
  Sequence s;
  s.id = read_or(j, "id", std::string{});
  s.name = read_or(j, "name", std::string{});
  s.canvas_w = read_or(j, "canvas_w", s.canvas_w);
  s.canvas_h = read_or(j, "canvas_h", s.canvas_h);
  s.fps = read_or(j, "fps", s.fps);
  s.master_gain = read_or(j, "master_gain", s.master_gain);
  s.drop_frame = read_or(j, "drop_frame", s.drop_frame);

  const auto master_keys = j.find("master_gain_keyframes");
  if (master_keys != j.end()) s.master_gain_keyframes = read_keyframes(*master_keys);
  s.master_automation = read_or(j, "master_automation", AutomationMode::Read);
  const auto master_effects = j.find("master_effects");
  if (master_effects != j.end() && master_effects->is_array()) {
    for (const json& e : *master_effects) s.master_effects.push_back(read_audio_effect(e));
  }

  const auto tracks = j.find("tracks");
  if (tracks != j.end() && tracks->is_array()) {
    for (const json& t : *tracks) s.tracks.push_back(read_track(t));
  }

  const auto markers = j.find("markers");
  if (markers != j.end() && markers->is_array()) {
    for (const json& m : *markers) {
      s.markers.push_back(Marker{
          .id = read_or(m, "id", std::string{}),
          .time = read_or(m, "time", 0.0),
          .label = read_or(m, "label", std::string{}),
          .color = read_or(m, "color", std::string{}),
          .duration = read_or(m, "duration", 0.0),
          .comment = read_or(m, "comment", std::string{}),
      });
    }
  }

  s.in_point = read_optional<double>(j, "in_point");
  s.out_point = read_optional<double>(j, "out_point");
  return s;
}

Project read_project(const json& j) {
  Project p;
  // The first sequence is the project's own top-level fields, which is where a
  // single-sequence project has always kept them.
  p.sequences = {read_sequence(j)};
  p.use_proxies = read_or(j, "use_proxies", p.use_proxies);

  const auto sequences = j.find("sequences");
  if (sequences != j.end() && sequences->is_array()) {
    for (const json& s : *sequences) p.sequences.push_back(read_sequence(s));
  }
  p.open = std::min(read_or(j, "open", std::size_t{0}), p.sequences.size() - 1);

  const auto media = j.find("media");
  if (media != j.end() && media->is_array()) {
    for (const json& m : *media) p.media.push_back(read_media(m));
  }

  const auto bins = j.find("bins");
  if (bins != j.end() && bins->is_array()) {
    for (const json& b : *bins) {
      p.bins.push_back(Bin{
          .id = read_or(b, "id", std::string{}),
          .name = read_or(b, "name", std::string{}),
          .parent = read_or(b, "parent", std::string{}),
      });
    }
  }

  return p;
}

/// Every generated name the project already uses, so the counter starts past
/// them. Group ids count: a paste remaps groups through freshly minted ones.
void note_ids(const Project& p) {
  for (const Media& m : p.media) note_id(m.id);
  for (const Bin& b : p.bins) note_id(b.id);
  for (const Marker& m : p.sequence().markers) note_id(m.id);
  for (const Track& t : p.sequence().tracks) {
    note_id(t.id);
    for (const Clip& c : t.clips) {
      note_id(c.id);
      if (c.group_id.has_value()) note_id(*c.group_id);
    }
  }
}

}  // namespace

std::string to_json(const Project& p, int indent) {
  const json document{{"version", kProjectSchemaVersion}, {"project", write(p)}};
  return document.dump(indent);
}

std::expected<LoadedProject, std::string> from_json(std::string_view text) {
  json document = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (document.is_discarded()) return std::unexpected("not valid JSON");
  if (!document.is_object()) return std::unexpected("expected a JSON object at the top level");

  const int version = read_or(document, "version", 0);
  if (version <= 0) return std::unexpected("missing or invalid schema version");
  if (version > kProjectSchemaVersion) {
    return std::unexpected(std::format(
        "project was written by a newer version of Cutline (schema {}, this build reads {})",
        version, kProjectSchemaVersion));
  }

  const auto body = document.find("project");
  if (body == document.end() || !body->is_object()) {
    return std::unexpected("document has no project");
  }

  LoadedProject loaded;
  try {
    loaded.project = read_project(*body);
  } catch (const json::exception& e) {
    return std::unexpected(std::format("malformed project: {}", e.what()));
  }

  // Every id the file already holds is spoken for. Done here rather than at
  // each call site, because a project that came in through some other door
  // would be exactly as dangerous and nobody would think to do it.
  note_ids(loaded.project);

  // Missing media are reported, never silently dropped: the clips that depend
  // on them stay put so the caller can offer a relink.
  for (const Media& m : loaded.project.media) {
    if (m.path.empty()) continue;  // generated media have no file
    std::error_code ec;
    if (!std::filesystem::exists(m.path, ec)) {
      loaded.warnings.push_back(std::format("media file not found: {}", m.path));
    }
  }

  return loaded;
}

std::string effects_to_json(const EffectStacks& stacks, int indent) {
  json out = json::object();

  json video = json::array();
  for (const ClipEffect& e : stacks.video) video.push_back(write(e));
  out["effects"] = std::move(video);

  json audio = json::array();
  for (const AudioClipEffect& e : stacks.audio) audio.push_back(write(e));
  out["audio_effects"] = std::move(audio);

  return out.dump(indent);
}

std::expected<EffectStacks, std::string> effects_from_json(std::string_view text) {
  const json parsed = json::parse(text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return std::unexpected("not a JSON object");
  }

  EffectStacks out;
  if (const auto video = parsed.find("effects"); video != parsed.end() && video->is_array()) {
    for (const json& e : *video) out.video.push_back(read_clip_effect(e));
  }
  if (const auto audio = parsed.find("audio_effects");
      audio != parsed.end() && audio->is_array()) {
    for (const json& e : *audio) out.audio.push_back(read_audio_effect(e));
  }
  return out;
}

}  // namespace cutline::core
