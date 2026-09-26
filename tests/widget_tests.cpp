// Controls and application event routing, without a terminal.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

import avionix.entity.geometry;
import avionix.entity.style;
import avionix.entity.event;
import avionix.entity.constraint;
import avionix.entity.cell;
import avionix.object.buffer;
import avionix.interface.widget;
import avionix.interface.layout;
import avionix.interface.controls;
import avionix.interface.application;
import avionix_tests.check;

using namespace avionix;
using avionix_tests::check;
using avionix_tests::check_equal;

namespace {

std::string row_text(const render_buffer& b, std::uint32_t y) {
  std::string out;
  for (const cell& c : b.row(y)) {
    if (!c.is_continuation()) out += c.text();
  }
  return out;
}

event key(char32_t c, modifiers m = modifiers::none) {
  return event{key_event{avionix::key::character, c, m}};
}

event key(avionix::key k, modifiers m = modifiers::none) {
  return event{key_event{k, 0, m}};
}

const avionix_tests::suite text_widget{
    "text",
    {
        {"alignment",
         [] {
           text t{"ab"};
           t.align(alignment::right);
           check(row_text(application::render_to_buffer(t, {5, 1}), 0) == "   ab");
           t.align(alignment::center);
           check(row_text(application::render_to_buffer(t, {6, 1}), 0) == "  ab  ");
         }},
        {"multiline",
         [] {
           text t{"one\ntwo"};
           const auto b = application::render_to_buffer(t, {5, 3});
           check(row_text(b, 0) == "one  ");
           check(row_text(b, 1) == "two  ");
         }},
        {"wrap_by_words",
         [] {
           const auto lines = wrap_text("the quick brown fox", 10);
           check_equal(lines.size(), std::size_t{2});
           check(lines[0] == "the quick");
           check(lines[1] == "brown fox");
         }},
        {"wrap_breaks_long_words",
         [] {
           const auto lines = wrap_text("abcdefgh", 3);
           check_equal(lines.size(), std::size_t{3});
           check(lines[2] == "gh");
         }},
        {"wrap_counts_display_width",
         [] {
           // Each CJK character is two cells wide.
           const auto lines = wrap_text("\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97", 4);
           check_equal(lines.size(), std::size_t{2});
         }},
        {"wrap_keeps_blank_lines",
         [] {
           const auto lines = wrap_text("a\n\nb", 5);
           check_equal(lines.size(), std::size_t{3});
           check(lines[1].empty());
         }},
    }};

const avionix_tests::suite block_widget{
    "block",
    {
        {"border_and_title",
         [] {
           block b{"T", border_kind::single};
           const auto buf = application::render_to_buffer(b, {6, 3});
           check(row_text(buf, 0) == "\xE2\x94\x8C T \xE2\x94\x80\xE2\x94\x90");
           check(row_text(buf, 1) == "\xE2\x94\x82    \xE2\x94\x82");
           check(row_text(buf, 2) ==
                 "\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94"
                 "\x98");
         }},
        {"child_inside_border",
         [] {
           block b{"", border_kind::single, text{"hi"}};
           const auto buf = application::render_to_buffer(b, {4, 3});
           check(row_text(buf, 1) == "\xE2\x94\x82hi\xE2\x94\x82");
         }},
        {"too_small_skips_border",
         [] {
           block b{"x", border_kind::single};
           const auto buf = application::render_to_buffer(b, {1, 1});
           check(row_text(buf, 0) == " ");
         }},
    }};

const avionix_tests::suite list_widget{
    "list_view",
    {
        {"navigation",
         [] {
           list_view list{{"a", "b", "c"}};
           application app;
           app.simulate(list, key(key::down));
           check_equal(list.selected(), std::size_t{1});
           app.simulate(list, key(key::end));
           check_equal(list.selected(), std::size_t{2});
           app.simulate(list, key(key::down));
           check_equal(list.selected(), std::size_t{2});
           app.simulate(list, key(key::home));
           check_equal(list.selected(), std::size_t{0});
         }},
        {"activation_callback",
         [] {
           list_view list{{"a", "b"}};
           std::string activated;
           list.on_activate(
               [&](std::size_t, const std::string& item) { activated = item; });
           application app;
           app.simulate(list, key(key::down));
           app.simulate(list, key(key::enter));
           check(activated == "b");
         }},
        {"scrolls_to_selection",
         [] {
           list_view list{{"0", "1", "2", "3", "4", "5"}};
           list.select(5);
           const auto buf = application::render_to_buffer(list, {4, 2});
           check(row_text(buf, 1).find('5') != std::string::npos);
         }},
        {"mouse_click_selects",
         [] {
           list_view list{{"a", "b", "c"}};
           (void)application::render_to_buffer(list, {5, 3});
           application app;
           app.simulate(
               list,
               event{mouse_event{
                   mouse_button::left, mouse_action::press, {1, 2}, modifiers::none}});
           check_equal(list.selected(), std::size_t{2});
         }},
    }};

const avionix_tests::suite input_widget{
    "text_input",
    {
        {"typing_and_backspace",
         [] {
           text_input input;
           application app;
           app.simulate(input, key(U'h'));
           app.simulate(input, key(U'i'));
           check(input.value() == "hi");
           app.simulate(input, key(key::backspace));
           check(input.value() == "h");
         }},
        {"cursor_moves_by_grapheme",
         [] {
           text_input input;
           input.set_value(
               "e\xCC\x81"
               "a");  // é (decomposed) + a
           application app;
           app.simulate(input, key(key::left));
           app.simulate(input, key(key::left));
           check_equal(input.cursor(), std::size_t{0});
           app.simulate(input, key(key::right));
           check_equal(input.cursor(), std::size_t{3});
           app.simulate(input, key(key::del));
           check(input.value() == "e\xCC\x81");
         }},
        {"backspace_removes_whole_cluster",
         [] {
           text_input input;
           input.set_value(
               "a\xF0\x9F\x91\x8B\xF0\x9F\x8F\xBD");  // a + waving hand + tone
           application app;
           app.simulate(input, key(key::backspace));
           check(input.value() == "a");
         }},
        {"paste_is_sanitized",
         [] {
           text_input input;
           application app;
           app.simulate(input, event{paste_event{"a\nb\x1b[31mc"}});
           check(input.value() == "a b[31mc");
         }},
        {"submit",
         [] {
           text_input input;
           std::string submitted;
           input.on_submit([&](const std::string& v) { submitted = v; });
           input.set_value("go");
           application app;
           app.simulate(input, key(key::enter));
           check(submitted == "go");
         }},
        {"cursor_is_placed_when_focused",
         [] {
           text_input input;
           input.set_value("ab");
           input.set_focused(true);
           render_buffer buffer{{10, 1}};
           frame_state frame;
           render_context context{buffer, rect{{3, 0}, {5, 1}}, frame};
           input.render_in(context);
           check(frame.cursor == position{5, 0});
         }},
        {"horizontal_scroll_keeps_cursor_visible",
         [] {
           text_input input;
           input.set_value("abcdefghij");
           input.set_focused(true);
           render_buffer buffer{{4, 1}};
           frame_state frame;
           render_context context{buffer, buffer.area(), frame};
           input.render_in(context);
           check(frame.cursor.has_value() && frame.cursor->x == 3);
           check(row_text(buffer, 0) == "hij ");
         }},
    }};

const avionix_tests::suite progress_widget{
    "progress_bar",
    {
        {"full_and_empty",
         [] {
           progress_bar bar{1.0};
           bar.show_percentage(false);
           check(row_text(application::render_to_buffer(bar, {3, 1}), 0) ==
                 "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88");
           bar.set(0.0);
           check(row_text(application::render_to_buffer(bar, {3, 1}), 0) ==
                 "\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91");
         }},
        {"clamps", [] { check(progress_bar{2.0}.ratio() == 1.0); }},
        {"percentage_label",
         [] {
           progress_bar bar{0.5};
           const auto buf = application::render_to_buffer(bar, {10, 1});
           check(row_text(buf, 0).ends_with("  50%"));
         }},
    }};

const avionix_tests::suite routing{
    "application",
    {
        {"tab_moves_focus",
         [] {
           column root;
           auto& first = root.add(constraint::fixed(1), text_input{});
           auto& second = root.add(constraint::fixed(1), text_input{});
           application app;
           app.simulate(root, key(U'x'));
           check(first.focused());
           check(first.value() == "x");
           app.simulate(root, key(key::tab));
           check(second.focused());
           check(!first.focused());
           app.simulate(root, key(U'y'));
           check(second.value() == "y");
           app.simulate(root, key(key::tab, modifiers::shift));
           check(first.focused());
         }},
        {"unhandled_keys_bubble_to_hook",
         [] {
           column root;
           root.add(constraint::fixed(1), list_view{{"a"}});
           application app;
           bool seen = false;
           app.on_event([&](const event& e) {
             seen = std::get_if<key_event>(&e) != nullptr;
             return true;
           });
           app.simulate(root, key(U'q'));
           check(seen);
         }},
        {"ctrl_c_quits_by_default",
         [] {
           text root{"x"};
           application app;
           bool hooked = false;
           app.on_event([&](const event&) { return hooked = true; });
           app.simulate(root, key(U'c', modifiers::ctrl));
           check(!hooked);  // handled by the quit shortcut first
         }},
        {"early_hook_can_consume",
         [] {
           text_input input;
           application app;
           app.on_event([](const event&) { return true; }, true);
           app.simulate(input, key(U'z'));
           check(input.value().empty());
         }},
        {"mouse_hits_deepest_component",
         [] {
           row root;
           auto& left = root.add(constraint::fixed(3), list_view{{"a", "b"}});
           auto& right = root.add(constraint::fill(), list_view{{"c", "d"}});
           (void)application::render_to_buffer(root, {8, 2});
           application app;
           app.simulate(
               root,
               event{mouse_event{
                   mouse_button::left, mouse_action::press, {5, 1}, modifiers::none}});
           check_equal(right.selected(), std::size_t{1});
           check_equal(left.selected(), std::size_t{0});
           check(right.focused());
         }},
        {"resize_updates_screen_size",
         [] {
           text root{"x"};
           application app;
           app.simulate(root, event{resize_event{{120, 40}}});
           check(app.screen_size() == size{120, 40});
         }},
        {"widget_concept_types_are_adapted",
         [] {
           struct star {
             void render(render_context& context) const {
               context.draw_text({0, 0}, "*");
             }
           };
           static_assert(widget<star>);
           static_assert(!interactive_widget<star>);
           row root;
           root.add(constraint::fill(), star{});
           check(row_text(application::render_to_buffer(root, {2, 1}), 0) == "* ");
         }},
        {"canvas_draws_with_callable",
         [] {
           canvas c{[](render_context& context) { context.draw_text({1, 0}, "c"); }};
           check(row_text(application::render_to_buffer(c, {3, 1}), 0) == " c ");
         }},
    }};

}  // namespace
