module;

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module avionix.entity.markdown;

import avionix.entity.unicode;

// A practical Markdown subset. Pure values: no I/O, no terminal styles.
// layout_markdown wraps a document to a display width. The widget layer
// maps roles and marks onto colors.
//
// Read: ATX headings, paragraphs, hard breaks, emphasis, strong text,
// strike, inline code, fenced code, links, autolinks, images, one-level
// quotes, unordered and ordered lists, task items, and thematic breaks.
//
// Not read: HTML, tables, reference links, setext headings, indented code
// blocks, and nested quotes. Markers follow a short set of rules rather
// than the full CommonMark flanking algorithm.

export namespace avionix {

enum class markdown_mark : std::uint8_t {
  none = 0,
  strong = 1U << 0U,
  emphasis = 1U << 1U,
  code = 1U << 2U,
  strike = 1U << 3U,
};

[[nodiscard]] constexpr markdown_mark operator|(markdown_mark a,
                                                markdown_mark b) noexcept {
  return static_cast<markdown_mark>(static_cast<std::uint8_t>(a) |
                                    static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr markdown_mark operator&(markdown_mark a,
                                                markdown_mark b) noexcept {
  return static_cast<markdown_mark>(static_cast<std::uint8_t>(a) &
                                    static_cast<std::uint8_t>(b));
}

constexpr markdown_mark& operator|=(markdown_mark& a, markdown_mark b) noexcept {
  return a = a | b;
}

[[nodiscard]] constexpr bool has(markdown_mark set, markdown_mark flag) noexcept {
  return flag != markdown_mark::none && (set & flag) == flag;
}

// One run of text. `target` is the link URL, empty when the run is not a link.
struct markdown_span {
  std::string text;
  markdown_mark marks{markdown_mark::none};
  std::string target{};
};

struct markdown_block {
  enum class kind : std::uint8_t { paragraph, heading, code, quote, list_item, rule };

  kind type{kind::paragraph};
  std::uint8_t level{};
  bool ordered{};
  bool task{};
  bool checked{};
  std::string marker{};
  std::string language{};
  std::vector<markdown_span> spans{};
};

struct markdown_document {
  std::vector<markdown_block> blocks{};
};

enum class markdown_role : std::uint8_t { body, heading, code, quote, rule };

// One visual row. Spans are already wrapped to the layout width.
struct markdown_line {
  markdown_role role{markdown_role::body};
  std::uint8_t level{};
  std::vector<markdown_span> spans{};
};

[[nodiscard]] markdown_document parse_markdown(std::string_view source);
[[nodiscard]] std::vector<markdown_line> layout_markdown(
    const markdown_document& document, std::uint32_t width);

}  // namespace avionix

namespace {

using avionix::display_width;
using avionix::grapheme_width;
using avionix::has;
using avionix::markdown_block;
using avionix::markdown_document;
using avionix::markdown_line;
using avionix::markdown_mark;
using avionix::markdown_role;
using avionix::markdown_span;
using avionix::next_grapheme_boundary;

[[nodiscard]] bool is_space(char c) noexcept { return c == ' ' || c == '\t'; }

[[nodiscard]] bool is_punct(unsigned char c) noexcept { return std::ispunct(c) != 0; }

[[nodiscard]] bool is_blank(std::string_view line) noexcept {
  return std::ranges::all_of(line, [](char c) { return is_space(c); });
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return text;
}

[[nodiscard]] std::vector<std::string_view> split_lines(std::string_view source) {
  if (source.starts_with("\xEF\xBB\xBF")) source.remove_prefix(3);
  std::vector<std::string_view> lines;
  std::size_t i = 0;
  while (i < source.size()) {
    std::size_t end = i;
    while (end < source.size() && source[end] != '\n' && source[end] != '\r') ++end;
    lines.push_back(source.substr(i, end - i));
    if (end < source.size() && source[end] == '\r') ++end;
    if (end < source.size() && source[end] == '\n') ++end;
    i = end;
  }
  return lines;
}

[[nodiscard]] std::size_t leading_indent(std::string_view line,
                                         std::size_t cap = 3) noexcept {
  std::size_t i = 0;
  while (i < line.size() && i < cap && line[i] == ' ') ++i;
  return i;
}

struct fence_open {
  char marker{};
  std::size_t count{};
  std::string info;
};

[[nodiscard]] std::optional<fence_open> match_fence(std::string_view line) {
  const auto indent = leading_indent(line);
  if (indent >= line.size()) return std::nullopt;
  const char marker = line[indent];
  if (marker != '`' && marker != '~') return std::nullopt;
  std::size_t count = 0;
  while (indent + count < line.size() && line[indent + count] == marker) ++count;
  if (count < 3) return std::nullopt;
  auto info = trim(line.substr(indent + count));
  if (marker == '`' && info.find('`') != std::string_view::npos) return std::nullopt;
  return fence_open{marker, count, std::string{info}};
}

[[nodiscard]] bool is_fence_close(std::string_view line,
                                  const fence_open& open) noexcept {
  const auto indent = leading_indent(line);
  if (indent >= line.size() || line[indent] != open.marker) return false;
  std::size_t count = 0;
  while (indent + count < line.size() && line[indent + count] == open.marker) ++count;
  if (count < open.count) return false;
  return is_blank(line.substr(indent + count));
}

[[nodiscard]] bool is_rule(std::string_view line) noexcept {
  const auto indent = leading_indent(line);
  if (indent >= line.size()) return false;
  const char marker = line[indent];
  if (marker != '-' && marker != '*' && marker != '_') return false;
  std::size_t count = 0;
  for (std::size_t i = indent; i < line.size(); ++i) {
    if (line[i] == marker)
      ++count;
    else if (!is_space(line[i]))
      return false;
  }
  return count >= 3;
}

[[nodiscard]] std::optional<std::pair<std::uint8_t, std::string>> match_heading(
    std::string_view line) {
  const auto indent = leading_indent(line);
  std::size_t i = indent;
  std::size_t hashes = 0;
  while (i + hashes < line.size() && line[i + hashes] == '#') ++hashes;
  if (hashes < 1 || hashes > 6) return std::nullopt;
  if (i + hashes < line.size() && !is_space(line[i + hashes])) return std::nullopt;
  auto text = line.substr(std::min(line.size(), i + hashes));
  text = trim(text);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
    text.remove_suffix(1);
  std::size_t closing = 0;
  while (closing < text.size() && text[text.size() - 1 - closing] == '#') ++closing;
  if (closing > 0 && closing < text.size() && text[text.size() - 1 - closing] == ' ') {
    text.remove_suffix(closing);
    text = trim(text);
  }
  return std::pair<std::uint8_t, std::string>{static_cast<std::uint8_t>(hashes),
                                              std::string{text}};
}

[[nodiscard]] bool is_quote_line(std::string_view line) noexcept {
  const auto indent = leading_indent(line);
  return indent < line.size() && line[indent] == '>';
}

[[nodiscard]] std::string_view strip_quote(std::string_view line) noexcept {
  auto i = leading_indent(line);
  if (i < line.size() && line[i] == '>') ++i;
  if (i < line.size() && line[i] == ' ') ++i;
  return line.substr(i);
}

struct list_match {
  std::string marker;
  std::string_view content;
  bool ordered{};
  bool task{};
  bool checked{};
  std::uint8_t level{};
};

[[nodiscard]] std::optional<list_match> match_list(std::string_view line) {
  std::size_t indent = 0;
  while (indent < line.size() && line[indent] == ' ') ++indent;
  if (indent > 24) return std::nullopt;
  const auto rest = line.substr(indent);
  bool ordered = false;
  std::size_t marker_len = 0;
  std::string label;
  if (rest.starts_with("- ") || rest.starts_with("* ") || rest.starts_with("+ ")) {
    marker_len = 2;
    label = "• ";
  } else {
    std::size_t digits = 0;
    while (digits < rest.size() && digits < 9 && rest[digits] >= '0' &&
           rest[digits] <= '9')
      ++digits;
    if (digits == 0 || digits + 1 >= rest.size() || rest[digits] != '.' ||
        rest[digits + 1] != ' ')
      return std::nullopt;
    marker_len = digits + 2;
    label = std::string{rest.substr(0, digits)} + ". ";
    ordered = true;
  }
  auto content = rest.substr(marker_len);
  bool task = false;
  bool checked = false;
  if (content.starts_with("[ ] ") || content.starts_with("[x] ") ||
      content.starts_with("[X] ")) {
    task = true;
    checked = content[1] != ' ';
    label = checked ? "[x] " : "[ ] ";
    content.remove_prefix(4);
  }
  if (indent > 0) label.insert(0, indent, ' ');
  return list_match{std::move(label),
                    content,
                    ordered,
                    task,
                    checked,
                    static_cast<std::uint8_t>(std::min<std::size_t>(indent / 2, 6))};
}

[[nodiscard]] bool starts_block(std::string_view line) {
  return match_fence(line).has_value() || match_heading(line).has_value() ||
         is_rule(line) || is_quote_line(line) || match_list(line).has_value();
}

[[nodiscard]] bool boundary_at(std::string_view text, std::size_t index) noexcept {
  if (index >= text.size()) return true;
  const unsigned char c = static_cast<unsigned char>(text[index]);
  return is_space(static_cast<char>(c)) || c == '(' || c == ')' || c == '[' ||
         c == ']' || c == '"' || c == '\'' || c == '.' || c == ',' || c == '!' ||
         c == '?' || c == ':' || c == ';';
}

[[nodiscard]] std::size_t find_run(std::string_view text, std::size_t from, char marker,
                                   std::size_t count) noexcept {
  while (from < text.size()) {
    if (text[from] != marker) {
      ++from;
      continue;
    }
    std::size_t run = 0;
    while (from + run < text.size() && text[from + run] == marker) ++run;
    if (run == count) return from;
    from += run;
  }
  return std::string_view::npos;
}

[[nodiscard]] std::string match_angle_url(std::string_view text) {
  if (!text.starts_with("<http://") && !text.starts_with("<https://") &&
      !text.starts_with("<mailto:"))
    return {};
  const auto close = text.find('>');
  if (close == std::string_view::npos || close < 2) return {};
  const auto url = text.substr(1, close - 1);
  if (url.find_first_of(" \t\r\n") != std::string_view::npos) return {};
  return std::string{url};
}

[[nodiscard]] std::string match_bare_url(std::string_view text) {
  const bool https = text.starts_with("https://");
  const bool http = text.starts_with("http://");
  if (!https && !http) return {};
  const std::size_t scheme = https ? 8 : 7;
  std::size_t n = scheme;
  while (n < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[n]);
    if (c <= 0x20 || c == 0x7F || c == '<' || c == '>') break;
    ++n;
  }
  while (n > scheme && (text[n - 1] == '.' || text[n - 1] == ',' ||
                        text[n - 1] == ';' || text[n - 1] == ':' ||
                        text[n - 1] == '!' || text[n - 1] == '?' || text[n - 1] == ')'))
    --n;
  if (n <= scheme) return {};
  return std::string{text.substr(0, n)};
}

[[nodiscard]] std::vector<markdown_span> parse_inlines(std::string_view text,
                                                       markdown_mark base, int depth);

[[nodiscard]] std::optional<std::string> read_destination(std::string_view text,
                                                          std::size_t open,
                                                          std::size_t& end) {
  if (open >= text.size() || text[open] != '(') return std::nullopt;
  std::size_t i = open + 1;
  while (i < text.size() && is_space(text[i])) ++i;
  std::string url;
  if (i < text.size() && text[i] == '<') {
    const auto close = text.find('>', i + 1);
    if (close == std::string_view::npos) return std::nullopt;
    url.assign(text.substr(i + 1, close - i - 1));
    i = close + 1;
  } else {
    int depth = 1;
    for (; i < text.size(); ++i) {
      if (is_space(text[i])) break;
      if (text[i] == '(') {
        ++depth;
        url.push_back('(');
        continue;
      }
      if (text[i] == ')') {
        --depth;
        if (depth == 0) {
          end = i;
          return url;
        }
        url.push_back(')');
        continue;
      }
      url.push_back(text[i]);
    }
  }
  while (i < text.size() && is_space(text[i])) ++i;
  if (i < text.size() && (text[i] == '"' || text[i] == '\'')) {
    const char quote = text[i++];
    while (i < text.size() && text[i] != quote) ++i;
    if (i < text.size()) ++i;
  }
  while (i < text.size() && is_space(text[i])) ++i;
  if (i >= text.size() || text[i] != ')') return std::nullopt;
  end = i;
  return url;
}

struct link_parse {
  std::vector<markdown_span> spans;
  std::size_t end{};
};

[[nodiscard]] std::optional<link_parse> try_link(std::string_view text,
                                                 markdown_mark base, int depth,
                                                 std::string_view empty_label) {
  if (depth > 8 || text.empty() || text.front() != '[') return std::nullopt;
  int brackets = 1;
  std::size_t i = 1;
  for (; i < text.size(); ++i) {
    if (text[i] == '\\' && i + 1 < text.size()) {
      ++i;
      continue;
    }
    if (text[i] == '[')
      ++brackets;
    else if (text[i] == ']') {
      if (--brackets == 0) break;
    }
  }
  if (i >= text.size() || text[i] != ']' || i + 1 >= text.size() || text[i + 1] != '(')
    return std::nullopt;
  std::size_t close = 0;
  const auto url = read_destination(text, i + 1, close);
  if (!url || close == 0) return std::nullopt;
  std::vector<markdown_span> spans;
  if (i == 1) {
    spans.push_back(
        {empty_label.empty() ? *url : std::string{empty_label}, base, *url});
  } else {
    spans = parse_inlines(text.substr(1, i - 1), base, depth + 1);
    if (spans.empty()) spans.push_back({*url, base, *url});
    for (auto& span : spans) {
      if (span.target.empty()) span.target = *url;
    }
  }
  return link_parse{std::move(spans), close + 1};
}

struct marker_parse {
  std::vector<markdown_span> spans;
  std::size_t next{};
};

[[nodiscard]] std::optional<marker_parse> try_marker(std::string_view text,
                                                     std::size_t at, markdown_mark base,
                                                     int depth) {
  if (depth > 8 || at >= text.size()) return std::nullopt;
  const char marker = text[at];
  std::size_t count = 0;
  markdown_mark added = markdown_mark::none;
  if (marker == '~') {
    if (at + 1 >= text.size() || text[at + 1] != '~') return std::nullopt;
    count = 2;
    added = markdown_mark::strike;
  } else if (marker == '*' || marker == '_') {
    while (at + count < text.size() && text[at + count] == marker && count < 3) ++count;
    if (count == 3)
      added = markdown_mark::strong | markdown_mark::emphasis;
    else if (count == 2)
      added = markdown_mark::strong;
    else
      added = markdown_mark::emphasis;
    if (marker == '_' && !(at == 0 || boundary_at(text, at - 1))) return std::nullopt;
  } else {
    return std::nullopt;
  }
  const auto close = find_run(text, at + count, marker, count);
  if (close == std::string_view::npos || close == at + count) return std::nullopt;
  if (marker == '_' && !boundary_at(text, close + count)) return std::nullopt;
  auto inner = parse_inlines(text.substr(at + count, close - (at + count)),
                             base | added, depth + 1);
  return marker_parse{std::move(inner), close + count};
}

std::vector<markdown_span> parse_inlines(std::string_view text, markdown_mark base,
                                         int depth) {
  std::vector<markdown_span> out;
  std::string literal;
  const auto flush = [&] {
    if (literal.empty()) return;
    out.push_back({std::move(literal), base, {}});
    literal.clear();
  };
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '\\' && i + 1 < text.size() &&
        is_punct(static_cast<unsigned char>(text[i + 1]))) {
      literal.push_back(text[i + 1]);
      i += 2;
      continue;
    }
    if (text[i] == '`') {
      std::size_t count = 0;
      while (i + count < text.size() && text[i + count] == '`') ++count;
      const auto close = find_run(text, i + count, '`', count);
      if (close == std::string_view::npos) {
        literal.append(count, '`');
        i += count;
        continue;
      }
      flush();
      auto body = text.substr(i + count, close - (i + count));
      if (body.size() >= 2 && body.front() == ' ' && body.back() == ' ' &&
          body.find(' ') != std::string_view::npos) {
        const bool edge = body.front() == ' ' && body.back() == ' ';
        if (edge && !(body.size() > 2 && body[1] == '`')) {
          body.remove_prefix(1);
          body.remove_suffix(1);
        }
      }
      out.push_back({std::string{body}, base | markdown_mark::code, {}});
      i = close + count;
      continue;
    }
    if (text[i] == '<') {
      const auto url = match_angle_url(text.substr(i));
      if (!url.empty()) {
        flush();
        out.push_back({url, base, url});
        i += url.size() + 2;
        continue;
      }
    }
    if (text[i] == '!' && i + 1 < text.size() && text[i + 1] == '[') {
      if (auto linked = try_link(text.substr(i + 1), base, depth, "image")) {
        flush();
        out.insert(out.end(), linked->spans.begin(), linked->spans.end());
        i += 1 + linked->end;
        continue;
      }
    }
    if (text[i] == '[') {
      if (auto linked = try_link(text.substr(i), base, depth, {})) {
        flush();
        out.insert(out.end(), linked->spans.begin(), linked->spans.end());
        i += linked->end;
        continue;
      }
    }
    if (auto marked = try_marker(text, i, base, depth)) {
      flush();
      out.insert(out.end(), marked->spans.begin(), marked->spans.end());
      i = marked->next;
      continue;
    }
    if (const auto url = match_bare_url(text.substr(i)); !url.empty()) {
      flush();
      out.push_back({url, base, url});
      i += url.size();
      continue;
    }
    literal.push_back(text[i]);
    ++i;
  }
  flush();
  return out;
}

