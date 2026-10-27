module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

export module avionix.entity.image;

// An 8-bit RGBA image and a PNG encoder. Pure: the encoder returns bytes and
// the caller decides where they go.

export namespace avionix {

// Output size in physical pixels (framebuffer pixels, not window points).
struct pixel_extent {
  std::uint32_t width{};
  std::uint32_t height{};

  friend constexpr bool operator==(const pixel_extent&,
                                   const pixel_extent&) noexcept = default;
  [[nodiscard]] constexpr bool empty() const noexcept {
    return width == 0 || height == 0;
  }
  [[nodiscard]] constexpr double aspect() const noexcept {
    return empty() ? 1.0 : static_cast<double>(width) / static_cast<double>(height);
  }
};

// Rows are stored top to bottom, four bytes per pixel, no padding.
struct image_rgba8 {
  std::uint32_t width{};
  std::uint32_t height{};
  std::vector<std::uint8_t> pixels;

  [[nodiscard]] std::array<std::uint8_t, 4> at(std::uint32_t x, std::uint32_t y) const {
    const std::size_t i = (std::size_t{y} * width + x) * 4;
    return {pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]};
  }
};

namespace image_detail {

constexpr std::array<std::uint32_t, 256> crc_table = [] {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t n = 0; n < 256; ++n) {
    std::uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = (c & 1U) ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
    table[n] = c;
  }
  return table;
}();

inline void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v >> 24U));
  out.push_back(static_cast<std::uint8_t>(v >> 16U));
  out.push_back(static_cast<std::uint8_t>(v >> 8U));
  out.push_back(static_cast<std::uint8_t>(v));
}

inline void chunk(std::vector<std::uint8_t>& out, std::string_view type,
                  std::span<const std::uint8_t> data) {
  put32(out, static_cast<std::uint32_t>(data.size()));
  const std::size_t start = out.size();
  for (const char c : type) out.push_back(static_cast<std::uint8_t>(c));
  out.insert(out.end(), data.begin(), data.end());
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = start; i < out.size(); ++i)
    crc = crc_table[(crc ^ out[i]) & 0xFFU] ^ (crc >> 8U);
  put32(out, crc ^ 0xFFFFFFFFU);
}

}  // namespace image_detail

// Encodes `image` as a PNG with uncompressed (stored) deflate blocks. The
// output is about the size of the raw pixels; it favors a small, obviously
// correct encoder over compression ratio.
[[nodiscard]] inline std::vector<std::uint8_t> encode_png(const image_rgba8& image) {
  using namespace image_detail;
  std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

  std::vector<std::uint8_t> header;
  put32(header, image.width);
  put32(header, image.height);
  header.insert(header.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, no interlace
  chunk(out, "IHDR", header);

  // Scanlines with filter type 0.
  const std::size_t stride = std::size_t{image.width} * 4;
  std::vector<std::uint8_t> raw;
  raw.reserve((stride + 1) * image.height);
  for (std::uint32_t y = 0; y < image.height; ++y) {
    raw.push_back(0);
    const auto row = std::span{image.pixels}.subspan(y * stride, stride);
    raw.insert(raw.end(), row.begin(), row.end());
  }

  std::vector<std::uint8_t> z{0x78, 0x01};
  std::size_t offset = 0;
  do {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - offset);
    const bool last = offset + n == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<std::uint8_t>(n));
    z.push_back(static_cast<std::uint8_t>(n >> 8U));
    z.push_back(static_cast<std::uint8_t>(~n));
    z.push_back(static_cast<std::uint8_t>(~n >> 8U));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
             raw.begin() + static_cast<std::ptrdiff_t>(offset + n));
    offset += n;
  } while (offset < raw.size());
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (const std::uint8_t byte : raw) {
    a = (a + byte) % 65521U;
    b = (b + a) % 65521U;
  }
  put32(z, (b << 16U) | a);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});
  return out;
}

}  // namespace avionix
