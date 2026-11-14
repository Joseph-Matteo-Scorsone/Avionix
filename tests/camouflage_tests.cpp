// The camouflage wrapper and its field, on virtual buffers only.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

import avionix.entity.geometry;
import avionix.entity.color;
import avionix.entity.style;
import avionix.entity.event;
import avionix.entity.cell;
import avionix.entity.camouflage;
import avionix.object.buffer;
import avionix.interface.widget;
import avionix.interface.layout;
import avionix.interface.camouflage;
import avionix.interface.application;
import avionix_tests.check;

using namespace avionix;
using namespace std::chrono_literals;
using avionix_tests::check;
using avionix_tests::check_equal;

namespace {

constexpr avionix::size screen{40, 14};
// Where the child draws, in screen coordinates.
constexpr rect panel{{8, 3}, {20, 7}};

// Lowercase letters that differ in every direction, so a displaced cell
// identifies where it came from.
void draw_letters(render_context& context) {
  for (std::int32_t y = 0; y < static_cast<std::int32_t>(context.height()); ++y) {
    for (std::int32_t x = 0; x < static_cast<std::int32_t>(context.width()); ++x) {
      const char c = static_cast<char>('a' + (x * 7 + y * 3) % 26);
      context.draw_text({x, y}, std::string_view{&c, 1},
                        {.foreground = color::rgb(90, 100, 110)});
    }
  }
}

// Fills `panel` with X on a colored background.
void draw_panel(render_context& context) {
  const rect local = panel.translated(position{} - context.area().origin);
  context.fill(
      local, "X",
      {.foreground = color::rgb(250, 250, 250), .background = color::rgb(10, 20, 30)});
}

render_buffer background() {
  canvas lower{draw_letters};
  return application::render_to_buffer(lower, screen);
}

render_buffer draw(component& root, const render_buffer& under,
                   color_depth depth = color_depth::truecolor) {
  render_buffer buffer = under;
  frame_state frame;
  frame.graphics.colors = depth;
  render_context context{buffer, buffer.area(), frame};
  root.render_in(context);
  return buffer;
}

void settle(detail::camouflage_core& effect, double target) {
  effect.set_reveal(target);
  effect.advance(2s);
}

// Every wide cell is followed by its continuation, and every continuation
// follows a wide cell.
bool wide_cells_whole(const render_buffer& b) {
  for (std::uint32_t y = 0; y < b.height(); ++y) {
    const auto row = b.row(y);
    for (std::size_t x = 0; x < row.size(); ++x) {
      if (row[x].is_wide() && (x + 1 >= row.size() || !row[x + 1].is_continuation()))
        return false;
      if (row[x].is_continuation() && (x == 0 || !row[x - 1].is_wide())) return false;
    }
  }
  return true;
}

// A focusable child that counts what it receives.
struct counter final : component {
  int keys{};
  int presses{};
  bool focusable() const noexcept override { return true; }
  void render(render_context& context) override { draw_panel(context); }
  event_result on_event(const event& e, event_context&) override {
    if (as<key_event>(e)) {
      ++keys;
      return event_result::handled;
    }
    if (const auto* m = as<mouse_event>(e); m && m->action == mouse_action::press) {
      ++presses;
      return event_result::handled;
    }
    return event_result::ignored;
  }
};

const avionix_tests::suite field_tests{
    "camouflage.field",
    {
        {"solid_at_rest",
         [] {
           bool always_solid = true;
           bool never_solid = true;
           for (std::int32_t y = -3; y < 30; ++y) {
             for (std::int32_t x = -3; x < 60; ++x) {
               for (const double t : {0.0, 0.37, 5.0, 123.456, 86400.5}) {
                 camouflage_point p{.cell = {x, y}, .seconds = t, .reveal = 1.0};
                 always_solid = always_solid && camouflage_scan_at(p).solid;
                 p.reveal = 0.0;
                 const auto scan = camouflage_scan_at(p);
                 never_solid = never_solid && !scan.solid;
                 never_solid = never_solid && scan.tear == 0 && scan.edge == 0.0;
               }
             }
           }
           check(always_solid, "reveal 1 is solid everywhere");
           check(never_solid, "reveal 0 without a pointer is never solid");
         }},
        {"offset_bound",
         [] {
           bool bounded = true;
           bool moved = false;
           std::uint32_t seed = 1;
           for (int i = 0; i < 20000; ++i) {
             seed = seed * 1664525U + 1013904223U;
             const camouflage_point p{
                 .cell = {static_cast<std::int32_t>(seed % 97),
                          static_cast<std::int32_t>((seed >> 8) % 41)},
                 .seconds = (seed >> 12) % 1000 * 0.05,
                 .reveal = (seed >> 4) % 11 / 10.0,
                 .pointer = position{20, 10},
                 .pointer_radius = 6.0};
             const auto c = camouflage_field(p, [&](position q) {
               return ((q.x * 31 + q.y * 17 + static_cast<std::int32_t>(seed)) & 3) !=
                      0;
             });
             bounded = bounded && std::abs(c.offset.x) <= camouflage_max_offset &&
                       std::abs(c.offset.y) <= camouflage_max_offset;
             bounded = bounded && c.rim >= 0.0 && c.rim <= 1.0 && c.accent >= 0.0 &&
                       c.accent <= 1.0;
             if (c.solid) bounded = bounded && c.offset == position{} && c.rim == 0.0;
             moved = moved || c.offset != position{};
           }
           check(bounded, "offsets within the bound, highlights within [0, 1]");
           check(moved, "the field displaces something");
         }},
        {"deterministic_constexpr",
         [] {
           constexpr camouflage_point p{.cell = {5, 3}, .seconds = 1.25, .reveal = 0.4};
           constexpr auto mask = [](position q) { return q.x > 2 && q.y > 1; };
           constexpr camouflage_cell at_compile_time = camouflage_field(p, mask);
           check(camouflage_field(p, mask) == at_compile_time);
           // Pinned values: a change here changes every frame on every platform.
           static_assert(camouflage_math::hash(0, 0, 0) ==
                         camouflage_math::hash(0, 0, 0));
           check_equal(camouflage_math::mix(1U), 0x688990c0U);
         }},
        {"blend",
         [] {
           const color white = color::rgb(255, 255, 255);
           check(camouflage_blend(colors::red, white, 0.0, {}) == colors::red);
           check(camouflage_blend(color::rgb(0, 0, 0), white, 1.0, {}) == white);
           check(camouflage_blend(color{}, white, 0.5, {0, 0, 0}) ==
                 color::rgb(128, 128, 128));
         }},
    }};

const avionix_tests::suite wrapper_tests{
    "camouflage.wrapper",
    {
        {"revealed_equals_child",
         [] {
           const render_buffer under = background();
           canvas alone{draw_panel};
           const render_buffer expected = draw(alone, under);
           camouflage wrapped{canvas{draw_panel}};
           settle(wrapped, 1.0);
           check(!wrapped.animating());
           const render_buffer actual = draw(wrapped, under);
           check(actual.cells().size() == expected.cells().size());
           bool same = true;
           for (std::size_t i = 0; i < actual.cells().size(); ++i)
             same = same && actual.cells()[i] == expected.cells()[i];
           check(same, "cell-for-cell identical to the child alone");
         }},
        {"camouflaged_shows_only_background",
         [] {
           const render_buffer under = background();
           camouflage wrapped{canvas{draw_panel}};
           wrapped.advance(1500ms);
           const render_buffer b = draw(wrapped, under);
           bool hidden = true;
           bool sampled_nearby = true;
           bool outside_unchanged = true;
           int displaced = 0;
           for (std::int32_t y = 0; y < static_cast<std::int32_t>(screen.height); ++y) {
             for (std::int32_t x = 0; x < static_cast<std::int32_t>(screen.width);
                  ++x) {
               const cell& c = b.at({x, y});
               if (!panel.contains(position{x, y})) {
                 outside_unchanged = outside_unchanged && c == under.at({x, y});
                 continue;
               }
               hidden = hidden && c.text() != "X";
               bool found = false;
               for (std::int32_t dy = -camouflage_max_offset;
                    dy <= camouflage_max_offset; ++dy) {
                 for (std::int32_t dx = -camouflage_max_offset;
                      dx <= camouflage_max_offset; ++dx) {
                   const position from{x + dx, y + dy};
                   if (under.contains(from) && under.at(from).text() == c.text())
                     found = true;
                 }
               }
               sampled_nearby = sampled_nearby && found;
               if (!(c.text() == under.at({x, y}).text())) ++displaced;
             }
           }
           check(hidden, "no cell of the child's content shows");
           check(sampled_nearby, "every masked cell is a nearby background cell");
           check(outside_unchanged, "cells outside the mask are untouched");
           check(displaced > static_cast<int>(panel.extent.area() / 2),
                 "most of the shape is displaced");
         }},
        {"outline_without_background",
         [] {
           // A one-row word over nothing at all: no detail to bend, so the
           // outline is the only thing left, and it must cover the word.
           const render_buffer empty{screen};
           camouflage word{canvas{[](render_context& context) {
             context.draw_text({10, 5}, "SIGNAL ACQUIRED");
           }}};
           for (int frame = 0; frame < 30; ++frame) {
             word.advance(50ms);
             const render_buffer b = draw(word, empty);
             bool outlined = true;
             for (std::int32_t x = 10; x < 25; ++x) {
               const cell& c = b.at({x, 5});
               outlined = outlined && c.text() == " " &&
                          c.appearance().background.type() == color::kind::rgb;
             }
             check(outlined, "every cell of the hidden word shows the outline");
             check(
                 b.at({9, 5}) == empty.at({9, 5}) && b.at({10, 4}) == empty.at({10, 4}),
                 "cells beside the word stay untouched");
           }
         }},
        {"tint_needs_truecolor",
         [] {
           const render_buffer under = background();
           camouflage wrapped{canvas{draw_panel}};
           wrapped.advance(1s);
           const render_buffer full = draw(wrapped, under);
           const render_buffer reduced = draw(wrapped, under, color_depth::indexed256);
           const style plain = under.at({0, 0}).appearance();
           bool tinted = false;
           bool untinted = true;
           for (std::int32_t y = panel.top(); y < panel.bottom(); ++y) {
             for (std::int32_t x = panel.left(); x < panel.right(); ++x) {
               tinted = tinted || !(full.at({x, y}).appearance() == plain);
               untinted = untinted && reduced.at({x, y}).appearance() == plain;
               check(full.at({x, y}).text() == reduced.at({x, y}).text());
             }
           }
           check(tinted, "the rim tints with truecolor");
           check(untinted, "no tint below truecolor; displacement stays");
         }},
        {"deterministic",
         [] {
           const render_buffer under = background();
           camouflage a{canvas{draw_panel}, {.pointer_reveals = true}};
           camouflage b{canvas{draw_panel}, {.pointer_reveals = true}};
           for (auto* w : {&a, &b}) {
             w->set_reveal(0.5);
             w->advance(310ms);
             event_context ignored;
             (void)draw(*w, under);  // records the hit area
             w->on_event(mouse_event{mouse_button::none, mouse_action::move, {15, 5}},
                         ignored);
           }
           const render_buffer first = draw(a, under);
           const render_buffer again = draw(a, under);
           const render_buffer other = draw(b, under);
           bool same = true;
           for (std::size_t i = 0; i < first.cells().size(); ++i) {
             same = same && first.cells()[i] == again.cells()[i] &&
                    first.cells()[i] == other.cells()[i];
           }
           check(same, "same clock, reveal, and pointer give the same cells");
         }},
        {"pointer_reveals_nearby",
         [] {
           const render_buffer under = background();
           camouflage wrapped{canvas{draw_panel},
                              {.pointer_reveals = true, .pointer_radius = 4}};
           wrapped.advance(1s);
           (void)draw(wrapped, under);
           const position pointer{12, 6};
           event_context context;
           check(wrapped.on_event(
                     mouse_event{mouse_button::none, mouse_action::move, pointer},
                     context) == event_result::ignored,
                 "the wrapper observes the pointer without consuming it");
           const render_buffer b = draw(wrapped, under);
           check(b.at(pointer).text() == "X", "the cell under the pointer is solid");
           check(b.at({pointer.x + 1, pointer.y}).text() == "X");
           bool far_hidden = true;
           for (std::int32_t y = panel.top(); y < panel.bottom(); ++y)
             for (std::int32_t x = 22; x < panel.right(); ++x)
               far_hidden = far_hidden && b.at({x, y}).text() != "X";
           check(far_hidden, "cells far from the pointer stay camouflaged");
         }},
        {"wide_glyphs_never_split",
         [] {
           canvas lower{[](render_context& context) {
             for (std::int32_t y = 0; y < static_cast<std::int32_t>(context.height());
                  ++y) {
               // Odd rows start one column in, so wide glyphs straddle
               // every column parity.
               for (std::int32_t x = y % 2;
                    x + 1 < static_cast<std::int32_t>(context.width()); x += 3)
                 context.draw_text({x, y}, "\xE4\xB8\xAD", {}, "https://example.com");
             }
           }};
           const render_buffer under = application::render_to_buffer(lower, screen);
           camouflage wrapped{canvas{draw_panel}};
           bool whole = true;
           bool unlinked = true;
           for (int frame = 0; frame < 40; ++frame) {
             wrapped.set_reveal(frame < 20 ? 1.0 : 0.0);
             wrapped.advance(50ms);
             const render_buffer b = draw(wrapped, under);
             whole = whole && wide_cells_whole(b);
             for (std::int32_t y = panel.top(); y < panel.bottom(); ++y)
               for (std::int32_t x = panel.left(); x < panel.right(); ++x)
                 if (b.at({x, y}).text() != "X")
                   unlinked = unlinked && b.at({x, y}).link() == 0;
           }
           check(whole, "no wide glyph is separated from its continuation");
           check(unlinked, "displaced cells carry no hyperlink");
         }},
        {"empty_and_resized",
         [] {
           camouflage wrapped{canvas{draw_panel}};
           wrapped.advance(1s);
           render_buffer none{{0, 0}};
           frame_state frame;
           render_context zero{none, none.area(), frame};
           wrapped.render_in(zero);
           check(wrapped.last_area().empty());

           const render_buffer under = background();
           render_buffer clipped = under;
           render_context outside{clipped, {{100, 100}, {10, 10}}, frame};
           wrapped.render_in(outside);
           check(clipped.cells().size() == under.cells().size());
           bool untouched = true;
           for (std::size_t i = 0; i < under.cells().size(); ++i)
             untouched = untouched && clipped.cells()[i] == under.cells()[i];
           check(untouched, "a fully clipped area writes nothing");

           camouflage empty{canvas{[](render_context&) {}}};
           empty.advance(1s);
           const render_buffer b = draw(empty, under);
           untouched = true;
           for (std::size_t i = 0; i < under.cells().size(); ++i)
             untouched = untouched && b.cells()[i] == under.cells()[i];
           check(untouched, "a child that draws nothing changes nothing");

           for (const avionix::size extent :
                {avionix::size{40, 14}, avionix::size{9, 4}, avionix::size{3, 1},
                 avionix::size{70, 30}, avionix::size{40, 14}}) {
             canvas lower{draw_letters};
             render_buffer base = application::render_to_buffer(lower, extent);
             wrapped.advance(50ms);
             const render_buffer drawn = draw(wrapped, base);
             bool hidden = true;
             for (const cell& c : drawn.cells()) hidden = hidden && c.text() != "X";
             check(hidden, "the child stays hidden at every size");
             check(wide_cells_whole(drawn));
           }
         }},
        {"nested",
         [] {
           const render_buffer under = background();
           // Spelled out: CTAD would deduce a copy of the inner wrapper.
           camouflage<camouflage<canvas>> outer{camouflage{canvas{draw_panel}}};
           outer.advance(1s);
           outer.child().advance(1s);
           const render_buffer hidden = draw(outer, under);
           bool none_shown = true;
           for (const cell& c : hidden.cells())
             none_shown = none_shown && c.text() != "X";
           check(none_shown, "nested camouflage hides the child");

           settle(outer, 1.0);
           settle(outer.child(), 1.0);
           canvas alone{draw_panel};
           const render_buffer expected = draw(alone, under);
           const render_buffer actual = draw(outer, under);
           bool same = true;
           for (std::size_t i = 0; i < actual.cells().size(); ++i)
             same = same && actual.cells()[i] == expected.cells()[i];
           check(same, "both revealed is the child alone");
         }},
        {"events_reach_child",
         [] {
           layer root;
           root.add(canvas{draw_letters});
           auto& wrapped = root.add(camouflage{counter{}});
           wrapped.advance(1s);
           (void)application::render_to_buffer(root, screen);
           std::vector<component*> kids;
           wrapped.children(kids);
           check(kids.size() == 1 && kids[0] == &wrapped.child());
           check(!wrapped.focusable(), "the wrapper is not a focus stop");

           application app;
           app.simulate(root, key_event{key::character, U'a', modifiers::none});
           check_equal(wrapped.child().keys, 1);
           check(wrapped.child().focused(), "the camouflaged child takes focus");
           app.simulate(root, mouse_event{mouse_button::left,
                                          mouse_action::press,
                                          {panel.left() + 2, panel.top() + 2}});
           check_equal(wrapped.child().presses, 1);
         }},
        {"advance_and_reveal",
         [] {
           const render_buffer under = background();
           camouflage wrapped{canvas{draw_panel}};
           check(!wrapped.advance(-5ms), "negative elapsed is ignored");
           check(!wrapped.advance(0ms));
           wrapped.set_reveal(7.0);
           check_equal(wrapped.reveal_target(), 1.0);
           wrapped.set_reveal(-1.0);
           check_equal(wrapped.reveal_target(), 0.0);
           check_equal(wrapped.reveal(), 0.0);

           (void)draw(wrapped, under);
           int changed = 0;
           int frames = 0;
           for (int i = 0; i < 40; ++i) {
             const render_buffer before = draw(wrapped, under);
             const bool reported = wrapped.advance(50ms);
             const render_buffer after = draw(wrapped, under);
             bool same = true;
             for (std::size_t k = 0; k < after.cells().size(); ++k)
               same = same && after.cells()[k] == before.cells()[k];
             check(reported == !same, "advance reports exactly when cells change");
             changed += reported ? 1 : 0;
             ++frames;
           }
           check(changed < frames, "at rest some frames are unchanged");

           wrapped.set_reveal(1.0);
           check(wrapped.advance(300ms));
           check(wrapped.reveal() > 0.0 && wrapped.reveal() < 1.0);
           check(wrapped.animating());
           wrapped.advance(1s);
           check_equal(wrapped.reveal(), 1.0);
           (void)draw(wrapped, under);
           check(!wrapped.animating());
           check(!wrapped.advance(50ms), "solid at rest never changes");
         }},
    }};

}  // namespace
