module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

export module avionix.entity.terminal_graphics;

export import avionix.entity.image;
export import avionix.entity.color;

// Pixel images inside a terminal. Pure: encoders return escape sequences
// and never write.
//
//   image_rgba8 ──fit_image──► panel-sized pixels
//        ├──append_kitty──► APC _G (Kitty, Ghostty, WezTerm)
//        ├──append_sixel──► DCS q  (Windows Terminal 1.22+, foot, xterm, mlterm,
//        │                         WezTerm, iTerm2, Konsole, VTE)
//        └──append_iterm2─► OSC 1337 (iTerm2, WezTerm)
//
// Terminals without any of these show images as half-block characters,
// which image_view draws through the ordinary cell renderer.

export namespace avionix {

enum class graphics_protocol : std::uint8_t { none, kitty, sixel, iterm2 };

[[nodiscard]] constexpr std::string_view name(graphics_protocol p) noexcept {
  switch (p) {
    case graphics_protocol::none:
      return "blocks";
    case graphics_protocol::kitty:
      return "kitty";
    case graphics_protocol::sixel:
      return "sixel";
    case graphics_protocol::iterm2:
      return "iterm2";
  }
  return "blocks";
}

// Pixel size of one terminal cell. Zero when the terminal has not said.
struct cell_pixels {
  std::uint32_t width{};
  std::uint32_t height{};

  friend constexpr bool operator==(const cell_pixels&,
                                   const cell_pixels&) noexcept = default;
  [[nodiscard]] constexpr bool known() const noexcept {
    return width > 0 && height > 0;
  }
};

// What the running terminal can show, as the event loop currently knows
// it. Widgets read it while drawing (render_context::graphics()).
struct graphics_support {
  graphics_protocol protocol{graphics_protocol::none};
  cell_pixels cell{};
  // Text colors. Half-block images need color; monochrome terminals
  // (NO_COLOR, dumb) get a character brightness ramp instead.
  color_depth colors{color_depth::truecolor};

  friend constexpr bool operator==(const graphics_support&,
                                   const graphics_support&) noexcept = default;

  // True when pixel images can be placed. Sixel needs the exact cell size
  // because the terminal draws sixels unscaled; Kitty and iTerm2 scale the
  // image to the requested cells.
  [[nodiscard]] constexpr bool images() const noexcept {
    return protocol == graphics_protocol::sixel ? cell.known()
                                                : protocol != graphics_protocol::none;
  }
  // The cell size to render at: the reported one, or 8x16 when unknown.
  [[nodiscard]] constexpr cell_pixels effective_cell() const noexcept {
    return cell.known() ? cell : cell_pixels{8, 16};
  }
};

namespace graphics_detail {

constexpr std::string_view base64_alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline void append_base64(std::string& out, const std::uint8_t* data,
                          std::size_t size) {
  out.reserve(out.size() + (size + 2) / 3 * 4);
  for (std::size_t i = 0; i < size; i += 3) {
    const std::uint32_t a = data[i];
    const std::uint32_t b = i + 1 < size ? data[i + 1] : 0U;
    const std::uint32_t c = i + 2 < size ? data[i + 2] : 0U;
    const std::uint32_t bits = (a << 16U) | (b << 8U) | c;
    out.push_back(base64_alphabet[(bits >> 18U) & 63U]);
    out.push_back(base64_alphabet[(bits >> 12U) & 63U]);
    out.push_back(i + 1 < size ? base64_alphabet[(bits >> 6U) & 63U] : '=');
    out.push_back(i + 2 < size ? base64_alphabet[bits & 63U] : '=');
  }
}

}  // namespace graphics_detail

// Resamples `src` to exactly `width` x `height`, preserving its aspect
// ratio. The uncovered margin is transparent.
[[nodiscard]] inline image_rgba8 fit_image(const image_rgba8& src, std::uint32_t width,
                                           std::uint32_t height) {
  image_rgba8 out{width, height,
                  std::vector<std::uint8_t>(std::size_t{width} * height * 4, 0)};
  if (src.width == width && src.height == height) {
    out.pixels = src.pixels;
    return out;
  }
  if (src.width == 0 || src.height == 0 || width == 0 || height == 0) return out;
  const double scale = std::min(static_cast<double>(width) / src.width,
                                static_cast<double>(height) / src.height);
  const double dw = src.width * scale;
  const double dh = src.height * scale;
  const double ox = (width - dw) * 0.5;
  const double oy = (height - dh) * 0.5;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const double sx = (x + 0.5 - ox) / scale - 0.5;
      const double sy = (y + 0.5 - oy) / scale - 0.5;
      if (sx < -0.5 || sy < -0.5 || sx > src.width - 0.5 || sy > src.height - 0.5)
        continue;
      const double fx = std::clamp(sx, 0.0, src.width - 1.0);
      const double fy = std::clamp(sy, 0.0, src.height - 1.0);
      const auto x0 = static_cast<std::uint32_t>(fx);
      const auto y0 = static_cast<std::uint32_t>(fy);
      const std::uint32_t x1 = std::min(x0 + 1, src.width - 1);
      const std::uint32_t y1 = std::min(y0 + 1, src.height - 1);
      const double tx = fx - x0;
      const double ty = fy - y0;
      const std::size_t o = (std::size_t{y} * width + x) * 4;
      for (std::size_t c = 0; c < 4; ++c) {
        const double top = src.at(x0, y0)[c] * (1 - tx) + src.at(x1, y0)[c] * tx;
        const double bottom = src.at(x0, y1)[c] * (1 - tx) + src.at(x1, y1)[c] * tx;
        out.pixels[o + c] =
            static_cast<std::uint8_t>(std::lround(top * (1 - ty) + bottom * ty));
      }
    }
  }
  return out;
}

