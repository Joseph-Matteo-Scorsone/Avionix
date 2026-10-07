// Diffing, changed-run generation, ANSI encoding, and frame presentation.
// Everything runs against virtual buffers; no terminal is involved.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

import avionix.entity.geometry;
import avionix.entity.color;
import avionix.entity.style;
import avionix.entity.cell;
import avionix.entity.unicode;
import avionix.entity.error;
import avionix.object.buffer;
import avionix.object.ansi_encoder;
import avionix.object.renderer;
import avionix.task.render;
import avionix.task.resize;
import avionix_tests.check;

using namespace avionix;
using avionix_tests::check;
using avionix_tests::check_bytes;
using avionix_tests::check_equal;

namespace {

// Minimal VT interpreter for the subset the renderer emits. Applying the
// renderer's output to it must reproduce the back buffer, which checks the
// encoder end to end without trusting any exact byte sequence.
class virtual_terminal {
 public:
  explicit virtual_terminal(size extent) : extent_{extent}, text_(extent.area(), " ") {}

  void apply(std::string_view bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
      if (bytes[i] == '\x1b' && i + 1 < bytes.size() && bytes[i + 1] == '[') {
        std::size_t j = i + 2;
        while (j < bytes.size() && !(bytes[j] >= 0x40 && bytes[j] <= 0x7e)) ++j;
        csi(bytes.substr(i + 2, j - i - 2), bytes[j]);
        i = j + 1;
      } else if (bytes[i] == '\x1b' && i + 1 < bytes.size() && bytes[i + 1] == ']') {
        // OSC, including hyperlinks (OSC 8) and clipboard copies (OSC 52).
        // Terminated by BEL or ST (ESC \). The text that follows is cells.
        i += 2;
        while (i < bytes.size()) {
          if (bytes[i] == '\x07') {
            ++i;
            break;
          }
          if (bytes[i] == '\x1b' && i + 1 < bytes.size() && bytes[i + 1] == '\\') {
            i += 2;
            break;
          }
          ++i;
        }
      } else if (bytes[i] == '\r') {
        x_ = 0;
        ++i;
      } else {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        const std::size_t n = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        put(std::string{bytes.substr(i, n)});
        i += n;
      }
    }
  }

  [[nodiscard]] std::string row(std::uint32_t y) const {
    std::string out;
    for (std::uint32_t x = 0; x < extent_.width; ++x)
      out += text_[y * extent_.width + x];
    return out;
  }

 private:
  static std::vector<int> numbers(std::string_view params) {
    std::vector<int> out;
    int current = 0;
    bool any = false;
    for (const char c : params) {
      if (c >= '0' && c <= '9') {
        current = current * 10 + (c - '0');
        any = true;
      } else if (c == ';') {
        out.push_back(current);
        current = 0;
        any = false;
      }
    }
    if (any || !params.empty()) out.push_back(current);
    return out;
  }

  void csi(std::string_view params, char final_byte) {
    if (!params.empty() && params[0] == '?') return;  // modes
    const auto n = numbers(params);
    const auto arg = [&](std::size_t i, int fallback) {
      return i < n.size() && n[i] != 0 ? n[i] : fallback;
    };
    switch (final_byte) {
      case 'H':
        y_ = arg(0, 1) - 1;
        x_ = arg(1, 1) - 1;
        break;
      case 'C':
        x_ += arg(0, 1);
        break;
      case 'D':
        x_ -= arg(0, 1);
        break;
      case 'A':
        y_ -= arg(0, 1);
        break;
      case 'B':
        y_ += arg(0, 1);
        break;
      case 'J':
        for (auto& t : text_) t = " ";
        break;
      default:
        break;  // SGR and others do not affect text
    }
  }

  void put(std::string glyph) {
    const int width = code_point_width(decode_utf8(glyph).value) == 2 ? 2 : 1;
    if (x_ >= 0 && y_ >= 0 && x_ + width <= static_cast<int>(extent_.width) &&
        y_ < static_cast<int>(extent_.height)) {
      const std::size_t at =
          static_cast<std::size_t>(y_) * extent_.width + static_cast<std::size_t>(x_);
      text_[at] = glyph;
      if (width == 2) text_[at + 1] = "";
    }
    x_ += width;
  }

  size extent_;
  std::vector<std::string> text_;
  int x_{};
  int y_{};
};

std::string buffer_row(const render_buffer& b, std::uint32_t y) {
  std::string out;
  for (const cell& c : b.row(y)) {
    if (c.is_continuation()) continue;
    out += c.text();
  }
  return out;
}

