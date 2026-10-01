module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module avionix.interface.controls;

import avionix.entity.geometry;
import avionix.entity.color;
import avionix.entity.style;
import avionix.entity.unicode;
import avionix.entity.event;
import avionix.interface.widget;

// Built-in widgets. All are components, draw only through render_context,
// and hold only UI state. Text handling goes through avionix.entity.unicode,
// so widths and cursor movement respect grapheme clusters and wide cells.

export namespace avionix {

enum class alignment : std::uint8_t { left, center, right };

// Greedy word wrap by display width. Words wider than `width` are broken at
// grapheme boundaries. Explicit '\n' starts a new line. The returned views
// point into `text`.
[[nodiscard]] std::vector<std::string_view> wrap_text(std::string_view text,
                                                      std::uint32_t width) {
  std::vector<std::string_view> lines;
  if (width == 0) {
    return lines;
  }
  std::size_t paragraph_start = 0;
  while (paragraph_start <= text.size()) {
    std::size_t paragraph_end = text.find('\n', paragraph_start);
    if (paragraph_end == std::string_view::npos) paragraph_end = text.size();
    const std::string_view paragraph =
        text.substr(paragraph_start, paragraph_end - paragraph_start);

    std::size_t line_start = 0;
    std::size_t line_width = 0;
    std::size_t last_break = std::string_view::npos;  // offset of a space in the line
    std::size_t offset = 0;
    if (paragraph.empty()) {
      lines.push_back(paragraph);
    }
    while (offset < paragraph.size()) {
      const std::size_t next = next_grapheme_boundary(paragraph, offset);
      const std::string_view cluster = paragraph.substr(offset, next - offset);
      const std::size_t w = grapheme_width(cluster);
      if (cluster == " ") {
        last_break = offset;
      }
      if (line_width + w > width && offset > line_start) {
        if (last_break != std::string_view::npos && last_break > line_start) {
          lines.push_back(paragraph.substr(line_start, last_break - line_start));
          line_start = last_break + 1;
        } else {
          lines.push_back(paragraph.substr(line_start, offset - line_start));
          line_start = offset;
        }
        // Skip leading spaces on the continuation line.
        while (line_start < paragraph.size() && paragraph[line_start] == ' ')
          ++line_start;
        last_break = std::string_view::npos;
        line_width = display_width(paragraph.substr(line_start, next - line_start));
        offset = std::max(next, line_start);
        continue;
      }
      line_width += w;
      offset = next;
    }
    if (line_start < paragraph.size()) {
      lines.push_back(paragraph.substr(line_start));
    }
    paragraph_start = paragraph_end + 1;
  }
  return lines;
}

// Static or dynamic text. Lines split on '\n'; optional word wrap.
class text final : public component {
 public:
  text() = default;
  explicit text(std::string content, style_patch appearance = {})
      : content_{std::move(content)}, style_{appearance} {}

  text& set(std::string content) {
    content_ = std::move(content);
    cached_width_ = 0;
    return *this;
  }
  [[nodiscard]] const std::string& content() const noexcept { return content_; }

  text& set_style(style_patch appearance) noexcept {
    style_ = appearance;
    return *this;
  }
  text& align(alignment value) noexcept {
    align_ = value;
    return *this;
  }
  text& wrap(bool enabled) noexcept {
    wrap_ = enabled;
    cached_width_ = 0;
    return *this;
  }

  text& follow_tail(bool enabled) noexcept {
    follow_tail_ = enabled;
    return *this;
  }

  void render(render_context& context) override {
    const std::uint32_t width = context.width();
    if (width == 0 || context.height() == 0) return;
    // lines_ views into content_; a move of this component can relocate
    // the characters (small-string storage), so the data pointer is part
    // of the cache key.
    if (cached_width_ != width || cached_data_ != content_.data()) {
      lines_.clear();
      if (wrap_) {
        lines_ = wrap_text(content_, width);
      } else {
        std::size_t start = 0;
        while (start <= content_.size()) {
          std::size_t end = content_.find('\n', start);
          if (end == std::string::npos) end = content_.size();
          lines_.emplace_back(std::string_view{content_}.substr(start, end - start));
          start = end + 1;
        }
      }
      cached_width_ = width;
      cached_data_ = content_.data();
    }
    const std::size_t rows = std::min<std::size_t>(lines_.size(), context.height());
    const std::size_t first = follow_tail_ ? lines_.size() - rows : 0;
    for (std::size_t y = 0; y < rows; ++y) {
      const std::string_view line = lines_[first + y];
      const auto line_width = static_cast<std::uint32_t>(display_width(line));
      std::int32_t x = 0;
      if (align_ == alignment::center && line_width < width) {
        x = static_cast<std::int32_t>((width - line_width) / 2);
      } else if (align_ == alignment::right && line_width < width) {
        x = static_cast<std::int32_t>(width - line_width);
      }
      context.draw_text({x, static_cast<std::int32_t>(y)}, line, style_);
    }
  }