// One pixel of `src` scaled to cover a `width` x `height` area (filling it,
// cropping the overflow, centered), sampled at area pixel (x, y) with
// bilinear filtering. Wallpaper placement.
[[nodiscard]] inline std::array<std::uint8_t, 4> sample_cover(const image_rgba8& src,
                                                              double width,
                                                              double height, double x,
                                                              double y) {
  if (src.width == 0 || src.height == 0 || width <= 0.0 || height <= 0.0) return {};
  const double scale = std::max(width / src.width, height / src.height);
  const double sx = std::clamp((x - (width - src.width * scale) * 0.5) / scale - 0.5,
                               0.0, src.width - 1.0);
  const double sy = std::clamp((y - (height - src.height * scale) * 0.5) / scale - 0.5,
                               0.0, src.height - 1.0);
  const auto x0 = static_cast<std::uint32_t>(sx);
  const auto y0 = static_cast<std::uint32_t>(sy);
  const std::uint32_t x1 = std::min(x0 + 1, src.width - 1);
  const std::uint32_t y1 = std::min(y0 + 1, src.height - 1);
  const double tx = sx - x0;
  const double ty = sy - y0;
  std::array<std::uint8_t, 4> out{};
  for (std::size_t c = 0; c < 4; ++c) {
    const double top = src.at(x0, y0)[c] * (1 - tx) + src.at(x1, y0)[c] * tx;
    const double bottom = src.at(x0, y1)[c] * (1 - tx) + src.at(x1, y1)[c] * tx;
    out[c] = static_cast<std::uint8_t>(std::lround(top * (1 - ty) + bottom * ty));
  }
  return out;
}

// Average of the pixels in [x0, x1) x [y0, y1), clamped to the image.
[[nodiscard]] inline std::array<std::uint8_t, 4> box_average(const image_rgba8& img,
                                                             std::uint32_t x0,
                                                             std::uint32_t y0,
                                                             std::uint32_t x1,
                                                             std::uint32_t y1) {
  x1 = std::min(std::max(x1, x0 + 1), img.width);
  y1 = std::min(std::max(y1, y0 + 1), img.height);
  std::array<std::uint64_t, 4> sum{};
  std::uint64_t n = 0;
  for (std::uint32_t y = y0; y < y1; ++y) {
    for (std::uint32_t x = x0; x < x1; ++x) {
      const auto p = img.at(x, y);
      for (std::size_t c = 0; c < 4; ++c) sum[c] += p[c];
      ++n;
    }
  }
  std::array<std::uint8_t, 4> out{};
  if (n == 0) return out;
  for (std::size_t c = 0; c < 4; ++c) out[c] = static_cast<std::uint8_t>(sum[c] / n);
  return out;
}

