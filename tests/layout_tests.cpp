// Layout containers rendered into virtual buffers.

#include <cstdint>
#include <string>
#include <string_view>

import avionix.entity.geometry;
import avionix.entity.constraint;
import avionix.entity.cell;
import avionix.object.buffer;
import avionix.interface.widget;
import avionix.interface.layout;
import avionix.interface.controls;
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

render_buffer draw(component& root, size extent) {
  render_buffer buffer{extent};
  frame_state frame;
  render_context context{buffer, bounds(extent), frame};
  root.render_in(context);
  return buffer;
}

// A widget that fills its whole area with one character, to make slot
// boundaries visible.
struct fill_with {
  char glyph;
  void render(render_context& context) const {
    const char text[2] = {glyph, 0};
    context.fill(bounds(context.extent()), std::string_view{text, 1});
  }
};

const avionix_tests::suite containers{
    "layout",
    {
        {"row_splits_horizontally",
         [] {
           row r;
           r.add(constraint::fixed(2), fill_with{'a'});
           r.add(constraint::fill(), fill_with{'b'});
           r.add(constraint::fixed(1), fill_with{'c'});
           const auto b = draw(r, {6, 2});
           check(row_text(b, 0) == "aabbbc");
           check(row_text(b, 1) == "aabbbc");
         }},
        {"column_splits_vertically",
         [] {
           column c;
           c.add(constraint::fixed(1), fill_with{'x'});
           c.add(constraint::fill(), fill_with{'y'});
           const auto b = draw(c, {3, 3});
           check(row_text(b, 0) == "xxx");
           check(row_text(b, 1) == "yyy");
           check(row_text(b, 2) == "yyy");
         }},
        {"spacing_leaves_gaps",
         [] {
           row r{1};
           r.add(constraint::fill(), fill_with{'a'});
           r.add(constraint::fill(), fill_with{'b'});
           const auto b = draw(r, {5, 1});
           check(row_text(b, 0) == "aa bb");
         }},
        {"children_are_clipped_to_slots",
         [] {
           row r;
           r.add(constraint::fixed(3), text{"overflowing"});
           r.add(constraint::fill(), fill_with{'.'});
           const auto b = draw(r, {6, 1});
           check(row_text(b, 0) == "ove...");
         }},
        {"nested_layouts",
         [] {
           column outer;
           auto& inner = outer.add(constraint::fixed(1), row{});
           inner.add(constraint::percentage(50), fill_with{'l'});
           inner.add(constraint::fill(), fill_with{'r'});
           outer.add(constraint::fill(), fill_with{'-'});
           const auto b = draw(outer, {4, 2});
           check(row_text(b, 0) == "llrr");
           check(row_text(b, 1) == "----");
         }},
        {"padding_insets_child",
         [] {
           padding p{1, 1, fill_with{'#'}};
           const auto b = draw(p, {4, 3});
           check(row_text(b, 0) == "    ");
           check(row_text(b, 1) == " ## ");
           check(row_text(b, 2) == "    ");
         }},
        {"last_area_is_recorded",
         [] {
           row r;
           auto& left = r.add(constraint::fixed(2), text{"ab"});
           auto& right = r.add(constraint::fill(), text{"cd"});
           (void)draw(r, {10, 1});
           check(left.last_area() == rect{{0, 0}, {2, 1}});
           check(right.last_area() == rect{{2, 0}, {8, 1}});
         }},
        {"borrowed_child",
         [] {
           text shared{"hi"};
           row r;
           r.add_borrowed(constraint::fill(), shared);
           shared.set("yo");
           const auto b = draw(r, {2, 1});
           check(row_text(b, 0) == "yo");
         }},
        {"relayout_on_resize",
         [] {
           row r;
           r.add(constraint::percentage(50), fill_with{'a'});
           r.add(constraint::fill(), fill_with{'b'});
           check(row_text(draw(r, {4, 1}), 0) == "aabb");
           check(row_text(draw(r, {8, 1}), 0) == "aaaabbbb");
         }},
    }};

}  // namespace