 private:
  std::string content_{};
  style_patch style_{};
  alignment align_{alignment::left};
  bool wrap_{};
  bool follow_tail_{};
  // Line views into content_, recomputed when the width or text changes.
  std::vector<std::string_view> lines_{};
  std::uint32_t cached_width_{};
  const char* cached_data_{};
};

enum class border_kind : std::uint8_t { none, single, rounded, double_line, heavy };

namespace detail {

struct border_glyphs {
  std::string_view top_left, top_right, bottom_left, bottom_right, horizontal, vertical;
};

constexpr border_glyphs glyphs_for(border_kind kind) noexcept {
  switch (kind) {
    case border_kind::single:
      return {"┌", "┐", "└", "┘", "─", "│"};
    case border_kind::rounded:
      return {"╭", "╮", "╰", "╯", "─", "│"};
    case border_kind::double_line:
      return {"╔", "╗", "╚", "╝", "═", "║"};
    case border_kind::heavy:
      return {"┏", "┓", "┗", "┛", "━", "┃"};
    case border_kind::none:
      break;
  }
  return {" ", " ", " ", " ", " ", " "};
}

}  // namespace detail

// A border with an optional title around an optional child.
class block final : public component {
 public:
  explicit block(std::string title = {}, border_kind kind = border_kind::rounded)
      : title_{std::move(title)}, kind_{kind} {}

  template <typename T>
    requires std::derived_from<std::remove_cvref_t<T>, component> ||
                 widget<std::remove_cvref_t<T>>
  block(std::string title, border_kind kind, T&& child)
      : title_{std::move(title)},
        kind_{kind},
        child_{make_component(std::forward<T>(child)).owner} {}

  template <typename T>
    requires std::derived_from<std::remove_cvref_t<T>, component> ||
             widget<std::remove_cvref_t<T>>
  std::remove_cvref_t<T>& set_child(T&& child) {
    auto made = make_component(std::forward<T>(child));
    auto& ref = made.value;
    child_ = std::move(made.owner);
    return ref;
  }

  block& set_title(std::string title) {
    title_ = std::move(title);
    return *this;
  }
  block& set_border_style(style_patch appearance) noexcept {
    border_style_ = appearance;
    return *this;
  }
  // Border style used while any descendant has focus.
  block& set_focus_border_style(style_patch appearance) noexcept {
    focus_border_style_ = appearance;
    return *this;
  }
  block& set_focus_title_style(style_patch appearance) noexcept {
    focus_title_style_ = appearance;
    return *this;
  }
  block& set_title_style(style_patch appearance) noexcept {
    title_style_ = appearance;
    return *this;
  }

  void render(render_context& context) override {
    const std::uint32_t w = context.width();
    const std::uint32_t h = context.height();
    if (w == 0 || h == 0) {
      if (child_) child_->clear_area();
      return;
    }

    if (kind_ != border_kind::none && w >= 2 && h >= 2) {
      const auto g = detail::glyphs_for(kind_);
      const style_patch border = contains_focus(child_.get()) && focus_border_style_
                                     ? *focus_border_style_
                                     : border_style_;
      const auto right = static_cast<std::int32_t>(w - 1);
      const auto bottom = static_cast<std::int32_t>(h - 1);
      context.fill({{1, 0}, {w - 2, 1}}, g.horizontal, border);
      context.fill({{1, bottom}, {w - 2, 1}}, g.horizontal, border);
      context.fill({{0, 1}, {1, h - 2}}, g.vertical, border);
      context.fill({{right, 1}, {1, h - 2}}, g.vertical, border);
      context.draw_text({0, 0}, g.top_left, border);
      context.draw_text({right, 0}, g.top_right, border);
      context.draw_text({0, bottom}, g.bottom_left, border);
      context.draw_text({right, bottom}, g.bottom_right, border);
      if (!title_.empty() && w > 4) {
        const std::size_t fit = prefix_fitting_width(title_, w - 4);
        const std::string_view shown = std::string_view{title_}.substr(0, fit);
        context.draw_text({1, 0}, " ", border);
        const auto used = context.draw_text(
            {2, 0}, shown,
            contains_focus(child_.get()) && focus_title_style_ ? *focus_title_style_
                                                               : title_style_);
        context.draw_text({2 + static_cast<std::int32_t>(used), 0}, " ", border);
      }
    }
    if (child_) {
      const rect inner = kind_ == border_kind::none ? bounds(context.extent())
                                                    : bounds(context.extent()).inset(1);
      if (!inner.empty()) {
        render_context sub = context.child(inner);
        child_->render_in(sub);
      } else
        child_->clear_area();
    }
  }

  void children(std::vector<component*>& out) override {
    if (child_) out.push_back(child_.get());
  }

 private:
  static bool contains_focus(component* node) {
    if (node == nullptr) return false;
    if (node->focused()) return true;
    std::vector<component*> kids;
    node->children(kids);
    return std::ranges::any_of(kids, [](component* c) { return contains_focus(c); });
  }

  std::string title_;
  border_kind kind_;
  std::unique_ptr<component> child_{};
  style_patch border_style_{};
  std::optional<style_patch> focus_border_style_{
      style_patch{.foreground = colors::bright_cyan}};
  std::optional<style_patch> focus_title_style_{};
  style_patch title_style_{.add = attribute::bold};
};

struct styled_span {
  std::string text;
  style_patch appearance{};
};
using styled_line = std::vector<styled_span>;
struct list_item {
  styled_span glyph;
  styled_span label;
  styled_span detail;
};

// A selectable, scrollable list of text items.
class list_view final : public component {
 public:
  using select_handler =
      std::function<void(std::size_t index, const std::string& item)>;

  list_view() = default;
  explicit list_view(std::vector<std::string> items) : items_{std::move(items)} {}

  list_view& set_items(std::vector<std::string> items) {
    styled_items_.clear();
    items_ = std::move(items);
    selected_ = items_.empty() ? 0 : std::min(selected_, items_.size() - 1);
    return *this;
  }
  list_view& add_item(std::string item) {
    items_.push_back(std::move(item));
    return *this;
  }
  [[nodiscard]] const std::vector<std::string>& items() const noexcept {
    return items_;
  }