// Sixel with square pixels (raster attributes 1;1).
//
// Colors: a palette of up to 256 entries built from the image itself by
// median cut over a 15-bit color histogram, then Floyd-Steinberg error
// diffusion (serpentine, 3/4 strength) against that palette. Rendered CAD
// views are mostly smooth shading, which a fixed palette can only
// approximate with a visible dither pattern; an adaptive palette spends its
// entries on the colors actually present.
//
// Pixels with alpha below 128 are left untouched so the cell background
// shows through.
inline void append_sixel(std::string& out, const image_rgba8& img) {
  using rgb = std::array<std::int32_t, 3>;
  const std::uint32_t w = img.width;
  const std::uint32_t h = img.height;
  out += std::format("\x1bP0;1;0q\"1;1;{};{}", w, h);

  // Histogram on 5 bits per channel, with exact sums for bin centroids.
  struct bin {
    std::uint64_t count{};
    std::array<std::uint64_t, 3> sum{};
  };
  std::vector<bin> bins(32768);
  const auto key = [](std::int32_t r, std::int32_t g, std::int32_t b) {
    return static_cast<std::size_t>(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
  };
  for (std::size_t i = 0; i + 3 < img.pixels.size(); i += 4) {
    if (img.pixels[i + 3] < 128) continue;
    bin& b = bins[key(img.pixels[i], img.pixels[i + 1], img.pixels[i + 2])];
    ++b.count;
    for (std::size_t c = 0; c < 3; ++c) b.sum[c] += img.pixels[i + c];
  }
  std::vector<std::uint16_t> used;
  for (std::size_t k = 0; k < bins.size(); ++k)
    if (bins[k].count > 0) used.push_back(static_cast<std::uint16_t>(k));

  // Median cut: repeatedly split the box with the widest channel range
  // (weighted by population) at its population median.
  struct box {
    std::size_t begin{};
    std::size_t end{};
  };
  const auto channel = [](std::uint16_t k, std::size_t c) {
    return static_cast<std::int32_t>((k >> (10 - 5 * c)) & 31U);
  };
  const auto measure = [&](const box& bx, std::size_t& widest) {
    std::array<std::int32_t, 3> lo{31, 31, 31};
    std::array<std::int32_t, 3> hi{0, 0, 0};
    std::uint64_t population = 0;
    for (std::size_t i = bx.begin; i < bx.end; ++i) {
      for (std::size_t c = 0; c < 3; ++c) {
        lo[c] = std::min(lo[c], channel(used[i], c));
        hi[c] = std::max(hi[c], channel(used[i], c));
      }
      population += bins[used[i]].count;
    }
    widest = 0;
    for (std::size_t c = 1; c < 3; ++c)
      if (hi[c] - lo[c] > hi[widest] - lo[widest]) widest = c;
    return static_cast<double>(hi[widest] - lo[widest]) *
           std::sqrt(static_cast<double>(population));
  };
  std::vector<box> boxes;
  if (!used.empty()) boxes.push_back({0, used.size()});
  while (boxes.size() < 256) {
    std::size_t pick = boxes.size();
    std::size_t axis = 0;
    double best = 0.0;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
      if (boxes[i].end - boxes[i].begin < 2) continue;
      std::size_t a = 0;
      const double score = measure(boxes[i], a);
      if (score > best) {
        best = score;
        pick = i;
        axis = a;
      }
    }
    if (pick == boxes.size()) break;
    const box bx = boxes[pick];
    const auto first = used.begin() + static_cast<std::ptrdiff_t>(bx.begin);
    const auto last = used.begin() + static_cast<std::ptrdiff_t>(bx.end);
    std::sort(first, last, [&](std::uint16_t a, std::uint16_t b) {
      return channel(a, axis) < channel(b, axis);
    });
    std::uint64_t total = 0;
    for (auto it = first; it != last; ++it) total += bins[*it].count;
    std::uint64_t running = 0;
    std::size_t split = bx.begin + 1;
    for (std::size_t i = bx.begin; i + 1 < bx.end; ++i) {
      running += bins[used[i]].count;
      split = i + 1;
      if (running * 2 >= total) break;
    }
    boxes[pick] = {bx.begin, split};
    boxes.push_back({split, bx.end});
  }

  // Palette entries are the population-weighted mean of each box, rounded
  // to Sixel's percent precision so diffusion sees the color shown.
  std::vector<rgb> palette;
  palette.reserve(boxes.size());
  for (const box& bx : boxes) {
    std::array<std::uint64_t, 3> sum{};
    std::uint64_t count = 0;
    for (std::size_t i = bx.begin; i < bx.end; ++i) {
      count += bins[used[i]].count;
      for (std::size_t c = 0; c < 3; ++c) sum[c] += bins[used[i]].sum[c];
    }
    rgb color{};
    std::array<int, 3> percent{};
    for (std::size_t c = 0; c < 3; ++c) {
      percent[c] = static_cast<int>(std::lround(
          static_cast<double>(sum[c]) / static_cast<double>(count) * 100.0 / 255.0));
      color[c] = static_cast<std::int32_t>(std::lround(percent[c] * 255.0 / 100.0));
    }
    out += std::format("#{};2;{};{};{}", palette.size(), percent[0], percent[1],
                       percent[2]);
    palette.push_back(color);
  }

  // Nearest palette entry per 15-bit color, filled on demand.
  std::vector<std::int16_t> nearest(32768, -1);
  const auto lookup = [&](std::int32_t r, std::int32_t g,
                          std::int32_t b) -> std::uint32_t {
    std::int16_t& slot = nearest[key(r, g, b)];
    if (slot < 0) {
      const std::int32_t cr = (r & ~7) + 4;
      const std::int32_t cg = (g & ~7) + 4;
      const std::int32_t cb = (b & ~7) + 4;
      std::int64_t best = std::numeric_limits<std::int64_t>::max();
      for (std::size_t i = 0; i < palette.size(); ++i) {
        const std::int64_t dr = palette[i][0] - cr;
        const std::int64_t dg = palette[i][1] - cg;
        const std::int64_t db = palette[i][2] - cb;
        const std::int64_t d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
        if (d < best) {
          best = d;
          slot = static_cast<std::int16_t>(i);
        }
      }
    }
    return static_cast<std::uint32_t>(slot);
  };

  // Index image with serpentine Floyd-Steinberg diffusion.
  constexpr std::uint32_t transparent = 0xFFFF;
  std::vector<std::uint32_t> indices(std::size_t{w} * h, transparent);
  std::vector<std::array<float, 3>> err_now(std::size_t{w} + 2);
  std::vector<std::array<float, 3>> err_next(std::size_t{w} + 2);
  constexpr float strength = 0.75F;
  if (!palette.empty()) {
    for (std::uint32_t y = 0; y < h; ++y) {
      std::fill(err_next.begin(), err_next.end(), std::array<float, 3>{});
      const bool forward = (y & 1U) == 0;
      for (std::uint32_t step = 0; step < w; ++step) {
        const std::uint32_t x = forward ? step : w - 1 - step;
        const std::uint8_t* p = &img.pixels[(std::size_t{y} * w + x) * 4];
        if (p[3] < 128) continue;
        const std::size_t e = std::size_t{x} + 1;
        std::array<std::int32_t, 3> v{};
        for (std::size_t c = 0; c < 3; ++c)
          v[c] = std::clamp(
              static_cast<std::int32_t>(std::lround(p[c] + err_now[e][c])), 0, 255);
        const std::uint32_t index = lookup(v[0], v[1], v[2]);
        indices[std::size_t{y} * w + x] = index;
        const std::size_t ahead = forward ? e + 1 : e - 1;
        const std::size_t behind = forward ? e - 1 : e + 1;
        for (std::size_t c = 0; c < 3; ++c) {
          const float q = static_cast<float>(v[c] - palette[index][c]) * strength;
          err_now[ahead][c] += q * (7.0F / 16.0F);
          err_next[behind][c] += q * (3.0F / 16.0F);
          err_next[e][c] += q * (5.0F / 16.0F);
          err_next[ahead][c] += q * (1.0F / 16.0F);
        }
      }
      std::swap(err_now, err_next);
    }
  }

  const std::size_t colors = palette.size();
  std::vector<std::uint8_t> bits(colors * w, 0);
  std::vector<bool> present(colors, false);
  for (std::uint32_t band = 0; band < h; band += 6) {
    std::fill(present.begin(), present.end(), false);
    for (std::uint32_t row = 0; row < 6 && band + row < h; ++row) {
      const std::uint32_t y = band + row;
      for (std::uint32_t x = 0; x < w; ++x) {
        const std::uint32_t c = indices[std::size_t{y} * w + x];
        if (c == transparent) continue;
        bits[std::size_t{c} * w + x] |= static_cast<std::uint8_t>(1U << row);
        present[c] = true;
      }
    }
    bool first = true;
    for (std::size_t c = 0; c < colors; ++c) {
      if (!present[c]) continue;
      if (!first) out += '$';
      first = false;
      out += std::format("#{}", c);
      std::uint8_t* line = &bits[c * w];
      // Trailing empty columns are omitted.
      std::uint32_t end = w;
      while (end > 0 && line[end - 1] == 0) --end;
      for (std::uint32_t x = 0; x < end;) {
        const std::uint8_t v = line[x];
        std::uint32_t run = 1;
        while (x + run < end && line[x + run] == v) ++run;
        const char ch = static_cast<char>(63 + v);
        if (run >= 4) {
          out += std::format("!{}{}", run, ch);
        } else {
          out.append(run, ch);
        }
        x += run;
      }
      std::fill(line, line + w, std::uint8_t{0});
    }
    out += '-';
  }
  out += "\x1b\\";
}

