// Render buffer operations, clipping, and wide-cell invariants.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

import avionix.entity.geometry;
import avionix.entity.style;
import avionix.entity.cell;
import avionix.object.buffer;
import avionix_tests.check;

using namespace avionix;
using avionix_tests::check;
using avionix_tests::check_equal;

namespace {

// Row contents as text; continuation cells are shown as '~'.
std::string row_text(const render_buffer& b, std::uint32_t y) {
  std::string out;
  for (const cell& c : b.row(y)) {
    out += c.is_continuation() ? std::string_view{"~"} : c.text();
  }
  return out;
}

// Checks the wide-cell invariant over the whole buffer.
bool invariant_holds(const render_buffer& b) {
  for (std::uint32_t y = 0; y < b.height(); ++y) {
    const auto row = b.row(y);
    for (std::size_t x = 0; x < row.size(); ++x) {
      if (row[x].is_wide()) {
        if (x + 1 >= row.size() || !row[x + 1].is_continuation()) return false;
      }
      if (row[x].is_continuation()) {
        if (x == 0 || !row[x - 1].is_wide()) return false;
      }
    }
  }
  return true;
}

const avionix_tests::suite buffer{
    "buffer",
    {
        {"resize_fills_blank",
         [] {
           render_buffer b{{4, 2}};
           check_equal(b.cells().size(), std::size_t{8});
           check(row_text(b, 1) == "    ");
         }},
        {"row_major_layout",
         [] {
           render_buffer b{{3, 2}};
           b.put_text({1, 1}, "x", {}, b.area());
           check(b.cells()[1 * 3 + 1].text() == "x");
         }},
        {"put_text_ascii",
         [] {
           render_buffer b{{6, 1}};
           const auto advanced = b.put_text({1, 0}, "abc", {}, b.area());
           check_equal(advanced, 3U);
           check(row_text(b, 0) == " abc  ");
         }},
        {"put_text_clips_right",
         [] {
           render_buffer b{{4, 1}};
           const auto advanced = b.put_text({2, 0}, "hello", {}, b.area());
           check(row_text(b, 0) == "  he");
           check_equal(advanced, 5U);
         }},
        {"put_text_clips_to_rect",
         [] {
           render_buffer b{{6, 1}};
           b.put_text({0, 0}, "abcdef", {}, rect{{2, 0}, {2, 1}});
           check(row_text(b, 0) == "  cd  ");
         }},
        {"put_text_negative_start",
         [] {
           render_buffer b{{4, 1}};
           b.put_text({-2, 0}, "abcd", {}, b.area());
           check(row_text(b, 0) == "cd  ");
         }},
        {"wide_glyph_occupies_two_cells",
         [] {
           render_buffer b{{4, 1}};
           b.put_text({0, 0}, "\xE4\xB8\xAD", {}, b.area());
           check(b.row(0)[0].is_wide());
           check(b.row(0)[1].is_continuation());
           check(invariant_holds(b));
         }},
        {"wide_glyph_clipped_at_edge_becomes_blank",
         [] {
           render_buffer b{{3, 1}};
           b.put_text({2, 0}, "\xE4\xB8\xAD", {}, b.area());
           check(row_text(b, 0) == "   ");
           check(invariant_holds(b));
         }},
        {"overwrite_right_half_blanks_left",
         [] {
           render_buffer b{{4, 1}};
           b.put_text({0, 0}, "\xE4\xB8\xAD", {}, b.area());
           b.put_text({1, 0}, "x", {}, b.area());
           check(row_text(b, 0) == " x  ");
           check(invariant_holds(b));
         }},
        {"overwrite_left_half_blanks_right",
         [] {
           render_buffer b{{4, 1}};
           b.put_text({1, 0}, "\xE4\xB8\xAD", {}, b.area());
           b.put_text({1, 0}, "y", {}, b.area());
           check(row_text(b, 0) == " y  ");
           check(invariant_holds(b));
         }},
        {"wide_over_wide_offset",
         [] {
           render_buffer b{{6, 1}};
           b.put_text({0, 0}, "\xE4\xB8\xAD\xE6\x96\x87", {},
                      b.area());                              // 中文 at 0..3
           b.put_text({1, 0}, "\xE5\xAD\x97", {}, b.area());  // 字 at 1..2
           check(invariant_holds(b));
           check(row_text(b, 0) == " \xE5\xAD\x97~   ");
         }},
        {"controls_never_stored",
         [] {
           render_buffer b{{6, 1}};
           b.put_text({0, 0}, "a\x1b[31mb", {}, b.area());
           // ESC is dropped; the rest is literal text.
           check(row_text(b, 0) == "a[31mb");
         }},
        {"tab_is_one_space",
         [] {
           render_buffer b{{4, 1}};
           b.put_text({0, 0}, "a\tb", {}, b.area());
           check(row_text(b, 0) == "a b ");
         }},
        {"combining_mark_stays_with_base",
         [] {
           render_buffer b{{3, 1}};
           const auto advanced = b.put_text({0, 0}, "e\xCC\x81x", {}, b.area());
           check_equal(advanced, 2U);
           check(b.row(0)[0].text() == "e\xCC\x81");
           check(b.row(0)[1].text() == "x");
         }},
        {"fill_repairs_edges",
         [] {
           render_buffer b{{6, 1}};
           b.put_text({0, 0}, "\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97", {}, b.area());
           b.fill({{1, 0}, {3, 1}}, cell::from_ascii('#'));
           check(invariant_holds(b));
           check(row_text(b, 0) == " ###字~");
         }},
        {"apply_style_keeps_text",
         [] {
           render_buffer b{{3, 1}};
           b.put_text({0, 0}, "abc", {}, b.area());
           b.apply_style({{1, 0}, {1, 1}}, {.add = attribute::bold});
           check(row_text(b, 0) == "abc");
           check(has(b.row(0)[1].appearance().attributes, attribute::bold));
           check(!has(b.row(0)[0].appearance().attributes, attribute::bold));
         }},
        {"oversized_cluster_is_replacement",
         [] {
           // Many combining marks exceed inline capacity.
           std::string s = "a";
           for (int i = 0; i < 20; ++i) s += "\xCC\x81";
           render_buffer b{{2, 1}};
           b.put_text({0, 0}, s, {}, b.area());
           check(b.row(0)[0].text() == "\xEF\xBF\xBD");
         }},
        {"clear_resets_everything",
         [] {
           render_buffer b{{3, 1}};
           b.put_text({0, 0}, "abc", {.foreground = colors::red}, b.area());
           b.clear();
           check(b.row(0)[0] == cell::blank());
         }},
        {"cell_size_is_small", [] { check(sizeof(cell) <= 40); }},
    }};

}  // namespace
