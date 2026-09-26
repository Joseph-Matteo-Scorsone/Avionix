module;

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

export module avionix.object.input_decoder;

import avionix.entity.geometry;
import avionix.entity.unicode;
import avionix.entity.event;

// Decodes terminal input bytes into typed events.
//
//   raw bytes ──feed──► pending bytes ──decode_one──► event
//                            │
//                            └── bracketed paste accumulates until ESC[201~
//
// decode_one() is a pure function over a byte prefix. input_decoder adds
// the state that spans reads: incomplete sequences split across reads and
// paste bodies. A lone ESC is ambiguous (Escape key or the start of a
// sequence), so the caller calls flush() when no further input arrives
// within a short timeout.

export namespace avionix {

enum class decode_failure : std::uint8_t {
  incomplete,  // more bytes are needed; nothing was consumed
  malformed,   // `consumed` bytes form an invalid sequence and are dropped
};

struct decode_error {
  decode_failure kind{};
  std::size_t consumed{};

  friend constexpr bool operator==(const decode_error&,
                                   const decode_error&) noexcept = default;
};

struct decoded_input {
  std::size_t consumed{};
  std::optional<event> value{};  // empty for recognized-but-ignored sequences
  bool paste_begin{};            // ESC[200~
};

namespace detail {

constexpr std::size_t max_sequence_length = 64;

constexpr modifiers xterm_modifiers(std::uint32_t param) noexcept {
  if (param <= 1) return modifiers::none;
  const std::uint32_t bits = param - 1;
  modifiers m = modifiers::none;
  if ((bits & 1U) != 0) m |= modifiers::shift;
  if ((bits & 2U) != 0) m |= modifiers::alt;
  if ((bits & 4U) != 0) m |= modifiers::ctrl;
  if ((bits & 8U) != 0) m |= modifiers::super;
  return m;
}

// Parameters of a CSI sequence: up to 8 numeric fields separated by ';'.
// Sub-parameters after ':' are ignored.
struct csi_params {
  std::array<std::uint32_t, 8> values{};
  std::size_t count{};
  char prefix{};  // private marker such as '<' or '?'

