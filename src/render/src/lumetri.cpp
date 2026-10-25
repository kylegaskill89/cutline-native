#include "cutline/render/lumetri.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>

namespace cutline::render {
namespace {

[[nodiscard]] double param(const core::ClipEffect& effect, std::string_view key,
                           double fallback) {
  const auto found = effect.params.find(std::string(key));
  return found == effect.params.end() ? fallback : found->second;
}

[[nodiscard]] bool on(const core::ClipEffect& effect, std::string_view key) {
  return param(effect, key, 1.0) >= 0.5;
}

[[nodiscard]] std::string_view trimmed(std::string_view text) {
  const std::size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) return {};
  const std::size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

/// Numbers separated by spaces, as many as there are.
[[nodiscard]] std::vector<float> numbers(std::string_view text) {
  std::vector<float> out;
  while (true) {
    text = trimmed(text);
    if (text.empty()) break;
    float value = 0.0f;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{}) return {};
    out.push_back(value);
    text.remove_prefix(static_cast<std::size_t>(end - text.data()));
  }
  return out;
}

/// The Rec.709 weights, which every white-balance gain is normalised against so
/// a warmer picture is not also a brighter one.
constexpr std::array<float, 3> kLuma{0.2126f, 0.7152f, 0.0722f};

/// Premiere's Temperature and Tint, -100 to 100, as three gains in linear
/// light. Temperature trades red against blue and tint green against magenta,
/// and the gains are scaled so the picture's luma stays where it was.
[[nodiscard]] std::array<float, 3> balance_gains(double temperature, double tint) {
  const double t = temperature / 100.0;
  const double n = tint / 100.0;
  std::array<float, 3> gains{static_cast<float>(std::exp2(0.35 * t + 0.1 * n)),
                             static_cast<float>(std::exp2(-0.2 * n)),
                             static_cast<float>(std::exp2(-0.35 * t + 0.1 * n))};
  const float luma = gains[0] * kLuma[0] + gains[1] * kLuma[1] + gains[2] * kLuma[2];
  for (float& gain : gains) gain /= luma;
  return gains;
}

void put(std::vector<float>& data, std::size_t at, std::array<float, 3> rgb) {
  std::copy(rgb.begin(), rgb.end(), data.begin() + static_cast<std::ptrdiff_t>(at));
}

[[nodiscard]] std::array<float, 3> scaled(std::array<float, 3> rgb, double by, double plus = 0.0) {
  for (float& c : rgb) c = static_cast<float>(c * by + plus);
  return rgb;
}

/// Appends a LUT to a pass's data, returning where it starts.
float append_lut(std::vector<float>& data, const Lut& lut) {
  const auto start = static_cast<float>(data.size());
  data.push_back(static_cast<float>(lut.size));
  data.push_back(lut.three_d ? 1.0f : 0.0f);
  data.insert(data.end(), lut.domain_min.begin(), lut.domain_min.end());
  data.insert(data.end(), lut.domain_max.begin(), lut.domain_max.end());
  data.insert(data.end(), lut.rgb.begin(), lut.rgb.end());
  return start;
}

/// The LUT a file names, or none: one that cannot be read leaves the grade
/// without it rather than costing the whole effect.
[[nodiscard]] std::shared_ptr<const Lut> lut_named(const core::ClipEffect& effect,
                                                   std::string_view key) {
  const auto file = effect.files.find(std::string(key));
  if (file == effect.files.end() || file->second.empty()) return nullptr;
  auto lut = load_lut(file->second);
  return lut.has_value() ? *lut : nullptr;
}

[[nodiscard]] bool curve_set(const core::ClipEffect& effect, std::string_view key) {
  const auto found = effect.curves.find(std::string(key));
  return found != effect.curves.end() && !found->second.empty();
}

}  // namespace

