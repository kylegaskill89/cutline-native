#include "cutline/editor/graphic_templates.hpp"

#include "cutline/core/graphic.hpp"
#include "cutline/core/serialize.hpp"
#include "cutline/editor/graphics.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <ios>
#include <map>
#include <sstream>
#include <system_error>

namespace cutline::editor {
namespace {

using nlohmann::json;
using core::Graphic;
using core::GraphicKind;
using core::GraphicLayer;

constexpr int kSchemaVersion = 1;
constexpr std::string_view kExtension = ".cutgfx";
constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] std::expected<std::string, std::string> read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::unexpected("could not open " + path.string());
  std::ostringstream text;
  text << file.rdbuf();
  if (file.bad()) return std::unexpected("could not read " + path.string());
  return text.str();
}

/// A name for a picture by what is in it, so the same picture from two
/// templates is one file, and a changed picture never overwrites another.
[[nodiscard]] std::string content_name(std::string_view bytes) {
  std::uint64_t hash = 14695981039346656037ull;
  for (const char c : bytes) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }
  return std::format("{:016x}", hash);
}

/// Characters a Windows file name cannot have, which a template's name is.
[[nodiscard]] bool usable_name(std::string_view name) {
  if (name.empty() || name.find_first_of("\\/:*?\"<>|") != std::string_view::npos) return false;
  return name.find_first_not_of(" .") != std::string_view::npos;
}

[[nodiscard]] GraphicLayer text(std::string id, std::string words, double size, double x, double y) {
  GraphicLayer layer = text_layer(std::move(words));
  layer.id = std::move(id);
  layer.style.font_size = size;
  layer.x = x;
  layer.y = y;
  return layer;
}

}  // namespace

std::vector<GraphicTemplate> built_in_templates() {
  std::vector<GraphicTemplate> out;

  {
    // A name and a role at the bottom left, on a bar that grows with the name:
    // the bar is pinned round it.
    GraphicLayer name = text("name", "Jane Smith", 64.0, 0.08, 0.80);
    name.anchor_x = 0.0;
    name.style.align = core::TextAlign::Left;
    name.reveal_in = core::GraphicReveal{.kind = core::RevealKind::SlideLeft, .duration = 0.5};
    name.reveal_out = core::GraphicReveal{.kind = core::RevealKind::Fade, .duration = 0.4};
    GraphicLayer role = text("role", "Director", 40.0, 0.08, 0.875);
    role.anchor_x = 0.0;
    role.style.align = core::TextAlign::Left;
    role.style.bold = false;
    role.style.fill = "#dddddd";
    role.reveal_in = core::GraphicReveal{.kind = core::RevealKind::Fade, .duration = 0.8};
    role.reveal_out = core::GraphicReveal{.kind = core::RevealKind::Fade, .duration = 0.4};
    GraphicLayer bar = shape_layer(GraphicKind::Rectangle);
    bar.id = "bar";
    bar.name = "Bar";
    bar.style.fill = "#1b5fa8";
    bar.pin = core::GraphicPin{.to = "name", .left = true, .top = true, .right = true,
                               .bottom = true, .left_offset = -24.0, .top_offset = -10.0,
                               .right_offset = 24.0, .bottom_offset = 10.0};
    bar.reveal_in = core::GraphicReveal{.kind = core::RevealKind::Wipe, .duration = 0.4};
    bar.reveal_out = core::GraphicReveal{.kind = core::RevealKind::Fade, .duration = 0.4};
    out.push_back({.name = "Lower Third", .graphic = Graphic{.layers = {name, role, bar}}});
  }
  {
    GraphicLayer title = text("title", "Title", 140.0, 0.5, 0.45);
    title.reveal_in = core::GraphicReveal{.kind = core::RevealKind::Fade, .duration = 0.6};
    title.reveal_out = core::GraphicReveal{.kind = core::RevealKind::Fade, .duration = 0.6};
    GraphicLayer subtitle = text("subtitle", "Subtitle", 56.0, 0.5, 0.58);
    subtitle.style.bold = false;
    subtitle.reveal_in = core::GraphicReveal{.kind = core::RevealKind::Fade, .duration = 1.0};
    subtitle.reveal_out = core::GraphicReveal{.kind = core::RevealKind::Fade, .duration = 0.6};
    out.push_back({.name = "Title Card", .graphic = Graphic{.layers = {title, subtitle}}});
  }
  {
    GraphicLayer credits = text("credits",
                                "Directed by\nJane Smith\n\nEdited by\nJohn Smith\n\nMusic by\n"
                                "Sam Smith",
                                56.0, 0.5, 0.5);
    credits.style.leading = 10.0;
    credits.style.bold = false;
    Graphic graphic{.layers = {credits}};
    graphic.scroll.mode = core::ScrollMode::Roll;
    out.push_back({.name = "Credits Roll", .graphic = graphic, .length = 10.0});
  }
  {
    GraphicLayer bug = text("bug", "LIVE", 40.0, 0.95, 0.06);
    bug.anchor_x = 1.0;
    bug.anchor_y = 0.0;
    bug.style.background = core::GraphicBackground{.color = "#c8102e", .opacity = 1.0,
                                                   .padding = 12.0, .corner_radius = 6.0};
    out.push_back({.name = "Corner Bug", .graphic = Graphic{.layers = {bug}}});
  }
  {
    GraphicLayer caption = text("caption", "A caption at the foot of the frame", 48.0, 0.5, 0.88);
    caption.style.bold = false;
    caption.style.background = core::GraphicBackground{.color = "#000000", .opacity = 0.6,
                                                       .padding = 12.0};
    caption.reveal_in = core::GraphicReveal{.kind = core::RevealKind::Typewriter, .duration = 1.0};
    out.push_back({.name = "Caption", .graphic = Graphic{.layers = {caption}}});
  }
  return out;
}