  [[nodiscard]] constexpr std::uint32_t get(std::size_t i,
                                            std::uint32_t fallback) const noexcept {
    return i < count && values[i] != 0 ? values[i] : fallback;
  }
};

constexpr bool parse_params(std::string_view body, csi_params& out) noexcept {
  std::size_t i = 0;
  if (!body.empty() &&
      (body[0] == '<' || body[0] == '?' || body[0] == '>' || body[0] == '=')) {
    out.prefix = body[0];
    i = 1;
  }
  std::uint32_t current = 0;
  bool has_digit = false;
  bool in_sub = false;
  for (; i < body.size(); ++i) {
    const char c = body[i];
    if (c >= '0' && c <= '9') {
      if (!in_sub) {
        if (current > 100000) return false;
        current = current * 10 + static_cast<std::uint32_t>(c - '0');
        has_digit = true;
      }
    } else if (c == ';') {
      if (out.count < out.values.size()) out.values[out.count++] = current;
      current = 0;
      has_digit = false;
      in_sub = false;
    } else if (c == ':') {
      in_sub = true;
    } else {
      return false;
    }
  }
  if ((has_digit || !body.empty()) && out.count < out.values.size()) {
    out.values[out.count++] = current;
  }
  return true;
}

constexpr key_event make_key(key code, modifiers mods = modifiers::none) noexcept {
  return key_event{code, 0, mods};
}

constexpr key_event make_char(char32_t c, modifiers mods = modifiers::none) noexcept {
  return key_event{key::character, c, mods};
}

constexpr std::optional<key> tilde_key(std::uint32_t code) noexcept {
  switch (code) {
    case 1:
    case 7:
      return key::home;
    case 2:
      return key::insert;
    case 3:
      return key::del;
    case 4:
    case 8:
      return key::end;
    case 5:
      return key::page_up;
    case 6:
      return key::page_down;
    case 11:
      return key::f1;
    case 12:
      return key::f2;
    case 13:
      return key::f3;
    case 14:
      return key::f4;
    case 15:
      return key::f5;
    case 17:
      return key::f6;
    case 18:
      return key::f7;
    case 19:
      return key::f8;
    case 20:
      return key::f9;
    case 21:
      return key::f10;
    case 23:
      return key::f11;
    case 24:
      return key::f12;
    default:
      return std::nullopt;
  }
}

constexpr std::optional<key> letter_key(char final_byte) noexcept {
  switch (final_byte) {
    case 'A':
      return key::up;
    case 'B':
      return key::down;
    case 'C':
      return key::right;
    case 'D':
      return key::left;
    case 'H':
      return key::home;
    case 'F':
      return key::end;
    case 'P':
      return key::f1;
    case 'Q':
      return key::f2;
    case 'R':
      return key::f3;
    case 'S':
      return key::f4;
    default:
      return std::nullopt;
  }
}

constexpr key_event control_key(std::uint8_t byte) noexcept {
  switch (byte) {
    case 0x0D:
      return make_key(key::enter);
    case 0x0A:
      return make_key(key::enter);
    case 0x09:
      return make_key(key::tab);
    case 0x7F:
      return make_key(key::backspace);
    case 0x08:
      return make_key(key::backspace);
    case 0x1B:
      return make_key(key::escape);
    case 0x00:
      return make_char(U' ', modifiers::ctrl);
    default:
      break;
  }
  if (byte >= 0x01 && byte <= 0x1A) {
    return make_char(static_cast<char32_t>(U'a' + (byte - 1)), modifiers::ctrl);
  }
  // 0x1C-0x1F: Ctrl+\ ] ^ _
  return make_char(static_cast<char32_t>(U'\\' + (byte - 0x1C)), modifiers::ctrl);
}

constexpr std::optional<event> decode_csi(char final_byte,
                                          const csi_params& p) noexcept {
  if (p.prefix == '<' && (final_byte == 'M' || final_byte == 'm')) {
    if (p.count < 3) return std::nullopt;
    const std::uint32_t b = p.values[0];
    mouse_event m;
    m.where = {static_cast<std::int32_t>(p.values[1]) - 1,
               static_cast<std::int32_t>(p.values[2]) - 1};
    if ((b & 4U) != 0) m.mods |= modifiers::shift;
    if ((b & 8U) != 0) m.mods |= modifiers::alt;
    if ((b & 16U) != 0) m.mods |= modifiers::ctrl;
    const std::uint32_t low = b & 3U;
    if ((b & 64U) != 0) {
      m.button = low == 0   ? mouse_button::wheel_up
                 : low == 1 ? mouse_button::wheel_down
                 : low == 2 ? mouse_button::wheel_left
                            : mouse_button::wheel_right;
      m.action = mouse_action::press;
    } else {
      m.button = low == 0   ? mouse_button::left
                 : low == 1 ? mouse_button::middle
                 : low == 2 ? mouse_button::right
                            : mouse_button::none;
      if ((b & 32U) != 0) {
        m.action =
            m.button == mouse_button::none ? mouse_action::move : mouse_action::drag;
      } else {
        m.action = final_byte == 'm' ? mouse_action::release : mouse_action::press;
      }
    }
    return event{m};
  }
  if (p.prefix != 0) {
    return std::nullopt;  // private replies (e.g. DA) are ignored
  }
  switch (final_byte) {
    case '~': {
      const auto k = tilde_key(p.get(0, 0));
      if (!k) return std::nullopt;
      return event{make_key(*k, xterm_modifiers(p.get(1, 1)))};
    }
    case 'Z':
      return event{make_key(key::tab, modifiers::shift)};
    case 'I':
      return event{focus_event{true}};
    case 'O':
      return event{focus_event{false}};
    case 'u': {
      // CSI code ; modifiers u (fixterms / kitty basic form).
      const std::uint32_t code = p.get(0, 0);
      const modifiers mods = xterm_modifiers(p.get(1, 1));
      switch (code) {
        case 9:
          return event{make_key(key::tab, mods)};
        case 13:
          return event{make_key(key::enter, mods)};
        case 27:
          return event{make_key(key::escape, mods)};
        case 127:
          return event{make_key(key::backspace, mods)};
        default:
          if (code == 0 || code > 0x10FFFF) return std::nullopt;
          return event{make_char(static_cast<char32_t>(code), mods)};
      }
    }
    default: {
      const auto k = letter_key(final_byte);
      if (!k) return std::nullopt;
      return event{make_key(*k, xterm_modifiers(p.get(1, 1)))};
    }
  }
}

}  // namespace detail

// Decodes one event from the start of `bytes`.
//
// Returns decode_failure::incomplete when `bytes` is a strict prefix of a
// sequence (the caller should wait for more input or flush), and
// decode_failure::malformed with the number of bytes to drop for invalid
// input. Never consumes zero bytes on success.
[[nodiscard]] constexpr std::expected<decoded_input, decode_error> decode_one(
    std::string_view bytes) noexcept {
  using namespace detail;
  if (bytes.empty()) {
    return std::unexpected(decode_error{decode_failure::incomplete, 0});
  }
  const auto lead = static_cast<std::uint8_t>(bytes[0]);

  if (lead == 0x1B) {
    if (bytes.size() == 1) {
      return std::unexpected(decode_error{decode_failure::incomplete, 0});
    }
    const char introducer = bytes[1];
    if (introducer == '[') {
      std::size_t i = 2;
      while (i < bytes.size()) {
        const auto c = static_cast<std::uint8_t>(bytes[i]);
        if (c >= 0x40 && c <= 0x7E) break;  // final byte
        if (c < 0x20 || c > 0x3F) {         // not param/intermediate
          return std::unexpected(decode_error{decode_failure::malformed, i});
        }
        ++i;
        if (i > max_sequence_length) {
          return std::unexpected(decode_error{decode_failure::malformed, i});
        }
      }
      if (i >= bytes.size()) {
        return std::unexpected(decode_error{decode_failure::incomplete, 0});
      }
      const char final_byte = bytes[i];
      const std::string_view body = bytes.substr(2, i - 2);
      const std::size_t consumed = i + 1;
      // Intermediate bytes (0x20-0x2F) are not used by any input
      // sequence Avionix recognizes.
      if (body.find_first_of(" !\"#$%&'()*+,-./") != std::string_view::npos) {
        return decoded_input{consumed, std::nullopt, false};
      }
      csi_params params;
      if (!parse_params(body, params)) {
        return std::unexpected(decode_error{decode_failure::malformed, consumed});
      }
      if (final_byte == '~' && params.prefix == 0 && params.get(0, 0) == 200) {
        return decoded_input{consumed, std::nullopt, true};
      }
      return decoded_input{consumed, decode_csi(final_byte, params), false};
    }
    if (introducer == 'O') {
      if (bytes.size() < 3) {
        return std::unexpected(decode_error{decode_failure::incomplete, 0});
      }
      const auto k = letter_key(bytes[2]);
      if (!k) {
        return std::unexpected(decode_error{decode_failure::malformed, 3});
      }
      return decoded_input{3, event{make_key(*k)}, false};
    }
    if (static_cast<std::uint8_t>(introducer) == 0x1B) {
      return decoded_input{1, event{make_key(key::escape)}, false};
    }
    // ESC followed by a key: Alt+key.
    auto inner = decode_one(bytes.substr(1));
    if (!inner) {
      if (inner.error().kind == decode_failure::incomplete) {
        return std::unexpected(inner.error());
      }
      return std::unexpected(
          decode_error{decode_failure::malformed, 1 + inner.error().consumed});
    }
    if (inner->value) {
      if (auto* k = std::get_if<key_event>(&*inner->value)) {
        k->mods |= modifiers::alt;
      }
    }
    inner->consumed += 1;
    return inner;
  }

  if (lead < 0x20 || lead == 0x7F) {
    return decoded_input{1, event{control_key(lead)}, false};
  }

  if (lead < 0x80) {
    return decoded_input{1, event{make_char(lead)}, false};
  }

  const auto cp = decode_utf8(bytes);
  if (!cp.valid) {
    // A valid lead byte with too few bytes available is a split read,
    // not an error.
    const std::size_t needed = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
    if (lead >= 0xC2 && lead <= 0xF4 && bytes.size() < needed &&
        cp.length == bytes.size()) {
      return std::unexpected(decode_error{decode_failure::incomplete, 0});
    }
    return std::unexpected(decode_error{decode_failure::malformed, cp.length});
  }
  return decoded_input{cp.length, event{make_char(cp.value)}, false};
}

class input_decoder {
 public:
  static constexpr std::size_t default_max_paste_bytes = 8 * 1024 * 1024;