struct capture_output {
  std::string bytes;
  int writes{};

  std::expected<void, error> write(std::string_view data) {
    bytes += data;
    ++writes;
    return {};
  }
};

struct failing_output {
  std::expected<void, error> write(std::string_view /*data*/) {
    return std::unexpected(error{"write", failure::io_failed, "test", 0});
  }
};

const avionix_tests::suite diff{
    "diff",
    {
        {"identical_buffers_have_no_runs",
         [] {
           render_buffer a{{10, 3}};
           render_buffer b{{10, 3}};
           std::vector<changed_run> runs;
           const auto stats = collect_changed_runs(a, b, runs);
           check(runs.empty());
           check_equal(stats.cells_inspected, std::uint64_t{30});
           check_equal(stats.cells_changed, std::uint64_t{0});
         }},
        {"single_change",
         [] {
           render_buffer a{{10, 3}};
           render_buffer b{{10, 3}};
           b.put_text({4, 1}, "x", {}, b.area());
           std::vector<changed_run> runs;
           collect_changed_runs(a, b, runs);
           check_equal(runs.size(), std::size_t{1});
           check(runs[0] == changed_run{1, 4, 5});
         }},
        {"nearby_changes_merge",
         [] {
           render_buffer a{{20, 1}};
           render_buffer b{{20, 1}};
           b.put_text({2, 0}, "a", {}, b.area());
           b.put_text({5, 0}, "b", {}, b.area());
           std::vector<changed_run> runs;
           collect_changed_runs(a, b, runs, 4);
           check_equal(runs.size(), std::size_t{1});
           check(runs[0] == changed_run{0, 2, 6});
         }},
        {"distant_changes_split",
         [] {
           render_buffer a{{20, 1}};
           render_buffer b{{20, 1}};
           b.put_text({1, 0}, "a", {}, b.area());
           b.put_text({15, 0}, "b", {}, b.area());
           std::vector<changed_run> runs;
           collect_changed_runs(a, b, runs, 4);
           check_equal(runs.size(), std::size_t{2});
         }},
        {"run_includes_whole_wide_cell",
         [] {
           render_buffer a{{6, 1}};
           render_buffer b{{6, 1}};
           b.put_text({2, 0}, "\xE4\xB8\xAD", {}, b.area());
           std::vector<changed_run> runs;
           collect_changed_runs(a, b, runs, 0);
           check_equal(runs.size(), std::size_t{1});
           check(runs[0] == changed_run{0, 2, 4});
         }},
        {"style_only_change_is_detected",
         [] {
           render_buffer a{{3, 1}};
           render_buffer b{{3, 1}};
           a.put_text({0, 0}, "abc", {}, a.area());
           b.put_text({0, 0}, "abc", {}, b.area());
           b.apply_style({{1, 0}, {1, 1}}, {.foreground = colors::red});
           std::vector<changed_run> runs;
           collect_changed_runs(a, b, runs, 0);
           check_equal(runs.size(), std::size_t{1});
           check(runs[0] == changed_run{0, 1, 2});
         }},
    }};