std::expected<Lut, std::string> parse_cube(std::string_view text) {
  Lut lut;
  bool sized = false;
  std::size_t line_number = 0;
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    const std::string_view line = trimmed(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    ++line_number;
    if (line.empty() || line.front() == '#') continue;

    const auto keyword = [&line](std::string_view name) {
      return line.starts_with(name) &&
             (line.size() == name.size() || line[name.size()] == ' ' || line[name.size()] == '\t');
    };
    if (keyword("TITLE")) {
      std::string_view title = trimmed(line.substr(5));
      if (title.size() >= 2 && title.front() == '"' && title.back() == '"') {
        title = title.substr(1, title.size() - 2);
      }
      lut.title = std::string(title);
    } else if (keyword("LUT_3D_SIZE") || keyword("LUT_1D_SIZE")) {
      const bool cube = keyword("LUT_3D_SIZE");
      const std::vector<float> size = numbers(line.substr(11));
      const int points = size.size() == 1 ? static_cast<int>(size[0]) : 0;
      // A cube larger than 256 a side would be most of a gigabyte, and no
      // grading application writes one.
      if (points < 2 || points > (cube ? 256 : 65536)) {
        return std::unexpected(std::format("line {}: a LUT size has to be 2 to {}", line_number,
                                           cube ? 256 : 65536));
      }
      lut.size = points;
      lut.three_d = cube;
      sized = true;
    } else if (keyword("DOMAIN_MIN") || keyword("DOMAIN_MAX")) {
      const std::vector<float> bound = numbers(line.substr(10));
      if (bound.size() != 3) {
        return std::unexpected(std::format("line {}: a domain is three numbers", line_number));
      }
      std::ranges::copy(bound, (keyword("DOMAIN_MIN") ? lut.domain_min : lut.domain_max).begin());
    } else if (std::isalpha(static_cast<unsigned char>(line.front())) != 0) {
      // Keywords other applications add, such as Resolve's LUT_IN_VIDEO_RANGE,
      // say nothing the table does not.
      continue;
    } else {
      const std::vector<float> entry = numbers(line);
      if (entry.size() != 3) {
        return std::unexpected(std::format("line {}: an entry is three numbers", line_number));
      }
      lut.rgb.insert(lut.rgb.end(), entry.begin(), entry.end());
    }
  }
  if (!sized) return std::unexpected("there is no LUT_3D_SIZE or LUT_1D_SIZE");
  const std::size_t wanted = lut.three_d ? static_cast<std::size_t>(lut.size) * lut.size * lut.size
                                         : static_cast<std::size_t>(lut.size);
  if (lut.rgb.size() != wanted * 3) {
    return std::unexpected(std::format("it should have {} entries and has {}", wanted,
                                       lut.rgb.size() / 3));
  }
  for (std::size_t i = 0; i < 3; ++i) {
    if (!(lut.domain_max[i] > lut.domain_min[i])) {
      return std::unexpected("its domain is empty");
    }
  }
  return lut;
}

std::expected<std::shared_ptr<const Lut>, std::string> load_lut(
    const std::filesystem::path& path) {
  struct Held {
    std::filesystem::file_time_type written;
    std::shared_ptr<const Lut> lut;
  };
  static std::mutex guard;
  static std::map<std::filesystem::path, Held> held;

  std::error_code error;
  const auto written = std::filesystem::last_write_time(path, error);
  if (error) return std::unexpected(path.filename().string() + " could not be found");
  {
    const std::scoped_lock lock(guard);
    if (const auto found = held.find(path);
        found != held.end() && found->second.written == written) {
      return found->second.lut;
    }
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) return std::unexpected("could not open " + path.filename().string());
  std::ostringstream text;
  text << file.rdbuf();
  auto lut = parse_cube(text.str());
  if (!lut.has_value()) {
    return std::unexpected(path.filename().string() + " is not a LUT Cutline can read: " +
                           lut.error());
  }
  auto shared = std::make_shared<const Lut>(std::move(*lut));
  const std::scoped_lock lock(guard);
  held[path] = Held{.written = written, .lut = shared};
  return shared;
}