  explicit input_decoder(std::size_t max_paste_bytes = default_max_paste_bytes)
      : max_paste_bytes_{max_paste_bytes} {
    pending_.reserve(256);
  }

  // Decodes as much of `bytes` as possible, appending events to `out`.
  // Incomplete trailing sequences are kept for the next call.
  void feed(std::string_view bytes, std::vector<event>& out) {
    pending_.append(bytes);
    std::size_t offset = 0;
    while (offset < pending_.size()) {
      if (in_paste_) {
        offset = consume_paste(offset, out);
        if (in_paste_) break;
        continue;
      }
      const auto step = decode_one(std::string_view{pending_}.substr(offset));
      if (!step) {
        if (step.error().kind == decode_failure::incomplete) break;
        ++malformed_;
        offset += step.error().consumed;
        continue;
      }
      offset += step->consumed;
      if (step->paste_begin) {
        in_paste_ = true;
        paste_.clear();
        paste_discarded_ = false;
      } else if (step->value) {
        out.push_back(std::move(*step->value));
      }
    }
    pending_.erase(0, offset);
  }

  // Resolves pending bytes after the input went quiet: a lone ESC becomes
  // the Escape key and truncated sequences are dropped. An unterminated
  // paste stays pending.
  void flush(std::vector<event>& out) {
    if (in_paste_) {
      return;
    }
    while (!pending_.empty()) {
      if (pending_[0] == '\x1b' && pending_.size() == 1) {
        out.push_back(event{key_event{key::escape, 0, modifiers::none}});
        pending_.clear();
        return;
      }
      if (pending_[0] == '\x1b' && pending_.size() == 2) {
        // ESC + '[' or 'O' typed quickly: Alt+'[' / Alt+'O'.
        out.push_back(event{key_event{
            key::character, static_cast<char32_t>(pending_[1]), modifiers::alt}});
        pending_.clear();
        return;
      }
      ++malformed_;
      pending_.clear();
    }
  }

