module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

export module avionix.entity.cell;

export import avionix.entity.style;
import avionix.entity.unicode;

export namespace avionix {

// One terminal cell: a grapheme cluster stored inline plus its style.
//
// Invariants:
// - A leading cell has width 1 or 2 and holds a printable grapheme.
// - The cell to the right of a width-2 cell is a continuation cell:
//   width 0, empty text, same style. Continuations are never emitted; the
//   terminal fills them when it draws the wide leading cell.
// - Unused bytes of `bytes_` are zero, so equal cells are equal byte for
//   byte. operator== and the renderer rely on this.
//
// Grapheme bytes are stored inline to keep per-cell operations free of heap
// allocation. Clusters longer than `capacity` bytes are stored as U+FFFD
// with the cluster's width, which keeps layout stable.
class cell {
 public:
  static constexpr std::size_t capacity = 26;

  // A blank cell (a single space) with the default style.
  constexpr cell() noexcept { bytes_[0] = ' '; }

  [[nodiscard]] static constexpr cell blank(style appearance = {}) noexcept {
    cell value;
    value.style_ = appearance;
    return value;
  }

  // Builds a leading cell from one grapheme cluster. `width` must be 1 or
  // 2; callers get it from grapheme_width(). The text is assumed to be one
  // printable cluster.
  [[nodiscard]] static constexpr cell from_grapheme(std::string_view text,
                                                    std::uint8_t width,
                                                    style appearance = {}) noexcept {
    cell value;
    value.style_ = appearance;
    value.width_ = width;
    if (text.size() <= capacity) {
      value.bytes_[0] = 0;
      std::ranges::copy(text, value.bytes_.begin());
      value.length_ = static_cast<std::uint8_t>(text.size());
    } else {
      value.bytes_ = {};
      std::array<char, 4> encoded{};
      const auto n = encode_utf8(replacement_character, encoded);
      std::ranges::copy(std::string_view{encoded.data(), n}, value.bytes_.begin());
      value.length_ = static_cast<std::uint8_t>(n);
    }
    return value;
  }

  [[nodiscard]] static constexpr cell from_ascii(char c,
                                                 style appearance = {}) noexcept {
    cell value;
    value.style_ = appearance;
    value.bytes_[0] = c;
    return value;
  }

  // Right half of a wide cell.
  [[nodiscard]] static constexpr cell continuation(style appearance = {}) noexcept {
    cell value;
    value.bytes_[0] = 0;
    value.length_ = 0;
    value.width_ = 0;
    value.style_ = appearance;
    return value;
  }

  [[nodiscard]] constexpr std::string_view text() const noexcept {
    return {bytes_.data(), length_};
  }

  [[nodiscard]] constexpr std::uint8_t width() const noexcept { return width_; }
  [[nodiscard]] constexpr bool is_continuation() const noexcept { return width_ == 0; }
  [[nodiscard]] constexpr bool is_wide() const noexcept { return width_ == 2; }

  [[nodiscard]] constexpr const avionix::style& appearance() const noexcept {
    return style_;
  }
  constexpr void set_appearance(const avionix::style& value) noexcept {
    style_ = value;
  }

  // 0 means the cell is not a hyperlink. Any other value is an index into
  // the render buffer's link table. The id is part of the cell's value, so
  // a link change is a cell change and the diff redraws it.
  [[nodiscard]] constexpr std::uint16_t link() const noexcept { return link_; }
  constexpr void set_link(std::uint16_t id) noexcept { link_ = id; }

  // Cells have no padding (checked below), so byte equality is value
  // equality. The diff compares every cell every frame; memcmp is several
  // times faster than member-wise comparison of the inline glyph array.
  friend constexpr bool operator==(const cell& a, const cell& b) noexcept {
    if consteval {
      return a.bytes_ == b.bytes_ && a.length_ == b.length_ && a.width_ == b.width_ &&
             a.style_ == b.style_ && a.link_ == b.link_;
    } else {
      return std::memcmp(&a, &b, sizeof(cell)) == 0;
    }
  }

 private:
  std::array<char, capacity> bytes_{};
  std::uint8_t length_{1};
  std::uint8_t width_{1};
  avionix::style style_{};
  std::uint16_t link_{};
};

static_assert(std::has_unique_object_representations_v<cell>,
              "cell must have no padding: operator== and the renderer compare bytes");
static_assert(std::is_trivially_copyable_v<cell>);

}  // namespace avionix