std::filesystem::path default_templates_folder() {
  const char* roaming = std::getenv("APPDATA");
  const std::filesystem::path base =
      roaming == nullptr ? std::filesystem::path{"."} : std::filesystem::path{roaming};
  return base / "Cutline" / "Graphics Templates";
}

std::expected<std::filesystem::path, std::string> save_template(
    const std::filesystem::path& folder, const std::string& name, const Graphic& graphic,
    double length) {
  if (!usable_name(name)) {
    return std::unexpected("A template needs a name, without \\ / : * ? \" < > or |.");
  }

  // The pictures, carried inside. Keyed by layer, as each layer names its own.
  json pictures = json::object();
  for (const GraphicLayer& layer : graphic.layers) {
    if (layer.kind != GraphicKind::Image || layer.path.empty()) continue;
    auto bytes = read_file(layer.path);
    if (!bytes.has_value()) {
      return std::unexpected("The picture " + std::filesystem::path(layer.path).filename().string() +
                             " could not be read, so the template would be missing it.");
    }
    pictures[layer.id] = {{"name", std::filesystem::path(layer.path).filename().string()},
                          {"data", to_base64(*bytes)}};
  }

  json document{{"version", kSchemaVersion},
                {"name", name},
                {"length", length > 0.0 ? length : kDefaultTitleLength},
                {"graphic", json::parse(core::graphic_to_json(graphic))}};
  if (!pictures.empty()) document["pictures"] = std::move(pictures);

  std::error_code error;
  std::filesystem::create_directories(folder, error);
  if (error) return std::unexpected("could not make " + folder.string() + ": " + error.message());
  const std::filesystem::path path = folder / std::filesystem::path(name + std::string(kExtension));
  std::filesystem::path staging = path;
  staging += ".saving";
  {
    std::ofstream file(staging, std::ios::binary | std::ios::trunc);
    if (!file) return std::unexpected("could not write " + staging.string());
    file << document.dump(2);
    file.flush();
    if (!file) return std::unexpected("could not write " + staging.string());
  }
  std::filesystem::rename(staging, path, error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
    return std::unexpected("could not replace " + path.string() + ": " + error.message());
  }
  return path;
}

