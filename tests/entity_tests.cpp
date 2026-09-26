// Geometry, color, style, and constraint rules.

#include <array>
#include <cstdint>
#include <vector>

import avionix.entity.geometry;
import avionix.entity.color;
import avionix.entity.style;
import avionix.entity.constraint;
import avionix_tests.check;

using namespace avionix;
using avionix_tests::check;
using avionix_tests::check_equal;

namespace {

// Compile-time checks: these rules are constexpr and must stay so.
static_assert(intersect(rect{{0, 0}, {10, 10}}, rect{{5, 5}, {10, 10}}) ==
              rect{{5, 5}, {5, 5}});
static_assert(rect{{2, 3}, {4, 5}}.right() == 6);
static_assert(downgrade(color::rgb(255, 0, 0), color_depth::ansi16) ==
              colors::bright_red);

std::vector<std::uint32_t> lengths(std::uint32_t available, std::vector<constraint> cs,
                                   std::uint32_t spacing = 0) {
  std::vector<std::uint32_t> out(cs.size());
  solve_lengths(available, cs, out, spacing);
  return out;
}

const avionix_tests::suite geometry{
    "geometry",
    {
        {"contains_is_half_open",
         [] {
           const rect r{{1, 1}, {3, 2}};
           check(r.contains(position{1, 1}));
           check(r.contains(position{3, 2}));
           check(!r.contains(position{4, 1}));
           check(!r.contains(position{1, 3}));
           check(!r.contains(position{0, 1}));
         }},
        {"intersection_disjoint_is_empty",
         [] {
           const rect a{{0, 0}, {2, 2}};
           const rect b{{5, 5}, {2, 2}};
           check(intersect(a, b).empty());
         }},
        {"inset_collapses_when_too_small",
         [] {
           const rect r{{0, 0}, {4, 4}};
           check_equal(r.inset(1), rect{{1, 1}, {2, 2}});
           check(r.inset(2).empty());
           check(r.inset(0, 0, 4, 0).empty());
         }},
        {"right_saturates",
         [] {
           const rect r{{INT32_MAX - 1, 0}, {10, 1}};
           check_equal(r.right(), INT32_MAX);
         }},
        {"contains_rect",
         [] {
           const rect outer{{0, 0}, {10, 10}};
           check(outer.contains(rect{{2, 2}, {3, 3}}));
           check(!outer.contains(rect{{8, 8}, {3, 3}}));
           check(outer.contains(rect{{50, 50}, {0, 0}}));
         }},
    }};

const avionix_tests::suite color_rules{
    "color",
    {
        {"default_is_preserved",
         [] {
           check_equal(downgrade(color::terminal_default(), color_depth::monochrome),
                       color::terminal_default());
         }},
        {"monochrome_drops_color",
         [] { check(downgrade(colors::red, color_depth::monochrome).is_default()); }},
        {"truecolor_keeps_rgb",
         [] {
           const color c = color::rgb(1, 2, 3);
           check_equal(downgrade(c, color_depth::truecolor), c);
         }},
        {"rgb_to_256_uses_cube",
         [] {
           // Pure red is cube entry 5,0,0 = 16 + 180 = 196.
           check_equal(
               downgrade(color::rgb(255, 0, 0), color_depth::indexed256).index(),
               std::uint8_t{196});
           // Mid grey maps into the grey ramp or cube, never the themed 0-15.
           check(
               downgrade(color::rgb(128, 128, 128), color_depth::indexed256).index() >=
               16);
         }},
        {"indexed_to_16",
         [] {
           check_equal(downgrade(colors::blue, color_depth::ansi16), colors::blue);
           // 231 is the cube's white.
           check_equal(downgrade(color::indexed(231), color_depth::ansi16),
                       colors::bright_white);
         }},
        {"hex",
         [] {
           const color c = color::hex(0x123456);
           check_equal(c.red(), std::uint8_t{0x12});
           check_equal(c.green(), std::uint8_t{0x34});
           check_equal(c.blue(), std::uint8_t{0x56});
         }},
        {"palette_grey_ramp",
         [] {
           check(palette_rgb(232) == rgb_triplet{8, 8, 8});
           check(palette_rgb(255) == rgb_triplet{238, 238, 238});
         }},
    }};

const avionix_tests::suite style_rules{
    "style",
    {
        {"attribute_flags",
         [] {
           const attribute a = attribute::bold | attribute::underline;
           check(has(a, attribute::bold));
           check(!has(a, attribute::italic));
           check(!has(a & ~attribute::bold, attribute::bold));
         }},
        {"patch_inherits_unset_fields",
         [] {
           const style base{colors::white, colors::blue, attribute::bold};
           const style result = apply(base, {.foreground = colors::red});
           check_equal(result.foreground, colors::red);
           check_equal(result.background, colors::blue);
           check(has(result.attributes, attribute::bold));
         }},
        {"patch_add_and_remove",
         [] {
           const style base{{}, {}, attribute::bold | attribute::italic};
           const style result =
               apply(base, {.add = attribute::underline, .remove = attribute::italic});
           check(has(result.attributes, attribute::bold));
           check(has(result.attributes, attribute::underline));
           check(!has(result.attributes, attribute::italic));
         }},
        {"builders",
         [] {
           const style s = style{}.with_foreground(colors::green).with(attribute::dim);
           check_equal(s.foreground, colors::green);
           check(has(s.attributes, attribute::dim));
           check(!has(s.without(attribute::dim).attributes, attribute::dim));
         }},
    }};

const avionix_tests::suite layout_rules{
    "constraint",
    {
        {"fixed_and_fill",
         [] {
           check(lengths(10, {constraint::fixed(3), constraint::fill()}) ==
                 std::vector<std::uint32_t>{3, 7});
         }},
        {"fill_weights",
         [] {
           check(lengths(12, {constraint::fill(1), constraint::fill(2)}) ==
                 std::vector<std::uint32_t>{4, 8});
         }},
        {"remainder_goes_to_first",
         [] {
           check(lengths(10, {constraint::fill(), constraint::fill(),
                              constraint::fill()}) ==
                 std::vector<std::uint32_t>{4, 3, 3});
         }},
        {"percentage",
         [] {
           check(lengths(80, {constraint::percentage(25), constraint::fill()}) ==
                 std::vector<std::uint32_t>{20, 60});
         }},
        {"minimum_grows",
         [] {
           check(lengths(10, {constraint::minimum(4), constraint::fill()}) ==
                 std::vector<std::uint32_t>{7, 3});
         }},
        {"maximum_caps_and_redistributes",
         [] {
           check(lengths(20, {constraint::maximum(3), constraint::fill()}) ==
                 std::vector<std::uint32_t>{3, 17});
         }},
        {"overflow_shrinks_from_the_end",
         [] {
           check(lengths(5, {constraint::fixed(4), constraint::fixed(4)}) ==
                 std::vector<std::uint32_t>{4, 1});
         }},
        {"spacing",
         [] {
           check(lengths(11, {constraint::fill(), constraint::fill()}, 1) ==
                 std::vector<std::uint32_t>{5, 5});
           const auto rects =
               split(rect{{0, 0}, {11, 2}}, direction::horizontal,
                     std::array{constraint::fill(), constraint::fill()}, 1);
           check_equal(rects[1].left(), 6);
         }},
        {"split_vertical_positions",
         [] {
           const auto rects = split(rect{{2, 3}, {10, 10}}, direction::vertical,
                                    std::array{constraint::fixed(1), constraint::fill(),
                                               constraint::fixed(2)});
           check_equal(rects[0], rect{{2, 3}, {10, 1}});
           check_equal(rects[1], rect{{2, 4}, {10, 7}});
           check_equal(rects[2], rect{{2, 11}, {10, 2}});
         }},
        {"deterministic",
         [] {
           const std::vector cs{constraint::fill(3), constraint::percentage(33),
                                constraint::maximum(5), constraint::minimum(2)};
           check(lengths(97, cs) == lengths(97, cs));
           std::uint32_t total = 0;
           for (auto v : lengths(97, cs)) total += v;
           check_equal(total, 97U);
         }},
        {"many_segments_use_heap_path",
         [] {
           std::vector<constraint> cs(40, constraint::fill());
           const auto rects = split(rect{{0, 0}, {80, 1}}, direction::horizontal, cs);
           check_equal(rects.size(), std::size_t{40});
           check_equal(rects.back().right(), 80);
         }},
    }};

}  // namespace
