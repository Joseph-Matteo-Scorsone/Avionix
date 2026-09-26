module;

#include <cstdint>
#include <string>
#include <variant>

export module avionix.entity.event;

export import avionix.entity.geometry;

// Typed input and application events. Producers:
//
//   input_decoder ──► key/mouse/paste/focus events
//   terminal resize ──► resize_event
//   worker threads ──► user_event (through the event queue)

export namespace avionix {

enum class modifiers : std::uint8_t {
  none = 0,
  shift = 1U << 0U,
  alt = 1U << 1U,
  ctrl = 1U << 2U,
  super = 1U << 3U,
};

[[nodiscard]] constexpr modifiers operator|(modifiers a, modifiers b) noexcept {
  return static_cast<modifiers>(static_cast<std::uint8_t>(a) |
                                static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr modifiers operator&(modifiers a, modifiers b) noexcept {
  return static_cast<modifiers>(static_cast<std::uint8_t>(a) &
                                static_cast<std::uint8_t>(b));
}

constexpr modifiers& operator|=(modifiers& a, modifiers b) noexcept {
  return a = a | b;
}

[[nodiscard]] constexpr bool has(modifiers set, modifiers flag) noexcept {
  return (set & flag) == flag && flag != modifiers::none;
}

// Non-character keys. Character keys use key::character with a code point.
enum class key : std::uint8_t {
  character,
  enter,
  tab,
  backspace,
  escape,
  up,
  down,
  left,
  right,
  home,
  end,
  page_up,
  page_down,
  insert,
  del,
  f1,
  f2,
  f3,
  f4,
  f5,
  f6,
  f7,
  f8,
  f9,
  f10,
  f11,
  f12,
};

struct key_event {
  avionix::key code{key::character};
  // The code point for key::character. Ctrl+letter arrives as the letter
  // with modifiers::ctrl, not as a C0 control.
  char32_t character{};
  avionix::modifiers mods{modifiers::none};

  friend constexpr bool operator==(const key_event&,
                                   const key_event&) noexcept = default;

  [[nodiscard]] constexpr bool is(
      char32_t c, avionix::modifiers m = modifiers::none) const noexcept {
    return code == key::character && character == c && mods == m;
  }

  [[nodiscard]] constexpr bool is(
      avionix::key k, avionix::modifiers m = modifiers::none) const noexcept {
    return code == k && mods == m;
  }
};

enum class mouse_button : std::uint8_t {
  none,
  left,
  middle,
  right,
  wheel_up,
  wheel_down,
  wheel_left,
  wheel_right,
};

enum class mouse_action : std::uint8_t { press, release, move, drag };

struct mouse_event {
  mouse_button button{mouse_button::none};
  mouse_action action{mouse_action::press};
  position where{};  // zero-based cell coordinates
  avionix::modifiers mods{modifiers::none};

  friend constexpr bool operator==(const mouse_event&,
                                   const mouse_event&) noexcept = default;
};

// Text delivered through bracketed paste. Owns its bytes (UTF-8, may be
// invalid; consumers must decode through avionix.entity.unicode).
struct paste_event {
  std::string text;

  friend bool operator==(const paste_event&, const paste_event&) = default;
};

struct resize_event {
  avionix::size extent{};

  friend constexpr bool operator==(const resize_event&,
                                   const resize_event&) noexcept = default;
};

struct focus_event {
  bool gained{};

  friend constexpr bool operator==(const focus_event&,
                                   const focus_event&) noexcept = default;
};

// Application-defined notification posted from any thread. Avionix never
// interprets `tag` or `value`.
struct user_event {
  std::uint64_t tag{};
  std::uint64_t value{};

  friend constexpr bool operator==(const user_event&,
                                   const user_event&) noexcept = default;
};

using event = std::variant<key_event, mouse_event, paste_event, resize_event,
                           focus_event, user_event>;

// The alternative held by `value`, or nullptr. Lets consumers inspect events
// without including <variant>; std::visit remains available for exhaustive
// dispatch.
template <typename T>
[[nodiscard]] constexpr const T* as(const event& value) noexcept {
  return std::get_if<T>(&value);
}

template <typename T>
[[nodiscard]] constexpr bool holds(const event& value) noexcept {
  return std::holds_alternative<T>(value);
}

}  // namespace avionix