  list_view& set_styled_items(std::vector<list_item> items) {
    styled_items_ = std::move(items);
    items_.clear();
    for (const auto& item : styled_items_) items_.push_back(item.label.text);
    selected_ = items_.empty() ? 0 : std::min(selected_, items_.size() - 1);
    return *this;
  }
  void select_without_callback(std::size_t index) {
    if (!items_.empty()) selected_ = std::min(index, items_.size() - 1);
  }
  // Called on Enter or double activation of an item.
  list_view& on_activate(select_handler handler) {
    activate_ = std::move(handler);
    return *this;
  }
  // Called whenever the selection moves.
  list_view& on_select(select_handler handler) {
    select_ = std::move(handler);
    return *this;
  }
  list_view& set_highlight_style(style_patch appearance) noexcept {
    highlight_ = appearance;
    return *this;
  }

  [[nodiscard]] std::size_t selected() const noexcept { return selected_; }
  void select(std::size_t index) {
    if (items_.empty()) return;
    const std::size_t next = std::min(index, items_.size() - 1);
    if (next != selected_) {
      selected_ = next;
      if (select_) select_(selected_, items_[selected_]);
    }
  }

  [[nodiscard]] bool focusable() const noexcept override { return true; }

  void render(render_context& context) override {
    const std::uint32_t h = context.height();
    page_ = std::max<std::uint32_t>(h, 1);
    if (h == 0) return;
    // Keep the selection visible.
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + h) scroll_ = selected_ - h + 1;
    if (items_.size() <= h) scroll_ = 0;

    const std::size_t end = std::min(items_.size(), scroll_ + h);
    for (std::size_t i = scroll_; i < end; ++i) {
      const auto y = static_cast<std::int32_t>(i - scroll_);
      if (i == selected_) {
        const style_patch patch =
            focused() ? highlight_ : style_patch{.add = attribute::reverse};
        context.fill({{0, y}, {context.width(), 1}}, " ", patch);
        draw_item(context, i, y, patch);
      } else {
        draw_item(context, i, y, {});
      }
    }
    if (items_.size() > h && context.width() > 0) {
      draw_scrollbar(context);
    }
  }

  event_result on_event(const event& value, event_context& context) override {
    if (const auto* k = std::get_if<key_event>(&value)) {
      if (items_.empty()) return event_result::ignored;
      if (k->is(key::up) || k->is(U'k')) {
        select(selected_ == 0 ? 0 : selected_ - 1);
      } else if (k->is(key::down) || k->is(U'j')) {
        select(selected_ + 1);
      } else if (k->is(key::home)) {
        select(0);
      } else if (k->is(key::end)) {
        select(items_.size() - 1);
      } else if (k->is(key::page_up)) {
        select(selected_ > page_ ? selected_ - page_ : 0);
      } else if (k->is(key::page_down)) {
        select(selected_ + page_);
      } else if (k->is(key::enter)) {
        if (activate_) activate_(selected_, items_[selected_]);
      } else {
        return event_result::ignored;
      }
      return event_result::handled;
    }
    if (const auto* m = std::get_if<mouse_event>(&value)) {
      const rect area = last_area();
      if (m->button == mouse_button::wheel_up) {
        select(selected_ == 0 ? 0 : selected_ - 1);
        return event_result::handled;
      }
      if (m->button == mouse_button::wheel_down) {
        select(selected_ + 1);
        return event_result::handled;
      }
      if (m->button == mouse_button::left && m->action == mouse_action::press &&
          area.contains(m->where)) {
        const auto row = static_cast<std::size_t>(m->where.y - area.top());
        const std::size_t index = scroll_ + row;
        if (index < items_.size()) {
          context.request_focus(*this);
          if (index == selected_ && activate_) {
            activate_(selected_, items_[selected_]);
          }
          select(index);
        }
        return event_result::handled;
      }
    }
    return event_result::ignored;
  }

 private:
  void draw_item(render_context& context, std::size_t i, std::int32_t y,
                 style_patch patch) const {
    if (i >= styled_items_.size()) {
      context.draw_text({1, y}, items_[i], patch);
      return;
    }
    const auto width =
        context.width() -
        (items_.size() > context.height() && context.width() > 0 ? 1U : 0U);
    auto row = context.child({{0, y}, {width, 1}}).with_style(patch);
    const auto& item = styled_items_[i];
    const auto detail_width =
        std::min(display_width(item.detail.text), static_cast<std::size_t>(width));
    const auto right = width - static_cast<std::uint32_t>(detail_width);
    auto left = row.child({{0, 0}, {right, 1}});
    auto x = left.draw_text({0, 0}, item.glyph.text, item.glyph.appearance);
    left.draw_text({static_cast<std::int32_t>(x), 0}, item.label.text,
                   item.label.appearance);
    row.draw_text({static_cast<std::int32_t>(right), 0}, item.detail.text,
                  item.detail.appearance);
  }
  void draw_scrollbar(render_context& context) const {
    const std::uint32_t h = context.height();
    const auto x = static_cast<std::int32_t>(context.width() - 1);
    const double visible = static_cast<double>(h) / static_cast<double>(items_.size());
    const auto thumb =
        std::max<std::uint32_t>(1, static_cast<std::uint32_t>(visible * h));
    const auto max_scroll = items_.size() - h;
    const auto top = max_scroll == 0
                         ? 0U
                         : static_cast<std::uint32_t>(static_cast<double>(scroll_) /
                                                      static_cast<double>(max_scroll) *
                                                      static_cast<double>(h - thumb));
    context.fill({{x, 0}, {1, h}}, "│", {.foreground = colors::bright_black});
    context.fill({{x, static_cast<std::int32_t>(top)}, {1, thumb}}, "┃", {});
  }

