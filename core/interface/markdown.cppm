module;

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module avionix.interface.markdown;

import avionix.entity.geometry;
import avionix.entity.color;
import avionix.entity.style;
import avionix.entity.markdown;
import avionix.interface.widget;
import avionix.interface.controls;

// A Markdown document drawn by a child scroll_view. The child owns
// scrolling, pointer capture, text selection, and link hits. This widget
// parses on set_source and lays out when the content width changes.

export namespace avionix {

class markdown_view final : public component {
 public:
  using link_handler = scroll_view::link_handler;

  markdown_view() { configure(); }
  explicit markdown_view(std::string source) : markdown_view() {
    set_source(std::move(source));
  }

  markdown_view& set_source(std::string source) {
    source_ = std::move(source);
    document_ = parse_markdown(source_);
    dirty_ = true;
    laid_out_width_ = 0;
    view_.scroll_to_start();
    view_.clear_selection();
    return *this;
  }

  markdown_view& on_link(link_handler handler) {
    view_.on_link(std::move(handler));
    return *this;
  }
  markdown_view& set_highlight_style(style_patch appearance) noexcept {
    view_.set_highlight_style(appearance);
    return *this;
  }
  void scroll_to_start() noexcept { view_.scroll_to_start(); }

  [[nodiscard]] const std::string& source() const noexcept { return source_; }
  [[nodiscard]] bool has_selection() const noexcept { return view_.has_selection(); }
  [[nodiscard]] std::string selected_text() const { return view_.selected_text(); }
  [[nodiscard]] std::size_t scroll_offset() const noexcept {
    return view_.scroll_offset();
  }

  [[nodiscard]] bool focusable() const noexcept override { return false; }
  void children(std::vector<component*>& out) override { out.push_back(&view_); }

  void render(render_context& context) override {
    // Same content column scroll_view keeps clear of the scrollbar.
    const auto width = context.width() > 1 ? context.width() - 1 : context.width();
    if (dirty_ || width != laid_out_width_) {
      const bool width_changed = laid_out_width_ != 0 && width != laid_out_width_;
      if (width_changed) view_.clear_selection();
      apply_layout(width);
      dirty_ = false;
      laid_out_width_ = width;
    }
    view_.render_in(context);
  }

 private:
  void configure() {
    view_.set_wrap(false);
    view_.set_follow_tail(false);
    view_.set_jump_prompt(false);
  }

  void apply_layout(std::uint32_t width) {
    std::vector<styled_line> styled;
    const auto lines = layout_markdown(document_, width);
    styled.reserve(lines.size());
    for (const auto& line : lines) {
      styled_line row;
      row.reserve(line.spans.size());
      for (const auto& span : line.spans)
        row.push_back({span.text, appearance_for(line, span), span.target});
      styled.push_back(std::move(row));
    }
    view_.set_lines(std::move(styled));
  }

  [[nodiscard]] static color heading_color(std::uint8_t level) noexcept {
    if (level <= 1) return colors::bright_white;
    if (level == 2) return colors::bright_cyan;
    if (level == 3) return colors::bright_yellow;
    return colors::bright_green;
  }

  [[nodiscard]] static style_patch appearance_for(const markdown_line& line,
                                                  const markdown_span& span) {
    style_patch patch;
    switch (line.role) {
      case markdown_role::heading:
        patch.add = attribute::bold;
        patch.foreground = heading_color(line.level);
        break;
      case markdown_role::code:
        patch.foreground = colors::bright_white;
        patch.background = colors::bright_black;
        break;
      case markdown_role::quote:
        if (span.text != "│ " && span.text.find_first_not_of(' ') != std::string::npos)
          patch.add = attribute::italic;
        break;
      case markdown_role::rule:
        patch.foreground = colors::bright_black;
        break;
      case markdown_role::body:
        break;
    }
    if (has(span.marks, markdown_mark::code)) {
      patch.foreground = colors::bright_white;
      patch.background = colors::bright_black;
    } else {
      if (has(span.marks, markdown_mark::strong)) patch.add |= attribute::bold;
      if (has(span.marks, markdown_mark::emphasis)) patch.add |= attribute::italic;
      if (has(span.marks, markdown_mark::strike)) patch.add |= attribute::strikethrough;
    }
    if (!span.target.empty()) {
      patch.foreground = colors::bright_blue;
      patch.add |= attribute::underline;
    }
    return patch;
  }

  scroll_view view_;
  std::string source_;
  markdown_document document_;
  std::uint32_t laid_out_width_{};
  bool dirty_{true};
};

}  // namespace avionix