std::expected<GraphicTemplate, std::string> read_template(const std::filesystem::path& file) {
  auto text = read_file(file);
  if (!text.has_value()) return std::unexpected(text.error());
  const json document = json::parse(*text, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return std::unexpected(file.filename().string() + " is not a template");
  }
  if (document.value("version", 0) > kSchemaVersion) {
    return std::unexpected(file.filename().string() +
                           " was written by a newer version of Cutline");
  }
  const auto graphic_json = document.find("graphic");
  if (graphic_json == document.end() || !graphic_json->is_object()) {
    return std::unexpected(file.filename().string() + " has no graphic in it");
  }
  auto graphic = core::graphic_from_json(graphic_json->dump());
  if (!graphic.has_value()) return std::unexpected(graphic.error());

  GraphicTemplate made;
  made.name = document.value("name", file.stem().string());
  made.length = std::max(0.1, document.value("length", kDefaultTitleLength));
  made.file = file;
  made.graphic = std::move(*graphic);

  // The pictures, written out where the layers can be pointed at them.
  if (const auto pictures = document.find("pictures");
      pictures != document.end() && pictures->is_object()) {
    const std::filesystem::path store = file.parent_path() / ".pictures";
    for (GraphicLayer& layer : made.graphic.layers) {
      const auto found = pictures->find(layer.id);
      if (found == pictures->end() || !found->is_object()) continue;
      auto bytes = from_base64(found->value("data", std::string{}));
      if (!bytes.has_value()) continue;
      const std::string original = found->value("name", std::string{});
      const std::filesystem::path path =
          store / (content_name(*bytes) + std::filesystem::path(original).extension().string());
      std::error_code error;
      if (!std::filesystem::exists(path, error)) {
        std::filesystem::create_directories(store, error);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << *bytes;
      }
      layer.path = path.string();
    }
  }
  return made;
}

std::vector<GraphicTemplate> read_templates(const std::filesystem::path& folder) {
  std::vector<GraphicTemplate> out;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    if (!entry.is_regular_file() || entry.path().extension() != kExtension) continue;
    if (auto made = read_template(entry.path()); made.has_value()) {
      out.push_back(std::move(*made));
    }
  }
  std::ranges::sort(out, {}, &GraphicTemplate::name);
  return out;
}

bool delete_template(const std::filesystem::path& file) {
  std::error_code error;
  return std::filesystem::remove(file, error);
}

core::Project add_template_at(core::Project project, const GraphicTemplate& made, double at,
                              std::string* clip_id) {
  return add_graphic_at(std::move(project), made.graphic, at, {}, clip_id, made.length);
}

std::string to_base64(std::string_view bytes) {
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const std::uint32_t a = static_cast<unsigned char>(bytes[i]);
    const std::uint32_t b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
    const std::uint32_t c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
    const std::uint32_t n = (a << 16) | (b << 8) | c;
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back(i + 1 < bytes.size() ? kAlphabet[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < bytes.size() ? kAlphabet[n & 63] : '=');
  }
  return out;
}

std::expected<std::string, std::string> from_base64(std::string_view text) {
  std::array<int, 256> value{};
  value.fill(-1);
  for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
    value[static_cast<unsigned char>(kAlphabet[i])] = static_cast<int>(i);
  }
  std::string out;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char c : text) {
    if (c == '=') break;
    if (c == '\n' || c == '\r' || c == ' ') continue;
    const int v = value[static_cast<unsigned char>(c)];
    if (v < 0) return std::unexpected("not base64");
    buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
    }
  }
  return out;
}

}  // namespace cutline::editor
