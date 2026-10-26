#include "cutline/editor/lumetri.hpp"

#include "cutline/core/effects.hpp"
#include "cutline/core/query.hpp"
#include "cutline/editor/effects_binding.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <system_error>

namespace cutline::editor {
namespace {

using Rgb = std::array<double, 3>;

constexpr Rgb kLuma{0.2126, 0.7152, 0.0722};

[[nodiscard]] double luma_of(const Rgb& c) {
  return c[0] * kLuma[0] + c[1] * kLuma[1] + c[2] * kLuma[2];
}

[[nodiscard]] double linear_of(double coded) {
  return coded <= 0.04045 ? coded / 12.92 : std::pow((coded + 0.055) / 1.055, 2.4);
}

[[nodiscard]] double coded_of(double linear) {
  return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

/// Hue in degrees, saturation and value, from coded values 0 to 1.
[[nodiscard]] Rgb hsv_of(const Rgb& c) {
  const double high = std::max({c[0], c[1], c[2]});
  const double low = std::min({c[0], c[1], c[2]});
  const double span = high - low;
  double hue = 0.0;
  if (span > 0.0) {
    if (high == c[0]) {
      hue = 60.0 * std::fmod((c[1] - c[2]) / span, 6.0);
    } else if (high == c[1]) {
      hue = 60.0 * ((c[2] - c[0]) / span + 2.0);
    } else {
      hue = 60.0 * ((c[0] - c[1]) / span + 4.0);
    }
  }
  if (hue < 0.0) hue += 360.0;
  return {hue, high > 0.0 ? span / high : 0.0, high};
}

/// The same S as the grade's contrast: black, middle and white stay put.
[[nodiscard]] double s_curve(double x, double k) {
  x = std::clamp(x, 0.0, 1.0);
  return x < 0.5 ? 0.5 * std::pow(2.0 * x, k) : 1.0 - 0.5 * std::pow(2.0 - 2.0 * x, k);
}

[[nodiscard]] Rgb saturate_by(const Rgb& c, double amount) {
  const double l = luma_of(c);
  return {l + (c[0] - l) * amount, l + (c[1] - l) * amount, l + (c[2] - l) * amount};
}

struct Look {
  const char* name;
  std::function<Rgb(Rgb)> grade;
};

/// Cutline's own looks, each a few lines of the same arithmetic the grade
/// uses, so what each does can be read here.
[[nodiscard]] const std::vector<Look>& looks() {
  static const std::vector<Look> all{
      {"Warm Film",
       [](Rgb c) {
         c = saturate_by(c, 0.9);
         for (double& v : c) v = 0.035 + s_curve(v, 1.1) * 0.95;
         return Rgb{c[0] * 1.05, c[1] * 1.0, c[2] * 0.88};
       }},
      {"Teal and Orange",
       [](Rgb c) {
         const double l = std::clamp(luma_of(c), 0.0, 1.0);
         const Rgb shadow{-0.06, 0.02, 0.05};
         const Rgb highlight{0.07, 0.015, -0.07};
         for (std::size_t i = 0; i < 3; ++i) c[i] += shadow[i] * (1.0 - l) + highlight[i] * l;
         return saturate_by(c, 1.1);
       }},
      {"Bleach Bypass",
       [](Rgb c) {
         c = saturate_by(c, 0.45);
         for (double& v : c) v = s_curve(v, 1.5);
         return c;
       }},
      {"Cool Night",
       [](Rgb c) {
         c = saturate_by(c, 0.8);
         return Rgb{c[0] * 0.78, c[1] * 0.86, c[2] * 1.0};
       }},
      {"Faded Matte",
       [](Rgb c) {
         c = saturate_by(c, 0.85);
         for (double& v : c) v = 0.08 + v * 0.86;
         return c;
       }},
      {"Monochrome",
       [](Rgb c) {
         const double l = s_curve(luma_of(c), 1.25);
         return Rgb{l, l, l};
       }},
      {"Vivid",
       [](Rgb c) {
         c = saturate_by(c, 1.35);
         for (double& v : c) v = s_curve(v, 1.2);
         return c;
       }},
      {"Sepia",
       [](Rgb c) {
         const double l = luma_of(c);
         return Rgb{0.03 + l * 1.05, 0.02 + l * 0.9, l * 0.7};
       }},
  };
  return all;
}

}  // namespace

std::optional<std::size_t> lumetri_on(const core::Project& project, std::string_view clip_id) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return std::nullopt;
  for (std::size_t i = 0; i < clip->effects.size(); ++i) {
    if (clip->effects[i].type == "lumetri") return i;
  }
  return std::nullopt;
}

core::Project with_lumetri(core::Project project, std::string_view clip_id, std::size_t* index) {
  std::optional<std::size_t> at = lumetri_on(project, clip_id);
  if (!at.has_value()) {
    project = add_effect(std::move(project), clip_id, "lumetri");
    at = lumetri_on(project, clip_id);
  }
  if (index != nullptr && at.has_value()) *index = *at;
  return project;
}

core::Project set_lumetri_param(core::Project project, std::string_view clip_id,
                                std::string_view key, double value, double local_t) {
  std::size_t index = 0;
  project = with_lumetri(std::move(project), clip_id, &index);
  if (!lumetri_on(project, clip_id).has_value()) return project;
  return set_effect_parameter(std::move(project), clip_id, index, key, value, local_t);
}

core::Project set_lumetri_params(core::Project project, std::string_view clip_id,
                                 const std::vector<std::pair<std::string, double>>& values,
                                 double local_t) {
  for (const auto& [key, value] : values) {
    project = set_lumetri_param(std::move(project), clip_id, key, value, local_t);
  }
  return project;
}

core::Project set_lumetri_curve(core::Project project, std::string_view clip_id,
                                std::string_view key, std::vector<core::CurvePoint> points) {
  std::size_t index = 0;
  project = with_lumetri(std::move(project), clip_id, &index);
  if (!lumetri_on(project, clip_id).has_value()) return project;
  return core::set_clip_effect_curve(std::move(project), clip_id, index, std::string(key),
                                     std::move(points));
}

core::Project set_lumetri_file(core::Project project, std::string_view clip_id,
                               std::string_view key, std::string path) {
  std::size_t index = 0;
  project = with_lumetri(std::move(project), clip_id, &index);
  if (!lumetri_on(project, clip_id).has_value()) return project;
  return core::set_clip_effect_file(std::move(project), clip_id, index, std::string(key),
                                    std::move(path));
}

core::Project without_lumetri(core::Project project, std::string_view clip_id,
                              std::vector<std::string> sections) {
  const std::optional<std::size_t> index = lumetri_on(project, clip_id);
  if (!index.has_value()) return project;
  if (sections.empty()) {
    if (core::Clip* clip = core::find_clip(project, clip_id); clip != nullptr) {
      clip->effects[*index].enabled = false;
    }
    return project;
  }
  for (std::string& section : sections) {
    project = core::set_clip_effect_param(std::move(project), clip_id, *index,
                                          std::move(section), 0.0);
  }
  return project;
}

std::pair<double, double> white_balance_for(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  // The gains that make the picked light grey are one over it. The grade's
  // temperature trades red against blue at 0.35 of a stop per unit each way,
  // and its tint green against the other two at 0.3, so those are undone.
  const double red = std::max(linear_of(r / 255.0), 1e-4);
  const double green = std::max(linear_of(g / 255.0), 1e-4);
  const double blue = std::max(linear_of(b / 255.0), 1e-4);
  const double temperature = std::log2(blue / red) / 0.7;
  const double tint = -std::log2(std::sqrt(red * blue) / green) / 0.3;
  return {std::clamp(temperature * 100.0, -100.0, 100.0), std::clamp(tint * 100.0, -100.0, 100.0)};
}

std::vector<std::pair<std::string, double>> key_for(std::uint8_t r, std::uint8_t g,
                                                    std::uint8_t b) {
  const Rgb coded{r / 255.0, g / 255.0, b / 255.0};
  const Rgb hsv = hsv_of(coded);
  const double luma = luma_of(coded);
  std::vector<std::pair<std::string, double>> out;
  // A grey has no hue to key on, so it keys every hue and lets saturation and
  // luma do the choosing.
  if (hsv[1] < 0.08) {
    out = {{"key_hue_lo", 0.0}, {"key_hue_hi", 360.0}, {"key_hue_soft", 0.0}};
  } else {
    out = {{"key_hue_lo", std::fmod(hsv[0] + 340.0, 360.0)},
           {"key_hue_hi", std::fmod(hsv[0] + 20.0, 360.0)},
           {"key_hue_soft", 15.0}};
  }
  const auto around = [&out](std::string_view name, double value, double reach) {
    const std::string key = std::string("key_") + std::string(name);
    out.emplace_back(key + "_lo", std::max(0.0, value - reach) * 100.0);
    out.emplace_back(key + "_hi", std::min(1.0, value + reach) * 100.0);
    out.emplace_back(key + "_soft", 10.0);
  };
  around("sat", hsv[1], 0.25);
  around("luma", luma, 0.3);
  return out;
}

std::vector<std::pair<std::string, double>> auto_tone(const std::uint8_t* pixels, int width,
                                                      int height) {
  std::array<std::size_t, 256> counts{};
  std::size_t total = 0;
  for (int i = 0; i < width * height; ++i) {
    const std::uint8_t* p = pixels + static_cast<std::ptrdiff_t>(i) * 4;
    const double l = luma_of(Rgb{p[0] / 255.0, p[1] / 255.0, p[2] / 255.0});
    ++counts[static_cast<std::size_t>(std::clamp(std::lround(l * 255.0), 0L, 255L))];
    ++total;
  }
  if (total == 0) return {};
  const auto percentile = [&](double fraction) {
    const auto wanted = static_cast<std::size_t>(fraction * static_cast<double>(total));
    std::size_t seen = 0;
    for (std::size_t level = 0; level < counts.size(); ++level) {
      seen += counts[level];
      if (seen > wanted) return static_cast<double>(level) / 255.0;
    }
    return 1.0;
  };
  const double low = percentile(0.005);
  const double middle = std::max(percentile(0.5), 0.02);
  const double high = percentile(0.995);

  // The middle brought to a little under half, by a change of light.
  const double stops =
      std::clamp(std::log2(linear_of(0.46) / std::max(linear_of(middle), 1e-4)), -2.0, 2.0);
  const auto exposed = [stops](double coded) {
    return std::clamp(coded_of(linear_of(coded) * std::exp2(stops)), 0.0, 1.0);
  };
  // Then the ends spread to just inside black and white, by the same
  // arithmetic the grade's Whites and Blacks use.
  const double top = std::max(exposed(high), 0.05);
  const double bottom = std::min(exposed(low), 0.95);
  const double whites = std::clamp((0.95 - top) / (0.25 * top * top), -1.0, 1.0);
  const double blacks =
      std::clamp((0.03 - bottom) / (0.25 * (1.0 - bottom) * (1.0 - bottom)), -1.0, 1.0);
  return {{"exposure", std::round(stops * 100.0) / 100.0},
          {"contrast", 0.0},
          {"highlights", 0.0},
          {"shadows", 0.0},
          {"whites", std::round(whites * 100.0)},
          {"blacks", std::round(blacks * 100.0)}};
}

std::filesystem::path default_looks_folder() {
  const char* roaming = std::getenv("APPDATA");
  const std::filesystem::path base =
      roaming == nullptr ? std::filesystem::path{"."} : std::filesystem::path{roaming};
  return base / "Cutline" / "Looks";
}

std::vector<std::string> built_in_look_names() {
  std::vector<std::string> out;
  for (const Look& look : looks()) out.emplace_back(look.name);
  return out;
}

void write_built_in_looks(const std::filesystem::path& folder) {
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  if (error) return;
  constexpr int kSize = 33;
  for (const Look& look : looks()) {
    const std::filesystem::path path = folder / (std::string(look.name) + ".cube");
    if (std::filesystem::exists(path, error)) continue;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) continue;
    file << std::format("TITLE \"{}\"\n# Made by Cutline.\nLUT_3D_SIZE {}\n", look.name, kSize);
    for (int b = 0; b < kSize; ++b) {
      for (int g = 0; g < kSize; ++g) {
        for (int r = 0; r < kSize; ++r) {
          const Rgb in{r / double(kSize - 1), g / double(kSize - 1), b / double(kSize - 1)};
          const Rgb out = look.grade(in);
          file << std::format("{:.6f} {:.6f} {:.6f}\n", std::clamp(out[0], 0.0, 1.0),
                              std::clamp(out[1], 0.0, 1.0), std::clamp(out[2], 0.0, 1.0));
        }
      }
    }
  }
}

std::vector<std::filesystem::path> looks_in(const std::filesystem::path& folder) {
  std::vector<std::filesystem::path> out;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    if (!entry.is_regular_file()) continue;
    std::string extension = entry.path().extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".cube") out.push_back(entry.path());
  }
  std::ranges::sort(out, {}, [](const std::filesystem::path& p) { return p.stem().string(); });
  return out;
}

}  // namespace cutline::editor