[[nodiscard]] bool take_hard_break(std::string_view& line) noexcept {
  if (line.size() >= 2 && line.ends_with("  ")) {
    while (!line.empty() && line.back() == ' ') line.remove_suffix(1);
    return true;
  }
  if (!line.empty() && line.back() == '\\') {
    line.remove_suffix(1);
    return true;
  }
  return false;
}

[[nodiscard]] std::vector<markdown_span> join_lines(
    const std::vector<std::string_view>& lines) {
  std::vector<markdown_span> out;
  bool hard_before = false;
  bool any = false;
  for (auto line : lines) {
    const bool hard = take_hard_break(line);
    if (any) {
      out.push_back({hard_before ? "\n" : " ", markdown_mark::none, {}});
    }
    auto part = parse_inlines(line, markdown_mark::none, 0);
    out.insert(out.end(), std::make_move_iterator(part.begin()),
               std::make_move_iterator(part.end()));
    hard_before = hard;
    any = true;
  }
  return out;
}

struct unit {
  std::size_t span{};
  std::size_t begin{};
  std::size_t end{};
  std::uint32_t width{};
  bool space{};
};

void push_span_units(const std::vector<markdown_span>& spans, std::size_t index,
                     bool frozen, std::vector<unit>& units) {
  const auto& span = spans[index];
  if (span.text.empty() || span.text == "\n") return;
  if (frozen) {
    units.push_back({index, 0, span.text.size(),
                     static_cast<std::uint32_t>(display_width(span.text)), false});
    return;
  }
  std::size_t offset = 0;
  while (offset < span.text.size()) {
    const std::size_t next = next_grapheme_boundary(span.text, offset);
    const auto cluster = span.text.substr(offset, next - offset);
    units.push_back({index, offset, next,
                     static_cast<std::uint32_t>(grapheme_width(cluster)),
                     cluster == " "});
    offset = next;
  }
}