  std::vector<list_item> styled_items_{};
  std::vector<std::string> items_{};
  std::size_t selected_{};
  std::size_t scroll_{};
  std::uint32_t page_{1};
  select_handler activate_{};
  select_handler select_{};
  style_patch highlight_{.foreground = colors::black, .background = colors::cyan};
};

// Single-line text entry with grapheme-aware editing.
class text_input final : public component {
 public:
  using text_handler = std::function<void(const std::string&)>;

  text_input() = default;
  explicit text_input(std::string placeholder) : placeholder_{std::move(placeholder)} {}

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  text_input& set_value(std::string value) {
    value_ = std::move(value);
    cursor_ = value_.size();
    return *this;
  }
  text_input& on_submit(text_handler handler) {
    submit_ = std::move(handler);
    return *this;
  }
  text_input& on_change(text_handler handler) {
    change_ = std::move(handler);
    return *this;
  }
  // Draw the value as bullets (password entry).
  text_input& set_masked(bool masked) noexcept {
    masked_ = masked;
    return *this;
  }

  // Byte offset of the cursor; always on a grapheme boundary.
  [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }

  [[nodiscard]] bool focusable() const noexcept override { return true; }

  void render(render_context& context) override {
    const std::uint32_t w = context.width();
    if (w == 0 || context.height() == 0) return;
    context.fill({{0, 0}, {w, 1}}, " ", {.add = attribute::underline});

    if (value_.empty()) {
      context.draw_text({0, 0}, placeholder_,
                        {.foreground = colors::bright_black, .add = attribute::italic});
      if (focused()) context.set_cursor({0, 0});
      return;
    }

    const std::string shown = masked_ ? mask(value_) : std::string{};
    const std::string_view display =
        masked_ ? std::string_view{shown} : std::string_view{value_};
    const std::size_t display_cursor = masked_ ? masked_offset(cursor_) : cursor_;

    // Horizontal scroll: keep the cursor inside the field.
    const std::size_t cursor_column = display_width(display.substr(0, display_cursor));
    if (cursor_column < scroll_) scroll_ = cursor_column;
    if (cursor_column >= scroll_ + w) scroll_ = cursor_column - w + 1;

    // Find the byte offset of the first visible grapheme.
    std::size_t offset = 0;
    std::size_t column = 0;
    while (offset < display.size() && column < scroll_) {
      const std::size_t next = next_grapheme_boundary(display, offset);
      column += grapheme_width(display.substr(offset, next - offset));
      offset = next;
    }
    context.draw_text({0, 0}, display.substr(offset), {.add = attribute::underline});
    if (focused()) {
      context.set_cursor({static_cast<std::int32_t>(cursor_column - scroll_), 0});
    }
  }

  event_result on_event(const event& value, event_context& context) override {
    if (const auto* p = std::get_if<paste_event>(&value)) {
      insert(sanitize(p->text));
      return event_result::handled;
    }
    if (const auto* m = std::get_if<mouse_event>(&value)) {
      if (m->button == mouse_button::left && m->action == mouse_action::press &&
          last_area().contains(m->where)) {
        context.request_focus(*this);
        return event_result::handled;
      }
      return event_result::ignored;
    }
    const auto* k = std::get_if<key_event>(&value);
    if (k == nullptr) return event_result::ignored;

    if (k->code == key::character && !has(k->mods, modifiers::ctrl) &&
        !has(k->mods, modifiers::alt)) {
      std::array<char, 4> bytes{};
      const std::size_t n = encode_utf8(k->character, bytes);
      insert({bytes.data(), n});
      return event_result::handled;
    }
    if (k->is(key::backspace) || k->is(key::backspace, modifiers::shift)) {
      if (cursor_ > 0) {
        const std::size_t start = previous_boundary(cursor_);
        value_.erase(start, cursor_ - start);
        cursor_ = start;
        changed();
      }
    } else if (k->is(key::del)) {
      if (cursor_ < value_.size()) {
        const std::size_t end = next_grapheme_boundary(value_, cursor_);
        value_.erase(cursor_, end - cursor_);
        changed();
      }
    } else if (k->is(key::left)) {
      cursor_ = previous_boundary(cursor_);
    } else if (k->is(key::right)) {
      cursor_ = next_grapheme_boundary(value_, cursor_);
    } else if (k->is(key::home) || k->is(U'a', modifiers::ctrl)) {
      cursor_ = 0;
    } else if (k->is(key::end) || k->is(U'e', modifiers::ctrl)) {
      cursor_ = value_.size();
    } else if (k->is(U'u', modifiers::ctrl)) {
      value_.erase(0, cursor_);
      cursor_ = 0;
      changed();
    } else if (k->is(U'k', modifiers::ctrl)) {
      value_.erase(cursor_);
      changed();
    } else if (k->is(key::enter)) {
      if (submit_) submit_(value_);
    } else {
      return event_result::ignored;
    }
    return event_result::handled;
  }

 private:
  // Removes controls; newlines and tabs become spaces so a paste stays on
  // one line and cannot inject terminal sequences.
  static std::string sanitize(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    std::size_t offset = 0;
    while (offset < input.size()) {
      const auto cp = decode_utf8(input.substr(offset));
      if (cp.value == U'\n' || cp.value == U'\r' || cp.value == U'\t') {
        if (cp.value != U'\n' || out.empty() || out.back() != ' ') out.push_back(' ');
      } else if (!is_control(cp.value)) {
        std::array<char, 4> bytes{};
        out.append(bytes.data(), encode_utf8(cp.value, bytes));
      }
      offset += std::max<std::size_t>(cp.length, 1);
    }
    return out;
  }