std::vector<float> curve_table(std::span<const core::CurvePoint> given, CurveRest rest,
                               bool periodic) {
  std::vector<float> table(kCurveSamples);
  const auto resting = [rest](double x) { return rest == CurveRest::Diagonal ? x : 0.5; };

  std::vector<core::CurvePoint> points(given.begin(), given.end());
  std::ranges::sort(points, {}, &core::CurvePoint::x);
  if (points.empty()) {
    for (std::size_t i = 0; i < kCurveSamples; ++i) {
      table[i] = static_cast<float>(resting(static_cast<double>(i) / (kCurveSamples - 1)));
    }
    return table;
  }
  if (periodic) {
    // One turn either side, so the ends join up.
    const std::vector<core::CurvePoint> once = points;
    points.clear();
    for (const double shift : {-1.0, 0.0, 1.0}) {
      for (const core::CurvePoint& point : once) {
        points.push_back(core::CurvePoint{.x = point.x + shift, .y = point.y});
      }
    }
  } else {
    // Pinned at the ends where nothing says otherwise, so a point in the middle
    // bends the curve rather than tilting all of it.
    if (points.front().x > 0.0) points.insert(points.begin(), {.x = 0.0, .y = resting(0.0)});
    if (points.back().x < 1.0) points.push_back({.x = 1.0, .y = resting(1.0)});
  }

  // Fritsch and Carlson's monotone cubic: smooth through every point, and never
  // overshooting between two of them, which is what makes a curve's points
  // mean what they look like they mean.
  const std::size_t n = points.size();
  std::vector<double> slope(n, 0.0);
  std::vector<double> secant(n > 1 ? n - 1 : 0, 0.0);
  for (std::size_t i = 0; i + 1 < n; ++i) {
    const double dx = points[i + 1].x - points[i].x;
    secant[i] = dx > 0.0 ? (points[i + 1].y - points[i].y) / dx : 0.0;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (i == 0) {
      slope[i] = n > 1 ? secant[0] : 0.0;
    } else if (i + 1 == n) {
      slope[i] = secant[i - 1];
    } else if (secant[i - 1] * secant[i] <= 0.0) {
      slope[i] = 0.0;
    } else {
      slope[i] = (secant[i - 1] + secant[i]) / 2.0;
    }
  }
  for (std::size_t i = 0; i + 1 < n; ++i) {
    if (secant[i] == 0.0) {
      slope[i] = slope[i + 1] = 0.0;
      continue;
    }
    const double a = slope[i] / secant[i];
    const double b = slope[i + 1] / secant[i];
    const double length = std::hypot(a, b);
    if (length > 3.0) {
      slope[i] = 3.0 * a / length * secant[i];
      slope[i + 1] = 3.0 * b / length * secant[i];
    }
  }

  std::size_t segment = 0;
  for (std::size_t s = 0; s < kCurveSamples; ++s) {
    const double x = static_cast<double>(s) / (kCurveSamples - 1);
    double y = 0.0;
    if (n == 1 || x <= points.front().x) {
      y = points.front().y;
    } else if (x >= points.back().x) {
      y = points.back().y;
    } else {
      while (segment + 2 < n && points[segment + 1].x < x) ++segment;
      const core::CurvePoint& p0 = points[segment];
      const core::CurvePoint& p1 = points[segment + 1];
      const double h = p1.x - p0.x;
      const double u = h > 0.0 ? (x - p0.x) / h : 0.0;
      const double u2 = u * u;
      const double u3 = u2 * u;
      y = (2 * u3 - 3 * u2 + 1) * p0.y + (u3 - 2 * u2 + u) * h * slope[segment] +
          (-2 * u3 + 3 * u2) * p1.y + (u3 - u2) * h * slope[segment + 1];
    }
    table[s] = static_cast<float>(std::clamp(y, 0.0, 1.0));
  }
  return table;
}

std::array<float, 3> wheel_offset(double x, double y) noexcept {
  // Across the wheel is B-Y and up it is R-Y, as on a vectorscope, put back
  // into red, green and blue with Rec.709's matrix and no luma of its own.
  const double cb = std::clamp(x, -1.0, 1.0) * 0.5;
  const double cr = std::clamp(y, -1.0, 1.0) * 0.5;
  return {static_cast<float>(1.5748 * cr), static_cast<float>(-0.1873 * cb - 0.4681 * cr),
          static_cast<float>(1.8556 * cb)};
}