void emit_units(std::vector<markdown_line>& out, markdown_role role, std::uint8_t level,
                const std::vector<markdown_span>& spans, const std::vector<unit>& units,
                std::size_t from, std::size_t to, std::uint32_t indent) {
  markdown_line line{role, level, {}};
  if (indent > 0)
    line.spans.push_back({std::string(indent, ' '), markdown_mark::none, {}});
  for (std::size_t i = from; i < to; ++i) {
    const auto& unit_value = units[i];
    const auto& source = spans[unit_value.span];
    auto text = source.text.substr(unit_value.begin, unit_value.end - unit_value.begin);
    if (text.empty()) continue;
    if (!line.spans.empty() && line.spans.back().marks == source.marks &&
        line.spans.back().target == source.target) {
      line.spans.back().text += text;
    } else {
      line.spans.push_back({std::string{text}, source.marks, source.target});
    }
  }
  out.push_back(std::move(line));
}

void wrap_segment(std::vector<markdown_line>& out, markdown_role role,
                  std::uint8_t level, const std::vector<markdown_span>& spans,
                  std::uint32_t width, std::uint32_t first_indent,
                  std::uint32_t hanging, std::size_t frozen) {
  if (width == 0) return;
  std::vector<unit> units;
  for (std::size_t i = 0; i < spans.size(); ++i)
    push_span_units(spans, i, i < frozen, units);
  if (units.empty()) {
    markdown_line line{role, level, {}};
    if (first_indent > 0)
      line.spans.push_back({std::string(first_indent, ' '), markdown_mark::none, {}});
    out.push_back(std::move(line));
    return;
  }
  bool first = true;
  std::size_t cursor = 0;
  while (cursor < units.size()) {
    const auto indent =
        first ? std::min(first_indent, width - 1) : std::min(hanging, width - 1);
    std::uint32_t column = width == 0 ? 0 : indent;
    const std::size_t start = cursor;
    std::size_t last_space = std::string_view::npos;
    while (cursor < units.size()) {
      const auto cell_width = units[cursor].width;
      if (column + cell_width > width && cursor > start) break;
      if (units[cursor].space) last_space = cursor;
      column += cell_width;
      ++cursor;
      if (column >= width) break;
    }
    std::size_t end = cursor;
    if (cursor < units.size() && last_space != std::string_view::npos &&
        last_space >= start) {
      end = last_space;
      cursor = last_space + 1;
      if (end == start) {
        end = start + 1;
        cursor = end;
      }
    } else if (end == start) {
      end = start + 1;
      cursor = end;
    }
    emit_units(out, role, level, spans, units, start, end, indent);
    first = false;
  }
}

