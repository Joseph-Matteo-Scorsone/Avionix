module;

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <vector>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 16384
#include <stb_image.h>

export module avionix.object.image_file;

import avionix.entity.image;
import avionix.entity.error;

// PNG and JPEG decoding into image_rgba8, through the vendored stb_image
// (libs/stb). No stb type leaves this module.
//
//   bytes ──decode_image──► image_rgba8 (top-down RGBA rows)
//   path  ──load_image────► read file ──► decode_image
//
// Limits: 16384 pixels per side and 256 MiB of decoded pixels. Inputs over
// those fail instead of allocating. Decoding runs on the calling thread;
// large images take tens of milliseconds, so load on a worker and post the
// result to the UI thread when that matters.

export namespace avionix {

inline constexpr std::size_t max_image_bytes = std::size_t{256} * 1024 * 1024;

[[nodiscard]] inline std::expected<image_rgba8, error> decode_image(
    std::span<const std::uint8_t> bytes) {
  const auto fail = [](std::string why) {
    return std::unexpected(
        error{"decode image", failure::invalid_argument, std::move(why), 0});
  };
  if (bytes.empty()) return fail("no data");
  if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return fail("data too large");
  const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
  const int length = static_cast<int>(bytes.size());
  int w = 0;
  int h = 0;
  int channels = 0;
  if (stbi_info_from_memory(data, length, &w, &h, &channels) == 0)
    return fail(std::format("not a PNG or JPEG image ({})", stbi_failure_reason()));
  if (static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4 > max_image_bytes)
    return fail(std::format("{}x{} pixels exceeds the 256 MiB decode limit", w, h));
  stbi_uc* pixels = stbi_load_from_memory(data, length, &w, &h, &channels, 4);
  if (pixels == nullptr)
    return fail(std::format("decoding failed ({})", stbi_failure_reason()));
  image_rgba8 out{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), {}};
  out.pixels.assign(
      pixels, pixels + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
  stbi_image_free(pixels);
  return out;
}

[[nodiscard]] inline std::expected<image_rgba8, error> load_image(
    const std::filesystem::path& path) {
  std::ifstream in{path, std::ios::binary};
  if (!in) {
    return std::unexpected(error{"load image", failure::io_failed,
                                 std::format("cannot open {}", path.string()), 0});
  }
  std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{in},
                                  std::istreambuf_iterator<char>{}};
  auto image = decode_image(bytes);
  if (!image)
    image.error().context = std::format("{}: {}", path.string(), image.error().context);
  return image;
}

}  // namespace avionix
