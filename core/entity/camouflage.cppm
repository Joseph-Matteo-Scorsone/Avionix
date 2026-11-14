module;

#include <array>
#include <bit>
#include <cstdint>
#include <optional>

export module avionix.entity.camouflage;

import avionix.entity.geometry;
import avionix.entity.color;

// The camouflage field: for one cell of a camouflaged widget, whether the
// widget shows solid there, and if not, which nearby background cell to
// show instead and how strongly to tint it. Pure functions of position,
// time, reveal, pointer, and the shape mask. No cells, buffers, or widgets.
//
//   time, reveal, pointer ──► camouflage_frame ──┬─► camouflage_row (per y)
//                                                └─► camouflage_column (per x)
//   frame + row + cell ──► camouflage_scan_at ──► camouflage_scan
//                              (reveal, solid row, tear)   │
//   mask around cell + tear ──► camouflage_neighborhood ───┤
//                                                          ▼
//                frame + row + column ──► camouflage_shade ──► camouflage_cell
//
// camouflage_field() runs every stage for one cell. A widget filling many
// cells computes the frame once and each row and column once.
//
// Determinism: the same inputs give bit-identical outputs on every
// platform, so tests can compare frames. The hash is integer arithmetic,
// sine is a polynomial here instead of the C library's, and floating-point
// contraction is off so no compiler fuses a multiply and an add into an
// FMA on one target and not another. Only IEEE-754 +, -, *, / remain.

#pragma STDC FP_CONTRACT OFF

export namespace avionix {

// Largest displacement on either axis, in cells.
inline constexpr std::int32_t camouflage_max_offset = 2;

struct camouflage_point {
  // Cell position relative to the widget's top-left cell. Anchoring the
  // field to the widget keeps its pattern still when the widget moves.
  position cell{};
  double seconds{};
  // Base reveal: 0 camouflaged, 1 solid.
  double reveal{};
  // Pointer in the same coordinates as `cell`; empty for no disruption.
  std::optional<position> pointer{};
  // Pointer disruption radius in rows. Columns count half, so the
  // disrupted region looks round on cells about twice as tall as wide.
  double pointer_radius{};
};

// The part of the field that does not depend on the shape.
struct camouflage_scan {
  double reveal{};  // base reveal plus pointer disruption, [0, 1]
  double edge{};    // 1 mid-transition, 0 at rest
  bool solid{};
  std::int32_t tear{};  // sideways shift of the mask read, in columns
};

// Mask bits for a cell and its 8 neighbors. Bit (dy + 1) * 3 + (dx + 1)
// is set when the widget drew the cell at (dx, dy) from the center.
struct camouflage_neighborhood {
  std::uint16_t bits{};

  [[nodiscard]] constexpr bool at(std::int32_t dx, std::int32_t dy) const noexcept {
    return ((bits >> static_cast<unsigned>((dy + 1) * 3 + (dx + 1))) & 1U) != 0;
  }
  [[nodiscard]] constexpr bool center() const noexcept { return at(0, 0); }
};

struct camouflage_cell {
  bool solid{};
  position offset{};  // each axis in [-camouflage_max_offset, camouflage_max_offset]
  double rim{};       // [0, 1], multiples of 1/4; 0 where solid
  double accent{};    // [0, 1], multiples of 1/4; 0 where solid