void plan_lumetri(const core::ClipEffect& effect, std::vector<EffectPass>& passes) {
  const auto value = [&effect](std::string_view key, double fallback = 0.0) {
    return param(effect, key, fallback);
  };
  const auto wheel = [&value](std::string_view prefix) {
    return wheel_offset(value(std::string(prefix) + "_x") / 100.0,
                        value(std::string(prefix) + "_y") / 100.0);
  };

  // What each section has to do, worked out first so a neutral one adds
  // nothing and a wholly neutral grade adds no pass at all.
  const std::shared_ptr<const Lut> input = on(effect, "basic") ? lut_named(effect, "input_lut")
                                                               : nullptr;
  const std::shared_ptr<const Lut> look = on(effect, "creative") ? lut_named(effect, "look")
                                                                 : nullptr;
  constexpr std::array kBasicKeys{"temperature", "tint", "exposure", "contrast", "highlights",
                                  "shadows",     "whites", "blacks", "hdr_specular"};
  const bool basic = on(effect, "basic") &&
                     (input != nullptr || value("saturation", 100.0) != 100.0 ||
                      std::ranges::any_of(kBasicKeys, [&](const char* key) {
                        return value(key) != 0.0;
                      }));
  constexpr std::array kCreativeKeys{"faded_film",     "vibrance",       "shadow_tint_x",
                                     "shadow_tint_y",  "highlight_tint_x", "highlight_tint_y"};
  const bool creative = on(effect, "creative") &&
                        ((look != nullptr && value("intensity", 100.0) != 0.0) ||
                         value("creative_saturation", 100.0) != 100.0 ||
                         std::ranges::any_of(kCreativeKeys, [&](const char* key) {
                           return value(key) != 0.0;
                         }));
  const double sharpen = on(effect, "creative") ? value("sharpen") : 0.0;
  const bool curves = on(effect, "curves") && std::ranges::any_of(kLumetriCurveKeys, [&](auto key) {
                        return curve_set(effect, key);
                      });
  constexpr std::array kWheelKeys{"shadows_x",   "shadows_y",   "shadows_luma",
                                  "midtones_x",  "midtones_y",  "midtones_luma",
                                  "highlights_x", "highlights_y", "highlights_luma"};
  const bool wheels = on(effect, "wheels") && std::ranges::any_of(kWheelKeys, [&](auto key) {
                        return value(key) != 0.0;
                      });

  // One grade pass for the given sections, carrying only what they read.
  const auto grade = [&](int sections) {
    EffectPass pass;
    pass.kind = EffectPassKind::Lumetri;
    pass.values = {static_cast<float>(sections), -1.0f, -1.0f, -1.0f, 0, 0, 0, 0};
    std::vector<float>& data = pass.data;
    data.assign(kLumetriSettings, 0.0f);

    put(data, kWhiteBalance, balance_gains(value("temperature"), value("tint")));
    data[kExposure] = static_cast<float>(std::exp2(value("exposure")));
    data[kContrast] = static_cast<float>(std::exp2(value("contrast") / 100.0));
    data[kHighlights] = static_cast<float>(value("highlights") / 100.0);
    data[kShadows] = static_cast<float>(value("shadows") / 100.0);
    data[kWhites] = static_cast<float>(value("whites") / 100.0);
    data[kBlacks] = static_cast<float>(value("blacks") / 100.0);
    data[kSaturation] = static_cast<float>(value("saturation", 100.0) / 100.0);
    data[kSpecular] = static_cast<float>(1.0 + value("hdr_specular") / 100.0);
    data[kLookIntensity] = static_cast<float>(value("intensity", 100.0) / 100.0);
    data[kFadedFilm] = static_cast<float>(value("faded_film") / 100.0);
    data[kVibrance] = static_cast<float>(value("vibrance") / 100.0);
    data[kCreativeSaturation] = static_cast<float>(value("creative_saturation", 100.0) / 100.0);
    put(data, kShadowTint, scaled(wheel("shadow_tint"), 0.3));
    put(data, kHighlightTint, scaled(wheel("highlight_tint"), 0.3));
    data[kTintBalance] = static_cast<float>(value("tint_balance") / 100.0);
    put(data, kLift, scaled(wheel("shadows"), 0.25, value("shadows_luma") / 100.0 * 0.25));
    put(data, kGamma, scaled(wheel("midtones"), 0.5, value("midtones_luma") / 100.0 * 0.5));
    put(data, kGain, scaled(wheel("highlights"), 0.5, value("highlights_luma") / 100.0 * 0.5));

    if ((sections & kLumetriBasic) != 0 && input != nullptr) {
      pass.values[1] = append_lut(data, *input);
    }
    if ((sections & kLumetriCreative) != 0 && look != nullptr) {
      pass.values[2] = append_lut(data, *look);
    }
    if ((sections & kLumetriCurves) != 0) {
      pass.values[3] = static_cast<float>(data.size());
      // Whether the RGB curves and the hue curves have anything to do, so the
      // shader can pass over whichever is straight.
      const auto any = [&effect](std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to; ++i) {
          if (curve_set(effect, kLumetriCurveKeys[i])) return 1.0f;
        }
        return 0.0f;
      };
      pass.values[4] = any(0, 4);
      pass.values[5] = any(4, kLumetriCurveKeys.size());
      for (std::size_t i = 0; i < kLumetriCurveKeys.size(); ++i) {
        const std::string_view key = kLumetriCurveKeys[i];
        const auto found = effect.curves.find(std::string(key));
        const std::span<const core::CurvePoint> points =
            found == effect.curves.end() ? std::span<const core::CurvePoint>{} : found->second;
        // The RGB curves and Sat vs Sat rest on the diagonal; the others are
        // changes, at rest in the middle. The hue curves go round.
        const bool diagonal = i < 4 || key == "sat_sat";
        const bool periodic = key.starts_with("hue_");
        const std::vector<float> table = curve_table(
            points, diagonal ? CurveRest::Diagonal : CurveRest::Middle, periodic);
        data.insert(data.end(), table.begin(), table.end());
      }
    }
    passes.push_back(std::move(pass));
  };

  const int first = (basic ? kLumetriBasic : 0) | (creative ? kLumetriCreative : 0);
  const int second = (curves ? kLumetriCurves : 0) | (wheels ? kLumetriWheels : 0);
  if (sharpen == 0.0) {
    if ((first | second) != 0) grade(first | second);
  } else {
    // Creative's sharpen comes after the look and before the curves, as in
    // Premiere, so the grade is split round it. Softening is the same slider
    // taken below nought.
    if (first != 0) grade(first);
    passes.push_back(sharpen > 0.0 ? sharpen_pass(static_cast<float>(sharpen / 100.0 * 1.5), 1.0f)
                                   : blur_pass(static_cast<float>(-sharpen / 100.0 * 1.5)));
    if (second != 0) grade(second);
  }

  // HSL Secondary.
  constexpr std::array kCorrectionKeys{"sec_x",        "sec_y",    "sec_luma",
                                       "sec_temperature", "sec_tint", "sec_contrast",
                                       "sec_sharpen"};
  const bool show_mask = value("show_mask") >= 0.5;
  if (on(effect, "secondary") &&
      (show_mask || value("sec_saturation", 100.0) != 100.0 ||
       std::ranges::any_of(kCorrectionKeys, [&](auto key) { return value(key) != 0.0; }))) {
    EffectPass pass;
    pass.kind = EffectPassKind::LumetriSecondary;
    std::vector<float>& data = pass.data;
    data.assign(kSecondarySettings, 0.0f);

    // A hue range from low to high going round, so 330 to 30 is the reds.
    const double low = std::clamp(value("key_hue_lo"), 0.0, 360.0);
    const double high = std::clamp(value("key_hue_hi", 360.0), 0.0, 360.0);
    const double width = high >= low ? high - low : 360.0 - low + high;
    data[kKeyHue] = static_cast<float>(std::fmod(low + width / 2.0, 360.0) / 360.0);
    data[kKeyHue + 1] = static_cast<float>(width >= 360.0 ? 1.0 : width / 720.0);
    data[kKeyHue + 2] = static_cast<float>(value("key_hue_soft") / 360.0);
    const auto range = [&](std::size_t at, std::string_view name) {
      const std::string key = std::string("key_") + std::string(name);
      data[at] = static_cast<float>(value(key + "_lo") / 100.0);
      data[at + 1] = static_cast<float>(value(key + "_hi", 100.0) / 100.0);
      data[at + 2] = static_cast<float>(value(key + "_soft") / 100.0);
    };
    range(kKeySaturation, "sat");
    range(kKeyLuma, "luma");
    data[kDenoise] = static_cast<float>(value("denoise") / 100.0 * 4.0);
    data[kKeyBlur] = static_cast<float>(value("blur") / 100.0 * 16.0);
    data[kKeyInvert] = value("invert_key") >= 0.5 ? 1.0f : 0.0f;
    data[kShowMask] = show_mask ? 1.0f : 0.0f;
    put(data, kSecondaryOffset,
        scaled(wheel("sec"), 0.3, value("sec_luma") / 100.0 * 0.3));
    put(data, kSecondaryBalance, balance_gains(value("sec_temperature"), value("sec_tint")));
    data[kSecondaryContrast] = static_cast<float>(std::exp2(value("sec_contrast") / 100.0));
    data[kSecondarySaturation] = static_cast<float>(value("sec_saturation", 100.0) / 100.0);
    data[kSecondarySharpen] = static_cast<float>(value("sec_sharpen") / 100.0 * 1.5);
    passes.push_back(std::move(pass));
  }

  // Vignette.
  const double amount = value("vig_amount");
  if (on(effect, "vignette") && amount != 0.0) {
    EffectPass pass;
    pass.kind = EffectPassKind::LumetriVignette;
    pass.values = {static_cast<float>(std::clamp(amount, -5.0, 5.0) / 5.0),
                   static_cast<float>(value("vig_midpoint", 50.0) / 100.0),
                   static_cast<float>(value("vig_roundness") / 100.0),
                   static_cast<float>(value("vig_feather", 50.0) / 100.0),
                   0, 0, 0, 0};
    passes.push_back(std::move(pass));
  }
}

}  // namespace cutline::render