const avionix_tests::suite encoder{
    "ansi",
    {
        {"cursor_position_is_one_based",
         [] {
           ansi_encoder e;
           e.move_to({0, 0});
           check_bytes(e.bytes(), "\x1b[1H");
           e.clear();
           e.forget_state();
           e.move_to({4, 2});
           check_bytes(e.bytes(), "\x1b[3;5H");
         }},
        {"redundant_move_is_skipped",
         [] {
           ansi_encoder e;
           e.move_to({3, 3});
           e.clear();
           e.move_to({3, 3});
           check(e.empty());
         }},
        {"short_forward_move_uses_cuf",
         [] {
           ansi_encoder e;
           e.move_to({10, 5});
           e.clear();
           e.move_to({13, 5});
           check_bytes(e.bytes(), "\x1b[3C");
         }},
        {"move_to_column_zero_uses_cr",
         [] {
           ansi_encoder e;
           e.move_to({10, 5});
           e.clear();
           e.move_to({0, 5});
           check_bytes(e.bytes(), "\r");
         }},
        {"glyph_advances_cursor",
         [] {
           ansi_encoder e;
           e.set_screen_width(80);
           e.move_to({0, 0});
           e.write_glyph("a", 1);
           e.clear();
           e.move_to({1, 0});
           check(e.empty());
         }},
        {"pending_wrap_forgets_cursor",
         [] {
           ansi_encoder e;
           e.set_screen_width(3);
           e.move_to({2, 0});
           e.write_glyph("a", 1);
           check(!e.cursor().has_value());
         }},
        {"sgr_full_then_delta",
         [] {
           ansi_encoder e;
           e.set_style({colors::red, {}, attribute::bold});
           check_bytes(e.bytes(), "\x1b[0;1;31m");
           e.clear();
           e.set_style({colors::red, {}, attribute::bold});
           check(e.empty());
           e.set_style({colors::green, {}, attribute::bold});
           check_bytes(e.bytes(), "\x1b[32m");
         }},
        {"sgr_removal_uses_off_codes",
         [] {
           ansi_encoder e;
           e.set_style({{}, {}, attribute::bold | attribute::italic | attribute::dim});
           e.clear();
           e.set_style({{}, {}, attribute::italic | attribute::dim});
           // 22 clears bold and dim; dim is re-added.
           check_bytes(e.bytes(), "\x1b[22;2m");
         }},
        {"sgr_colors",
         [] {
           ansi_encoder e;
           e.set_style({color::rgb(1, 2, 3), color::indexed(200), attribute::none});
           check_bytes(e.bytes(), "\x1b[0;38;2;1;2;3;48;5;200m");
           e.clear();
           e.set_style(
               {colors::bright_blue, color::terminal_default(), attribute::none});
           // A reset plus the new foreground is shorter than "94;49".
           check_bytes(e.bytes(), "\x1b[0;94m");
         }},
        {"sgr_downgrades_to_depth",
         [] {
           ansi_encoder e{color_depth::ansi16};
           e.set_style({color::rgb(255, 0, 0), {}, attribute::none});
           check_bytes(e.bytes(), "\x1b[0;91m");
         }},
        {"monochrome_emits_no_color",
         [] {
           ansi_encoder e{color_depth::monochrome};
           e.set_style({colors::red, colors::blue, attribute::underline});
           check_bytes(e.bytes(), "\x1b[0;4m");
         }},
    }};

const avionix_tests::suite present{
    "renderer",
    {
        {"first_frame_clears_and_draws",
         [] {
           renderer r{color_depth::truecolor, false};
           r.resize({5, 2});
           r.back().put_text({0, 0}, "hi", {}, r.back().area());
           const auto bytes = std::string{r.present()};
           check(bytes.find("\x1b[2J") != std::string::npos);
           check(bytes.find("hi") != std::string::npos);
           check(r.last_frame().full_redraw);
         }},
        {"unchanged_frame_emits_nothing",
         [] {
           renderer r{color_depth::truecolor, false};
           r.resize({5, 2});
           r.back().put_text({0, 0}, "hi", {}, r.back().area());
           (void)r.present();
           r.back().clear();
           r.back().put_text({0, 0}, "hi", {}, r.back().area());
           check(r.present().empty());
           check_equal(r.last_frame().runs, std::uint64_t{0});
         }},
        {"only_changed_cells_are_written",
         [] {
           renderer r{color_depth::truecolor, false};
           r.resize({10, 3});
           r.back().put_text({0, 0}, "hello", {}, r.back().area());
           (void)r.present();
           r.back().clear();
           r.back().put_text({0, 0}, "hellO", {}, r.back().area());
           const auto bytes = std::string{r.present()};
           check_equal(r.last_frame().cells_written, std::uint64_t{1});
           check(bytes.find("O") != std::string::npos);
           check(bytes.find("hell") == std::string::npos);
         }},
        {"blank_cells_are_not_drawn_on_full_redraw",
         [] {
           renderer r{color_depth::truecolor, false};
           r.resize({80, 24});
           (void)r.present();
           check_equal(r.last_frame().cells_written, std::uint64_t{0});
         }},
        {"output_reproduces_back_buffer",
         [] {
           renderer r{color_depth::truecolor, true};
           r.resize({12, 3});
           virtual_terminal vt{{12, 3}};
           const auto draw = [&](std::string_view a, std::string_view b) {
             r.back().clear();
             r.back().put_text({0, 0}, a, {.foreground = colors::red}, r.back().area());
             r.back().put_text({2, 2}, b, {}, r.back().area());
             vt.apply(r.present());
           };
           draw(
               "abc\xE4\xB8\xAD"
               "d",
               "xyz");
           check(vt.row(0) == buffer_row(r.front(), 0));
           check(vt.row(2) == buffer_row(r.front(), 2));
           draw(
               "a\xE4\xB8\xAD"
               "cd",
               "x z");
           check(vt.row(0) == buffer_row(r.front(), 0));
           check(vt.row(2) == buffer_row(r.front(), 2));
         }},
        {"synchronized_output_wraps_frame",
         [] {
           renderer r{color_depth::truecolor, true};
           r.resize({4, 1});
           r.back().put_text({0, 0}, "x", {}, r.back().area());
           const auto bytes = r.present();
           check(bytes.starts_with("\x1b[?2026h"));
           check(bytes.ends_with("\x1b[?2026l"));
         }},
        {"cursor_request_is_honored",
         [] {
           renderer r{color_depth::truecolor, false};
           r.resize({10, 5});
           (void)r.present();
           r.back().clear();
           r.set_cursor(position{3, 2});
           const auto bytes = std::string{r.present()};
           check(bytes.ends_with("\x1b[3;4H\x1b[?25h"));
         }},
        {"resize_forces_full_redraw",
         [] {
           renderer r{color_depth::truecolor, false};
           r.resize({5, 1});
           (void)r.present();
           check(apply_resize(r, {8, 2}));
           check(!apply_resize(r, {8, 2}));
           r.back().clear();
           (void)r.present();
           check(r.last_frame().full_redraw);
           check_equal(r.front().width(), 8U);
         }},
        {"zero_size_is_clamped",
         [] { check(clamp_terminal_size({0, 0}) == size{1, 1}); }},
        {"hyperlink_text_survives_osc",
         [] {
           renderer r{color_depth::truecolor, false};
           r.resize({8, 1});
           r.begin_frame();
           const auto id = r.back().intern_link("https://example.com");
           r.back().put_text({0, 0}, "docs", {}, r.back().area(), id);
           virtual_terminal vt{{8, 1}};
           vt.apply(r.present());
           check(vt.row(0) == "docs    ");
         }},
    }};

