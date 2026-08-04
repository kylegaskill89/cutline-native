/// Renders one frame of a project file to a PNG.
///
/// The first thing that exercises the whole chain in one command: load a
/// project, plan the frame, decode what it names, composite, and write the
/// result out. That makes the pipeline checkable by eye and by script long
/// before there is an editor, and it is most of what export will do per frame.
///
///     render_frame <project.json> <time-in-seconds> <out.png> [frames]
///
/// With a frame count it renders that many in sequence and reports what they
/// cost, which is the measurement every decision about the pipeline rests on.

#include "cutline/core/serialize.hpp"
#include "cutline/engine/frame_renderer.hpp"
#include "cutline/gpu/device.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <vector>

namespace {

/// A minimal PNG writer: one uncompressed zlib block per image.
///
/// Deflate's "stored" mode means no compression code and no dependency, at the
/// cost of a file slightly larger than the raw pixels. For a debug tool that
/// writes one frame at a time, the trade is obviously worth it; an exporter
/// writing thousands of frames would not make the same call.
class PngWriter {
 public:
  [[nodiscard]] static bool write(const std::string& path, int width, int height,
                                  const std::vector<std::uint8_t>& rgba) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    static constexpr std::uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.write(reinterpret_cast<const char*>(signature), sizeof(signature));

    std::vector<std::uint8_t> header;
    append_be32(header, static_cast<std::uint32_t>(width));
    append_be32(header, static_cast<std::uint32_t>(height));
    header.push_back(8);  // bits per channel
    header.push_back(6);  // colour type: RGBA
    header.push_back(0);  // deflate
    header.push_back(0);  // no filtering
    header.push_back(0);  // no interlacing
    write_chunk(out, "IHDR", header);

    // Each scanline is prefixed with its filter type, which is always zero
    // here: filtering only pays off once the data is actually compressed.
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (1 + width * 4));
    for (int y = 0; y < height; ++y) {
      raw.push_back(0);
      const std::size_t row = static_cast<std::size_t>(y) * width * 4;
      raw.insert(raw.end(), rgba.begin() + row, rgba.begin() + row + width * 4);
    }

