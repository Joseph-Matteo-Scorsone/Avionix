module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

export module avionix.object.buffer;

import avionix.entity.geometry;
import avionix.entity.style;
import avionix.entity.unicode;
import avionix.entity.cell;

export namespace avionix {

// A virtual terminal screen: width * height cells in one contiguous vector,
// indexed y * width + x.
//
// Owns its cells. Spans returned by row() and cells() are invalidated by
// resize(). Every write keeps the wide-cell invariant from
// avionix.entity.cell: a width-2 cell is always followed by exactly one
// continuation cell, and a continuation never appears elsewhere. Writes
// that would split a wide cell blank its other half.
//
// Not thread safe. The UI thread owns render buffers.
class render_buffer {
 public:
  render_buffer() = default;

  explicit render_buffer(avionix::size extent, const cell& fill = cell::blank()) {
    resize(extent, fill);
  }

  // Changes the dimensions and fills every cell with `fill`. Content is
  // not preserved: after a resize every widget redraws anyway.
  void resize(avionix::size extent, const cell& fill = cell::blank()) {
    extent_ = extent;
    cells_.assign(static_cast<std::size_t>(extent.area()), fill);
  }

  void clear(const cell& fill = cell::blank()) noexcept {
    std::ranges::fill(cells_, fill);
  }

  [[nodiscard]] avionix::size extent() const noexcept { return extent_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return extent_.width; }
  [[nodiscard]] std::uint32_t height() const noexcept { return extent_.height; }
  [[nodiscard]] rect area() const noexcept { return bounds(extent_); }

  [[nodiscard]] std::span<const cell> cells() const noexcept { return cells_; }

  [[nodiscard]] std::span<cell> row(std::uint32_t y) noexcept {
    return std::span<cell>{cells_}.subspan(std::size_t{y} * extent_.width,
                                           extent_.width);
  }

  [[nodiscard]] std::span<const cell> row(std::uint32_t y) const noexcept {
    return std::span<const cell>{cells_}.subspan(std::size_t{y} * extent_.width,
                                                 extent_.width);
  }

  // Precondition: area().contains(p).
  [[nodiscard]] const cell& at(position p) const noexcept { return cells_[index(p)]; }

  [[nodiscard]] bool contains(position p) const noexcept { return area().contains(p); }

  // Writes one grapheme cluster at `p`, clipped to `clip` and the buffer.
  // Returns the number of columns the grapheme occupies (its width), even
  // when clipped, so callers can advance consistently. Width-0 clusters
  // and controls are not drawn and return 0. A wide cluster whose right
  // half would be clipped is drawn as a blank cell instead.
  std::uint8_t put_grapheme(position p, std::string_view cluster, std::uint8_t width,
                            const style& appearance, const rect& clip) {
    if (width == 0 || cluster.empty()) {
      return 0;
    }
    const rect limit = intersect(clip, area());
    if (!limit.contains(p)) {
      return width;
    }
    if (width == 2) {
      const position right{p.x + 1, p.y};
      if (!limit.contains(right)) {
        store(p, cell::blank(appearance));
        return width;
      }
      store(p, cell::from_grapheme(cluster, 2, appearance));
      store_continuation(right, appearance);
      return width;
    }
    store(p, cell::from_grapheme(cluster, width, appearance));
    return width;
  }

