module;

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module avionix.interface.camouflage;

import avionix.entity.geometry;
import avionix.entity.color;
import avionix.entity.style;
import avionix.entity.cell;
import avionix.entity.event;
import avionix.entity.camouflage;
import avionix.object.buffer;
import avionix.interface.widget;

// Active camouflage around one child. The child is not drawn. Its shape
// bends whatever earlier siblings drew beneath it.
//
//   camouflage ──owns──► child (unique_ptr<component>)
//        │       ──owns──► scratch render_buffer, mask, source cells, plan
//        └─uses─► avionix.entity.camouflage (pure field)
//
// One frame:
//
//   child.render_in ──► scratch (back-buffer coordinates) ──► mask
//   back buffer cells under the clip ──► source (copied before any write)
//   clock, reveal, pointer, mask ──► field ──► plan (one entry per cell)
//   plan + scratch + source ──► back buffer
//
// The scratch buffer shares the back buffer's coordinates, so the child's
// last_area(), cursor requests, and hit testing are the same as if it had
// drawn into the back buffer directly. Only the wrapper's clip is cleared
// and read each frame.

export namespace avionix {

struct camouflage_options {
  bool pointer_reveals{false};      // the field fails near the mouse
  std::uint32_t pointer_radius{6};  // rows; columns count half
  color rim{color::rgb(0xc7, 0xde, 0xff)};
  color accent{color::rgb(0x80, 0xcc, 0xff)};
};

namespace detail {

// The type-erased part of camouflage<T>. Everything except child() lives
// here so the per-cell code is compiled once, in the library.
class camouflage_core : public component {
 public:
  using duration = std::chrono::steady_clock::duration;

  // Seconds for reveal() to travel from 0 to 1 or back.
  static constexpr double transition_seconds = 0.75;

  // Sets the reveal target, clamped to [0, 1]; NaN counts as 0. advance()
  // moves reveal() toward it, and the rows dissolve on the way.
  void set_reveal(double target) noexcept {
    target_ = target >= 1.0 ? 1.0 : (target > 0.0 ? target : 0.0);
  }
  [[nodiscard]] double reveal() const noexcept { return reveal_; }
  [[nodiscard]] double reveal_target() const noexcept { return target_; }

  // Moves the effect's clock and the reveal toward its target. The wrapper
  // owns no timer; call this from application::set_interval. Returns true
  // when the effect's next render will write different cells than the
  // last one, assuming the child and the content behind it are unchanged.
  // Nonpositive `elapsed` is ignored and returns false.
  bool advance(duration elapsed);

  // False only at rest fully solid, where render is a pass-through and
  // advance() can never return true.
  [[nodiscard]] bool animating() const noexcept { return !pass_through(); }

  [[nodiscard]] const camouflage_options& options() const noexcept { return options_; }

  void render(render_context& context) override;

  // Records the pointer for disruption and returns ignored. The child is
  // reported by children(), so the application delivers events to it
  // before bubbling here, the way it does for any container.
  event_result on_event(const event& value, event_context& context) override;

  void children(std::vector<component*>& out) override { out.push_back(child_.get()); }

 protected:
  camouflage_core(std::unique_ptr<component> child, camouflage_options options)
      : child_{std::move(child)}, options_{options} {}

 private:
  enum class action : std::uint8_t { keep, solid, displaced };

  // What the composite does with one cell. Fields that do not apply to the
  // action are zero, so equal plans write equal cells.
  struct plan_cell {
    action kind{action::keep};
    std::int8_t dx{};
    std::int8_t dy{};
    std::uint8_t rim{};     // quarters
    std::uint8_t accent{};  // quarters

    friend constexpr bool operator==(const plan_cell&,
                                     const plan_cell&) noexcept = default;
  };

  // Everything the plan depends on besides the mask. At rest (reveal 0,
  // no pointer) the field reads the clock only through the slow step, so
  // the key holds the step index instead of the clock and frames within
  // one step share a plan.
  struct plan_key {
    bool at_rest{};
    duration::rep clock{};  // the step index when at_rest
    double reveal{};
    std::optional<position> pointer{};
    rect clip{};
    position origin{};

    friend bool operator==(const plan_key&, const plan_key&) noexcept = default;
  };

