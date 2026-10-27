module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module avionix.object.renderer;

import avionix.entity.geometry;
import avionix.entity.color;
import avionix.entity.style;
import avionix.entity.cell;
import avionix.object.buffer;
import avionix.object.ansi_encoder;
import avionix.entity.terminal_graphics;

// Double-buffered diff renderer.
//
//   widgets ──draw──► back buffer
//   back vs front ──collect_changed_runs──► changed runs
//   changed runs ──ansi_encoder──► one contiguous byte buffer
//   swap: back becomes the new front
//
// Pixel images (graphics_placement) are written after the changed runs in
// the same synchronized update. The renderer remembers what it showed and
// re-sends an image only when its payload or area changed, the screen was
// cleared, or, for Sixel and iTerm2 images that live in the cells, a run
// overwrote part of it. A Sixel or iTerm2 image that disappears or moves
// has its old cells repainted; a Kitty image is deleted by id.
//
// The renderer never writes to the terminal. task.render hands the bytes to
// terminal_session, which keeps terminal I/O out of this object and lets
// tests inspect the exact output.

export namespace avionix {

// Half-open column range [begin, end) on row `y` whose contents changed.
struct changed_run {
  std::uint32_t y{};
  std::uint32_t begin{};
  std::uint32_t end{};

  friend constexpr bool operator==(const changed_run&,
                                   const changed_run&) noexcept = default;
};

struct diff_statistics {
  std::uint64_t cells_inspected{};
  std::uint64_t cells_changed{};
};

// Unchanged gap, in cells, below which two runs on a row are merged.
// Rewriting a short gap costs about one byte per cell, while a cursor jump
// (CSI n C or CUP) costs 3-8 bytes plus a possible SGR change.
inline constexpr std::uint32_t default_merge_gap = 4;

namespace detail {

// Byte equality over a whole row. Variable-length memcmp is a library call
// on some C runtimes (MinGW's msvcrt is byte-at-a-time); comparing 8-byte
// words with an accumulated XOR lets the compiler vectorize the loop.
inline bool same_bytes(const void* a, const void* b, std::size_t n) noexcept {
  const auto* pa = static_cast<const unsigned char*>(a);
  const auto* pb = static_cast<const unsigned char*>(b);
  std::size_t i = 0;
  for (; i + 32 <= n; i += 32) {
    std::uint64_t diff = 0;
    for (std::size_t k = 0; k < 32; k += 8) {
      std::uint64_t wa = 0;
      std::uint64_t wb = 0;
      std::memcpy(&wa, pa + i + k, 8);
      std::memcpy(&wb, pb + i + k, 8);
      diff |= wa ^ wb;
    }
    if (diff != 0) return false;
  }
  for (; i < n; ++i) {
    if (pa[i] != pb[i]) return false;
  }
  return true;
}

}  // namespace detail

// Appends the runs where `back` differs from `front` to `out`. Both buffers
// must have the same extent. Runs never start on a continuation cell or end
// between a wide cell and its continuation, so each run can be emitted on
// its own.
diff_statistics collect_changed_runs(const render_buffer& front,
                                     const render_buffer& back,
                                     std::vector<changed_run>& out,
                                     std::uint32_t merge_gap = default_merge_gap) {
  diff_statistics stats;
  const std::uint32_t width = back.width();
  for (std::uint32_t y = 0; y < back.height(); ++y) {
    const std::span<const cell> old_row = front.row(y);
    const std::span<const cell> new_row = back.row(y);
    stats.cells_inspected += width;
    // Most rows are unchanged between frames. One contiguous compare per
    // row skips them before the per-cell scan (cells are padding-free,
    // see avionix.entity.cell).
    if (detail::same_bytes(old_row.data(), new_row.data(), old_row.size_bytes())) {
      continue;
    }

    std::optional<changed_run> open;
    for (std::uint32_t x = 0; x < width; ++x) {
      if (old_row[x] == new_row[x]) {
        continue;
      }
      ++stats.cells_changed;
      std::uint32_t begin = x;
      if (new_row[x].is_continuation() && begin > 0) {
        --begin;
      }
      std::uint32_t end = x + 1;
      if (new_row[x].is_wide() && end < width) {
        ++end;
      }
      if (open && begin <= open->end + merge_gap) {
        open->end = std::max(open->end, end);
      } else {
        if (open) out.push_back(*open);
        open = changed_run{y, begin, end};
      }
    }
    if (open) out.push_back(*open);
  }
  return stats;
}

// One pixel image for this frame.
struct graphics_placement {
  std::uint32_t id{};       // nonzero, stable while the image stays on screen
  std::uint64_t version{};  // changes whenever `payload` draws something else
  rect area{};              // cells the image covers
  graphics_protocol protocol{graphics_protocol::none};
  // Draws the image with the cursor at area.origin. Borrowed: the widget
  // that placed it keeps the bytes alive until present() returns.
  std::string_view payload;
  // The image has transparent pixels. Sixel and iTerm2 leave those pixels
  // as they were, so a changed image first has its cells repainted to
  // clear the previous picture.
  bool transparent{};
};

struct frame_statistics {
  std::uint64_t cells_inspected{};
  std::uint64_t cells_changed{};
  std::uint64_t cells_written{};
  std::uint64_t runs{};
  std::uint64_t bytes{};
  std::uint64_t images_written{};
  bool full_redraw{};
};

class renderer {
 public:
  explicit renderer(color_depth depth = color_depth::truecolor,
                    bool synchronized_output = true)
      : encoder_{depth}, synchronized_output_{synchronized_output} {
    runs_.reserve(256);
  }