    write_chunk(out, "IDAT", zlib_stored(raw));
    write_chunk(out, "IEND", {});
    return out.good();
  }

 private:
  static void append_be32(std::vector<std::uint8_t>& to, std::uint32_t value) {
    to.push_back(static_cast<std::uint8_t>(value >> 24));
    to.push_back(static_cast<std::uint8_t>(value >> 16));
    to.push_back(static_cast<std::uint8_t>(value >> 8));
    to.push_back(static_cast<std::uint8_t>(value));
  }

  [[nodiscard]] static std::uint32_t crc32(const std::uint8_t* data, std::size_t length,
                                           std::uint32_t crc = 0xFFFFFFFFu) {
    static const auto table = [] {
      std::array<std::uint32_t, 256> t{};
      for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
      }
      return t;
    }();

    for (std::size_t i = 0; i < length; ++i) {
      crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
  }

  /// Adler-32, which is what a zlib stream checksums with.
  [[nodiscard]] static std::uint32_t adler32(const std::vector<std::uint8_t>& data) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t byte : data) {
      a = (a + byte) % 65521;
      b = (b + a) % 65521;
    }
    return (b << 16) | a;
  }

  /// Wraps the data in a zlib stream of uncompressed deflate blocks. Each block
  /// carries a 16-bit length, so anything larger is split.
  [[nodiscard]] static std::vector<std::uint8_t> zlib_stored(
      const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out;
    out.push_back(0x78);  // deflate, 32K window
    out.push_back(0x01);  // no preset dictionary, fastest

    constexpr std::size_t block = 65535;
    for (std::size_t offset = 0; offset < data.size() || offset == 0; offset += block) {
      const std::size_t length = std::min(block, data.size() - offset);
      const bool last = offset + length >= data.size();

      out.push_back(last ? 1 : 0);
      out.push_back(static_cast<std::uint8_t>(length & 0xFF));
      out.push_back(static_cast<std::uint8_t>(length >> 8));
      out.push_back(static_cast<std::uint8_t>(~length & 0xFF));
      out.push_back(static_cast<std::uint8_t>((~length >> 8) & 0xFF));
      out.insert(out.end(), data.begin() + offset, data.begin() + offset + length);
      if (last) break;
    }

    append_be32(out, adler32(data));
    return out;
  }

  static void write_chunk(std::ostream& out, const char (&type)[5],
                          const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> length;
    append_be32(length, static_cast<std::uint32_t>(data.size()));
    out.write(reinterpret_cast<const char*>(length.data()), 4);
    out.write(type, 4);
    if (!data.empty()) out.write(reinterpret_cast<const char*>(data.data()), data.size());

    std::uint32_t crc = crc32(reinterpret_cast<const std::uint8_t*>(type), 4);
    if (!data.empty()) crc = crc32(data.data(), data.size(), crc);
    crc ^= 0xFFFFFFFFu;

    std::vector<std::uint8_t> checksum;
    append_be32(checksum, crc);
    out.write(reinterpret_cast<const char*>(checksum.data()), 4);
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::println(stderr, "usage: render_frame <project.json> <seconds> <out.png>");
    return 2;
  }

  std::ifstream in(argv[1]);
  if (!in) {
    std::println(stderr, "cannot open {}", argv[1]);
    return 1;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();

  const auto loaded = cutline::core::from_json(buffer.str());
  if (!loaded) {
    std::println(stderr, "cannot read the project: {}", loaded.error());
    return 1;
  }
  for (const std::string& warning : loaded->warnings) {
    std::println(stderr, "warning: {}", warning);
  }

  const double time = std::strtod(argv[2], nullptr);
  const cutline::core::Project& project = loaded->project;

  auto device = cutline::gpu::Device::create({.allow_software = true});
  if (!device) {
    std::println(stderr, "cannot create a device: {}", device.error());
    return 1;
  }

  auto renderer =
      cutline::engine::FrameRenderer::create(*device, project.canvas_w, project.canvas_h);
  if (!renderer) {
    std::println(stderr, "cannot create the renderer: {}", renderer.error());
    return 1;
  }

  // Optionally the same thing over and over, forwards, which is how export and
  // playback walk time. One frame says the chain works; a run of them says what
  // it costs — and the cost is the whole reason for choosing a decoder, a
  // preview scale or a compositor path, so it needs measuring rather than
  // guessing at.
  const int frames = argc > 4 ? std::max(1, std::atoi(argv[4])) : 1;
  const double step = project.fps > 0.0 ? 1.0 / project.fps : 1.0 / 30.0;

  // The first frame is not like the others: it opens the file, builds the
  // decoder and warms every cache there is. Timing it with the rest would
  // report a throughput nothing sustains.
  if (auto ok = (*renderer)->render(project, time); !ok) {
    std::println(stderr, "render failed: {}", ok.error());
    return 1;
  }

  if (frames > 1) {
    const auto started = std::chrono::steady_clock::now();
    for (int i = 1; i < frames; ++i) {
      if (auto ok = (*renderer)->render(project, time + step * i); !ok) {
        std::println(stderr, "render failed at frame {}: {}", i, ok.error());
        return 1;
      }
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const int timed = frames - 1;
    std::println("{} frames in {:.2f}s   {:.1f} fps   {:.2f} ms/frame", timed, seconds,
                 seconds > 0.0 ? timed / seconds : 0.0,
                 timed > 0 ? seconds * 1000.0 / timed : 0.0);
  }
  for (const std::string& id : (*renderer)->missing_media()) {
    std::println(stderr, "missing media: {}", id);
  }

  const auto image = (*renderer)->read_back();
  if (!image) {
    std::println(stderr, "readback failed: {}", image.error());
    return 1;
  }

  if (!PngWriter::write(argv[3], image->width, image->height, image->pixels)) {
    std::println(stderr, "cannot write {}", argv[3]);
    return 1;
  }

  std::println("{}x{} at {:.3f}s -> {}  ({})", image->width, image->height, time, argv[3],
               (*device)->adapter_name());
  return 0;
}