// Kitty graphics: transmits RGBA and places it over `cols` x `rows` cells
// at the cursor without moving it (C=1). Re-sending the same `image_id`
// replaces the image. Chunks stay under the protocol's 4096-byte limit.
//
// `z` orders the image against text: 0 draws over text; a negative value
// draws under it, and below kitty_under_backgrounds also under cells that
// have a non-default background color.
inline constexpr std::int32_t kitty_under_backgrounds = -1'073'741'825;

inline void append_kitty(std::string& out, const image_rgba8& img,
                         std::uint32_t image_id, std::uint32_t cols, std::uint32_t rows,
                         std::int32_t z = 0) {
  std::string data;
  graphics_detail::append_base64(data, img.pixels.data(), img.pixels.size());
  constexpr std::size_t chunk = 4096;
  std::size_t offset = 0;
  do {
    const std::size_t n = std::min(chunk, data.size() - offset);
    const int more = offset + n < data.size() ? 1 : 0;
    if (offset == 0) {
      out += std::format("\x1b_Ga=T,f=32,s={},v={},i={},p=1,c={},r={},C=1,q=2,",
                         img.width, img.height, image_id, cols, rows);
      if (z != 0) out += std::format("z={},", z);
      out += std::format("m={};", more);
    } else {
      out += std::format("\x1b_Gm={};", more);
    }
    out.append(data, offset, n);
    out += "\x1b\\";
    offset += n;
  } while (offset < data.size());
}

// Deletes a Kitty image and frees its data.
inline void append_kitty_delete(std::string& out, std::uint32_t image_id) {
  out += std::format("\x1b_Ga=d,d=I,i={},q=2\x1b\\", image_id);
}

// iTerm2 inline image (OSC 1337), scaled by the terminal to the cells.
inline void append_iterm2(std::string& out, const image_rgba8& img, std::uint32_t cols,
                          std::uint32_t rows) {
  const auto png = encode_png(img);
  out += std::format(
      "\x1b]1337;File=inline=1;size={};width={};height={};preserveAspectRatio=0:",
      png.size(), cols, rows);
  graphics_detail::append_base64(out, png.data(), png.size());
  out += '\x07';
}

}  // namespace avionix