  [[nodiscard]] bool pass_through() const noexcept {
    return reveal_ >= 1.0 && target_ >= 1.0;
  }
  [[nodiscard]] plan_key current_key(rect clip, position origin) const noexcept;
  void build_plan(const plan_key& key, std::vector<plan_cell>& out);
  void forget_pointer_after_leave();
  [[nodiscard]] bool subtree_hovered();
  void composite(render_context& context);

  std::unique_ptr<component> child_;
  camouflage_options options_;
  double reveal_{};
  double target_{};
  duration clock_{};
  std::optional<position> pointer_{};  // screen coordinates
  bool hover_seen_{};

  render_buffer scratch_{};
  frame_state scratch_frame_{};
  // 1 where the child drew. Clip-sized plus zero padding (see mask_pad).
  std::vector<std::uint8_t> mask_{};
  std::vector<std::uint8_t> next_mask_{};  // swapped with mask_
  std::vector<cell> source_{};             // back-buffer cells under the clip
  std::vector<plan_cell> plan_{};          // clip-sized, row-major
  std::vector<plan_cell> next_plan_{};
  std::vector<camouflage_row> rows_{};
  std::vector<camouflage_column> columns_{};
  std::vector<component*> walk_{};
  plan_key plan_key_{};
  bool plan_valid_{};
  bool last_render_composited_{};
};

}  // namespace detail

// Owns one child: a component, or any type satisfying `widget`, which is
// wrapped like containers wrap it. Moving the wrapper before it joins a
// tree keeps child() valid because the child lives on the heap.
//
// Not thread safe. UI thread only, like every component.
template <typename T>
  requires std::derived_from<T, component> || widget<T>
class camouflage final : public detail::camouflage_core {
 public:
  using child_type = T;

  template <typename U>
    requires std::same_as<std::remove_cvref_t<U>, T>
  explicit camouflage(U&& child, camouflage_options options = {})
      : camouflage{make_component(std::forward<U>(child)), options} {}

  [[nodiscard]] child_type& child() noexcept { return *value_; }
  [[nodiscard]] const child_type& child() const noexcept { return *value_; }

 private:
  explicit camouflage(made_component<T> made, camouflage_options options)
      : camouflage_core{std::move(made.owner), options}, value_{&made.value} {}

  T* value_;
};

template <typename U>
camouflage(U&&, camouflage_options = {}) -> camouflage<std::remove_cvref_t<U>>;

}  // namespace avionix

