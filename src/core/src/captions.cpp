#include "cutline/core/captions.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/id.hpp"
#include "cutline/core/query.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <optional>

namespace cutline::core {
namespace {

Track* find_track(Project& p, std::string_view id) {
  const auto found = std::ranges::find(p.sequence().tracks, id, &Track::id);
  return found == p.sequence().tracks.end() ? nullptr : &*found;
}

Clip make_caption(double start, double length, std::string text) {
  Clip c;
  c.id = new_id("clip");
  c.kind = TrackKind::Caption;
  c.start = start;
  c.source_in = 0.0;
  c.source_out = length;
  c.caption = std::move(text);
  return c;
}

/// "HH:MM:SS,mmm" as SubRip writes it, into seconds. The comma may be a full
/// stop, the hours may be left out, and the fraction may have any number of
/// digits.
std::optional<double> srt_time(std::string_view text) {
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  // Anything after the time on its line -- some files put positions there.
  if (const std::size_t gap = text.find(' '); gap != std::string_view::npos) {
    text = text.substr(0, gap);
  }
  double fraction = 0.0;
  if (const std::size_t mark = text.find_first_of(",."); mark != std::string_view::npos) {
    double scale = 0.1;
    for (const char c : text.substr(mark + 1)) {
      if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
      fraction += (c - '0') * scale;
      scale /= 10.0;
    }
    text = text.substr(0, mark);
  }
  double seconds = 0.0;
  int parts = 0;
  while (!text.empty()) {
    const std::size_t colon = text.find(':');
    const std::string_view piece = text.substr(0, colon);
    int value = 0;
    const auto [end, ec] = std::from_chars(piece.data(), piece.data() + piece.size(), value);
    if (ec != std::errc{} || end != piece.data() + piece.size()) return std::nullopt;
    seconds = seconds * 60.0 + value;
    ++parts;
    if (colon == std::string_view::npos) break;
    text.remove_prefix(colon + 1);
  }
  if (parts < 2 || parts > 3) return std::nullopt;
  return seconds + fraction;
}

std::string srt_stamp(double seconds) {
  const auto total = static_cast<long long>(std::llround(std::max(0.0, seconds) * 1000.0));
  return std::format("{:02}:{:02}:{:02},{:03}", total / 3600000, total / 60000 % 60,
                     total / 1000 % 60, total % 1000);
}

/// The text without SubRip's tags: <b>, <i>, <u>, <font ...> and their ends,
/// and the {\an8}-style positioning some files carry.
std::string without_tags(std::string_view text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '<') {
      const std::size_t close = text.find('>', i);
      if (close != std::string_view::npos) {
        i = close;
        continue;
      }
    }
    if (text[i] == '{' && i + 1 < text.size() && text[i + 1] == '\\') {
      const std::size_t close = text.find('}', i);
      if (close != std::string_view::npos) {
        i = close;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

}  // namespace

bool is_caption_track(const Track& track) noexcept { return track.kind == TrackKind::Caption; }

Project add_caption_track(Project p, std::string* made_id) {
  Track track;
  track.id = new_id("track");
  track.kind = TrackKind::Caption;
  if (made_id != nullptr) *made_id = track.id;
  // Above the topmost caption track already there, which is above everything.
  p.sequence().tracks.insert(p.sequence().tracks.begin(), std::move(track));
  return p;
}

Project add_caption(Project p, std::string_view track_id, double start, std::string text,
                    double length, std::string* made_id) {
  Track* track = find_track(p, track_id);
  if (track == nullptr || !is_caption_track(*track)) return p;
  start = std::max(0.0, start);
  double end = start + std::max(length, 0.0);
  for (const Clip& clip : track->clips) {
    if (clip.start <= start + 1e-9 && start < clip_end(clip) - 1e-9) return p;
    if (clip.start > start) end = std::min(end, clip.start);
  }
  if (!(end > start)) return p;
  Clip caption = make_caption(start, end - start, std::move(text));
  if (made_id != nullptr) *made_id = caption.id;
  track->clips.push_back(std::move(caption));
  std::ranges::stable_sort(track->clips, {}, &Clip::start);
  return p;
}

Project set_caption_text(Project p, std::string_view clip_id, std::string text) {
  Clip* clip = find_clip(p, clip_id);
  if (clip == nullptr || clip->kind != TrackKind::Caption) return p;
  clip->caption = std::move(text);
  return p;
}

Project set_caption_style(Project p, std::string_view track_id, CaptionStyle style) {
  Track* track = find_track(p, track_id);
  if (track == nullptr || !is_caption_track(*track)) return p;
  track->caption_style = std::move(style);
  return p;
}

Project split_caption(Project p, std::string_view clip_id) {
  const Clip* clip = find_clip(p, clip_id);
  if (clip == nullptr || clip->kind != TrackKind::Caption) return p;
  const double middle = clip->start + clip_duration(*clip) / 2.0;
  const std::string text = clip->caption;
  const std::vector<std::string> one{std::string(clip_id)};
  p = split_at(std::move(p), middle, one);
  // Both halves keep the words; dividing them is for whoever split it.
  if (Track* track = track_of_clip(p, clip_id); track != nullptr) {
    for (Clip& piece : track->clips) {
      if (std::abs(piece.start - middle) < 1e-9) piece.caption = text;
    }
  }
  return p;
}

Project merge_captions(Project p, std::span<const std::string> clip_ids) {
  if (clip_ids.size() < 2) return p;
  Track* track = track_of_clip(p, clip_ids.front());
  if (track == nullptr || !is_caption_track(*track)) return p;
  std::vector<const Clip*> chosen;
  for (const Clip& clip : track->clips) {
    if (std::ranges::find(clip_ids, clip.id) != clip_ids.end()) chosen.push_back(&clip);
  }
  if (chosen.size() < 2) return p;
  std::string text;
  double first = chosen.front()->start;
  double last = clip_end(*chosen.front());
  for (const Clip* clip : chosen) {
    if (!text.empty() && !clip->caption.empty()) text += ' ';
    text += clip->caption;
    first = std::min(first, clip->start);
    last = std::max(last, clip_end(*clip));
  }
  const std::string keep = chosen.front()->id;
  std::erase_if(track->clips, [&](const Clip& clip) {
    return clip.id != keep && std::ranges::find(clip_ids, clip.id) != clip_ids.end();
  });
  // Anything else that sat in the gaps between them goes too: the merged
  // caption covers the whole span.
  std::erase_if(track->clips, [&](const Clip& clip) {
    return clip.id != keep && clip.start >= first - 1e-9 && clip_end(clip) <= last + 1e-9;
  });
  Clip* merged = find_clip(p, keep);
  merged->start = first;
  merged->source_in = 0.0;
  merged->source_out = (last - first) * clip_speed(*merged);
  merged->caption = std::move(text);
  return p;
}

const Clip* caption_at(const Sequence& s, double t) noexcept {
  for (const Track& track : s.tracks) {
    if (!is_caption_track(track) || track.hidden) continue;
    for (const Clip& clip : track.clips) {
      if (clip.disabled) continue;
      if (clip.start <= t + 1e-9 && t < clip_end(clip) - 1e-9) return &clip;
    }
  }
  return nullptr;
}

Graphic caption_graphic(const CaptionStyle& style, std::string_view text, int canvas_w,
                        int canvas_h) {
  (void)canvas_h;
  GraphicLayer layer;
  layer.id = "caption";
  layer.name = "Caption";
  layer.kind = GraphicKind::Text;
  layer.text = std::string(text);
  layer.box_width = std::max(1.0, style.width * canvas_w);
  layer.style = style.look;
  layer.x = style.x;
  layer.y = style.y;
  // Standing on its foot, so a second line grows upwards.
  layer.anchor_x = 0.5;
  layer.anchor_y = 1.0;
  Graphic graphic;
  graphic.layers.push_back(std::move(layer));
  return graphic;
}

// --------------------------------------------------------------------- SRT --

std::expected<std::vector<CaptionCue>, std::string> parse_srt(std::string_view text) {
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  std::vector<std::string> lines;
  std::string line;
  for (const char c : text) {
    if (c == '\r') continue;
    if (c == '\n') {
      lines.push_back(std::move(line));
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  lines.push_back(std::move(line));

  std::vector<CaptionCue> cues;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::size_t arrow = lines[i].find("-->");
    if (arrow == std::string::npos) continue;
    const auto from = srt_time(std::string_view(lines[i]).substr(0, arrow));
    const auto to = srt_time(std::string_view(lines[i]).substr(arrow + 3));
    if (!from.has_value() || !to.has_value()) continue;
    std::string body;
    std::size_t j = i + 1;
    for (; j < lines.size(); ++j) {
      if (lines[j].empty()) break;
      // The next cue's timing, where a file left out the blank line.
      if (lines[j].find("-->") != std::string::npos) break;
      if (!body.empty()) body += '\n';
      body += without_tags(lines[j]);
    }
    // A trailing index line belongs to the next cue rather than to this one.
    if (j < lines.size() && lines[j].find("-->") != std::string::npos) {
      const std::size_t cut = body.find_last_of('\n');
      const std::string tail = cut == std::string::npos ? body : body.substr(cut + 1);
      if (!tail.empty() && std::ranges::all_of(tail, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        body = cut == std::string::npos ? std::string{} : body.substr(0, cut);
      }
    }
    cues.push_back(CaptionCue{.start = *from, .end = std::max(*from, *to), .text = std::move(body)});
    i = j > i ? j - 1 : i;
  }
  if (cues.empty()) return std::unexpected(std::string("There are no captions in it"));
  return cues;
}

std::string write_srt(const Track& track, bool styled) {
  std::vector<const Clip*> captions;
  for (const Clip& clip : track.clips) {
    if (!clip.disabled) captions.push_back(&clip);
  }
  std::ranges::stable_sort(captions, {}, [](const Clip* c) { return c->start; });
  const GraphicStyle& look = track.caption_style.look;
  std::string out;
  int index = 1;
  for (const Clip* clip : captions) {
    std::string body = clip->caption;
    if (styled) {
      if (look.italic) body = "<i>" + body + "</i>";
      if (look.bold) body = "<b>" + body + "</b>";
      if (look.fill != "#ffffff" && !look.fill.empty()) {
        body = std::format("<font color=\"{}\">{}</font>", look.fill, body);
      }
    }
    out += std::format("{}\r\n{} --> {}\r\n{}\r\n\r\n", index++, srt_stamp(clip->start),
                       srt_stamp(clip_end(*clip)), body);
  }
  return out;
}

std::optional<std::string> sidecar_srt(const Sequence& s, double from, double to, bool styled) {
  const auto track = std::ranges::find_if(
      s.tracks, [](const Track& t) { return is_caption_track(t) && !t.hidden; });
  if (track == s.tracks.end()) return std::nullopt;
  // The track as the export sees it: what falls inside the span, cut to it,
  // and moved so the span starts at zero.
  Track span = *track;
  span.clips.clear();
  for (const Clip& clip : track->clips) {
    const double a = std::max(clip.start, from);
    const double b = std::min(clip_end(clip), to);
    if (!(b > a)) continue;
    Clip cut = clip;
    cut.start = a - from;
    cut.source_in = 0.0;
    cut.source_out = (b - a) * clip_speed(clip);
    span.clips.push_back(std::move(cut));
  }
  return write_srt(span, styled);
}

Project import_captions(Project p, std::span<const CaptionCue> cues, double offset,
                        std::string* made_id) {
  std::string track_id;
  p = add_caption_track(std::move(p), &track_id);
  if (made_id != nullptr) *made_id = track_id;
  Track* track = find_track(p, track_id);
  for (const CaptionCue& cue : cues) {
    const double start = std::max(0.0, cue.start + offset);
    const double end = cue.end + offset;
    if (!(end > start)) continue;
    track->clips.push_back(make_caption(start, end - start, cue.text));
  }
  std::ranges::stable_sort(track->clips, {}, &Clip::start);
  // Overlapping cues, which some files have, are cut back to the next one's
  // start: a track shows one caption at a time.
  for (std::size_t i = 0; i + 1 < track->clips.size(); ++i) {
    Clip& clip = track->clips[i];
    const double next = track->clips[i + 1].start;
    if (clip_end(clip) > next) clip.source_out = clip.source_in + std::max(0.0, next - clip.start);
  }
  std::erase_if(track->clips, [](const Clip& clip) { return !(clip_duration(clip) > 0.0); });
  return p;
}

}  // namespace cutline::core