  void insert(std::string_view bytes) {
    if (bytes.empty()) return;
    value_.insert(cursor_, bytes);
    cursor_ += bytes.size();
    // A combining mark may have merged with the previous grapheme; the
    // cursor stays after it either way since boundaries only move left.
    changed();
  }

  void changed() {
    if (change_) change_(value_);
  }

  [[nodiscard]] std::size_t previous_boundary(std::size_t offset) const {
    std::size_t last = 0;
    std::size_t current = 0;
    while (current < offset) {
      last = current;
      current = next_grapheme_boundary(value_, current);
    }
    return last;
  }

  [[nodiscard]] static std::string mask(std::string_view input) {
    std::string out;
    for (const auto g : graphemes(input)) {
      (void)g;
      out += "•";
    }
    return out;
  }

  [[nodiscard]] std::size_t masked_offset(std::size_t byte_offset) const {
    std::size_t count = 0;
    for (auto it = graphemes(value_).begin(); it.offset() < byte_offset; ++it) ++count;
    return count * std::string_view{"•"}.size();
  }

  std::string value_{};
  std::string placeholder_{};
  std::size_t cursor_{};
  std::size_t scroll_{};
  bool masked_{};
  text_handler submit_{};
  text_handler change_{};
};

// Horizontal progress bar with eighth-cell resolution.
class progress_bar final : public component {
 public:
  explicit progress_bar(double ratio = 0.0) : ratio_{std::clamp(ratio, 0.0, 1.0)} {}

  progress_bar& set(double ratio) noexcept {
    ratio_ = std::isnan(ratio) ? 0.0 : std::clamp(ratio, 0.0, 1.0);
    return *this;
  }
  [[nodiscard]] double ratio() const noexcept { return ratio_; }

  progress_bar& set_style(style_patch filled, style_patch empty) noexcept {
    filled_ = filled;
    empty_ = empty;
    return *this;
  }
  progress_bar& show_percentage(bool enabled) noexcept {
    show_percentage_ = enabled;
    return *this;
  }

  void render(render_context& context) override {
    const std::uint32_t w = context.width();
    if (w == 0 || context.height() == 0) return;
    const std::uint32_t label_width = show_percentage_ ? 5 : 0;
    const std::uint32_t bar_width = w > label_width ? w - label_width : 0;

    const auto eighths =
        static_cast<std::uint64_t>(std::llround(ratio_ * bar_width * 8.0));
    const auto full = static_cast<std::uint32_t>(eighths / 8);
    const auto partial = static_cast<std::size_t>(eighths % 8);
    static constexpr std::array<std::string_view, 8> parts{"",  "▏", "▎", "▍",
                                                           "▌", "▋", "▊", "▉"};

    context.fill({{0, 0}, {bar_width, 1}}, "░", empty_);
    if (full > 0) context.fill({{0, 0}, {full, 1}}, "█", filled_);
    if (partial > 0 && full < bar_width) {
      context.draw_text({static_cast<std::int32_t>(full), 0}, parts[partial], filled_);
    }
    if (show_percentage_ && w >= label_width) {
      const std::string label = std::format("{:4.0f}%", ratio_ * 100.0);
      context.draw_text({static_cast<std::int32_t>(bar_width), 0}, label);
    }
  }

 private:
  double ratio_;
  style_patch filled_{.foreground = colors::green};
  style_patch empty_{.foreground = colors::bright_black};
  bool show_percentage_{true};
};

// Empty space. Useful as a layout filler.
class spacer final : public component {
 public:
  void render(render_context& /*context*/) override {}
};

class styled_text final : public component {
 public:
  explicit styled_text(styled_line spans = {}) : spans_{std::move(spans)} {}
  styled_text& set(styled_line spans) {
    spans_ = std::move(spans);
    return *this;
  }
  void render(render_context& context) override {
    position at{};
    for (const auto& span : spans_) {
      std::string_view remaining = span.text;
      while (true) {
        const auto newline = remaining.find('\n');
        at.x += static_cast<std::int32_t>(
            context.draw_text(at, remaining.substr(0, newline), span.appearance));
        if (newline == std::string_view::npos) break;
        at.x = 0;
        ++at.y;
        remaining.remove_prefix(newline + 1);
      }
    }
  }

 private:
  styled_line spans_;
};

class button final : public component {
 public:
  explicit button(std::string label = {}) : label_{std::move(label)} {}
  button& set_label(std::string label) {
    label_ = std::move(label);
    return *this;
  }
  button& on_press(std::function<void()> callback) {
    press_ = std::move(callback);
    return *this;
  }
  button& set_enabled(std::function<bool()> predicate) {
    enabled_ = std::move(predicate);
    return *this;
  }
  button& set_confirm(bool value) {
    confirm_ = value;
    armed_ = false;
    return *this;
  }
  button& set_style(style_patch normal, style_patch focused_style, style_patch disabled,
                    style_patch hover) {
    normal_ = normal;
    focus_ = focused_style;
    disabled_ = disabled;
    hover_ = hover;
    return *this;
  }
  [[nodiscard]] bool enabled() const { return !enabled_ || enabled_(); }
  [[nodiscard]] bool armed() const noexcept { return armed_; }
  [[nodiscard]] bool focusable() const noexcept override { return true; }
  void on_focus_changed(bool value) override {
    if (!value) armed_ = false;
  }
  void render(render_context& context) override {
    const auto patch = !enabled()  ? disabled_
                       : focused() ? focus_
                       : hovered() ? hover_
                                   : normal_;
    context.clear(patch);
    context.draw_text({0, 0}, armed_ ? "Confirm: " + label_ : label_, patch);
  }
  event_result on_event(const event& value, event_context& context) override {
    bool activate = false;
    if (const auto* k = std::get_if<key_event>(&value))
      activate = k->is(key::enter) || k->is(U' ');
    if (const auto* m = std::get_if<mouse_event>(&value)) {
      activate = m->action == mouse_action::press && m->button == mouse_button::left &&
                 last_area().contains(m->where);
      if (activate) context.request_focus(*this);
    }
    if (!activate) return event_result::ignored;
    if (!enabled()) {
      armed_ = false;
      return event_result::handled;
    }
    if (confirm_ && !armed_) {
      armed_ = true;
      return event_result::handled;
    }
    armed_ = false;
    auto callback = press_;
    if (callback) callback();
    return event_result::handled;
  }