  [[nodiscard]] bool has_pending() const noexcept {
    return !pending_.empty() && !in_paste_;
  }
  [[nodiscard]] bool in_paste() const noexcept { return in_paste_; }
  [[nodiscard]] std::uint64_t malformed_count() const noexcept { return malformed_; }

 private:
  static constexpr std::string_view paste_end = "\x1b[201~";

  void append_paste(std::string_view bytes) {
    if (paste_discarded_) return;
    if (bytes.size() > max_paste_bytes_ - paste_.size()) {
      paste_.clear();
      paste_discarded_ = true;
      ++malformed_;
      return;
    }
    paste_.append(bytes);
  }

  std::size_t consume_paste(std::size_t offset, std::vector<event>& out) {
    const std::string_view rest = std::string_view{pending_}.substr(offset);
    const std::size_t end = rest.find(paste_end);
    if (end != std::string_view::npos) {
      append_paste(rest.substr(0, end));
      if (!paste_discarded_) {
        out.push_back(event{paste_event{std::move(paste_)}});
      }
      paste_.clear();
      in_paste_ = false;
      paste_discarded_ = false;
      return offset + end + paste_end.size();
    }
    // Keep a possible partial end marker in pending_.
    const std::size_t keep = std::min(rest.size(), paste_end.size() - 1);
    append_paste(rest.substr(0, rest.size() - keep));
    return pending_.size() - keep;
  }

  std::string pending_{};
  std::string paste_{};
  std::size_t max_paste_bytes_;
  bool in_paste_{};
  bool paste_discarded_{};
  std::uint64_t malformed_{};
};

}  // namespace avionix
