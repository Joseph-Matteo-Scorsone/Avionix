module;

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

export module avionix.object.ansi_encoder;

import avionix.entity.geometry;
import avionix.entity.color;
import avionix.entity.style;

// Encodes cursor movement, SGR state, and text into one contiguous byte
// buffer. The encoder tracks the terminal's cursor and pen so it can skip
// redundant sequences and pick the shortest cursor movement.
//
// The buffer is reused across frames; clear() keeps its capacity, so a
// steady-state frame performs no allocation.

export namespace avionix {

class ansi_encoder {
 public:
  explicit ansi_encoder(color_depth depth = color_depth::truecolor) : depth_{depth} {
    out_.reserve(16 * 1024);
  }

  void set_color_depth(color_depth depth) noexcept {
    depth_ = depth;
    pen_.reset();
  }

  [[nodiscard]] color_depth depth() const noexcept { return depth_; }

  // Width of the physical screen. Needed to detect the pending-wrap state
  // after writing the last column.
  void set_screen_width(std::uint32_t width) noexcept { screen_width_ = width; }

  // Forget cursor and pen state, e.g. after something else wrote to the
  // terminal. The next move and style change are emitted in full.
  void forget_state() noexcept {
    cursor_.reset();
    pen_.reset();
    // The terminal's hyperlink state is unknown after a full reset. The
    // next set_link emits a close before trusting current_link_.
    link_unknown_ = true;
    current_link_.clear();
  }

  // OSC 8 hyperlink. An empty URL closes the current link. The terminal
  // keeps the link on text already written; this only affects text written
  // afterwards. Controls and DEL are omitted so a URL cannot inject ESC.
  void set_link(std::string_view url) {
    if (!link_unknown_ && url == current_link_) return;
    const bool open = has_link_text(url);
    if (link_unknown_ || !current_link_.empty()) out_.append("\x1b]8;;\x1b\\");
    link_unknown_ = false;
    if (!open) {
      current_link_.clear();
      return;
    }
    current_link_.assign(url.begin(), url.end());
    out_.append("\x1b]8;;");
    for (const char byte : url) {
      const auto c = static_cast<unsigned char>(byte);
      if (c >= 0x20 && c != 0x7F) out_.push_back(byte);
    }
    out_.append("\x1b\\");
  }

  void move_to(position target) {
    if (cursor_ && *cursor_ == target) {
      return;
    }
    std::array<char, 32> best{};
    std::size_t best_length = encode_cup(target, best);

    if (cursor_) {
      std::array<char, 32> candidate{};
      std::size_t length = 0;
      if (cursor_->y == target.y) {
        if (target.x > cursor_->x) {
          length = encode_relative(candidate, 'C', target.x - cursor_->x);
        } else if (target.x == 0) {
          candidate[0] = '\r';
          length = 1;
        } else {
          length = encode_relative(candidate, 'D', cursor_->x - target.x);
        }
      } else if (target.x == cursor_->x) {
        length = target.y > cursor_->y
                     ? encode_relative(candidate, 'B', target.y - cursor_->y)
                     : encode_relative(candidate, 'A', cursor_->y - target.y);
      } else if (target.x == 0) {
        candidate[0] = '\r';
        length = 1 + (target.y > cursor_->y
                          ? encode_relative(std::span<char>{candidate}.subspan(1), 'B',
                                            target.y - cursor_->y)
                          : encode_relative(std::span<char>{candidate}.subspan(1), 'A',
                                            cursor_->y - target.y));
      }
      if (length != 0 && length < best_length) {
        best = candidate;
        best_length = length;
      }
    }
    out_.append(best.data(), best_length);
    cursor_ = target;
  }

  void set_style(const style& requested) {
    const style next{downgrade(requested.foreground, depth_),
                     downgrade(requested.background, depth_), requested.attributes};
    if (pen_ && *pen_ == next) {
      return;
    }

    // Candidate A: incremental change from the current pen.
    // Candidate B: reset (SGR 0) followed by the full style.
    std::array<char, 96> full{};
    std::size_t full_length = 0;
    {
      params p{full};
      p.add(0);
      append_attributes_on(p, next.attributes);
      if (!next.foreground.is_default()) append_color(p, next.foreground, false);
      if (!next.background.is_default()) append_color(p, next.background, true);
      full_length = p.finish();
    }

    if (pen_) {
      std::array<char, 96> delta{};
      params p{delta};
      const attribute removed = pen_->attributes & ~next.attributes;
      attribute to_add = next.attributes & ~pen_->attributes;
      // Bold and dim share one "off" code (22); turning off either one
      // turns off both, so the survivor must be re-added.
      if (has(removed, attribute::bold) || has(removed, attribute::dim)) {
        p.add(22);
        to_add |= next.attributes & (attribute::bold | attribute::dim);
      }
      if (has(removed, attribute::italic)) p.add(23);
      if (has(removed, attribute::underline)) p.add(24);
      if (has(removed, attribute::blink)) p.add(25);
      if (has(removed, attribute::reverse)) p.add(27);
      if (has(removed, attribute::hidden)) p.add(28);
      if (has(removed, attribute::strikethrough)) p.add(29);
      append_attributes_on(p, to_add);
      if (pen_->foreground != next.foreground) append_color(p, next.foreground, false);
      if (pen_->background != next.background) append_color(p, next.background, true);
      const std::size_t delta_length = p.finish();
      if (delta_length < full_length) {
        out_.append(delta.data(), delta_length);
        pen_ = next;
        return;
      }
    }
    out_.append(full.data(), full_length);
    pen_ = next;
  }