 private:
  std::string label_;
  std::function<void()> press_;
  std::function<bool()> enabled_;
  bool confirm_{};
  bool armed_{};
  style_patch normal_{};
  style_patch focus_{.add = attribute::reverse};
  style_patch disabled_{.foreground = colors::bright_black};
  style_patch hover_{.add = attribute::underline};
};

class tabs final : public component {
 public:
  explicit tabs(std::vector<std::string> labels = {}) : labels_{std::move(labels)} {}
  [[nodiscard]] std::size_t active() const noexcept { return active_; }
  tabs& set_active(std::size_t index) {
    const auto next = labels_.empty() ? 0 : std::min(index, labels_.size() - 1);
    if (next != active_) {
      active_ = next;
      if (change_) change_(active_);
    }
    return *this;
  }
  tabs& on_change(std::function<void(std::size_t)> callback) {
    change_ = std::move(callback);
    return *this;
  }
  template <typename T>
  T& set_status(std::uint32_t width, T&& value) {
    auto made = make_component(std::forward<T>(value));
    auto& ref = made.value;
    status_ = std::move(made.owner);
    status_width_ = width;
    return ref;
  }
  [[nodiscard]] bool focusable() const noexcept override { return true; }
  void children(std::vector<component*>& out) override {
    if (status_) out.push_back(status_.get());
  }
  void render(render_context& context) override {
    headers_.clear();
    const auto reserved = status_ ? std::min(status_width_, context.width()) : 0U;
    auto header = context.child(
        {{0, 0}, {context.width() - reserved, std::min(1U, context.height())}});
    std::uint32_t x = 0;
    for (std::size_t i = 0; i < labels_.size(); ++i) {
      const auto width = static_cast<std::uint32_t>(display_width(labels_[i]) + 2);
      headers_.push_back(intersect(rect{{static_cast<std::int32_t>(x), 0}, {width, 1}},
                                   bounds(header.extent()))
                             .translated(context.area().origin));
      header.draw_text(
          {static_cast<std::int32_t>(x), 0}, " " + labels_[i] + " ",
          i == active_ ? style_patch{.add = attribute::reverse} : style_patch{});
      x += width;
    }
    if (status_) {
      if (reserved == 0 || context.height() == 0)
        status_->clear_area();
      else {
        auto sub =
            context.child({{static_cast<std::int32_t>(context.width() - reserved), 0},
                           {reserved, 1}});
        status_->render_in(sub);
      }
    }
  }
  event_result on_event(const event& value, event_context& context) override {
    if (labels_.empty()) return event_result::ignored;
    if (const auto* k = std::get_if<key_event>(&value)) {
      if (k->is(key::left))
        set_active((active_ + labels_.size() - 1) % labels_.size());
      else if (k->is(key::right))
        set_active((active_ + 1) % labels_.size());
      else
        return event_result::ignored;
      return event_result::handled;
    }
    if (const auto* m = std::get_if<mouse_event>(&value);
        m && m->button == mouse_button::left && m->action == mouse_action::press) {
      for (std::size_t i = 0; i < headers_.size(); ++i)
        if (headers_[i].contains(m->where)) {
          set_active(i);
          context.request_focus(*this);
          return event_result::handled;
        }
    }
    return event_result::ignored;
  }

 private:
  std::vector<std::string> labels_;
  std::vector<rect> headers_;
  std::size_t active_{};
  std::unique_ptr<component> status_;
  std::uint32_t status_width_{};
  std::function<void(std::size_t)> change_;
};

