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
    for (std::size_t y = 0; y < rows; ++y) {
      const std::string_view line = lines_[y];
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
  block& set_title_style(style_patch appearance) noexcept {
    title_style_ = appearance;
    return *this;
  }

  void render(render_context& context) override {
    const std::uint32_t w = context.width();
    const std::uint32_t h = context.height();
    if (w == 0 || h == 0) return;

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
        const auto used = context.draw_text({2, 0}, shown, title_style_);
        context.draw_text({2 + static_cast<std::int32_t>(used), 0}, " ", border);
      }
    }
    if (child_) {
      const rect inner = kind_ == border_kind::none ? bounds(context.extent())
                                                    : bounds(context.extent()).inset(1);
      if (!inner.empty()) {
        render_context sub = context.child(inner);
        child_->render_in(sub);
      }
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
  style_patch title_style_{.add = attribute::bold};
};

// A selectable, scrollable list of text items.
class list_view final : public component {
 public:
  using select_handler =
      std::function<void(std::size_t index, const std::string& item)>;

  list_view() = default;
  explicit list_view(std::vector<std::string> items) : items_{std::move(items)} {}

  list_view& set_items(std::vector<std::string> items) {
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
        context.draw_text({1, y}, items_[i], patch);
      } else {
        context.draw_text({1, y}, items_[i]);
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

}  // namespace avionix