  // Writes one grapheme occupying `width` cells at the current cursor.
  void write_glyph(std::string_view text, std::uint8_t width) {
    out_.append(text);
    if (cursor_) {
      cursor_->x += width;
      // After writing the last column the terminal is in the
      // pending-wrap state; the next relative move is unreliable.
      if (screen_width_ != 0 &&
          cursor_->x >= static_cast<std::int32_t>(screen_width_)) {
        cursor_.reset();
      }
    }
  }

  void reset_style() {
    out_.append("\x1b[0m");
    pen_ = style{};
  }

  void clear_screen() {
    reset_style();
    out_.append("\x1b[2J");
  }

  void set_cursor_visible(bool visible) {
    out_.append(visible ? "\x1b[?25h" : "\x1b[?25l");
  }

  // DEC mode 2026: the terminal holds presentation until the end marker,
  // so a frame never appears half drawn.
  void begin_synchronized_update() { out_.append("\x1b[?2026h"); }
  void end_synchronized_update() { out_.append("\x1b[?2026l"); }

  void append_raw(std::string_view bytes) { out_.append(bytes); }

  [[nodiscard]] std::string_view bytes() const noexcept { return out_; }
  [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }
  [[nodiscard]] bool empty() const noexcept { return out_.empty(); }
  void clear() noexcept { out_.clear(); }

  [[nodiscard]] std::optional<position> cursor() const noexcept { return cursor_; }

 private:
  // Accumulates SGR parameters into "\x1b[a;b;cm" without allocation.
  class params {
   public:
    explicit params(std::span<char> storage) noexcept : storage_{storage} {
      storage_[0] = '\x1b';
      storage_[1] = '[';
      length_ = 2;
    }

    void add(std::uint32_t value) noexcept {
      if (count_ > 0) storage_[length_++] = ';';
      const auto result = std::to_chars(storage_.data() + length_,
                                        storage_.data() + storage_.size(), value);
      length_ = static_cast<std::size_t>(result.ptr - storage_.data());
      ++count_;
    }

    [[nodiscard]] std::size_t finish() noexcept {
      if (count_ == 0) return 0;
      storage_[length_++] = 'm';
      return length_;
    }

   private:
    std::span<char> storage_;
    std::size_t length_{};
    std::size_t count_{};
  };

  [[nodiscard]] static bool has_link_text(std::string_view url) noexcept {
    for (const char byte : url) {
      const auto c = static_cast<unsigned char>(byte);
      if (c >= 0x20 && c != 0x7F) return true;
    }
    return false;
  }

  static void append_attributes_on(params& p, attribute set) {
    if (has(set, attribute::bold)) p.add(1);
    if (has(set, attribute::dim)) p.add(2);
    if (has(set, attribute::italic)) p.add(3);
    if (has(set, attribute::underline)) p.add(4);
    if (has(set, attribute::blink)) p.add(5);
    if (has(set, attribute::reverse)) p.add(7);
    if (has(set, attribute::hidden)) p.add(8);
    if (has(set, attribute::strikethrough)) p.add(9);
  }

  static void append_color(params& p, color value, bool background) {
    const std::uint32_t base = background ? 40 : 30;
    switch (value.type()) {
      case color::kind::terminal_default:
        p.add(base + 9);
        return;
      case color::kind::indexed:
        if (value.index() < 8) {
          p.add(base + value.index());
        } else if (value.index() < 16) {
          p.add(base + 60 + (value.index() - 8U));
        } else {
          p.add(base + 8);
          p.add(5);
          p.add(value.index());
        }
        return;
      case color::kind::rgb:
        p.add(base + 8);
        p.add(2);
        p.add(value.red());
        p.add(value.green());
        p.add(value.blue());
        return;
    }
  }

  static std::size_t encode_cup(position target, std::span<char> out) noexcept {
    std::size_t n = 0;
    out[n++] = '\x1b';
    out[n++] = '[';
    const auto row = static_cast<std::uint32_t>(target.y + 1);
    auto r = std::to_chars(out.data() + n, out.data() + out.size(), row);
    n = static_cast<std::size_t>(r.ptr - out.data());
    if (target.x != 0) {
      out[n++] = ';';
      r = std::to_chars(out.data() + n, out.data() + out.size(),
                        static_cast<std::uint32_t>(target.x + 1));
      n = static_cast<std::size_t>(r.ptr - out.data());
    }
    out[n++] = 'H';
    return n;
  }

  static std::size_t encode_relative(std::span<char> out, char final_byte,
                                     std::int32_t amount) noexcept {
    std::size_t n = 0;
    out[n++] = '\x1b';
    out[n++] = '[';
    if (amount != 1) {
      const auto r = std::to_chars(out.data() + n, out.data() + out.size(),
                                   static_cast<std::uint32_t>(amount));
      n = static_cast<std::size_t>(r.ptr - out.data());
    }
    out[n++] = final_byte;
    return n;
  }

  std::string out_;
  std::string current_link_{};
  color_depth depth_;
  std::uint32_t screen_width_{};
  std::optional<position> cursor_{};
  std::optional<style> pen_{};
  bool link_unknown_{};
};

}  // namespace avionix
