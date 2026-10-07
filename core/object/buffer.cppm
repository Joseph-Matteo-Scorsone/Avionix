module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
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
    links_.clear();
  }

  // Replaces every cell. Link ids in `fill` are kept on the cells, and the
  // link table is dropped, so a non-zero id in `fill` no longer resolves.
  void clear(const cell& fill = cell::blank()) noexcept {
    std::ranges::fill(cells_, fill);
    links_.clear();
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

  [[nodiscard]] std::vector<std::string> rows() const {
    std::vector<std::string> result(height());
    for (std::uint32_t y = 0; y < height(); ++y)
      for (const auto& c : row(y))
        if (!c.is_continuation()) result[y].append(c.text());
    return result;
  }
  [[nodiscard]] std::optional<position> find(std::string_view text) const {
    for (std::uint32_t y = 0; y < height(); ++y) {
      std::string line;
      std::vector<std::int32_t> columns;
      for (std::uint32_t x = 0; x < width(); ++x) {
        const auto& c =
            at({static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)});
        if (c.is_continuation()) continue;
        line.append(c.text());
        columns.insert(columns.end(), c.text().size(), static_cast<std::int32_t>(x));
      }
      const auto offset = line.find(text);
      if (offset != std::string::npos && offset < columns.size())
        return position{columns[offset], static_cast<std::int32_t>(y)};
    }
    return std::nullopt;
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
                            const style& appearance, const rect& clip,
                            std::uint16_t link = 0) {
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
        cell blank = cell::blank(appearance);
        blank.set_link(link);
        store(p, blank);
        return width;
      }
      cell lead = cell::from_grapheme(cluster, 2, appearance);
      lead.set_link(link);
      store(p, lead);
      store_continuation(right, appearance);
      return width;
    }
    cell lead = cell::from_grapheme(cluster, width, appearance);
    lead.set_link(link);
    store(p, lead);
    return width;
  }

  // Writes UTF-8 text starting at `start` on a single row, clipped to
  // `clip`. Newlines are not interpreted. Tab is drawn as one space and
  // other controls are skipped so they can never reach the terminal.
  // Returns the number of columns advanced.
  // `link` is an id from intern_link(), or 0 for ordinary text. Every
  // cell written for this call, including wide-cell continuations, stores it.
  std::uint32_t put_text(position start, std::string_view text, const style& appearance,
                         const rect& clip, std::uint16_t link = 0) {
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
          cell narrow = cell::from_ascii(static_cast<char>(byte), appearance);
          narrow.set_link(link);
          store(cursor, narrow);
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
          cell blank = cell::blank(appearance);
          blank.set_link(link);
          store(cursor, blank);
        }
        ++cursor.x;
        continue;
      }
      const auto first = decode_utf8(cluster);
      if (is_control(first.value)) {
        continue;
      }
      cursor.x += put_grapheme(cursor, cluster, grapheme_width(cluster), appearance,
                               limit, link);
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

  // Registers `url` for this buffer and returns a non-zero cell link id.
  // The same URL in this table returns the same id. An empty URL, or a
  // table that already holds 65535 URLs, returns 0 and the text is drawn
  // as ordinary cells. clear() drops the table; adopt_links() replaces it.
  [[nodiscard]] std::uint16_t intern_link(std::string_view url) {
    if (url.empty() || url.size() > 4096) return 0;
    for (std::size_t i = 0; i < links_.size(); ++i) {
      if (links_[i] == url) return static_cast<std::uint16_t>(i + 1);
    }
    if (links_.size() >= 65535) return 0;
    links_.emplace_back(url);
    return static_cast<std::uint16_t>(links_.size());
  }

  // The URL for a cell link id, or empty when `id` is 0 or no longer in
  // the table. The view is invalidated by clear(), resize(), intern_link()
  // when it grows the table, and adopt_links().
  [[nodiscard]] std::string_view link_target(std::uint16_t id) const noexcept {
    if (id == 0 || id > links_.size()) return {};
    return links_[id - 1];
  }

  // Copies link identity from another buffer so a URL keeps the id it had
  // there. The renderer does this after clearing the back buffer and before
  // widgets draw, so a link that changes URL is a cell change.
  void adopt_links(const render_buffer& source) { links_ = source.links_; }

  // Applies a style patch to every cell in `region` without changing
  // text or link ids. Continuation cells take the patched style of their
  // leading cell.
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
    // the right edge needs repair. The continuation keeps the lead cell's
    // link id so both halves describe the same hyperlink.
    repair_right_edge(p);
    cell tail = cell::continuation(appearance);
    if (p.x > 0) tail.set_link(cells_[index({p.x - 1, p.y})].link());
    cells_[index(p)] = tail;
  }

  avionix::size extent_{};
  std::vector<cell> cells_{};
  // URLs addressed by cell::link(). Index 0 in a cell means "no link",
  // so links_[0] is cell id 1.
  std::vector<std::string> links_{};
};

}  // namespace avionix