const avionix_tests::suite frame_task{
    "render_task",
    {
        {"one_write_per_frame",
         [] {
           renderer r{color_depth::truecolor, false};
           r.resize({10, 2});
           capture_output out;
           auto result = render_frame(r, out, [](render_buffer& back) {
             back.put_text({0, 0}, "one", {}, back.area());
             back.put_text({0, 1}, "two", {}, back.area());
           });
           check(result.has_value());
           check_equal(out.writes, 1);
           check_equal(result->bytes, std::uint64_t{out.bytes.size()});
         }},
        {"idle_frame_writes_nothing",
         [] {
           renderer r{color_depth::truecolor, false};
           r.resize({10, 2});
           capture_output out;
           const auto draw = [](render_buffer& back) {
             back.put_text({0, 0}, "same", {}, back.area());
           };
           (void)render_frame(r, out, draw);
           (void)render_frame(r, out, draw);
           check_equal(out.writes, 1);
         }},
        {"successive_frames_keep_link_identity",
         [] {
           renderer r{color_depth::truecolor, false};
           r.resize({12, 1});
           capture_output out;
           const auto draw = [](render_buffer& back, std::string_view url) {
             const auto id = back.intern_link(url);
             back.put_text({0, 0}, "docs", {}, back.area(), id);
           };
           check(render_frame(r, out, [&](render_buffer& back) {
                   draw(back, "https://example.com");
                 }).has_value());
           check(out.bytes.find("\x1b]8;;https://example.com\x1b\\") !=
                 std::string::npos);
           check(out.bytes.find("\x1b]8;;\x1b\\") != std::string::npos);
           check(render_frame(r, out, [&](render_buffer& back) {
                   draw(back, "https://example.com");
                 }).has_value());
           check_equal(out.writes, 1);
           check(render_frame(r, out, [&](render_buffer& back) {
                   draw(back, "https://other.test");
                 }).has_value());
           check(out.bytes.find("\x1b]8;;https://other.test\x1b\\") !=
                 std::string::npos);
           check_equal(out.writes, 2);
         }},
        {"write_failure_invalidates",
         [] {
           renderer r{color_depth::truecolor, false};
           r.resize({4, 1});
           (void)r.present();
           failing_output out;
           auto result = render_frame(r, out, [](render_buffer& back) {
             back.put_text({0, 0}, "x", {}, back.area());
           });
           check(!result.has_value());
           check(result.error().kind == failure::io_failed);
           r.back().clear();
           (void)r.present();
           check(r.last_frame().full_redraw);
         }},
    }};

}  // namespace
