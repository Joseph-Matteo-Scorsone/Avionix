module;
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

export module avionix.entity.clipboard;

export namespace avionix {

enum class clipboard_error {
  unsupported,
  no_image,
  platform_failure,
  malformed_data,
  unsupported_format,
  allocation_limit
};

struct clipboard_limits {
  std::size_t max_bytes{64 * 1024 * 1024};
  std::uint32_t max_dimension{16384};
};

// Top-down, tightly packed RGBA8, straight opaque alpha. No color profile.
struct clipboard_image {
  std::uint32_t width{};
  std::uint32_t height{};
  std::size_t stride{};
  std::vector<std::byte> rgba8;
};
using clipboard_image_result = std::expected<clipboard_image, clipboard_error>;

// Pure decoder for Windows CF_DIB BITMAPINFOHEADER, 24/32-bit BI_RGB.
// Other headers/compression are rejected rather than guessed.
[[nodiscard]] inline clipboard_image_result decode_clipboard_dib(
    std::span<const std::byte> bytes, clipboard_limits limits = {}) {
  const auto fail = [](clipboard_error e) -> clipboard_image_result {
    return std::unexpected(e);
  };
  if (bytes.size() > limits.max_bytes) return fail(clipboard_error::allocation_limit);
  if (bytes.size() < 40) return fail(clipboard_error::malformed_data);
  const auto u16 = [&](std::size_t i) {
    return std::to_integer<std::uint32_t>(bytes[i]) |
           (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8U);
  };
  const auto u32 = [&](std::size_t i) { return u16(i) | (u16(i + 2) << 16U); };
  if (u32(0) != 40 || u32(16) != 0 || (u16(14) != 24 && u16(14) != 32))
    return fail(clipboard_error::unsupported_format);
  const auto w = u32(4), raw_h = u32(8);
  const bool top_down = (raw_h & 0x80000000U) != 0;
  const auto h = top_down ? 0U - raw_h : raw_h;
  if (w == 0 || w > 0x7fffffffU || h == 0 || h > 0x7fffffffU || u16(12) != 1)
    return fail(clipboard_error::malformed_data);
  if (w > limits.max_dimension || h > limits.max_dimension)
    return fail(clipboard_error::allocation_limit);
  // Compute in uint64_t before narrowing to size_t, including on 32-bit hosts.
  const std::uint64_t source_stride = ((std::uint64_t{w} * u16(14) + 31) / 32) * 4;
  const std::uint64_t output_stride = std::uint64_t{w} * 4;
  const auto output_size = output_stride * h;
  const std::uint64_t offset = 40 + std::uint64_t{u32(32)} * 4;
  if (output_size > limits.max_bytes || source_stride * h > limits.max_bytes)
    return fail(clipboard_error::allocation_limit);
  if (offset > bytes.size() || source_stride * h > bytes.size() - offset)
    return fail(clipboard_error::malformed_data);
  const auto declared_size = u32(20);
  if (declared_size != 0 && declared_size != source_stride * h)
    return fail(clipboard_error::malformed_data);
  clipboard_image image{w, h, static_cast<std::size_t>(output_stride), {}};
  image.rgba8.resize(static_cast<std::size_t>(output_size));
  const auto pixel_bytes = u16(14) / 8;
  for (std::uint32_t y = 0; y < h; ++y) {
    const auto source_y = top_down ? y : h - 1 - y;
    for (std::uint32_t x = 0; x < w; ++x) {
      const auto src = static_cast<std::size_t>(offset + source_stride * source_y +
                                                std::uint64_t{x} * pixel_bytes);
      const auto dst =
          static_cast<std::size_t>(output_stride * y + std::uint64_t{x} * 4);
      image.rgba8[dst] = bytes[src + 2];
      image.rgba8[dst + 1] = bytes[src + 1];
      image.rgba8[dst + 2] = bytes[src];
      image.rgba8[dst + 3] = std::byte{255};
    }
  }
  return image;
}
}  // namespace avionix