  friend constexpr bool operator==(const camouflage_cell&,
                                   const camouflage_cell&) noexcept = default;
};

namespace camouflage_math {

inline constexpr double pi = 3.14159265358979323846;

// Valid for |value| < 2^62, far beyond any cell coordinate or clock.
[[nodiscard]] constexpr double floor(double value) noexcept {
  const auto whole = static_cast<double>(static_cast<std::int64_t>(value));
  return whole > value ? whole - 1.0 : whole;
}

[[nodiscard]] constexpr double fract(double value) noexcept {
  return value - floor(value);
}

// Rounds half away from zero.
[[nodiscard]] constexpr std::int32_t round(double value) noexcept {
  return static_cast<std::int32_t>(value < 0.0 ? -floor(-value + 0.5)
                                               : floor(value + 0.5));
}

[[nodiscard]] constexpr double clamp(double value, double low, double high) noexcept {
  return value < low ? low : (value > high ? high : value);
}

[[nodiscard]] constexpr double smoothstep(double edge0, double edge1,
                                          double value) noexcept {
  const double t = clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

// Halving the exponent bits gives a guess within 6%; five Newton steps
// take that below 1e-15. A fixed count keeps the cost flat and avoids
// Newton's two-value oscillation in the last bit.
[[nodiscard]] constexpr double sqrt(double value) noexcept {
  if (!(value > 0.0)) return 0.0;
  const auto bits = std::bit_cast<std::uint64_t>(value);
  double guess = std::bit_cast<double>((bits >> 1U) + (std::uint64_t{1023} << 51U));
  for (int i = 0; i < 5; ++i) guess = 0.5 * (guess + value / guess);
  return guess;
}

// Taylor series to x^13 after reducing to [-pi/2, pi/2]. Absolute error
// below 1e-9, which is far finer than any threshold the field rounds at.
[[nodiscard]] constexpr double sin(double value) noexcept {
  const double turns = floor(value / (2.0 * pi) + 0.5);
  double x = value - turns * (2.0 * pi);
  if (x > pi / 2.0) x = pi - x;
  if (x < -pi / 2.0) x = -pi - x;
  constexpr double c3 = -1.0 / 6.0;
  constexpr double c5 = 1.0 / 120.0;
  constexpr double c7 = -1.0 / 5040.0;
  constexpr double c9 = 1.0 / 362880.0;
  constexpr double c11 = -1.0 / 39916800.0;
  constexpr double c13 = 1.0 / 6227020800.0;
  const double x2 = x * x;
  return x *
         (1.0 + x2 * (c3 + x2 * (c5 + x2 * (c7 + x2 * (c9 + x2 * (c11 + x2 * c13))))));
}

[[nodiscard]] constexpr double cos(double value) noexcept {
  return sin(value + pi / 2.0);
}

// lowbias32 (Chris Wellons), a well-mixed 32-bit integer finalizer.
[[nodiscard]] constexpr std::uint32_t mix(std::uint32_t h) noexcept {
  h ^= h >> 16U;
  h *= 0x7feb352dU;
  h ^= h >> 15U;
  h *= 0x846ca68bU;
  h ^= h >> 16U;
  return h;
}

// Uniform in [0, 1) for an integer lattice point. Stands in for the shader
// hash fract(sin(dot(v, (127.1, 311.7))) * 43758.5453), whose result
// depends on the last bits of the C library's sin and so differs between
// platforms.
[[nodiscard]] constexpr double hash(std::int64_t x, std::int64_t y,
                                    std::uint32_t seed) noexcept {
  const std::uint32_t h =
      mix(static_cast<std::uint32_t>(x) * 0x8da6b343U ^
          mix(static_cast<std::uint32_t>(y) * 0xd8163841U ^ seed * 0xcb1ab31fU));
  return static_cast<double>(h >> 8U) * (1.0 / 16777216.0);
}

// Bilinear value noise over hash(), smoothstep interpolated. [0, 1).
[[nodiscard]] constexpr double noise(double x, double y, std::uint32_t seed) noexcept {
  const double fx = floor(x);
  const double fy = floor(y);
  const auto ix = static_cast<std::int64_t>(fx);
  const auto iy = static_cast<std::int64_t>(fy);
  const double tx = x - fx;
  const double ty = y - fy;
  const double ux = tx * tx * (3.0 - 2.0 * tx);
  const double uy = ty * ty * (3.0 - 2.0 * ty);
  const double a = hash(ix, iy, seed);
  const double b = hash(ix + 1, iy, seed);
  const double c = hash(ix, iy + 1, seed);
  const double d = hash(ix + 1, iy + 1, seed);
  const double top = a + (b - a) * ux;
  const double bottom = c + (d - c) * ux;
  return top + (bottom - top) * uy;
}

[[nodiscard]] constexpr double quarter(double value) noexcept {
  return static_cast<double>(round(clamp(value, 0.0, 1.0) * 4.0)) / 4.0;
}

}  // namespace camouflage_math

// Tuning, in cell units. Changes from the pixel-shader reference are
// listed in docs/camouflage.md.
namespace camouflage_tuning {
// Rows flip between camouflaged and solid this many times per second
// during a transition.
inline constexpr double dissolve_rate = 24.0;
// Wobble and shimmer read the clock in steps of this many seconds, so a
// widget at rest changes on one frame in four at a 50 ms interval
// instead of a few cells on every frame.
inline constexpr double slow_step = 0.2;
inline constexpr double max_tear = 6.0;
inline constexpr double lens = 2.0;
inline constexpr double drift_x = 1.5;
inline constexpr double drift_y = -0.75;
inline constexpr double wobble_x = 0.75;
inline constexpr double wobble_y = 0.6;
}  // namespace camouflage_tuning

// Terms shared by every cell of one frame.
struct camouflage_frame {
  double seconds{};
  double slow{};  // seconds quantized to camouflage_tuning::slow_step
  std::int64_t tick{};
  double reveal{};
  std::optional<position> pointer{};
  double pointer_radius{};
};

// Terms shared by one row, and by one column, of one frame.
struct camouflage_row {
  double threshold{};  // the row is solid where reveal reaches this
  double tear{};       // [-0.5, 0.5), scaled by edge and max_tear
  double wobble{};     // x drift
  double shimmer_sin{};
  double shimmer_cos{};
};

struct camouflage_column {
  double wobble{};  // y drift
  double shimmer_sin{};
  double shimmer_cos{};
};

// Mask mean, gradient, gradient length, and outline strength for each of
// the 512 possible neighborhoods. The gradient points toward the inside
// of the shape. `outline` is 1 when three or more of the 9 cells are
// outside the shape. The gradient alone misses thin shapes: along a
// one-row word the left and right neighbors cancel and it is zero.
struct camouflage_shape {
  double blur{};
  double x{};
  double y{};
  double length{};
  double outline{};
};

inline constexpr auto camouflage_shapes = [] {
  std::array<camouflage_shape, 512> table{};
  constexpr double diagonal = 0.70710678118654752;
  for (unsigned bits = 0; bits < 512; ++bits) {
    const camouflage_neighborhood mask{static_cast<std::uint16_t>(bits)};
    int count = 0;
    double gx = 0.0;
    double gy = 0.0;
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
      for (std::int32_t dx = -1; dx <= 1; ++dx) {
        if (!mask.at(dx, dy)) continue;
        ++count;
        if (dx == 0 && dy == 0) continue;
        const double unit = dx != 0 && dy != 0 ? diagonal : 1.0;
        gx += dx * unit * 0.25;
        gy += dy * unit * 0.25;
      }
    }
    const double outside = (9 - count) / 3.0;
    table[bits] = {count / 9.0, gx, gy, camouflage_math::sqrt(gx * gx + gy * gy),
                   outside < 1.0 ? outside : 1.0};
  }
  return table;
}();

[[nodiscard]] constexpr camouflage_frame camouflage_frame_at(
    double seconds, double reveal, std::optional<position> pointer,
    double pointer_radius) noexcept {
  namespace m = camouflage_math;
  namespace k = camouflage_tuning;
  return {.seconds = seconds,
          .slow = m::floor(seconds / k::slow_step) * k::slow_step,
          .tick = static_cast<std::int64_t>(m::floor(seconds * k::dissolve_rate)),
          .reveal = m::clamp(reveal, 0.0, 1.0),
          .pointer = pointer_radius > 0.0 ? pointer : std::nullopt,
          .pointer_radius = pointer_radius};
}

[[nodiscard]] constexpr camouflage_row camouflage_row_at(const camouflage_frame& f,
                                                         std::int32_t y) noexcept {
  namespace m = camouflage_math;
  const double b = -0.2 * y;
  return {.threshold = m::hash(y, f.tick, 0x11U) * 0.98 + 0.01,
          .tear = m::hash(y, f.tick + 9, 0x22U) - 0.5,
          .wobble = m::sin(f.slow * 0.9 + y * 0.3),
          .shimmer_sin = m::sin(b),
          .shimmer_cos = m::cos(b)};
}

[[nodiscard]] constexpr camouflage_column camouflage_column_at(
    const camouflage_frame& f, std::int32_t x) noexcept {
  namespace m = camouflage_math;
  const double a = f.slow * 2.2 + x * 0.1;
  return {.wobble = m::cos(f.slow * 0.7 + x * 0.15),
          .shimmer_sin = m::sin(a),
          .shimmer_cos = m::cos(a)};
}

// Reveal, scanline dissolve, and tear for one cell.
[[nodiscard]] constexpr camouflage_scan camouflage_scan_at(const camouflage_frame& f,
                                                           const camouflage_row& row,
                                                           position cell) noexcept {
  namespace m = camouflage_math;
  namespace k = camouflage_tuning;
  double local = 0.0;
  if (f.pointer) {
    const double r = f.pointer_radius;
    const double dx = (cell.x - f.pointer->x) * 0.5;
    const double dy = cell.y - f.pointer->y;
    const double squared = dx * dx + dy * dy;
    // The ragged edge moves reach by at most r / 4, so beyond 1.25 r the
    // pointer has no effect and the noise is skipped.
    if (squared < r * r * 1.5625) {
      const double ragged = (m::noise(cell.x * 0.4 + f.seconds * 1.5,
                                      cell.y * 0.4 + f.seconds * 1.5, 0x51U) -
                             0.5) *
                            r * 0.5;
      local = 1.0 - m::smoothstep(r * 0.35, r, m::sqrt(squared) + ragged);
    }
  }
  camouflage_scan out;
  out.reveal = m::clamp(f.reveal + local, 0.0, 1.0);
  out.edge = 4.0 * out.reveal * (1.0 - out.reveal);
  out.solid = row.threshold <= out.reveal;
  out.tear = m::round(row.tear * k::max_tear * out.edge);
  return out;
}

// Refraction offset and highlights from the mask around the torn position.
[[nodiscard]] constexpr camouflage_cell camouflage_shade(
    const camouflage_frame& f, const camouflage_row& row,
    const camouflage_column& column, position cell, const camouflage_scan& scan,
    camouflage_neighborhood mask) noexcept {
  namespace m = camouflage_math;
  namespace k = camouflage_tuning;
  camouflage_cell out;
  out.solid = scan.solid;
  if (scan.solid) return out;
  const camouflage_shape& shape = camouflage_shapes[mask.bits & 511U];
  const auto limit = [](std::int32_t v) {
    return v < -camouflage_max_offset
               ? -camouflage_max_offset
               : (v > camouflage_max_offset ? camouflage_max_offset : v);
  };
  out.offset = {
      limit(m::round(-shape.x * k::lens +
                     shape.blur * (k::drift_x + k::wobble_x * row.wobble))),
      limit(m::round(-shape.y * k::lens +
                     shape.blur * (k::drift_y + k::wobble_y * column.wobble))),
  };
  // sin(slow * 2.2 + x * 0.1 - y * 0.2), expanded as sin(a + b).
  const double wave =
      column.shimmer_sin * row.shimmer_cos + column.shimmer_cos * row.shimmer_sin;
  // The floor of 0.6 keeps the outline visible over a flat background,
  // where the rim is all that shows.
  const double shimmer = 0.8 + 0.2 * wave;
  const double gradient = shape.length * 1.4;
  const double strength = gradient > shape.outline ? gradient : shape.outline;
  out.rim = m::quarter((strength < 1.0 ? strength : 1.0) * shimmer);
  if (scan.edge > 0.0) {
    const double band =
        m::noise(cell.x * 0.35, cell.y * 0.35 + f.seconds * 6.0, 0x33U) - 0.5;
    const double vein = band < 0.06 && band > -0.06 ? 1.0 : 0.0;
    out.accent = m::quarter(vein * scan.edge * m::smoothstep(0.05, 0.5, shape.blur));
  }
  return out;
}

// The scan for one cell, computing the frame and row terms itself.
[[nodiscard]] constexpr camouflage_scan camouflage_scan_at(
    const camouflage_point& in) noexcept {
  const camouflage_frame f =
      camouflage_frame_at(in.seconds, in.reveal, in.pointer, in.pointer_radius);
  return camouflage_scan_at(f, camouflage_row_at(f, in.cell.y), in.cell);
}

// The whole field for one cell. `mask_at(position)` returns whether the
// widget drew the cell at a position in camouflage_point::cell
// coordinates; positions outside the widget must return false. Gives the
// same result as the staged functions above, which a caller filling many
// cells uses to compute row and column terms once.
template <typename MaskAt>
[[nodiscard]] constexpr camouflage_cell camouflage_field(const camouflage_point& in,
                                                         MaskAt&& mask_at) {
  const camouflage_frame f =
      camouflage_frame_at(in.seconds, in.reveal, in.pointer, in.pointer_radius);
  const camouflage_row row = camouflage_row_at(f, in.cell.y);
  const camouflage_scan scan = camouflage_scan_at(f, row, in.cell);
  if (scan.solid) return {.solid = true};
  camouflage_neighborhood around;
  for (std::int32_t dy = -1; dy <= 1; ++dy) {
    for (std::int32_t dx = -1; dx <= 1; ++dx) {
      if (mask_at(position{in.cell.x + scan.tear + dx, in.cell.y + dy})) {
        around.bits = static_cast<std::uint16_t>(
            around.bits | (1U << static_cast<unsigned>((dy + 1) * 3 + (dx + 1))));
      }
    }
  }
  return camouflage_shade(f, row, camouflage_column_at(f, in.cell.x), in.cell, scan,
                          around);
}

// Moves `from` toward `to` by `amount` in [0, 1]. Default colors have no
// RGB value, so `default_rgb` stands in for them, and the result is RGB.
// An amount of 0 returns `from` unchanged, default or not.
[[nodiscard]] constexpr color camouflage_blend(color from, color to, double amount,
                                               rgb_triplet default_rgb) noexcept {
  if (!(amount > 0.0)) return from;
  const auto channels = [&](color value) {
    switch (value.type()) {
      case color::kind::rgb:
        return rgb_triplet{value.red(), value.green(), value.blue()};
      case color::kind::indexed:
        return palette_rgb(value.index());
      case color::kind::terminal_default:
        break;
    }
    return default_rgb;
  };
  const double t = amount < 1.0 ? amount : 1.0;
  const rgb_triplet a = channels(from);
  const rgb_triplet b = channels(to);
  const auto lerp = [t](std::uint8_t x, std::uint8_t y) {
    return static_cast<std::uint8_t>(camouflage_math::round(x + (y - x) * t));
  };
  return color::rgb(lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b));
}

}  // namespace avionix