class scroll_view final : public component {
 public:
  explicit scroll_view(std::string content = {}) { set_text(std::move(content)); }
  scroll_view& set_text(std::string content) {
    lines_ = {{{std::move(content), {}}}};
    return *this;
  }
  scroll_view& set_lines(std::vector<styled_line> lines) {
    lines_ = std::move(lines);
    return *this;
  }
  scroll_view& append(styled_line line) {
    lines_.push_back(std::move(line));
    return *this;
  }
  scroll_view& set_follow_tail(bool follow) {
    follow_ = follow;
    return *this;
  }
  void jump_to_latest() noexcept {
    follow_ = true;
    scroll_ = maximum_;
  }
  [[nodiscard]] bool following_tail() const noexcept { return follow_; }
  [[nodiscard]] std::size_t scroll_offset() const noexcept { return scroll_; }
  [[nodiscard]] bool focusable() const noexcept override { return true; }
  void render(render_context& context) override {
    wrapped_.clear();
    const auto width = context.width() > 1 ? context.width() - 1 : context.width();
    if (width == 0 || context.height() == 0) return;
    for (const auto& line : lines_) {
      std::string joined;
      for (const auto& span : line) joined += span.text;
      for (const auto piece : wrap_text(joined, width)) {
        styled_line result;
        const auto begin = static_cast<std::size_t>(piece.data() - joined.data());
        const auto end = begin + piece.size();
        std::size_t offset = 0;
        for (const auto& span : line) {
          const auto first = std::max(begin, offset);
          const auto last = std::min(end, offset + span.text.size());
          if (first < last)
            result.push_back(
                {span.text.substr(first - offset, last - first), span.appearance});
          offset += span.text.size();
        }
        wrapped_.push_back(std::move(result));
      }
    }
    page_ = context.height();
    auto available = page_;
    maximum_ = wrapped_.size() > available ? wrapped_.size() - available : 0;
    if (follow_)
      scroll_ = maximum_;
    else
      scroll_ = std::min(scroll_, maximum_);
    const bool away = !follow_ && scroll_ < maximum_;
    if (away && available > 1) --available;
    for (std::size_t i = 0; i < available && scroll_ + i < wrapped_.size(); ++i) {
      auto row = context.child({{0, static_cast<std::int32_t>(i)}, {width, 1}});
      std::uint32_t x = 0;
      for (const auto& span : wrapped_[scroll_ + i])
        x += row.draw_text({static_cast<std::int32_t>(x), 0}, span.text,
                           span.appearance);
    }
    latest_ = {};
    if (away) {
      const auto y = static_cast<std::int32_t>(page_ - 1);
      context.draw_text({0, y}, "Jump to latest", {.add = attribute::reverse});
      latest_ = rect{{0, y}, {width, 1}}.translated(context.area().origin);
    }
    if (context.width() > 1 && maximum_ > 0) {
      const auto x = static_cast<std::int32_t>(width);
      context.fill({{x, 0}, {1, page_}}, "|", {.foreground = colors::bright_black});
      const auto thumb = std::max<std::size_t>(
          1, static_cast<std::size_t>(page_) * page_ / wrapped_.size());
      const auto top = scroll_ * (page_ - thumb) / maximum_;
      context.fill(
          {{x, static_cast<std::int32_t>(top)}, {1, static_cast<std::uint32_t>(thumb)}},
          "#");
    }
  }
  event_result on_event(const event& value, event_context&) override {
    std::int64_t delta = 0;
    if (const auto* k = std::get_if<key_event>(&value)) {
      if (k->is(key::end)) {
        jump_to_latest();
        return event_result::handled;
      }
      if (k->is(key::home)) {
        follow_ = false;
        scroll_ = 0;
        return event_result::handled;
      }
      if (k->is(key::up))
        delta = -1;
      else if (k->is(key::down))
        delta = 1;
      else if (k->is(key::page_up))
        delta = -static_cast<std::int64_t>(page_);
      else if (k->is(key::page_down))
        delta = page_;
      else
        return event_result::ignored;
    } else if (const auto* m = std::get_if<mouse_event>(&value)) {
      if (m->button == mouse_button::left && m->action == mouse_action::press &&
          latest_.contains(m->where)) {
        jump_to_latest();
        return event_result::handled;
      }
      if (m->button == mouse_button::wheel_up)
        delta = -3;
      else if (m->button == mouse_button::wheel_down)
        delta = 3;
      else
        return event_result::ignored;
    } else
      return event_result::ignored;
    scroll_ = static_cast<std::size_t>(
        std::clamp(static_cast<std::int64_t>(scroll_) + delta, std::int64_t{0},
                   static_cast<std::int64_t>(maximum_)));
    follow_ = scroll_ == maximum_;
    return event_result::handled;
  }

 private:
  std::vector<styled_line> lines_;
  std::vector<styled_line> wrapped_;
  std::size_t scroll_{};
  std::size_t maximum_{};
  std::uint32_t page_{1};
  bool follow_{true};
  rect latest_{};
};

class text_area final : public component {
 public:
  using text_handler = std::function<void(const std::string&)>;
  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  text_area& set_value(std::string value) {
    value_ = sanitize(value);
    cursor_ = value_.size();
    return *this;
  }
  text_area& on_submit(text_handler callback) {
    submit_ = std::move(callback);
    return *this;
  }
  text_area& on_change(text_handler callback) {
    change_ = std::move(callback);
    return *this;
  }
  text_area& set_history(std::vector<std::string> history) {
    history_ = std::move(history);
    history_index_ = history_.size();
    return *this;
  }
  [[nodiscard]] bool focusable() const noexcept override { return true; }
  void render(render_context& context) override {
    const auto width = context.width();
    if (width == 0 || context.height() == 0) return;
    width_ = width;
    const auto lines = visual_lines(width);
    const auto [cursor_row, cursor_col] = cursor_position(lines);
    if (cursor_row < scroll_) scroll_ = cursor_row;
    if (cursor_row >= scroll_ + context.height())
      scroll_ = cursor_row - context.height() + 1;
    for (std::size_t i = scroll_; i < lines.size() && i - scroll_ < context.height();
         ++i)
      context.draw_text({0, static_cast<std::int32_t>(i - scroll_)},
                        std::string_view{value_}.substr(
                            lines[i].first, lines[i].second - lines[i].first));
    if (focused())
      context.set_cursor({static_cast<std::int32_t>(cursor_col),
                          static_cast<std::int32_t>(cursor_row - scroll_)});
  }

