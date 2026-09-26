module;

#include <array>
#include <cstdint>
#include <limits>

export module avionix.entity.color;

// Terminal colors and the rules for reducing them to what a terminal can
// display. Pure values; capability detection lives in
// avionix.object.terminal_capabilities.

export namespace avionix {

// Number of colors a terminal can render. Ordered so that a larger value
// means strictly more colors.
enum class color_depth : std::uint8_t {
  monochrome,  // no SGR color at all
  ansi16,      // SGR 30-37, 90-97
  indexed256,  // SGR 38;5;n
  truecolor,   // SGR 38;2;r;g;b
};

// A color is one of: the terminal's default, an indexed palette entry, or a
// 24-bit RGB value. Packed into 4 bytes so cells stay small.
class color {
 public:
  enum class kind : std::uint8_t { terminal_default, indexed, rgb };

  constexpr color() noexcept = default;

  [[nodiscard]] static constexpr color terminal_default() noexcept { return {}; }

  [[nodiscard]] static constexpr color indexed(std::uint8_t index) noexcept {
    return color{kind::indexed, index, 0, 0};
  }

  [[nodiscard]] static constexpr color rgb(std::uint8_t r, std::uint8_t g,
                                           std::uint8_t b) noexcept {
    return color{kind::rgb, r, g, b};
  }

  [[nodiscard]] static constexpr color hex(std::uint32_t value) noexcept {
    return rgb(static_cast<std::uint8_t>((value >> 16) & 0xff),
               static_cast<std::uint8_t>((value >> 8) & 0xff),
               static_cast<std::uint8_t>(value & 0xff));
  }

  [[nodiscard]] constexpr kind type() const noexcept { return kind_; }
  [[nodiscard]] constexpr bool is_default() const noexcept {
    return kind_ == kind::terminal_default;
  }

  // Palette index. Meaningful only when type() == kind::indexed.
  [[nodiscard]] constexpr std::uint8_t index() const noexcept { return a_; }

  // Channels. Meaningful only when type() == kind::rgb.
  [[nodiscard]] constexpr std::uint8_t red() const noexcept { return a_; }
  [[nodiscard]] constexpr std::uint8_t green() const noexcept { return b_; }
  [[nodiscard]] constexpr std::uint8_t blue() const noexcept { return c_; }

  friend constexpr bool operator==(const color&, const color&) noexcept = default;

 private:
  constexpr color(kind k, std::uint8_t a, std::uint8_t b, std::uint8_t c) noexcept
      : kind_{k}, a_{a}, b_{b}, c_{c} {}

  kind kind_{kind::terminal_default};
  std::uint8_t a_{};
  std::uint8_t b_{};
  std::uint8_t c_{};
};

namespace colors {
inline constexpr color black = color::indexed(0);
inline constexpr color red = color::indexed(1);
inline constexpr color green = color::indexed(2);
inline constexpr color yellow = color::indexed(3);
inline constexpr color blue = color::indexed(4);
inline constexpr color magenta = color::indexed(5);
inline constexpr color cyan = color::indexed(6);
inline constexpr color white = color::indexed(7);
inline constexpr color bright_black = color::indexed(8);
inline constexpr color bright_red = color::indexed(9);
inline constexpr color bright_green = color::indexed(10);
inline constexpr color bright_yellow = color::indexed(11);
inline constexpr color bright_blue = color::indexed(12);
inline constexpr color bright_magenta = color::indexed(13);
inline constexpr color bright_cyan = color::indexed(14);
inline constexpr color bright_white = color::indexed(15);
}  // namespace colors

struct rgb_triplet {
  std::uint8_t r{};
  std::uint8_t g{};
  std::uint8_t b{};

  friend constexpr bool operator==(const rgb_triplet&,
                                   const rgb_triplet&) noexcept = default;
};

// RGB value of a 256-color palette entry, using the xterm defaults. Entries
// 0-15 are terminal-themed in practice; the xterm values are used only as a
// reference for nearest-color matching.
[[nodiscard]] constexpr rgb_triplet palette_rgb(std::uint8_t index) noexcept {
  constexpr std::array<rgb_triplet, 16> base{{
      {0, 0, 0},
      {205, 0, 0},
      {0, 205, 0},
      {205, 205, 0},
      {0, 0, 238},
      {205, 0, 205},
      {0, 205, 205},
      {229, 229, 229},
      {127, 127, 127},
      {255, 0, 0},
      {0, 255, 0},
      {255, 255, 0},
      {92, 92, 255},
      {255, 0, 255},
      {0, 255, 255},
      {255, 255, 255},
  }};
  if (index < 16) {
    return base[index];
  }
  if (index < 232) {
    constexpr std::array<std::uint8_t, 6> steps{0, 95, 135, 175, 215, 255};
    const int cube = index - 16;
    return {steps[static_cast<std::size_t>(cube / 36)],
            steps[static_cast<std::size_t>((cube / 6) % 6)],
            steps[static_cast<std::size_t>(cube % 6)]};
  }
  const auto level = static_cast<std::uint8_t>(8 + (index - 232) * 10);
  return {level, level, level};
}

namespace detail {

constexpr std::uint32_t distance_squared(rgb_triplet a, rgb_triplet b) noexcept {
  const int dr = int{a.r} - int{b.r};
  const int dg = int{a.g} - int{b.g};
  const int db = int{a.b} - int{b.b};
  return static_cast<std::uint32_t>(dr * dr + dg * dg + db * db);
}

constexpr std::uint8_t nearest_in_range(rgb_triplet target, int first,
                                        int last) noexcept {
  std::uint8_t best = static_cast<std::uint8_t>(first);
  std::uint32_t best_distance = std::numeric_limits<std::uint32_t>::max();
  for (int i = first; i <= last; ++i) {
    const auto d = distance_squared(target, palette_rgb(static_cast<std::uint8_t>(i)));
    if (d < best_distance) {
      best_distance = d;
      best = static_cast<std::uint8_t>(i);
    }
  }
  return best;
}

}  // namespace detail

// Reduces a color to the given depth. Default colors are never changed.
// Returns the terminal default for monochrome terminals.
[[nodiscard]] constexpr color downgrade(color value, color_depth depth) noexcept {
  if (value.is_default()) {
    return value;
  }
  switch (depth) {
    case color_depth::truecolor:
      return value;
    case color_depth::monochrome:
      return color::terminal_default();
    case color_depth::indexed256:
      if (value.type() == color::kind::rgb) {
        // Only the cube and grey ramp (16-255) have well-defined RGB
        // values; 0-15 follow the user's theme.
        return color::indexed(detail::nearest_in_range(
            {value.red(), value.green(), value.blue()}, 16, 255));
      }
      return value;
    case color_depth::ansi16:
      if (value.type() == color::kind::indexed && value.index() < 16) {
        return value;
      }
      {
        const rgb_triplet source =
            value.type() == color::kind::rgb
                ? rgb_triplet{value.red(), value.green(), value.blue()}
                : palette_rgb(value.index());
        return color::indexed(detail::nearest_in_range(source, 0, 15));
      }
  }
  return value;
}

}  // namespace avionix
