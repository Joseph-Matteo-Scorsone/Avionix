#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
import avionix;
import avionix_tests.check;
using namespace avionix;
using avionix_tests::check;
namespace {
event key_press(key code, modifiers mods = modifiers::none) {
  return key_event{code, 0, mods};
}
event click(position p) {
  return mouse_event{mouse_button::left, mouse_action::press, p};
}
struct observed final : component {
  int* focus_changes;
  int* enters;
  int* leaves;
  observed(int& f, int& e, int& l) : focus_changes{&f}, enters{&e}, leaves{&l} {}
  bool focusable() const noexcept override { return true; }
  void render(render_context&) override {}
  void on_focus_changed(bool) override { ++*focus_changes; }
  void on_mouse_enter() override { ++*enters; }
  void on_mouse_leave() override { ++*leaves; }
};
const avionix_tests::suite features{
    "features",
    {{"stale_hit_areas",
      [] {
        column root;
        auto& nested = root.add(constraint::fixed(2), column{});
        auto& child = nested.add(constraint::fill(), button{"child"});
        auto b = application::render_to_buffer(root, {10, 2});
        check(!child.last_area().empty());
        root.set_constraint(0, constraint::fixed(0));
        b = application::render_to_buffer(root, {10, 2});
        check(child.last_area().empty());
        int presses = 0;
        child.on_press([&] { ++presses; });
        application app;
        app.simulate(root, click({0, 0}));
        check(presses == 0);
      }},
     {"visibility_and_tab",
      [] {
        row root{2};
        auto& first = root.add(constraint::fill(), button{"first"});
        auto& second = root.add(constraint::fill(), button{"second"});
        auto b = application::render_to_buffer(root, {12, 1});
        application app;
        app.simulate(root, key_press(key::tab));
        check(app.focused() == &second);
        root.set_visible(1, false);
        check(second.last_area().empty());
        b = application::render_to_buffer(root, {12, 1});
        check(first.last_area().width() == 12);
        app.simulate(root, key_press(key::tab));
        check(app.focused() == &first);
        root.set_visible(1, true);
        app.simulate(root, key_press(key::tab));
        check(app.focused() == &second);
        column vertical{1};
        vertical.add(constraint::fill(), button{"a"});
        auto& remaining = vertical.add(constraint::fill(), button{"b"});
        vertical.set_visible(0, false);
        b = application::render_to_buffer(vertical, {3, 5});
        check(remaining.last_area().height() == 5);
      }},
     {"focus_subtree_replacement",
      [] {
        block root;
        auto& old = root.set_child(button{"old"});
        application app;
        app.simulate(root, key_press(key::enter));
        check(app.focused() == &old);
        auto& next = root.set_child(button{"new"});
        int presses = 0;
        next.on_press([&] { ++presses; });
        app.simulate(root, key_press(key::enter));
        check(app.focused() == &next);
        check(presses == 1);
      }},
     {"interval_cancellation_and_mutation",
      [] {
        application app;
        int calls = 0;
        bool invalid = false;
        try {
          app.set_interval(std::chrono::milliseconds{0}, [] {});
        } catch (const std::invalid_argument&) {
          invalid = true;
        }
        check(invalid);
        auto timer = app.set_interval(std::chrono::milliseconds{10}, [&] { ++calls; });
        const auto now = std::chrono::steady_clock::now();
        app.poll_timers(now + std::chrono::milliseconds{100});
        check(calls == 1);
        timer.cancel();
        app.poll_timers(now + std::chrono::seconds{1});
        check(calls == 1);
        auto self = app.set_interval(std::chrono::milliseconds{1}, [&] {
          ++calls;
          app.set_interval(std::chrono::milliseconds{1}, [&] { ++calls; });
        });
        app.poll_timers(now + std::chrono::seconds{2});
        check(calls == 2);
        self.cancel();
        app.poll_timers(now + std::chrono::seconds{3});
        check(calls == 3);
        std::optional<interval_handle> cancelled;
        cancelled = app.set_interval(std::chrono::milliseconds{1},
                                     [&] { cancelled->cancel(); });
        app.poll_timers(now + std::chrono::seconds{4});
        check(!cancelled->active());
      }},
     {"hover_and_focus_notifications",
      [] {
        int focus_count = 0, enter_count = 0, leave_count = 0;
        row root;
        auto& node = root.add(constraint::fill(),
                              observed{focus_count, enter_count, leave_count});
        auto& other = root.add(constraint::fill(), button{"other"});
        auto b = application::render_to_buffer(root, {10, 1});
        application app{application_options{.mouse_motion = true}};
        app.simulate(root, mouse_event{mouse_button::none, mouse_action::move, {0, 0}});
        check(node.hovered());
        check(enter_count == 1);
        check(focus_count == 1);
        app.simulate(root, mouse_event{mouse_button::none, mouse_action::move, {6, 0}});
        check(!node.hovered());
        check(leave_count == 1);
        app.focus(other);
        check(focus_count == 2);
      }},
     {"button_confirmation_disabled_and_click",
      [] {
        button value{"Delete"};
        int presses = 0;
        bool enabled = true;
        value.on_press([&] { ++presses; }).set_confirm(true).set_enabled([&] {
          return enabled;
        });
        auto b = application::render_to_buffer(value, {20, 1});
        application app;
        app.simulate(value, key_press(key::enter));
        check(value.armed());
        check(presses == 0);
        value.set_focused(false);
        check(!value.armed());
        app.simulate(value, click({1, 0}));
        app.simulate(value, key_press(key::enter));
        check(presses == 1);
        value.set_confirm(false);
        app.simulate(value, key_event{key::character, U' ', modifiers::none});
        check(presses == 2);
        enabled = false;
        app.simulate(value, key_press(key::enter));
        check(presses == 2);
      }},
     {"tabs_headers_and_status",
      [] {
        tabs value{{"One", "Two"}};
        value.set_status(3, text{"OK"});
        auto b = application::render_to_buffer(value, {20, 1});
        check(b.find("OK") == position{17, 0});
        application app;
        app.simulate(value, key_press(key::right));
        check(value.active() == 1);
        app.simulate(value, click({1, 0}));
        check(value.active() == 0);
      }},
     {"styled_list_and_silent_selection",
      [] {
        list_view value;
        value.set_styled_items(
            {{{">", {}}, {"Alpha", {.add = attribute::bold}}, {"42", {}}},
             {{"", {}}, {"Beta", {}}, {"", {}}}});
        int callbacks = 0;
        value.on_select([&](auto, const auto&) { ++callbacks; });
        value.select_without_callback(1);
        check(callbacks == 0);
        check(value.selected() == 1);
        auto b = application::render_to_buffer(value, {12, 2});
        check(b.find("42") == position{10, 0});
        check(b.find("Alpha") == position{1, 0});
      }},
     {"styled_text_and_buffer_snapshots",
      [] {
        styled_text value{{{"A", {.add = attribute::bold}}, {"\u754cB\nnext", {}}}};
        auto b = application::render_to_buffer(value, {6, 2});
        check(b.rows()[0] == "A\u754cB  ");
        check(b.find("B") == position{3, 0});
        check(b.find("absent") == std::nullopt);
        check(b.find("next") == position{0, 1});
        check(has(b.at({0, 0}).appearance().attributes, attribute::bold));
      }},
     {"scroll_tail_wrap_and_latest",
      [] {
        scroll_view value{"one\ntwo\nthree\nfour\nfive"};
        auto b = application::render_to_buffer(value, {20, 3});
        check(b.find("five").has_value());
        application app;
        app.simulate(value, key_press(key::home));
        b = application::render_to_buffer(value, {20, 3});
        check(b.find("Jump to latest").has_value());
        app.simulate(value, click({1, 2}));
        b = application::render_to_buffer(value, {20, 3});
        check(value.following_tail());
        check(b.find("five").has_value());
        value.set_lines({{{"abcdefgh", {.add = attribute::bold}}}});
        b = application::render_to_buffer(value, {5, 3});
        check(b.find("efgh").has_value());
      }},
     {"overlay_top_layer_hit",
      [] {
        overlay root;
        int bottom = 0, top = 0;
        root.add(button{"bottom"}).on_press([&] { ++bottom; });
        root.add(button{"top"}).on_press([&] { ++top; });
        auto b = application::render_to_buffer(root, {10, 1});
        application app;
        app.simulate(root, click({0, 0}));
        check(top == 1);
        check(bottom == 0);
        root.set_visible(1, false);
        b = application::render_to_buffer(root, {10, 1});
        app.simulate(root, click({0, 0}));
        check(bottom == 1);
      }},
     {"text_area_newlines_submit_history",
      [] {
        text_area value;
        std::string submitted;
        value.set_value("first")
            .on_submit([&](const auto& text) { submitted = text; })
            .set_history({"older", "recent"});
        application app;
        app.simulate(value, key_press(key::enter, modifiers::shift));
        app.simulate(value, paste_event{"second"});
        check(value.value() == "first\nsecond");
        app.simulate(value, key_press(key::enter));
        check(submitted == "first\nsecond");
        app.simulate(value, key_press(key::up));
        check(value.value() == "recent");
        app.simulate(value, key_press(key::down));
        check(value.value() == submitted);
        auto b = application::render_to_buffer(value, {20, 3});
        check(b.find("second") == position{0, 1});
      }},
     {"text_area_vertical_editing_and_whitespace",
      [] {
        text_area value;
        value.set_value("ab  cd\nefg");
        auto b = application::render_to_buffer(value, {4, 4});
        check(b.rows()[0] == "ab  ");
        check(b.rows()[1] == "cd  ");
        application app;
        app.simulate(value, key_press(key::up));
        app.simulate(value, paste_event{"X"});
        check(value.value() == "ab  cdX\nefg");
        value.set_value("abcd");
        render_buffer buffer{{4, 2}};
        frame_state frame;
        render_context context{buffer, buffer.area(), frame};
        value.set_focused(true);
        value.render_in(context);
        check(frame.cursor == position{0, 1});
      }},
     {"button_styles",
      [] {
        button value{"Go"};
        value.set_style({.foreground = colors::red}, {.foreground = colors::green},
                        {.foreground = colors::blue}, {.foreground = colors::cyan});
        auto b = application::render_to_buffer(value, {6, 1});
        check(b.at({0, 0}).appearance().foreground == colors::red);
        value.set_hovered(true);
        b = application::render_to_buffer(value, {6, 1});
        check(b.at({0, 0}).appearance().foreground == colors::cyan);
        value.set_focused(true);
        b = application::render_to_buffer(value, {6, 1});
        check(b.at({0, 0}).appearance().foreground == colors::green);
        value.set_enabled([] { return false; });
        b = application::render_to_buffer(value, {6, 1});
        check(b.at({0, 0}).appearance().foreground == colors::blue);
      }},
     {"focus_title_style", [] {
        block root{"Title", border_kind::single, button{"OK"}};
        root.set_focus_title_style({.foreground = colors::red});
        application app;
        app.simulate(root, key_press(key::enter));
        auto b = application::render_to_buffer(root, {15, 3});
        check(b.at({2, 0}).appearance().foreground == colors::red);
      }}}};
}  // namespace