  event_result on_event(const event& value, event_context& context) override {
    if (const auto* p = std::get_if<paste_event>(&value)) {
      insert(sanitize(p->text));
      return event_result::handled;
    }
    if (const auto* m = std::get_if<mouse_event>(&value)) {
      if (m->button == mouse_button::left && m->action == mouse_action::press &&
          last_area().contains(m->where)) {
        context.request_focus(*this);
        return event_result::handled;
      }
      return event_result::ignored;
    }
    const auto* k = std::get_if<key_event>(&value);
    if (!k) return event_result::ignored;
    if (k->is(key::enter, modifiers::shift))
      insert("\n");
    else if (k->is(key::enter)) {
      if (submit_) submit_(value_);
    } else if (k->code == key::character && !has(k->mods, modifiers::ctrl) &&
               !has(k->mods, modifiers::alt)) {
      std::array<char, 4> bytes{};
      const auto count = encode_utf8(k->character, bytes);
      insert(sanitize({bytes.data(), count}));
    } else if (k->is(key::left))
      cursor_ = previous();
    else if (k->is(key::right))
      cursor_ = next_grapheme_boundary(value_, cursor_);
    else if (k->is(key::backspace)) {
      const auto start = previous();
      value_.erase(start, cursor_ - start);
      cursor_ = start;
      changed();
    } else if (k->is(key::del)) {
      value_.erase(cursor_, next_grapheme_boundary(value_, cursor_) - cursor_);
      changed();
    } else if (k->is(key::home)) {
      const auto start =
          cursor_ == 0 ? std::string::npos : value_.rfind('\n', cursor_ - 1);
      cursor_ = start == std::string::npos ? 0 : start + 1;
    } else if (k->is(key::end)) {
      const auto end = value_.find('\n', cursor_);
      cursor_ = end == std::string::npos ? value_.size() : end;
    } else if (k->is(key::up) || k->is(key::down)) {
      if (history_.empty()) {
        const auto lines = visual_lines(width_);
        const auto [row, column] = cursor_position(lines);
        const auto next =
            k->is(key::up)
                ? (row == 0 ? 0 : row - 1)
                : std::min(row + 1, static_cast<std::uint32_t>(lines.size() - 1));
        auto offset = lines[next].first;
        std::uint32_t used = 0;
        while (offset < lines[next].second) {
          const auto end = next_grapheme_boundary(value_, offset);
          const auto cells =
              grapheme_width(std::string_view{value_}.substr(offset, end - offset));
          if (used + cells > column) break;
          used += cells;
          offset = end;
        }
        cursor_ = offset;
        return event_result::handled;
      }
      if (history_index_ == history_.size()) draft_ = value_;
      if (k->is(key::up) && history_index_ > 0) --history_index_;
      if (k->is(key::down) && history_index_ < history_.size()) ++history_index_;
      set_value(history_index_ == history_.size() ? draft_ : history_[history_index_]);
      changed();
    } else
      return event_result::ignored;
    return event_result::handled;
  }

 private:
  using line_range = std::pair<std::size_t, std::size_t>;
  std::vector<line_range> visual_lines(std::uint32_t width) const {
    std::vector<line_range> lines;
    std::size_t start = 0, offset = 0;
    std::uint32_t column = 0;
    while (offset < value_.size()) {
      if (value_[offset] == '\n') {
        lines.push_back({start, offset});
        start = ++offset;
        column = 0;
        continue;
      }
      const auto next = next_grapheme_boundary(value_, offset);
      const auto cells =
          grapheme_width(std::string_view{value_}.substr(offset, next - offset));
      if (column + cells > width && offset > start) {
        lines.push_back({start, offset});
        start = offset;
        column = 0;
      }
      column += cells;
      offset = next;
    }
    lines.push_back({start, offset});
    if (column == width && start < offset) lines.push_back({offset, offset});
    return lines;
  }
  std::pair<std::uint32_t, std::uint32_t> cursor_position(
      const std::vector<line_range>& lines) const {
    std::uint32_t row = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
      if (cursor_ >= lines[i].first) row = static_cast<std::uint32_t>(i);
    const auto column = display_width(
        std::string_view{value_}.substr(lines[row].first, cursor_ - lines[row].first));
    return {row, static_cast<std::uint32_t>(column)};
  }
  static std::string sanitize(std::string_view input) {
    std::string out;
    for (std::size_t offset = 0; offset < input.size();) {
      const auto cp = decode_utf8(input.substr(offset));
      if (cp.value == U'\r') {
        out += '\n';
        if (offset + 1 < input.size() && input[offset + 1] == '\n') ++offset;
      } else if (cp.value == U'\n' || cp.value == U'\t')
        out += cp.value == U'\n' ? '\n' : ' ';
      else if (!is_control(cp.value)) {
        std::array<char, 4> bytes{};
        out.append(bytes.data(), encode_utf8(cp.value, bytes));
      }
      offset += std::max<std::size_t>(1, cp.length);
    }
    return out;
  }
  std::size_t previous() const {
    std::size_t before = 0;
    for (std::size_t at = 0; at < cursor_;) {
      before = at;
      at = next_grapheme_boundary(value_, at);
    }
    return before;
  }
  void insert(std::string_view bytes) {
    value_.insert(cursor_, bytes);
    cursor_ += bytes.size();
    changed();
  }
  void changed() {
    if (change_) change_(value_);
  }
  std::string value_;
  std::size_t cursor_{};
  std::uint32_t scroll_{};
  std::uint32_t width_{80};
  std::vector<std::string> history_;
  std::size_t history_index_{};
  std::string draft_;
  text_handler submit_;
  text_handler change_;
};

}  // namespace avionix