  // Resizes both buffers. The physical screen contents are unknown after a
  // resize (terminals reflow or clear differently), so the next frame
  // clears the screen and repaints.
  void resize(avionix::size extent) {
    front_.resize(extent);
    back_.resize(extent);
    encoder_.set_screen_width(extent.width);
    invalidate();
  }

  [[nodiscard]] avionix::size extent() const noexcept { return back_.extent(); }

  // The buffer widgets draw into. After present() it holds stale content
  // from two frames ago; callers clear it before drawing.
  [[nodiscard]] render_buffer& back() noexcept { return back_; }
  [[nodiscard]] const render_buffer& front() const noexcept { return front_; }

  // Forces the next present() to clear the screen and repaint.
  void invalidate() noexcept { full_redraw_ = true; }

  // Clears the back buffer and copies the front buffer's link table into
  // it. Widgets draw after this, so a URL keeps the same cell link id
  // across frames and a different URL is a different id. render_frame
  // calls this; callers that clear the back buffer themselves and draw
  // hyperlinks must call it too, or a reused id can hide a URL change.
  void begin_frame() {
    back_.clear();
    back_.adopt_links(front_);
  }

  void set_color_depth(color_depth depth) noexcept {
    encoder_.set_color_depth(depth);
    invalidate();
  }

  // Images for the next present(). Replaces the previous frame's list; an
  // image missing from it is removed from the screen.
  void set_graphics(std::vector<graphics_placement> placements) {
    graphics_ = std::move(placements);
  }

  // Bytes that delete every Kitty image still shown, for session teardown.
  // Sixel and iTerm2 pixels leave with the alternate screen.
  [[nodiscard]] std::string release_graphics() {
    std::string out;
    for (const shown_image& s : shown_)
      if (s.protocol == graphics_protocol::kitty) append_kitty_delete(out, s.id);
    shown_.clear();
    graphics_.clear();
    return out;
  }

  // Where to leave the visible cursor after the frame, or nullopt to hide
  // it. Used by text inputs.
  void set_cursor(std::optional<position> where) noexcept { cursor_request_ = where; }