  // Writes UTF-8 text starting at `start` on a single row, clipped to
  // `clip`. Newlines are not interpreted. Tab is drawn as one space and
  // other controls are skipped so they can never reach the terminal.
  // Returns the number of columns advanced.
  std::uint32_t put_text(position start, std::string_view text, const style& appearance,
                         const rect& clip) {
    const rect limit = intersect(clip, area());
    if (limit.empty() || start.y < limit.top() || start.y >= limit.bottom()) {
      return static_cast<std::uint32_t>(display_width(text));
    }
    position cursor = start;
    std::size_t offset = 0;
    while (offset < text.size()) {
      const auto byte = static_cast<unsigned char>(text[offset]);
      // ASCII fast path: printable ASCII not followed by a combining
      // sequence is one narrow cell.
      if (byte >= 0x20 && byte < 0x7F &&
          (offset + 1 == text.size() ||
           static_cast<unsigned char>(text[offset + 1]) < 0x80)) {
        if (limit.contains(cursor)) {
          store(cursor, cell::from_ascii(static_cast<char>(byte), appearance));
        }
        ++cursor.x;
        ++offset;
        if (cursor.x >= limit.right()) {
          cursor.x += static_cast<std::int32_t>(display_width(text.substr(offset)));
          break;
        }
        continue;
      }
      const std::size_t next = next_grapheme_boundary(text, offset);
      const std::string_view cluster = text.substr(offset, next - offset);
      offset = next;
      if (cluster == "\t") {
        if (limit.contains(cursor)) {
          store(cursor, cell::blank(appearance));
        }
        ++cursor.x;
        continue;
      }
      const auto first = decode_utf8(cluster);
      if (is_control(first.value)) {
        continue;
      }
      cursor.x +=
          put_grapheme(cursor, cluster, grapheme_width(cluster), appearance, limit);
    }
    return static_cast<std::uint32_t>(std::max(0, cursor.x - start.x));
  }

  // Sets every cell in `region` (clipped) to `value`, which must be a
  // narrow cell.
  void fill(const rect& region, const cell& value) {
    const rect r = intersect(region, area());
    if (r.empty()) {
      return;
    }
    for (std::int32_t y = r.top(); y < r.bottom(); ++y) {
      repair_left_edge({r.left(), y});
      repair_right_edge({r.right() - 1, y});
      auto line = row(static_cast<std::uint32_t>(y))
                      .subspan(static_cast<std::size_t>(r.left()), r.width());
      std::ranges::fill(line, value);
    }
  }

  // Applies a style patch to every cell in `region` without changing
  // text. Continuation cells take the patched style of their leading cell.
  void apply_style(const rect& region, const style_patch& patch) {
    const rect r = intersect(region, area());
    for (std::int32_t y = r.top(); y < r.bottom(); ++y) {
      for (std::int32_t x = r.left(); x < r.right(); ++x) {
        cell& c = cells_[index({x, y})];
        c.set_appearance(apply(c.appearance(), patch));
      }
    }
  }

  // Replaces one cell, repairing wide-cell neighbors. `value` must not be a
  // continuation; wide cells need a free cell to their right in bounds.
  void set(position p, const cell& value) {
    if (!contains(p)) {
      return;
    }
    if (value.is_wide()) {
      if (!contains({p.x + 1, p.y})) {
        store(p, cell::blank(value.appearance()));
        return;
      }
      store(p, value);
      store_continuation({p.x + 1, p.y}, value.appearance());
      return;
    }
    store(p, value);
  }

 private:
  [[nodiscard]] std::size_t index(position p) const noexcept {
    return static_cast<std::size_t>(p.y) * extent_.width +
           static_cast<std::size_t>(p.x);
  }

  // If p is the right half of a wide cell, blank the left half.
  void repair_left_edge(position p) noexcept {
    cell& c = cells_[index(p)];
    if (c.is_continuation() && p.x > 0) {
      cell& lead = cells_[index({p.x - 1, p.y})];
      lead = cell::blank(lead.appearance());
    }
  }

  // If p is the left half of a wide cell, blank the right half.
  void repair_right_edge(position p) noexcept {
    cell& c = cells_[index(p)];
    if (c.is_wide() && p.x + 1 < static_cast<std::int32_t>(extent_.width)) {
      cell& tail = cells_[index({p.x + 1, p.y})];
      tail = cell::blank(tail.appearance());
    }
  }

  void store(position p, const cell& value) noexcept {
    repair_left_edge(p);
    repair_right_edge(p);
    cells_[index(p)] = value;
  }

  void store_continuation(position p, const style& appearance) noexcept {
    // p is the right half of the wide cell just stored at p.x - 1, so only
    // the right edge needs repair.
    repair_right_edge(p);
    cells_[index(p)] = cell::continuation(appearance);
  }

  avionix::size extent_{};
  std::vector<cell> cells_{};
};

}  // namespace avionix