namespace avionix::detail {

namespace {

// The cleared state of the scratch buffer. A NUL glyph is never produced
// by text drawing (controls are dropped), so any cell that differs from
// this was written by the child, including a plain blank.
constexpr cell unwritten = cell::from_ascii('\0');

constexpr rgb_triplet default_background{0, 0, 0};
constexpr rgb_triplet default_foreground{229, 229, 229};

// The tint is weaker on the background than on the glyph so the shape
// reads as an edge of light rather than a block of color. Over a flat
// background the background tint is the whole outline, so it stays
// strong enough to see on a dark terminal.
constexpr double rim_foreground = 0.7;
constexpr double rim_background = 0.4;
constexpr double accent_foreground = 1.0;
constexpr double accent_background = 0.45;

[[nodiscard]] cell blank_over(const cell& source) noexcept {
  return cell::blank(style{.background = source.appearance().background});
}

[[nodiscard]] std::size_t cell_count(rect r) noexcept {
  return static_cast<std::size_t>(r.extent.area());
}

// The mask has this many zero columns on each side and one zero row above
// and below, so the field reads the neighborhood of a torn position
// without bounds checks. A tear is at most 3 columns.
constexpr std::size_t mask_pad = 4;

[[nodiscard]] std::size_t padded_count(rect r) noexcept {
  return (std::size_t{r.width()} + 2 * mask_pad) * (std::size_t{r.height()} + 2);
}

}  // namespace

camouflage_core::plan_key camouflage_core::current_key(rect clip,
                                                       position origin) const noexcept {
  const auto pointer = options_.pointer_reveals ? pointer_ : std::nullopt;
  const bool at_rest = reveal_ == 0.0 && !pointer;
  const double seconds = std::chrono::duration<double>(clock_).count();
  return {.at_rest = at_rest,
          .clock = at_rest ? static_cast<duration::rep>(camouflage_math::floor(
                                 seconds / camouflage_tuning::slow_step))
                           : clock_.count(),
          .reveal = reveal_,
          .pointer = pointer,
          .clip = clip,
          .origin = origin};
}

void camouflage_core::build_plan(const plan_key& key, std::vector<plan_cell>& out) {
  const rect clip = key.clip;
  const std::size_t n = cell_count(clip);
  out.assign(n, plan_cell{});
  if (n == 0 || mask_.size() != padded_count(clip)) return;
  const std::uint32_t width = clip.width();
  const std::size_t stride = width + 2 * mask_pad;
  const position to_local = clip.origin - key.origin;

  std::optional<position> pointer{};
  if (key.pointer) pointer = *key.pointer - key.origin;
  // At rest the middle of the step stands in for the clock, which gives
  // the same slow terms as any time within the step.
  const double seconds =
      key.at_rest
          ? (static_cast<double>(key.clock) + 0.5) * camouflage_tuning::slow_step
          : std::chrono::duration<double>(duration{key.clock}).count();
  const camouflage_frame frame =
      camouflage_frame_at(seconds, key.reveal, pointer,
                          pointer ? static_cast<double>(options_.pointer_radius) : 0.0);
  rows_.resize(clip.height());
  columns_.resize(width);
  for (std::uint32_t y = 0; y < clip.height(); ++y)
    rows_[y] = camouflage_row_at(frame, static_cast<std::int32_t>(y) + to_local.y);
  for (std::uint32_t x = 0; x < width; ++x)
    columns_[x] =
        camouflage_column_at(frame, static_cast<std::int32_t>(x) + to_local.x);

  for (std::uint32_t y = 0; y < clip.height(); ++y) {
    // Rows y - 1, y, y + 1 of the padded mask, at column 0 of the clip.
    const std::uint8_t* above = mask_.data() + std::size_t{y} * stride + mask_pad;
    const std::uint8_t* middle = above + stride;
    const std::uint8_t* below = middle + stride;
    // Without a pointer the scan is the same along the whole row.
    const camouflage_scan row_scan = camouflage_scan_at(
        frame, rows_[y], {0, static_cast<std::int32_t>(y) + to_local.y});
    for (std::uint32_t x = 0; x < width; ++x) {
      if (middle[x] == 0) continue;
      const std::size_t i = std::size_t{y} * width + x;
      const position local{static_cast<std::int32_t>(x) + to_local.x,
                           static_cast<std::int32_t>(y) + to_local.y};
      const camouflage_scan scan =
          frame.pointer ? camouflage_scan_at(frame, rows_[y], local) : row_scan;
      plan_cell& entry = out[i];
      if (scan.solid) {
        entry.kind = action::solid;
        continue;
      }
      // |tear| <= 3 < mask_pad, so every read stays inside the padding.
      const std::ptrdiff_t q = static_cast<std::ptrdiff_t>(x) + scan.tear;
      const auto bits = [&](const std::uint8_t* line) {
        return static_cast<unsigned>(line[q - 1]) |
               (static_cast<unsigned>(line[q]) << 1U) |
               (static_cast<unsigned>(line[q + 1]) << 2U);
      };
      const camouflage_neighborhood around{static_cast<std::uint16_t>(
          bits(above) | (bits(middle) << 3U) | (bits(below) << 6U))};
      const camouflage_cell field =
          camouflage_shade(frame, rows_[y], columns_[x], local, scan, around);
      if (field.solid) {
        entry.kind = action::solid;
        continue;
      }
      entry.kind = action::displaced;
      entry.dx = static_cast<std::int8_t>(field.offset.x);
      entry.dy = static_cast<std::int8_t>(field.offset.y);
      entry.rim = static_cast<std::uint8_t>(field.rim * 4.0);
      entry.accent = static_cast<std::uint8_t>(field.accent * 4.0);
    }
  }
}

bool camouflage_core::subtree_hovered() {
  walk_.clear();
  walk_.push_back(this);
  while (!walk_.empty()) {
    component* node = walk_.back();
    walk_.pop_back();
    if (node->hovered()) return true;
    node->children(walk_);
  }
  return false;
}

// The application reports hover only for the deepest component under the
// mouse, and moves outside this widget never reach it. A pointer is
// therefore dropped when the subtree stops being hovered. A caller that
// delivers mouse events without an application never sets hover, so the
// pointer stays until the next mouse event.
void camouflage_core::forget_pointer_after_leave() {
  const bool hovered = subtree_hovered();
  if (hover_seen_ && !hovered) pointer_.reset();
  hover_seen_ = hovered;
}

event_result camouflage_core::on_event(const event& value, event_context& /*context*/) {
  if (const auto* mouse = std::get_if<mouse_event>(&value)) {
    if (last_area().contains(mouse->where)) pointer_ = mouse->where;
  }
  return event_result::ignored;
}

bool camouflage_core::advance(duration elapsed) {
  if (elapsed <= duration::zero()) return false;
  const bool was_pass_through = pass_through();
  clock_ += elapsed;
  const double step =
      std::chrono::duration<double>(elapsed).count() / transition_seconds;
  if (reveal_ < target_) {
    reveal_ = reveal_ + step < target_ ? reveal_ + step : target_;
  } else if (reveal_ > target_) {
    reveal_ = reveal_ - step > target_ ? reveal_ - step : target_;
  }
  forget_pointer_after_leave();
  if (pass_through()) return !was_pass_through && last_render_composited_;
  if (!last_render_composited_) return true;

  const plan_key key = current_key(plan_key_.clip, plan_key_.origin);
  if (plan_valid_ && key == plan_key_) return false;
  build_plan(key, next_plan_);
  const bool changed = next_plan_ != plan_;
  std::swap(plan_, next_plan_);
  plan_key_ = key;
  plan_valid_ = true;
  return changed;
}

void camouflage_core::render(render_context& context) {
  forget_pointer_after_leave();
  if (pass_through()) {
    last_render_composited_ = false;
    child_->render_in(context);
    return;
  }
  last_render_composited_ = true;
  const rect clip = context.clip();
  const rect area = context.area();
  if (clip.empty()) {
    child_->clear_area();
    mask_.clear();
    plan_.clear();
    plan_key_ = current_key(clip, area.origin);
    plan_valid_ = true;
    return;
  }

  const avionix::size needed{static_cast<std::uint32_t>(clip.right()),
                             static_cast<std::uint32_t>(clip.bottom())};
  if (scratch_.extent() != needed) scratch_.resize(needed, unwritten);
  scratch_.fill(clip, unwritten);
  scratch_frame_.cursor.reset();
  scratch_frame_.placements.clear();
  scratch_frame_.graphics = context.graphics();
  {
    // Same area and clip as `context`, over the scratch buffer.
    render_context root{scratch_, scratch_.area(), scratch_frame_,
                        context.base_style()};
    const render_context clipped = root.child(clip);
    render_context inner = clipped.child(area.translated(position{} - clip.origin));
    child_->render_in(inner);
  }

  // The padding stays zero: only interior cells are written below.
  if (next_mask_.size() != padded_count(clip)) next_mask_.assign(padded_count(clip), 0);
  const std::size_t stride = clip.width() + 2 * mask_pad;
  bool any = false;
  for (std::uint32_t y = 0; y < clip.height(); ++y) {
    const auto row = scratch_.row(static_cast<std::uint32_t>(clip.top()) + y)
                         .subspan(static_cast<std::size_t>(clip.left()), clip.width());
    std::uint8_t* out = next_mask_.data() + (std::size_t{y} + 1) * stride + mask_pad;
    for (std::size_t x = 0; x < row.size(); ++x) {
      const bool drawn = !(row[x] == unwritten);
      out[x] = static_cast<std::uint8_t>(drawn);
      any = any || drawn;
    }
  }
  if (next_mask_ != mask_) {
    std::swap(mask_, next_mask_);
    plan_valid_ = false;
  }
  const plan_key key = current_key(clip, area.origin);
  if (!plan_valid_ || !(key == plan_key_)) {
    build_plan(key, plan_);
    plan_key_ = key;
    plan_valid_ = true;
  }
  if (!any) return;
  composite(context);
}

void camouflage_core::composite(render_context& context) {
  render_buffer& back = context.buffer();
  const rect clip = context.clip();
  const std::uint32_t width = clip.width();
  const std::size_t n = cell_count(clip);

  // Copy every source cell before writing any, so no offset samples a
  // cell this frame already displaced.
  source_.resize(n);
  for (std::uint32_t y = 0; y < clip.height(); ++y) {
    const auto row = back.row(static_cast<std::uint32_t>(clip.top()) + y)
                         .subspan(static_cast<std::size_t>(clip.left()), width);
    std::ranges::copy(row, source_.begin() + static_cast<std::ptrdiff_t>(y * width));
  }

  const bool tint = context.graphics().colors == color_depth::truecolor;
  const auto source_index = [&](std::uint32_t x, std::uint32_t y,
                                const plan_cell& entry) -> std::size_t {
    const auto clamp = [](std::int64_t v, std::uint32_t size) {
      return static_cast<std::size_t>(v < 0 ? 0 : (v >= size ? size - 1 : v));
    };
    return clamp(std::int64_t{y} + entry.dy, clip.height()) * width +
           clamp(std::int64_t{x} + entry.dx, width);
  };

  for (std::uint32_t y = 0; y < clip.height(); ++y) {
    bool skip = false;  // the previous cell wrote a wide glyph over this one
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::size_t i = std::size_t{y} * width + x;
      const plan_cell& entry = plan_[i];
      if (std::exchange(skip, false) || entry.kind == action::keep) continue;
      const position at{clip.left() + static_cast<std::int32_t>(x),
                        clip.top() + static_cast<std::int32_t>(y)};
      const bool next_in_row = x + 1 < width;

      if (entry.kind == action::solid) {
        cell drawn = scratch_.at(at);
        if (drawn.is_continuation()) {
          // Its lead was not solid, so the glyph cannot be kept whole.
          back.set(at, blank_over(drawn));
          continue;
        }
        if (drawn.is_wide() && !(next_in_row && plan_[i + 1].kind == action::solid)) {
          back.set(at, blank_over(drawn));
          continue;
        }
        if (drawn.text() == unwritten.text()) {
          // The child restyled a cell it never drew.
          drawn = cell::blank(drawn.appearance());
        }
        if (drawn.link() != 0) {
          const std::string_view url = scratch_.link_target(drawn.link());
          drawn.set_link(url.empty() ? std::uint16_t{0} : back.intern_link(url));
        }
        back.set(at, drawn);
        skip = drawn.is_wide();
        continue;
      }

      const std::size_t from = source_index(x, y, entry);
      cell shown = source_[from];
      if (shown.is_continuation()) {
        shown = blank_over(shown);
      } else if (shown.is_wide()) {
        // Keep the glyph only if the next cell copies its continuation.
        const bool whole = next_in_row && plan_[i + 1] == entry &&
                           source_index(x + 1, y, entry) == from + 1;
        if (!whole) shown = blank_over(shown);
      }
      shown.set_link(0);
      if (tint && (entry.rim != 0 || entry.accent != 0)) {
        const double rim = entry.rim / 4.0;
        const double accent = entry.accent / 4.0;
        style look = shown.appearance();
        look.foreground = camouflage_blend(look.foreground, options_.rim,
                                           rim * rim_foreground, default_foreground);
        look.foreground =
            camouflage_blend(look.foreground, options_.accent,
                             accent * accent_foreground, default_foreground);
        look.background = camouflage_blend(look.background, options_.rim,
                                           rim * rim_background, default_background);
        look.background =
            camouflage_blend(look.background, options_.accent,
                             accent * accent_background, default_background);
        shown.set_appearance(look);
      }
      back.set(at, shown);
      skip = shown.is_wide();
    }
  }

  // A cursor the child asked for stays visible only over a solid cell.
  if (const auto cursor = scratch_frame_.cursor; cursor && clip.contains(*cursor)) {
    const std::size_t i = static_cast<std::size_t>(cursor->y - clip.top()) * width +
                          static_cast<std::size_t>(cursor->x - clip.left());
    if (plan_[i].kind == action::solid)
      context.set_cursor(*cursor - context.area().origin);
  }
}

}  // namespace avionix::detail
