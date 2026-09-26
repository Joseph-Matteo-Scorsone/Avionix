module;

#include <cstdint>
#include <optional>

export module avionix.entity.style;

export import avionix.entity.color;

export namespace avionix {

// SGR text attributes as scoped bit flags.
enum class attribute : std::uint16_t {
  none = 0,
  bold = 1U << 0U,
  dim = 1U << 1U,
  italic = 1U << 2U,
  underline = 1U << 3U,
  blink = 1U << 4U,
  reverse = 1U << 5U,
  hidden = 1U << 6U,
  strikethrough = 1U << 7U,
};

[[nodiscard]] constexpr attribute operator|(attribute a, attribute b) noexcept {
  return static_cast<attribute>(static_cast<std::uint16_t>(a) |
                                static_cast<std::uint16_t>(b));
}

[[nodiscard]] constexpr attribute operator&(attribute a, attribute b) noexcept {
  return static_cast<attribute>(static_cast<std::uint16_t>(a) &
                                static_cast<std::uint16_t>(b));
}

[[nodiscard]] constexpr attribute operator~(attribute a) noexcept {
  return static_cast<attribute>(
      static_cast<std::uint16_t>(~static_cast<std::uint16_t>(a)));
}

constexpr attribute& operator|=(attribute& a, attribute b) noexcept {
  return a = a | b;
}
constexpr attribute& operator&=(attribute& a, attribute b) noexcept {
  return a = a & b;
}

[[nodiscard]] constexpr bool has(attribute set, attribute flag) noexcept {
  return (set & flag) == flag;
}

// Resolved appearance of one cell. Trivially copyable, 10 bytes.
struct style {
  color foreground{};
  color background{};
  attribute attributes{attribute::none};

  friend constexpr bool operator==(const style&, const style&) noexcept = default;

  [[nodiscard]] constexpr style with_foreground(color value) const noexcept {
    style copy = *this;
    copy.foreground = value;
    return copy;
  }

  [[nodiscard]] constexpr style with_background(color value) const noexcept {
    style copy = *this;
    copy.background = value;
    return copy;
  }

  [[nodiscard]] constexpr style with(attribute flags) const noexcept {
    style copy = *this;
    copy.attributes |= flags;
    return copy;
  }

  [[nodiscard]] constexpr style without(attribute flags) const noexcept {
    style copy = *this;
    copy.attributes &= ~flags;
    return copy;
  }
};

// A partial style. Unset fields inherit from the style it is applied to.
// Widgets use patches so a parent's background survives a child that only
// sets a foreground.
struct style_patch {
  std::optional<color> foreground{};
  std::optional<color> background{};
  attribute add{attribute::none};
  attribute remove{attribute::none};

  friend constexpr bool operator==(const style_patch&,
                                   const style_patch&) noexcept = default;
};

[[nodiscard]] constexpr style apply(style base, const style_patch& patch) noexcept {
  if (patch.foreground) {
    base.foreground = *patch.foreground;
  }
  if (patch.background) {
    base.background = *patch.background;
  }
  base.attributes = (base.attributes & ~patch.remove) | patch.add;
  return base;
}

}  // namespace avionix