void layout_flow(std::vector<markdown_line>& out, markdown_role role,
                 std::uint8_t level, const std::vector<markdown_span>& spans,
                 std::uint32_t width, std::uint32_t hanging, std::size_t frozen) {
  std::vector<markdown_span> segment;
  bool first = true;
  const auto flush = [&] {
    wrap_segment(out, role, level, segment, width, first ? 0U : hanging, hanging,
                 first ? frozen : 0U);
    segment.clear();
    first = false;
  };
  for (const auto& span : spans) {
    if (span.text == "\n" && span.marks == markdown_mark::none && span.target.empty()) {
      flush();
      continue;
    }
    segment.push_back(span);
  }
  if (!segment.empty() || first) flush();
}

}  // namespace

namespace avionix {

markdown_document parse_markdown(std::string_view source) {
  const auto lines = split_lines(source);
  markdown_document document;
  for (std::size_t i = 0; i < lines.size();) {
    if (is_blank(lines[i])) {
      ++i;
      continue;
    }
    if (const auto fence = match_fence(lines[i])) {
      markdown_block block;
      block.type = markdown_block::kind::code;
      block.language = fence->info;
      ++i;
      while (i < lines.size() && !is_fence_close(lines[i], *fence)) {
        block.spans.push_back({std::string{lines[i]}, markdown_mark::code, {}});
        ++i;
      }
      if (i < lines.size()) ++i;
      document.blocks.push_back(std::move(block));
      continue;
    }
    if (const auto heading = match_heading(lines[i])) {
      markdown_block block;
      block.type = markdown_block::kind::heading;
      block.level = heading->first;
      block.spans = parse_inlines(heading->second, markdown_mark::none, 0);
      document.blocks.push_back(std::move(block));
      ++i;
      continue;
    }
    if (is_rule(lines[i])) {
      markdown_block block;
      block.type = markdown_block::kind::rule;
      document.blocks.push_back(std::move(block));
      ++i;
      continue;
    }
    if (is_quote_line(lines[i])) {
      std::vector<std::string_view> quoted;
      while (i < lines.size() && is_quote_line(lines[i])) {
        const auto inner = strip_quote(lines[i]);
        if (is_blank(inner)) {
          ++i;
          break;
        }
        quoted.push_back(inner);
        ++i;
      }
      markdown_block block;
      block.type = markdown_block::kind::quote;
      block.spans = join_lines(quoted);
      document.blocks.push_back(std::move(block));
      continue;
    }
    if (const auto item = match_list(lines[i])) {
      markdown_block block;
      block.type = markdown_block::kind::list_item;
      block.level = item->level;
      block.ordered = item->ordered;
      block.task = item->task;
      block.checked = item->checked;
      block.marker = item->marker;
      block.spans = parse_inlines(item->content, markdown_mark::none, 0);
      document.blocks.push_back(std::move(block));
      ++i;
      continue;
    }
    std::vector<std::string_view> paragraph;
    while (i < lines.size() && !is_blank(lines[i]) && !starts_block(lines[i])) {
      paragraph.push_back(lines[i]);
      ++i;
    }
    markdown_block block;
    block.type = markdown_block::kind::paragraph;
    block.spans = join_lines(paragraph);
    document.blocks.push_back(std::move(block));
  }
  return document;
}

std::vector<markdown_line> layout_markdown(const markdown_document& document,
                                           std::uint32_t width) {
  std::vector<markdown_line> lines;
  if (width == 0) return lines;
  for (std::size_t index = 0; index < document.blocks.size(); ++index) {
    const auto& block = document.blocks[index];
    const bool tight =
        index > 0 && block.type == markdown_block::kind::list_item &&
        document.blocks[index - 1].type == markdown_block::kind::list_item;
    if (!lines.empty() && !tight)
      lines.push_back(markdown_line{markdown_role::body, 0, {}});
    switch (block.type) {
      case markdown_block::kind::rule: {
        std::string bar;
        bar.reserve(static_cast<std::size_t>(width) * 3U);
        for (std::uint32_t column = 0; column < width; ++column) bar += "─";
        lines.push_back(
            {markdown_role::rule, 0, {{std::move(bar), markdown_mark::none, {}}}});
        break;
      }
      case markdown_block::kind::code:
        for (const auto& span : block.spans) {
          wrap_segment(lines, markdown_role::code, 0, {span}, width, 0, 0, 0);
        }
        if (block.spans.empty()) lines.push_back({markdown_role::code, 0, {}});
        break;
      case markdown_block::kind::heading:
        layout_flow(lines, markdown_role::heading, block.level, block.spans, width, 0,
                    0);
        break;
      case markdown_block::kind::quote: {
        std::vector<markdown_span> spans{{"│ ", markdown_mark::none, {}}};
        spans.insert(spans.end(), block.spans.begin(), block.spans.end());
        layout_flow(lines, markdown_role::quote, 0, spans, width, 2, 1);
        break;
      }
      case markdown_block::kind::list_item: {
        std::vector<markdown_span> spans{{block.marker, markdown_mark::none, {}}};
        spans.insert(spans.end(), block.spans.begin(), block.spans.end());
        const auto hanging = static_cast<std::uint32_t>(display_width(block.marker));
        layout_flow(lines, markdown_role::body, block.level, spans, width, hanging, 1);
        break;
      }
      case markdown_block::kind::paragraph:
        layout_flow(lines, markdown_role::body, 0, block.spans, width, 0, 0);
        break;
    }
  }
  return lines;
}

}  // namespace avionix