  // Diffs back against front, encodes the changes, and swaps the buffers.
  // The returned bytes stay valid until the next call to present().
  std::string_view present() {
    encoder_.clear();
    runs_.clear();
    stats_ = {};

    const bool full = full_redraw_;
    if (full) {
      // A cleared screen is all default-style blanks, which is exactly
      // what a blank front buffer describes. Diffing against it then
      // emits only non-blank cells.
      front_.clear(cell::blank());
      encoder_.forget_state();
      cursor_visible_.reset();
      full_redraw_ = false;
    }

    // Old images that vanished or moved. Cell-resident images are erased by
    // making their old cells differ from anything a widget can draw, so the
    // diff repaints them.
    kitty_deletes_.clear();
    for (const shown_image& s : shown_) {
      const graphics_placement* now = find_graphics(s.id);
      if (now != nullptr && now->area == s.area && now->protocol == s.protocol)
        continue;
      if (s.protocol == graphics_protocol::kitty) {
        kitty_deletes_.push_back(s.id);
      } else if (!full) {
        front_.fill(intersect(s.area, front_.area()), stale_image_cell());
      }
    }

    if (!full) {
      for (const graphics_placement& p : graphics_) {
        if (!p.transparent || p.protocol == graphics_protocol::kitty) continue;
        const shown_image* before = find_shown(p.id);
        if (before != nullptr && before->version != p.version && before->area == p.area)
          front_.fill(intersect(p.area, front_.area()), stale_image_cell());
      }
    }

    const auto diff = collect_changed_runs(front_, back_, runs_);
    stats_.cells_inspected = diff.cells_inspected;
    stats_.cells_changed = diff.cells_changed;
    stats_.runs = runs_.size();
    stats_.full_redraw = full;

    const bool cursor_changes =
        !cursor_visible_ || *cursor_visible_ != cursor_request_.has_value() ||
        (cursor_request_ && encoder_.cursor() != cursor_request_);
    const auto needs_image = [&](const graphics_placement& p) {
      if (full) return true;
      const shown_image* before = find_shown(p.id);
      if (before == nullptr || before->version != p.version || before->area != p.area ||
          before->protocol != p.protocol)
        return true;
      if (p.protocol == graphics_protocol::kitty) return false;
      return std::ranges::any_of(runs_, [&](const changed_run& r) {
        return static_cast<std::int32_t>(r.y) >= p.area.top() &&
               static_cast<std::int32_t>(r.y) < p.area.bottom() &&
               static_cast<std::int32_t>(r.begin) < p.area.right() &&
               static_cast<std::int32_t>(r.end) > p.area.left();
      });
    };
    const bool images_change =
        !kitty_deletes_.empty() || std::ranges::any_of(graphics_, needs_image);

    if (!full && runs_.empty() && !cursor_changes && !images_change) {
      std::swap(front_, back_);
      return {};
    }

    if (synchronized_output_) encoder_.begin_synchronized_update();
    if (full) encoder_.clear_screen();
    // Hide the cursor while drawing so it does not flicker across the
    // screen as runs are written.
    if (cursor_visible_.value_or(true)) {
      encoder_.set_cursor_visible(false);
      cursor_visible_ = false;
    }

    for (const changed_run& run : runs_) {
      const std::span<const cell> line = back_.row(run.y);
      for (std::uint32_t x = run.begin; x < run.end; ++x) {
        const cell& c = line[x];
        if (c.is_continuation()) {
          continue;
        }
        encoder_.move_to(
            {static_cast<std::int32_t>(x), static_cast<std::int32_t>(run.y)});
        encoder_.set_link(back_.link_target(c.link()));
        encoder_.set_style(c.appearance());
        encoder_.write_glyph(c.text(), c.width());
        ++stats_.cells_written;
      }
    }
    // Already written text keeps its link. Closing here keeps the cursor
    // and the next frame from inheriting the last run's URL.
    encoder_.set_link({});

    // Images go over the cells just written.
    std::string deletes;
    for (const std::uint32_t id : kitty_deletes_) append_kitty_delete(deletes, id);
    encoder_.append_raw(deletes);
    for (const graphics_placement& p : graphics_) {
      if (p.payload.empty() || !needs_image(p)) continue;
      encoder_.move_to(p.area.origin);
      encoder_.reset_style();
      encoder_.append_raw(p.payload);
      // The terminal cursor position after an image is protocol dependent.
      encoder_.forget_state();
      ++stats_.images_written;
    }
    shown_.clear();
    for (const graphics_placement& p : graphics_)
      if (!p.payload.empty()) shown_.push_back({p.id, p.version, p.area, p.protocol});

    if (cursor_request_) {
      encoder_.move_to(*cursor_request_);
      encoder_.set_cursor_visible(true);
      cursor_visible_ = true;
    }
    if (synchronized_output_) encoder_.end_synchronized_update();

    stats_.bytes = encoder_.size();
    std::swap(front_, back_);
    return encoder_.bytes();
  }

  [[nodiscard]] const frame_statistics& last_frame() const noexcept { return stats_; }

 private:
  struct shown_image {
    std::uint32_t id{};
    std::uint64_t version{};
    rect area{};
    graphics_protocol protocol{};
  };

  // A cell no widget can draw (controls never reach the buffer), used to
  // force a repaint of cells that an image covered.
  [[nodiscard]] static cell stale_image_cell() noexcept {
    return cell::from_grapheme("\x01", 1, style{});
  }

  [[nodiscard]] const graphics_placement* find_graphics(
      std::uint32_t id) const noexcept {
    for (const auto& p : graphics_)
      if (p.id == id && !p.payload.empty()) return &p;
    return nullptr;
  }
  [[nodiscard]] const shown_image* find_shown(std::uint32_t id) const noexcept {
    for (const auto& s : shown_)
      if (s.id == id) return &s;
    return nullptr;
  }

  std::vector<graphics_placement> graphics_{};
  std::vector<shown_image> shown_{};
  std::vector<std::uint32_t> kitty_deletes_{};
  render_buffer front_{};
  render_buffer back_{};
  ansi_encoder encoder_;
  std::vector<changed_run> runs_{};
  frame_statistics stats_{};
  std::optional<position> cursor_request_{};
  std::optional<bool> cursor_visible_{};
  bool full_redraw_{true};
  bool synchronized_output_{};
};

}  // namespace avionix
