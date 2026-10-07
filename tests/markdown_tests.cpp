// Markdown parsing, layout, hyperlinks, and scroll-to-copy.

#include <format>
#include <string>
#include <string_view>
#include <vector>

import avionix;
import avionix_tests.check;

using namespace avionix;
using avionix_tests::check;
using avionix_tests::check_equal;

namespace {

std::string joined(const std::vector<markdown_span>& spans) {
  std::string out;
  for (const auto& span : spans) out += span.text;
  return out;
}

std::string line_text(const markdown_line& line) { return joined(line.spans); }

const markdown_span* span_with(const std::vector<markdown_span>& spans,
                               markdown_mark mark) {
  for (const auto& span : spans)
    if (has(span.marks, mark)) return &span;
  return nullptr;
}

event pointer(mouse_action action, position where) {
  return mouse_event{mouse_button::left, action, where};
}

const avionix_tests::suite markdown{
    "markdown",
    {
        {"headings_breaks_and_rules",
         [] {
           const auto doc =
               parse_markdown("# Title #\n\n#Title\n\nalpha  \nbeta\n\n---\n");
           check_equal(doc.blocks.size(), std::size_t{4});
           check(doc.blocks[0].type == markdown_block::kind::heading);
           check_equal(doc.blocks[0].level, 1);
           check(joined(doc.blocks[0].spans) == "Title");
           check(doc.blocks[1].type == markdown_block::kind::paragraph);
           check(joined(doc.blocks[1].spans) == "#Title");
           check(doc.blocks[2].type == markdown_block::kind::paragraph);
           check_equal(doc.blocks[2].spans.size(), std::size_t{3});
           check(doc.blocks[2].spans[0].text == "alpha");
           check(doc.blocks[2].spans[1].text == "\n");
           check(doc.blocks[2].spans[2].text == "beta");
           check(doc.blocks[3].type == markdown_block::kind::rule);

           const auto lines = layout_markdown(doc, 20);
           check(line_text(lines[0]) == "Title");
           check(lines[0].role == markdown_role::heading);
           bool saw_alpha = false;
           bool saw_beta = false;
           for (const auto& line : lines) {
             if (line_text(line) == "alpha") saw_alpha = true;
             if (line_text(line) == "beta") saw_beta = true;
           }
           check(saw_alpha);
           check(saw_beta);
           check(lines.back().role == markdown_role::rule);
           check_equal(static_cast<int>(display_width(line_text(lines.back()))), 20);
           check(layout_markdown(doc, 0).empty());
         }},
        {"emphasis_code_and_escapes",
         [] {
           const auto doc = parse_markdown(
               "say *em* **strong** ***both*** ~~gone~~ `code` ` a ` foo_bar_baz a "
               "_em_ "
               "b \\* <div>\n");
           check_equal(doc.blocks.size(), std::size_t{1});
           const auto& spans = doc.blocks[0].spans;
           const auto* em = span_with(spans, markdown_mark::emphasis);
           const auto* strong = span_with(spans, markdown_mark::strong);
           const auto* strike = span_with(spans, markdown_mark::strike);
           const auto* code = span_with(spans, markdown_mark::code);
           check(em != nullptr && em->text == "em");
           check(strong != nullptr && strong->text == "strong");
           check(strike != nullptr && strike->text == "gone");
           check(code != nullptr && code->text == "code");
           bool both = false;
           bool trimmed = false;
           bool literal_underscore = false;
           bool escaped = false;
           for (const auto& span : spans) {
             if (span.text == "both" && has(span.marks, markdown_mark::strong) &&
                 has(span.marks, markdown_mark::emphasis))
               both = true;
             if (span.text == "a" && has(span.marks, markdown_mark::code))
               trimmed = true;
             if (span.text.find("foo_bar_baz") != std::string::npos)
               literal_underscore = true;
             if (span.text.find('*') != std::string::npos &&
                 !has(span.marks, markdown_mark::emphasis))
               escaped = true;
           }
           check(both);
           check(trimmed);
           check(literal_underscore);
           check(escaped);
           check(joined(spans).find("<div>") != std::string::npos);
         }},
        {"links_images_and_autolinks",
         [] {
           const auto doc = parse_markdown(
               "[docs](https://example.com \"title\")\n"
               "![shot](https://example.com/a.png)\n"
               "![](https://example.com/b.png)\n"
               "<https://example.com/c>\n"
               "see https://example.com/d.\n");
           check_equal(doc.blocks.size(), std::size_t{1});
           const auto& spans = doc.blocks[0].spans;
           auto target_of = [&](std::string_view text) -> std::string {
             for (const auto& span : spans)
               if (span.text == text) return span.target;
             return {};
           };
           check(target_of("docs") == "https://example.com");
           check(target_of("shot") == "https://example.com/a.png");
           check(target_of("image") == "https://example.com/b.png");
           check(target_of("https://example.com/c") == "https://example.com/c");
           check(target_of("https://example.com/d") == "https://example.com/d");
         }},
        {"lists_quotes_and_fences",
         [] {
           const auto doc = parse_markdown(
               "- item\n"
               "- [x] done\n"
               "- [ ] later\n"
               "2. Second\n"
               "\n"
               "> quoted\n"
               "\n"
               "```cpp\n"
               "*not em*\n"
               "a  b\n"
               "```\n");
           check(doc.blocks[0].type == markdown_block::kind::list_item);
           check(doc.blocks[0].marker == "• ");
           check(joined(doc.blocks[0].spans) == "item");
           check(doc.blocks[1].task && doc.blocks[1].checked);
           check(doc.blocks[1].marker == "[x] ");
           check(doc.blocks[2].task && !doc.blocks[2].checked);
           check(doc.blocks[2].marker == "[ ] ");
           check(doc.blocks[3].ordered);
           check(doc.blocks[3].marker == "2. ");
           check(doc.blocks[4].type == markdown_block::kind::quote);
           check(joined(doc.blocks[4].spans) == "quoted");
           check(doc.blocks[5].type == markdown_block::kind::code);
           check(doc.blocks[5].language == "cpp");
           check(doc.blocks[5].spans[0].text == "*not em*");
           check(has(doc.blocks[5].spans[0].marks, markdown_mark::code));
           check(doc.blocks[5].spans[1].text == "a  b");

           const auto lines = layout_markdown(doc, 24);
           check(line_text(lines[0]) == "• item");
           bool quote = false;
           bool code = false;
           for (const auto& line : lines) {
             if (line.role == markdown_role::quote &&
                 line_text(line).find("quoted") != std::string::npos) {
               quote = true;
               check(line_text(line).starts_with("│ "));
             }
             if (line.role == markdown_role::code && line_text(line) == "a  b")
               code = true;
           }
           check(quote);
           check(code);
         }},
        {"view_styles_links_and_copy",
         [] {
           markdown_view view{
               "# Title\n\n## Section\n\n**bold** *lean* ~~out~~ `code`\n"};
           auto screen = application::render_to_buffer(view, {40, 10});
           const auto title = screen.find("Title");
           const auto section = screen.find("Section");
           const auto bold = screen.find("bold");
           const auto lean = screen.find("lean");
           const auto struck = screen.find("out");
           const auto code = screen.find("code");
           check(title.has_value());
           check(has(screen.at(*title).appearance().attributes, attribute::bold));
           check(screen.at(*title).appearance().foreground == colors::bright_white);
           check(section.has_value());
           check(screen.at(*section).appearance().foreground == colors::bright_cyan);
           check(bold.has_value());
           check(has(screen.at(*bold).appearance().attributes, attribute::bold));
           check(lean.has_value());
           check(has(screen.at(*lean).appearance().attributes, attribute::italic));
           check(struck.has_value());
           check(has(screen.at(*struck).appearance().attributes,
                     attribute::strikethrough));
           check(code.has_value());
           check(screen.at(*code).appearance().background == colors::bright_black);

           markdown_view linked{
               "See [docs](https://example.com) and [api](https://example.com/api)."};
           screen = application::render_to_buffer(linked, {60, 3});
           const auto docs = screen.find("docs");
           const auto api = screen.find("api");
           check(docs.has_value() && api.has_value());
           check(screen.at(*docs).link() != 0);
           check(screen.at(*api).link() != 0);
           check(screen.at(*docs).link() != screen.at(*api).link());
           check(screen.link_target(screen.at(*docs).link()) == "https://example.com");
           check(screen.link_target(screen.at(*api).link()) ==
                 "https://example.com/api");
           check(has(screen.at(*docs).appearance().attributes, attribute::underline));
           check(screen.at(*docs).appearance().foreground == colors::bright_blue);

           std::string opened;
           linked.on_link([&](const std::string& url) { opened = url; });
           application app{{.quit_on_ctrl_c = false}};
           app.simulate(linked, pointer(mouse_action::press, *docs));
           app.simulate(linked, pointer(mouse_action::release, *docs));
           check(opened == "https://example.com");
           check(app.last_copied().empty());
           check(!linked.has_selection());

           markdown_view sentence{"alpha beta"};
           (void)application::render_to_buffer(sentence, {20, 3});
           app.simulate(sentence, pointer(mouse_action::press, {0, 0}));
           app.simulate(sentence, pointer(mouse_action::drag, {4, 0}));
           app.simulate(sentence, pointer(mouse_action::release, {4, 0}));
           check(sentence.has_selection());
           check(sentence.selected_text() == "alpha");
           check(app.last_copied() == "alpha");
           screen = application::render_to_buffer(sentence, {20, 3});
           check(has(screen.at({0, 0}).appearance().attributes, attribute::reverse));

           bool late = false;
           app.on_event([&](const event&) {
             late = true;
             return false;
           });
           app.simulate(sentence, key_event{key::character, U'c', modifiers::ctrl});
           check(!late);
           check(app.last_copied() == "alpha");

           markdown_view idle{"alpha"};
           (void)application::render_to_buffer(idle, {10, 2});
           late = false;
           app.simulate(idle, key_event{key::character, U'c', modifiers::ctrl});
           check(late);
         }},
        {"scroll_to_copy_keeps_code_spaces",
         [] {
           std::string source = "```\n";
           for (int i = 0; i <= 20; ++i) source += std::format("Line {:02}\n", i);
           source += "```\n";
           markdown_view view{std::move(source)};
           auto screen = application::render_to_buffer(view, {16, 4});
           check(screen.find("Line 00").has_value());
           check(!screen.find("Jump to latest").has_value());
           check(!screen.find("Line 10").has_value());

           application app;
           app.simulate(view, pointer(mouse_action::press, {0, 0}));
           app.simulate(view, pointer(mouse_action::drag, {0, 30}));
           check(view.scroll_offset() > 4);
           app.simulate(view, pointer(mouse_action::release, {0, 30}));
           check(app.last_copied().find("Line 00") != std::string::npos);
           check(app.last_copied().find("Line 10") != std::string::npos);
           screen = application::render_to_buffer(view, {16, 4});
           check(has(screen.at({0, 0}).appearance().attributes, attribute::reverse));
           check(screen.find("Line 00") == std::nullopt);

           scroll_view code;
           code.set_wrap(false);
           code.set_follow_tail(false);
           code.set_jump_prompt(false);
           code.set_lines({{{"code  ", {}}}});
           (void)application::render_to_buffer(code, {10, 2});
           app.simulate(code, pointer(mouse_action::press, {0, 0}));
           app.simulate(code, pointer(mouse_action::drag, {5, 0}));
           app.simulate(code, pointer(mouse_action::release, {5, 0}));
           check(app.last_copied() == "code  ");
         }},
    }};

}  // namespace
