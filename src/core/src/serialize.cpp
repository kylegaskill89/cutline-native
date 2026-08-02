#include "cutline/core/serialize.hpp"

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

NLOHMANN_JSON_SERIALIZE_ENUM(Interp, {
                                         {Interp::Linear, "linear"},
                                         {Interp::Hold, "hold"},
                                         {Interp::Ease, "ease"},
                                     })

namespace {

/// Keyframe lists are stored under property names rather than array positions,
/// so reordering the AnimProp enumerators cannot silently reinterpret a file.
constexpr std::array<const char*, kAnimPropCount> kAnimPropNames{
    "x", "y", "scale_x", "scale_y", "rotation", "opacity", "anchor_x", "anchor_y", "pan",
};

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
          {"anchor_y", t.anchor_y}};
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

json write(const ClipEffect& e) {
  json j{{"type", e.type}, {"enabled", e.enabled}};
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
         {"transform", write(c.transform)}};
  put_if_set(j, "group_id", c.group_id);
  put_unless_empty(j, "gain_keyframes", write(c.gain_keyframes));

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
         {"muted", t.muted},
         {"solo", t.solo},
         {"locked", t.locked},
         {"hidden", t.hidden}};
  if (!t.label.empty()) j["label"] = t.label;
  put_if_set(j, "height", t.height);

  json clips = json::array();
  for (const Clip& c : t.clips) clips.push_back(write(c));
  put_unless_empty(j, "clips", clips);
  return j;
}

json write(const Marker& m) {
  json j{{"id", m.id}, {"time", m.time}};
  if (!m.label.empty()) j["label"] = m.label;
  if (!m.color.empty()) j["color"] = m.color;
  return j;
}

json write(const Project& p) {
  json j{{"canvas_w", p.canvas_w},
         {"canvas_h", p.canvas_h},
         {"fps", p.fps},
         {"master_gain", p.master_gain}};

  json media = json::array();
  for (const Media& m : p.media) media.push_back(write(m));
  put_unless_empty(j, "media", media);

  json tracks = json::array();
  for (const Track& t : p.tracks) tracks.push_back(write(t));
  put_unless_empty(j, "tracks", tracks);

  json markers = json::array();
  for (const Marker& m : p.markers) markers.push_back(write(m));
  put_unless_empty(j, "markers", markers);

  put_if_set(j, "in_point", p.in_point);
  put_if_set(j, "out_point", p.out_point);

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
    out.push_back(Keyframe{
        .t = read_or(k, "t", 0.0),
        .v = read_or(k, "v", 0.0),
        .e = read_or(k, "e", Interp::Linear),
    });
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

  const auto transform = j.find("transform");
  if (transform != j.end() && transform->is_object()) c.transform = read_transform(*transform);

  const auto gain_keyframes = j.find("gain_keyframes");
  if (gain_keyframes != j.end()) c.gain_keyframes = read_keyframes(*gain_keyframes);

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
  t.muted = read_or(j, "muted", false);
  t.solo = read_or(j, "solo", false);
  t.locked = read_or(j, "locked", false);
  t.hidden = read_or(j, "hidden", false);
  t.height = read_optional<double>(j, "height");

  const auto clips = j.find("clips");
  if (clips != j.end() && clips->is_array()) {
    for (const json& c : *clips) t.clips.push_back(read_clip(c));
  }
  return t;
}

Project read_project(const json& j) {
  Project p;
  p.canvas_w = read_or(j, "canvas_w", p.canvas_w);
  p.canvas_h = read_or(j, "canvas_h", p.canvas_h);
  p.fps = read_or(j, "fps", p.fps);
  p.master_gain = read_or(j, "master_gain", p.master_gain);

  const auto media = j.find("media");
  if (media != j.end() && media->is_array()) {
    for (const json& m : *media) p.media.push_back(read_media(m));
  }

  const auto tracks = j.find("tracks");
  if (tracks != j.end() && tracks->is_array()) {
    for (const json& t : *tracks) p.tracks.push_back(read_track(t));
  }

  const auto markers = j.find("markers");
  if (markers != j.end() && markers->is_array()) {
    for (const json& m : *markers) {
      p.markers.push_back(Marker{
          .id = read_or(m, "id", std::string{}),
          .time = read_or(m, "time", 0.0),
          .label = read_or(m, "label", std::string{}),
          .color = read_or(m, "color", std::string{}),
      });
    }
  }

  p.in_point = read_optional<double>(j, "in_point");
  p.out_point = read_optional<double>(j, "out_point");
  return p;
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

}  // namespace cutline::core
