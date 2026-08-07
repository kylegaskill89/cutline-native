#include "cutline/app/media_cache.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <fstream>
#include <ios>
#include <system_error>
#include <vector>

namespace cutline::app {
namespace {

/// Bumped when the shape of a stored file changes. A file that does not carry
/// the current one is a miss rather than a misread — which is the whole reason
/// to write a version at all, since everything here is regenerable and the
/// cheapest possible upgrade is to ignore what came before.
constexpr std::uint32_t kFormat = 1;
constexpr std::uint32_t kWaveMagic = 0x564157'01u;   // "WAV" 1
constexpr std::uint32_t kStripMagic = 0x535452'01u;  // "STR" 1

/// A ceiling on what a stored file may claim to hold.
///
/// Everything read here is a length taken straight out of a file, and a corrupt
/// or truncated one can say anything. Believing it means allocating whatever it
/// says — which is the one way a cache miss can take the application down
/// rather than merely costing a decode.
constexpr std::size_t kMostBuckets = 100u * 60u * 60u * 24u;  // a day of audio
constexpr std::size_t kMostPixels = 8192u * 8192u * 4u;
constexpr std::uint32_t kMostFrames = 4096u;

[[nodiscard]] std::string digest_of(std::string_view text) {
  constexpr std::uint64_t kOffset = 14695981039346656037ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  std::uint64_t hash = kOffset;
  for (const char c : text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= kPrime;
  }
  return std::format("{:016x}", hash);
}

template <typename T>
bool read_pod(std::istream& in, T& value) {
  in.read(reinterpret_cast<char*>(&value), sizeof(T));
  return static_cast<bool>(in);
}

template <typename T>
void write_pod(std::ostream& out, const T& value) {
  out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

/// The directory one source's entries live in. A level of nesting by the first
/// two characters, so a cache of a few thousand sources does not put a few
/// thousand directories in one place.
[[nodiscard]] std::filesystem::path source_dir(const std::filesystem::path& dir,
                                               std::string_view key) {
  if (key.size() < 2) return dir / std::string(key);
  return dir / std::string(key.substr(0, 2)) / std::string(key);
}

/// What one stretch's file is called. The range, the number of frames and the
/// height are all in it, because a stretch stored for one of them is not the
/// answer for another — and a name that says so needs no header check to know.
[[nodiscard]] std::string strip_name(const CachedStrip& what) {
  return std::format("strip-{}-{:.3f}-{:.3f}-{}.bin", what.height, what.from, what.to,
                     what.count);
}

/// Writes through a staging file, so nothing ever reads a half-written entry.
/// The same shape the projects, the workspaces and the settings are written
/// with, and here it also means a crash mid-write costs one entry rather than
/// leaving one that decodes into nonsense.
[[nodiscard]] bool commit(const std::filesystem::path& target,
                          const std::function<void(std::ostream&)>& body) {
  std::error_code error;
  std::filesystem::create_directories(target.parent_path(), error);
  if (error) return false;

  std::filesystem::path staging = target;
  staging += ".writing";
  {
    std::ofstream out(staging, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    body(out);
    out.flush();
    if (!out) {
      std::error_code ignored;
      std::filesystem::remove(staging, ignored);
      return false;
    }
  }
  std::filesystem::rename(staging, target, error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
    return false;
  }
  return true;
}

}  // namespace

std::filesystem::path default_media_cache_dir() {
  const char* local = std::getenv("LOCALAPPDATA");
  const std::filesystem::path base =
      local == nullptr ? std::filesystem::path{"."} : std::filesystem::path{local};
  return base / "Cutline" / "media-cache";
}

std::filesystem::path media_cache_dir(std::string_view configured) {
  if (configured.empty()) return default_media_cache_dir();
  return std::filesystem::path{configured};
}

std::string media_cache_key(const std::filesystem::path& source) {
  if (source.empty()) return {};

  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(source, error);
  if (error) return {};
  const std::filesystem::file_time_type written =
      std::filesystem::last_write_time(source, error);
  if (error) return {};

  // The path is part of the key as well as the size and the time, because two
  // different files of the same length written in the same second are not far
  // fetched at all — a card of clips off one camera is exactly that.
  return digest_of(std::format("{}|{}|{}", source.generic_string(), size,
                               written.time_since_epoch().count()));
}

// ------------------------------------------------------------- waveforms --

std::optional<ui::Waveform> read_cached_waveform(const std::filesystem::path& dir,
                                                 std::string_view key, int stream) {
  if (key.empty()) return std::nullopt;

  std::ifstream in(source_dir(dir, key) / std::format("wave-{}.bin", stream), std::ios::binary);
  if (!in) return std::nullopt;

  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  double buckets = 0.0;
  std::uint64_t count = 0;
  if (!read_pod(in, magic) || !read_pod(in, version) || !read_pod(in, buckets) ||
      !read_pod(in, count)) {
    return std::nullopt;
  }
  if (magic != kWaveMagic || version != kFormat) return std::nullopt;
  if (count > kMostBuckets) return std::nullopt;
  if (!(buckets > 0.0) || !std::isfinite(buckets)) return std::nullopt;

  ui::Waveform wave;
  wave.buckets_per_second = buckets;
  wave.minimum.resize(static_cast<std::size_t>(count));
  wave.maximum.resize(static_cast<std::size_t>(count));
  const auto bytes = static_cast<std::streamsize>(count * sizeof(float));
  in.read(reinterpret_cast<char*>(wave.minimum.data()), bytes);
  in.read(reinterpret_cast<char*>(wave.maximum.data()), bytes);
  // Truncated is a miss. Handing back half an envelope would draw a waveform
  // that stops in the middle of a clip, which reads as damaged footage.
  if (!in) return std::nullopt;
  return wave;
}

void write_cached_waveform(const std::filesystem::path& dir, std::string_view key, int stream,
                           const ui::Waveform& wave) {
  if (key.empty() || wave.empty()) return;
  const std::size_t count = wave.size();

  (void)commit(source_dir(dir, key) / std::format("wave-{}.bin", stream),
               [&](std::ostream& out) {
                 write_pod(out, kWaveMagic);
                 write_pod(out, kFormat);
                 write_pod(out, wave.buckets_per_second);
                 write_pod(out, static_cast<std::uint64_t>(count));
                 out.write(reinterpret_cast<const char*>(wave.minimum.data()),
                           static_cast<std::streamsize>(count * sizeof(float)));
                 out.write(reinterpret_cast<const char*>(wave.maximum.data()),
                           static_cast<std::streamsize>(count * sizeof(float)));
               });
}

// ------------------------------------------------------------ filmstrips --

std::optional<std::vector<ui::FilmFrame>> read_cached_strip(const std::filesystem::path& dir,
                                                            std::string_view key,
                                                            const CachedStrip& what) {
  if (key.empty()) return std::nullopt;

  std::ifstream in(source_dir(dir, key) / strip_name(what), std::ios::binary);
  if (!in) return std::nullopt;

  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t frames = 0;
  if (!read_pod(in, magic) || !read_pod(in, version) || !read_pod(in, frames)) {
    return std::nullopt;
  }
  if (magic != kStripMagic || version != kFormat) return std::nullopt;
  if (frames > kMostFrames) return std::nullopt;

  std::vector<ui::FilmFrame> out;
  out.reserve(frames);
  for (std::uint32_t i = 0; i < frames; ++i) {
    double t = 0.0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    if (!read_pod(in, t) || !read_pod(in, width) || !read_pod(in, height)) return std::nullopt;
    if (width <= 0 || height <= 0) return std::nullopt;

    const auto pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    if (pixels > kMostPixels) return std::nullopt;

    ui::FilmFrame frame;
    frame.t = t;
    frame.width = width;
    frame.height = height;
    frame.rgba.resize(pixels);
    in.read(reinterpret_cast<char*>(frame.rgba.data()), static_cast<std::streamsize>(pixels));
    // Truncated part-way through is a miss for the whole stretch. Half a
    // filmstrip is worse than none: it draws a clip that turns black in the
    // middle, which reads as damaged footage.
    if (!in) return std::nullopt;
    out.push_back(std::move(frame));
  }
  return out;
}

void write_cached_strip(const std::filesystem::path& dir, std::string_view key,
                        const CachedStrip& what, const std::vector<ui::FilmFrame>& frames) {
  if (key.empty() || frames.empty()) return;

  (void)commit(source_dir(dir, key) / strip_name(what), [&](std::ostream& out) {
    write_pod(out, kStripMagic);
    write_pod(out, kFormat);
    write_pod(out, static_cast<std::uint32_t>(frames.size()));
    for (const ui::FilmFrame& frame : frames) {
      write_pod(out, frame.t);
      write_pod(out, static_cast<std::int32_t>(frame.width));
      write_pod(out, static_cast<std::int32_t>(frame.height));
      out.write(reinterpret_cast<const char*>(frame.rgba.data()),
                static_cast<std::streamsize>(frame.rgba.size()));
    }
  });
}

// ---------------------------------------------------------- housekeeping --

std::uintmax_t media_cache_bytes(const std::filesystem::path& dir) {
  std::error_code error;
  if (!std::filesystem::exists(dir, error) || error) return 0;

  std::uintmax_t total = 0;
  std::filesystem::recursive_directory_iterator walk(
      dir, std::filesystem::directory_options::skip_permission_denied, error);
  if (error) return 0;
  for (const std::filesystem::directory_entry& entry : walk) {
    std::error_code ignored;
    if (entry.is_regular_file(ignored)) total += entry.file_size(ignored);
  }
  return total;
}

std::uintmax_t clear_media_cache(const std::filesystem::path& dir) {
  const std::uintmax_t was = media_cache_bytes(dir);
  std::error_code ignored;
  // The contents rather than the directory, so a folder somebody chose stays
  // chosen and stays there — and so this can never be talked into removing
  // something that is not ours by a path that has since become one.
  std::filesystem::directory_iterator entries(dir, ignored);
  if (ignored) return 0;
  for (const std::filesystem::directory_entry& entry : entries) {
    std::error_code error;
    std::filesystem::remove_all(entry.path(), error);
  }
  return was;
}

}  // namespace cutline::app
