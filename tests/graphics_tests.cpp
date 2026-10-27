// Pixel images in the terminal: detection, encoders, renderer placement
// rules, and image_view's two modes. No terminal needed.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

import avionix.entity.geometry;
import avionix.entity.style;
import avionix.entity.cell;
import avionix.entity.image;
import avionix.entity.terminal_graphics;
import avionix.object.buffer;
import avionix.object.renderer;
import avionix.object.terminal_capabilities;
import avionix.object.image_file;
import avionix.entity.error;
import avionix.interface.widget;
import avionix.interface.image;
import avionix_tests.check;

using namespace avionix;
using avionix_tests::check;
using avionix_tests::check_equal;

namespace {

image_rgba8 solid(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g,
                  std::uint8_t b) {
  image_rgba8 img{w, h, {}};
  for (std::uint32_t i = 0; i < w * h; ++i)
    img.pixels.insert(img.pixels.end(), {r, g, b, 255});
  return img;
}

bool contains(std::string_view hay, std::string_view needle) {
  return hay.find(needle) != std::string_view::npos;
}

const avionix_tests::suite detection{
    "graphics_detection",
    {
        {"environment_rules",
         [] {
           const auto detect = [](terminal_environment env) {
             return detect_capabilities(env);
           };
           check(detect({.term = "xterm-kitty"}).graphics == graphics_protocol::kitty);
           check(detect({.term = "xterm-256color", .kitty_window = true}).graphics ==
                 graphics_protocol::kitty);
           check(detect({.term = "xterm-ghostty"}).graphics ==
                 graphics_protocol::kitty);
           check(detect({.term_program = "WezTerm"}).graphics ==
                 graphics_protocol::kitty);
           check(detect({.term_program = "iTerm.app"}).graphics ==
                 graphics_protocol::iterm2);
           const auto wt = detect({.windows_terminal = true, .windows_console = true});
           check(wt.graphics == graphics_protocol::none && wt.query_graphics,
                 "Windows Terminal asks DA1");
           const auto apple =
               detect({.term = "xterm-256color", .term_program = "Apple_Terminal"});
           check(apple.graphics == graphics_protocol::none && apple.query_graphics);
           const auto tmux = detect({.term = "tmux-256color", .multiplexer = true});
           check(tmux.graphics == graphics_protocol::none && !tmux.query_graphics,
                 "multiplexers use blocks");
           check(!detect({.term = "linux"}).query_graphics);
           check(detect({.term = "xterm", .graphics = "sixel"}).graphics ==
                 graphics_protocol::sixel);
           const auto off = detect({.term = "xterm-kitty", .graphics = "blocks"});
           check(off.graphics == graphics_protocol::none && !off.query_graphics,
                 "override wins");
         }},
        {"support_rules",
         [] {
           check(!graphics_support{graphics_protocol::sixel, {}}.images(),
                 "sixel needs the cell size");
           check(graphics_support{graphics_protocol::sixel, {10, 20}}.images());
           check(graphics_support{graphics_protocol::kitty, {}}.images(),
                 "kitty scales to cells");
           check(graphics_support{graphics_protocol::kitty, {}}.effective_cell() ==
                 cell_pixels{8, 16});
         }},
    }};

const avionix_tests::suite encoders{
    "graphics_encoders",
    {
        {"cached_sixel_preserves_dither_and_reuses_storage",
         [] {
           auto img = solid(40, 24, 0, 0, 96);
           for (std::size_t i = 0; i < img.pixels.size(); i += 4) {
             img.pixels[i] = static_cast<std::uint8_t>((i / 4) % 256);
             img.pixels[i + 1] = static_cast<std::uint8_t>((i / 160) * 10);
           }
           std::string original, settled, drag;
           append_sixel(original, img);
           sixel_encoder_state state;
           append_sixel(settled, img, state);
           check(settled == original,
                 "stateful encoder preserves settled output exactly");
           const auto palette = state.palette_commands;
           const auto* indices = state.indices.data();
           const auto* errors = state.err_now.data();
           append_sixel(drag, img, state, true);
           check(drag == original,
                 "drag retains original colors and dithering exactly");
           check(state.indices.data() == indices && state.err_now.data() == errors,
                 "index and diffusion buffers are reused");
           auto changed = solid(8, 7, 255, 0, 0);
           drag.clear();
           append_sixel(drag, changed, state, true);
           check(state.palette_commands == palette,
                 "palette remains stable when drag pixels change");
           settled.clear();
           append_sixel(settled, changed, state);
           check(state.palette_commands != palette,
                 "settled frame refreshes the adaptive palette");
           original.clear();
           append_sixel(original, changed);
           check(settled == original,
                 "resize and refreshed lookup cache match a fresh encode");
           for (std::size_t i = 3; i < changed.pixels.size(); i += 4)
             changed.pixels[i] = 0;
           drag.clear();
           append_sixel(drag, changed, state, true);
           check(drag.find("#0!8") == std::string::npos,
                 "transparent frame retains no old pixels");
         }},
        {"interactive_sixel_reuses_band_storage",
         [] {
           auto img = solid(8, 7, 255, 0, 0);
           std::vector<std::uint8_t> scratch;
           std::string out;
           append_sixel_interactive(out, img, scratch);
           check(out.starts_with("\x1bP0;1;0q\"1;1;8;7"));
           check(out.ends_with("\x1b\\"));
           check(contains(out, "#48;2;100;0;0"));
           check(contains(out, "#48!8~"));
           check(contains(out, "#48!8@"), "partial final band");
           const auto* storage = scratch.data();
           const auto capacity = out.capacity();
           const auto expected = out;
           out.clear();
           append_sixel_interactive(out, img, scratch);
           check(out == expected, "repeated frames are identical");
           check(scratch.data() == storage && out.capacity() == capacity,
                 "band and payload allocations are reused after warmup");
           for (std::size_t i = 3; i < img.pixels.size(); i += 4) img.pixels[i] = 0;
           out.clear();
           append_sixel_interactive(out, img, scratch);
           check(!contains(out, "#48!"),
                 "transparent pixels and old band bits are skipped");
         }},
        {"sixel_structure",
         [] {
           std::string out;
           append_sixel(out, solid(8, 7, 255, 0, 0));
           check(out.starts_with("\x1bP0;1;0q\"1;1;8;7"),
                 "DCS with square-pixel raster attributes");
           check(out.ends_with("\x1b\\"));
           // One color in the image: one palette entry, exact.
           check(contains(out, "#0;2;100;0;0#0!8~"),
                 "palette, then the first band with all six rows");
           check(contains(out, "#0!8@"), "second band: only the top row");
           check(!contains(out, "#1;"), "no unused palette entries");
         }},
        {"sixel_skips_transparent_pixels",
         [] {
           image_rgba8 img = solid(4, 6, 0, 0, 255);
           for (std::size_t i = 3; i < img.pixels.size(); i += 4) img.pixels[i] = 0;
           std::string out;
           append_sixel(out, img);
           check(out.ends_with(std::string{"\"1;1;4;6-"} + char{27} + char{92}),
                 "no palette and no pixels");
         }},
        {"sixel_palette_follows_the_image",
         [] {
           // A smooth gradient: the adaptive palette spends entries on it
           // and every pixel lands within a few levels of its color.
           image_rgba8 img{256, 6, {}};
           for (std::uint32_t y = 0; y < 6; ++y)
             for (std::uint32_t x = 0; x < 256; ++x)
               img.pixels.insert(img.pixels.end(),
                                 {static_cast<std::uint8_t>(x),
                                  static_cast<std::uint8_t>(x / 2), 40, 255});
           std::string out;
           append_sixel(out, img);
           std::size_t entries = 0;
           for (std::size_t at = out.find(";2;"); at != std::string::npos;
                at = out.find(";2;", at + 1))
             ++entries;
           check(entries >= 24 && entries <= 256,
                 "a palette fitted to the gradient, one entry per histogram step");
         }},
        {"kitty_chunks",
         [] {
           std::string out;
           append_kitty(out, solid(64, 64, 1, 2, 3), 7, 8, 4);
           check(out.starts_with(
               "\x1b_Ga=T,f=32,s=64,v=64,i=7,p=1,c=8,r=4,C=1,q=2,m=1;"));
           check(contains(out, "\x1b_Gm=0;"), "last chunk");
           // 64*64*4 bytes = 21848 base64 chars = 6 chunks of at most 4096.
           std::size_t chunks = 0;
           for (std::size_t at = out.find("\x1b_G"); at != std::string::npos;
                at = out.find("\x1b_G", at + 1))
             ++chunks;
           check_equal(chunks, std::size_t{6});
           std::string del;
           append_kitty_delete(del, 7);
           check(del == "\x1b_Ga=d,d=I,i=7,q=2\x1b\\");
         }},
        {"iterm2_header",
         [] {
           std::string out;
           append_iterm2(out, solid(2, 2, 9, 9, 9), 5, 3);
           check(out.starts_with("\x1b]1337;File=inline=1;size="));
           check(contains(out, ";width=5;height=3;preserveAspectRatio=0:iVBORw0KGgo"),
                 "base64 PNG follows");
           check(out.ends_with("\x07"));
         }},
        {"fit_preserves_aspect",
         [] {
           const image_rgba8 fitted = fit_image(solid(10, 10, 200, 0, 0), 20, 10);
           check(fitted.width == 20 && fitted.height == 10);
           check(fitted.at(0, 5)[3] == 0 && fitted.at(10, 5)[3] == 255 &&
                 fitted.at(10, 5)[0] == 200);
         }},
    }};

graphics_placement placement(std::uint32_t id, std::uint64_t version, rect area,
                             graphics_protocol p) {
  return {id, version, area, p, "<IMG>"};
}

std::string present_frame(renderer& r, std::vector<graphics_placement> g,
                          std::string_view text = {}) {
  r.begin_frame();
  if (!text.empty()) r.back().put_text({0, 0}, text, style{}, r.back().area());
  r.set_graphics(std::move(g));
  return std::string{r.present()};
}

const avionix_tests::suite renderer_suite{
    "graphics_renderer",
    {
        {"images_follow_cells_and_are_not_resent",
         [] {
           renderer r;
           r.resize({20, 6});
           const rect area{{2, 1}, {6, 3}};
           const std::string first =
               present_frame(r, {placement(1, 1, area, graphics_protocol::sixel)});
           check(contains(first, "\x1b[2;3H") && contains(first, "<IMG>"),
                 "drawn at the area's origin");
           check_equal(r.last_frame().images_written, std::uint64_t{1});
           const std::string second =
               present_frame(r, {placement(1, 1, area, graphics_protocol::sixel)});
           check(second.empty(), "unchanged image and cells write nothing");
           const std::string third =
               present_frame(r, {placement(1, 2, area, graphics_protocol::sixel)});
           check(contains(third, "<IMG>"), "new version is resent");
         }},
        {"cell_writes_over_a_sixel_resend_it",
         [] {
           renderer r;
           r.resize({20, 6});
           const rect area{{0, 0}, {6, 3}};
           (void)present_frame(r, {placement(1, 1, area, graphics_protocol::sixel)});
           const std::string over = present_frame(
               r, {placement(1, 1, area, graphics_protocol::sixel)}, "hi");
           check(contains(over, "<IMG>"),
                 "text drawn under a sixel erases pixels, so the image is redrawn");
           (void)present_frame(r, {placement(5, 1, area, graphics_protocol::kitty)});
           const std::string kitty = present_frame(
               r, {placement(5, 1, area, graphics_protocol::kitty)}, "yo");
           check(!contains(kitty, "<IMG>"), "kitty images are a separate layer");
         }},
        {"removed_images_are_erased",
         [] {
           renderer r;
           r.resize({20, 6});
           (void)present_frame(
               r, {placement(1, 1, {{0, 0}, {4, 2}}, graphics_protocol::sixel)});
           const std::string gone = present_frame(r, {});
           check(r.last_frame().cells_written >= 8,
                 "the sixel's old cells are repainted");
           check(!contains(gone, "<IMG>"));
           (void)present_frame(
               r, {placement(9, 1, {{0, 0}, {4, 2}}, graphics_protocol::kitty)});
           const std::string deleted = present_frame(r, {});
           check(contains(deleted, "\x1b_Ga=d,d=I,i=9,q=2\x1b\\"),
                 "kitty image deleted by id");
           (void)present_frame(
               r, {placement(9, 1, {{0, 0}, {4, 2}}, graphics_protocol::kitty)});
           check(r.release_graphics() == "\x1b_Ga=d,d=I,i=9,q=2\x1b\\",
                 "teardown deletes kitty images");
         }},
        {"transparent_sixel_clears_its_cells_when_it_changes",
         [] {
           const rect area{{0, 0}, {4, 2}};
           for (const bool transparent : {false, true}) {
             renderer r;
             r.resize({20, 6});
             auto p = placement(1, 1, area, graphics_protocol::sixel);
             p.transparent = transparent;
             (void)present_frame(r, {p});
             p.version = 2;
             (void)present_frame(r, {p});
             check((r.last_frame().cells_written > 0) == transparent,
                   "only a transparent image repaints its cells before redrawing");
             check_equal(r.last_frame().images_written, std::uint64_t{1});
           }
         }},
        {"resize_resends",
         [] {
           renderer r;
           r.resize({20, 6});
           (void)present_frame(
               r, {placement(1, 1, {{0, 0}, {4, 2}}, graphics_protocol::sixel)});
           r.resize({30, 8});
           check(contains(present_frame(r, {placement(1, 1, {{0, 0}, {4, 2}},
                                                      graphics_protocol::sixel)}),
                          "<IMG>"));
         }},
    }};

std::vector<graphics_placement> draw(image_view& view, graphics_support support,
                                     rect area, size screen, render_buffer& buffer) {
  buffer = render_buffer{screen};
  frame_state frame;
  frame.graphics = support;
  render_context root{buffer, buffer.area(), frame};
  render_context ctx = root.child(area);
  view.render(ctx);
  return frame.placements;
}

const avionix_tests::suite image_view_suite{
    "image_view",
    {
        {"quadrants_without_a_protocol",
         [] {
           // Top half red, bottom half blue: the fit picks the upper half block.
           // Exactly 4x8 samples per cell, so no resampling.
           image_rgba8 img = solid(16, 16, 255, 0, 0);
           for (std::size_t o = 16 * 8 * 4; o < img.pixels.size(); o += 4) {
             img.pixels[o] = 0;
             img.pixels[o + 2] = 255;
           }
           image_view view{img};
           render_buffer b;
           const auto placed = draw(view, {}, {{0, 0}, {4, 2}}, {10, 3}, b);
           check(placed.empty() && view.shown_as() == graphics_protocol::none);
           const cell& c = b.at({1, 0});
           const cell& d = b.at({1, 1});
           check(c.text() == "\u2588" && c.appearance().foreground.red() > 200,
                 "top row of cells: red");
           check(d.text() == "\u2588" && d.appearance().foreground.blue() > 200,
                 "bottom row of cells: blue");
         }},
        {"quadrants_split_left_and_right",
         [] {
           // Left half white, right half black within one cell: the
           // left-half block, which half blocks cannot draw.
           // One cell's shape: 2 wide, 4 tall.
           image_rgba8 img = solid(2, 4, 0, 0, 0);
           for (std::size_t y = 0; y < 4; ++y) {
             const std::size_t o = y * 2 * 4;
             img.pixels[o] = img.pixels[o + 1] = img.pixels[o + 2] = 255;
           }
           image_view view{img};
           render_buffer b;
           (void)draw(view, {}, {{0, 0}, {1, 1}}, {4, 2}, b);
           const cell& c = b.at({0, 0});
           const bool left =
               c.text() == "\u258C" && c.appearance().foreground.red() > 200;
           const bool right =
               c.text() == "\u2590" && c.appearance().background.red() > 200;
           check(left || right, "a vertical split");
         }},
        {"brightness_ramp_without_color",
         [] {
           image_view view{solid(8, 8, 255, 255, 255)};
           render_buffer b;
           graphics_support mono{};
           mono.colors = color_depth::monochrome;
           (void)draw(view, mono, {{0, 0}, {4, 2}}, {10, 3}, b);
           check(b.at({1, 0}).text() == "@", "white is the densest character");
         }},
        {"pixel_protocols_place_once",
         [] {
           image_view view{solid(32, 32, 0, 200, 0)};
           render_buffer b;
           const graphics_support sixel{graphics_protocol::sixel, {10, 20}};
           const auto placed = draw(view, sixel, {{2, 1}, {8, 4}}, {20, 10}, b);
           check(placed.size() == 1 && view.shown_as() == graphics_protocol::sixel);
           check(!placed.empty() && placed[0].area == rect{{2, 1}, {8, 4}} &&
                     placed[0].payload.starts_with("\x1bP0;1;0q\"1;1;80;80"),
                 "80x80 pixels for 8x4 cells of 10x20");
           const auto again = draw(view, sixel, {{2, 1}, {8, 4}}, {20, 10}, b);
           check(!again.empty() && again[0].version == placed[0].version,
                 "cached payload keeps its version");
           view.set_image(solid(32, 32, 0, 0, 200));
           const auto changed = draw(view, sixel, {{2, 1}, {8, 4}}, {20, 10}, b);
           check(!changed.empty() && changed[0].version != placed[0].version);
         }},
        {"sixel_avoids_the_last_row",
         [] {
           image_view view{solid(8, 8, 9, 9, 9)};
           render_buffer b;
           const auto placed = draw(view, {graphics_protocol::sixel, {10, 20}},
                                    {{0, 6}, {5, 4}}, {20, 10}, b);
           check(!placed.empty() && placed[0].area.extent == size{5, 3},
                 "stops one row short of the bottom");
           const auto kitty = draw(view, {graphics_protocol::kitty, {10, 20}},
                                   {{0, 6}, {5, 4}}, {20, 10}, b);
           check(!kitty.empty() && kitty[0].area.extent == size{5, 4},
                 "kitty does not move the cursor");
         }},
    }};

const avionix_tests::suite image_file_suite{
    "image_file",
    {
        {"png_round_trip",
         [] {
           image_rgba8 img{3, 2, {}};
           for (std::uint8_t i = 0; i < 6; ++i)
             img.pixels.insert(img.pixels.end(),
                               {static_cast<std::uint8_t>(i * 40), 7, 200,
                                static_cast<std::uint8_t>(255 - i)});
           const auto bytes = encode_png(img);
           const auto decoded = decode_image(bytes);
           check(decoded.has_value() && decoded->width == 3 && decoded->height == 2 &&
                     decoded->pixels == img.pixels,
                 "Avionix's PNG decodes to the same pixels");
         }},
        {"rejects_garbage",
         [] {
           const std::vector<std::uint8_t> junk{1, 2, 3, 4, 5};
           const auto r = decode_image(junk);
           check(!r && r.error().kind == failure::invalid_argument);
           const auto missing = load_image("does/not/exist.png");
           check(!missing && missing.error().kind == failure::io_failed);
         }},
    }};

const avionix_tests::suite backdrop_suite{
    "backdrop",
    {
        {"cover_fit_crops_instead_of_letterboxing",
         [] {
           // A 2x1 image (left red, right blue) covering a square shows its
           // middle: the left edge of the square is red, the right blue,
           // and nothing is transparent.
           image_rgba8 img{2, 1, {255, 0, 0, 255, 0, 0, 255, 255}};
           const auto left = sample_cover(img, 10, 10, 0.5, 5);
           const auto right = sample_cover(img, 10, 10, 9.5, 5);
           check(left[3] == 255 && right[3] == 255);
           check(left[0] > left[2] && right[2] > right[0]);
         }},
        {"kitty_places_it_under_text",
         [] {
           backdrop wall{solid(64, 32, 200, 100, 50)};
           wall.set_dim(0.5);
           render_buffer b{{40, 10}};
           frame_state frame;
           frame.graphics = {graphics_protocol::kitty, {8, 16}};
           render_context ctx{b, b.area(), frame};
           wall.render(ctx);
           check(wall.under_text() && frame.placements.size() == 1);
           check(!frame.placements.empty() &&
                 frame.placements[0].payload.find("z=-1073741825") !=
                     std::string_view::npos);
           const auto px = wall.pixels_behind({{0, 0}, {40, 10}}, {4, 4});
           check(px.at(1, 1)[0] == 100 && px.at(1, 1)[3] == 255,
                 "dimmed by half, opaque");
         }},
        {"other_protocols_leave_text_alone",
         [] {
           backdrop wall{solid(8, 8, 1, 2, 3)};
           for (const graphics_protocol p :
                {graphics_protocol::sixel, graphics_protocol::iterm2,
                 graphics_protocol::none}) {
             render_buffer b{{20, 5}};
             frame_state frame;
             frame.graphics = {p, {10, 20}};
             render_context ctx{b, b.area(), frame};
             wall.render(ctx);
             check(!wall.under_text() && frame.placements.empty());
           }
         }},
    }};

}  // namespace
