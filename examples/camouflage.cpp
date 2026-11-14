// A camouflaged panel over a grid, and a word that camouflages itself on
// a timer over a blank band. The panel bends the grid behind it and fails
// near the mouse. Over the blank band nothing can bend, so the word shows
// only its outline while hidden. Space reveals or hides the panel, g turns
// the grid off and on, q quits.
// A large SIGNAL stays cloaked, its letter strokes traced by the rim.
// The panel toggles every 3 s. Space toggles it and restarts that timer.
// Usage: camouflage [--self-test]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>

import avionix;

namespace {
using namespace avionix;
using namespace std::chrono_literals;

// The word is solid for the first half of each cycle and camouflaged for
// the second. Each change takes camouflage's 0.75 s dissolve.
constexpr double word_cycle_seconds = 6.0;
constexpr double panel_toggle_seconds = 3.0;
constexpr std::string_view word_text = "SIGNAL ACQUIRED";
// Rows from the bottom of the screen: the word, and the blank band around it.
constexpr std::int32_t word_row_from_bottom = 4;

// Draw only the strokes. Drawing the spaces would cloak a rectangle and
// lose the letter shapes. Thin strokes keep every cell in the visible rim.
constexpr std::string_view big_word_rows[] = {
    "##### ##### ##### #   #  ###  #    ", "#       #   #     ##  # #   # #    ",
    "#####   #   # ### # # # ##### #    ", "    #   #   #   # #  ## #   # #    ",
    "##### ##### ##### #   # #   # #####",
};

void draw_big_word(render_context& context) {
  for (std::int32_t y = 0; y < 5; ++y) {
    const auto line = big_word_rows[y];
    for (std::int32_t x = 0; x < static_cast<std::int32_t>(line.size()); ++x) {
      if (line[static_cast<std::size_t>(x)] == '#') context.draw_text({x, y}, " ");
    }
  }
}

// Graph paper with some text on it, except for a blank band around the
// word. The panel bends the grid lines; the band shows what the effect
// looks like with nothing behind it.
void draw_grid(render_context& context, bool grid) {
  const style_patch line{.foreground = color::rgb(0x4a, 0x5a, 0x6e)};
  const style_patch label{.foreground = color::rgb(0xb8, 0xc4, 0xd0)};
  const auto width = static_cast<std::int32_t>(context.width());
  const auto height = static_cast<std::int32_t>(context.height());
  const std::int32_t band = height - word_row_from_bottom;
  if (grid) {
    for (std::int32_t y = 0; y < height; ++y) {
      if (y >= band - 7 && y <= band + 1) continue;
      for (std::int32_t x = 0; x < width; ++x) {
        const bool column = x % 6 == 0;
        const bool row = y % 3 == 0;
        const std::string_view glyph = column && row ? "+"
                                       : column      ? "|"
                                       : row         ? "-"
                                                     : " ";
        context.draw_text({x, y}, glyph, line);
      }
    }
  }
  constexpr std::string_view lines[] = {
      "SECTOR 7 / GRID REFERENCE 44-19 / NOTHING TO SEE HERE",
      "The panel in the middle bends the grid behind it.",
      "Move the mouse over it: the field fails near the pointer.",
  };
  for (std::int32_t i = 0; i < 3; ++i) {
    context.draw_text({2, 1 + i * 3}, lines[i], label);
  }
  if (height > 2) {
    context.draw_text({2, height - 2},
                      "Space reveals or hides the panel   g grid on/off   q quits",
                      label);
  }
}

void draw_panel(render_context& context) {
  context.clear({.background = color::rgb(0x1a, 0x24, 0x30)});
  const style_patch body{.foreground = color::rgb(0xe8, 0xee, 0xf4)};
  context.draw_text({1, 0}, "Status: all systems nominal", body);
  context.draw_text({1, 2}, "Reactor     98%  ||||||||||", body);
  context.draw_text({1, 3}, "Shields     71%  |||||||", body);
  context.draw_text({1, 5}, "Ctrl+C or q to quit.", body);
}

struct scene {
  layer root;
  camouflage<block>* panel{};
  camouflage<text>* word{};
  camouflage<canvas>* big_word{};
  bool grid{true};
  double word_clock{};
  double panel_clock{};

  scene() {
    root.add(canvas{[this](render_context& context) { draw_grid(context, grid); }});
    auto& upper = root.add(column{});
    upper.add(constraint::fill(), spacer{});
    auto& middle = upper.add(constraint::fixed(9), row{});
    middle.add(constraint::fill(), spacer{});
    panel = &middle.add(
        constraint::fixed(36),
        camouflage{block{"Cloaked", border_kind::rounded, canvas{draw_panel}},
                   camouflage_options{.pointer_reveals = true}});
    middle.add(constraint::fill(), spacer{});
    upper.add(constraint::fill(), spacer{});

    auto& lettering = upper.add(constraint::fixed(5), row{});
    lettering.add(constraint::fill(), spacer{});
    big_word = &lettering.add(
        constraint::fixed(static_cast<std::uint32_t>(big_word_rows[0].size())),
        camouflage{canvas{draw_big_word}});
    lettering.add(constraint::fill(), spacer{});
    upper.add(constraint::fixed(1), spacer{});

    auto& band = upper.add(constraint::fixed(1), row{});
    band.add(constraint::fill(), spacer{});
    word = &band.add(constraint::fixed(static_cast<std::uint32_t>(word_text.size())),
                     camouflage{text{std::string{word_text},
                                     {.foreground = color::rgb(0xff, 0xd0, 0x60),
                                      .add = attribute::bold}}});
    band.add(constraint::fill(), spacer{});
    upper.add(constraint::fixed(static_cast<std::uint32_t>(word_row_from_bottom - 1)),
              spacer{});

    // The word starts solid rather than dissolving in.
    word->set_reveal(1.0);
    word->advance(1s);
  }

  void toggle_panel() {
    panel->set_reveal(panel->reveal_target() < 0.5 ? 1.0 : 0.0);
    panel_clock = 0.0;
  }

  // Space restarts the panel timer so its change lasts a full interval.
  void tick(std::chrono::steady_clock::duration elapsed) {
    panel_clock += std::chrono::duration<double>(elapsed).count();
    if (panel_clock >= panel_toggle_seconds) {
      const double intervals = std::floor(panel_clock / panel_toggle_seconds);
      if (std::fmod(intervals, 2.0) >= 1.0)
        panel->set_reveal(panel->reveal_target() < 0.5 ? 1.0 : 0.0);
      panel_clock = std::fmod(panel_clock, panel_toggle_seconds);
    }
    panel->advance(elapsed);
    big_word->advance(elapsed);
    word_clock += std::chrono::duration<double>(elapsed).count();
    const bool solid =
        std::fmod(word_clock, word_cycle_seconds) < word_cycle_seconds / 2;
    word->set_reveal(solid ? 1.0 : 0.0);
    word->advance(elapsed);
  }
};

// Renders the scene offscreen in each state and checks what shows.
bool self_test() {
  scene s;
  application app;
  const auto frame = [&] { return application::render_to_buffer(s.root, {80, 24}); };
  bool ok = true;
  const auto expect = [&](bool condition, std::string_view what) {
    if (!condition) std::println("  failed: {}", what);
    ok = ok && condition;
  };
  const auto readable_big_word = [&](const render_buffer& buffer) {
    const rect area = s.big_word->last_area();
    if (area.width() != big_word_rows[0].size() || area.height() != 5) return false;
    for (std::int32_t y = 0; y < 5; ++y) {
      for (std::int32_t x = 0; x < static_cast<std::int32_t>(area.width()); ++x) {
        const auto& c = buffer.at({area.left() + x, area.top() + y});
        const bool tinted = c.appearance().background.type() == color::kind::rgb;
        if (tinted != (big_word_rows[y][static_cast<std::size_t>(x)] == '#'))
          return false;
      }
    }
    return s.big_word->reveal() == 0.0 && s.big_word->animating();
  };
  // Every cell of the word shows a tinted blank: the outline, and no text.
  const auto outline_only = [&](const render_buffer& b) {
    const rect area = s.word->last_area();
    bool outlined = !area.empty();
    for (std::int32_t x = area.left(); x < area.right(); ++x) {
      const cell& c = b.at({x, area.top()});
      outlined = outlined && c.text() == " " &&
                 c.appearance().background.type() == color::kind::rgb;
    }
    return outlined;
  };

  s.panel->advance(50ms);
  const render_buffer hidden = frame();
  expect(hidden.find("Space reveals").has_value(), "hint visible");
  expect(!hidden.find("Cloaked").has_value(), "panel title hidden while camouflaged");
  expect(!hidden.find("nominal").has_value(), "panel text hidden while camouflaged");
  expect(std::ranges::equal(frame().cells(), hidden.cells()), "repeatable frame");
  expect(hidden.find(word_text).has_value(), "word solid at the start of its cycle");
  expect(readable_big_word(hidden), "big cloaked word has readable letter shapes");

  // Into the hidden half of the word's cycle, in 50 ms ticks like main().
  for (int i = 0; i < 80; ++i) {
    s.tick(50ms);
    expect(readable_big_word(frame()), "big word stays readable while animating");
  }
  const render_buffer word_hidden = frame();
  expect(word_hidden.find("nominal").has_value(), "timer reveals the panel");
  expect(!word_hidden.find("SIGNAL").has_value(), "word hidden in the second half");
  expect(outline_only(word_hidden), "hidden word keeps a visible outline");
  // And back: the word returns completely.
  for (int i = 0; i < 60; ++i) s.tick(50ms);
  const render_buffer word_back = frame();
  expect(!word_back.find("nominal").has_value(), "timer cloaks the panel again");
  expect(word_back.find(word_text).has_value(), "word back in the next cycle");
  expect(!s.word->animating(), "word fully solid again");

  // Without any grid the panel still shows its outline.
  s.grid = false;
  const render_buffer bare = frame();
  expect(readable_big_word(bare), "big word readable without a grid");
  const rect panel_area = s.panel->last_area();
  expect(!bare.find("Cloaked").has_value(), "panel hidden without a grid");
  expect(bare.at(panel_area.origin).appearance().background.type() == color::kind::rgb,
         "panel outline visible without a grid");
  s.grid = true;

  s.panel->set_reveal(1.0);
  s.panel->advance(300ms);
  expect(s.panel->animating(), "transition in progress");
  (void)frame();
  s.panel->advance(1s);
  const render_buffer shown = frame();
  expect(shown.find("Cloaked").has_value(), "panel title visible when revealed");
  expect(shown.find("nominal").has_value(), "panel text visible when revealed");
  expect(!s.panel->animating() && !s.panel->advance(50ms), "solid at rest is static");

  s.panel->set_reveal(0.0);
  s.panel->advance(1s);
  const render_buffer hidden_again = frame();
  // The pointer sits on the "i" of "nominal". The cell under it shows the
  // panel; the same cell without the pointer shows the bent grid.
  const position word = shown.find("nominal").value_or(position{});
  const position pointer{word.x + 3, word.y};
  app.simulate(s.root, mouse_event{mouse_button::none, mouse_action::move, pointer});
  const render_buffer pointed = frame();
  expect(pointed.at(pointer) == shown.at(pointer), "pointer reveals the panel nearby");
  expect(!(hidden_again.at(pointer) == shown.at(pointer)),
         "panel hidden without pointer");

  scene manual;
  manual.tick(2s);
  manual.toggle_panel();
  manual.tick(1s);
  expect(manual.panel->reveal() == 1.0, "Space reveals and restarts the timer");
  manual.tick(1s);
  expect(manual.panel->reveal_target() == 1.0,
         "manual reveal lasts until the new deadline");
  manual.tick(1s);
  expect(manual.panel->reveal_target() == 0.0, "timer resumes after Space");

  std::println("camouflage self-test: {}", ok ? "passed" : "FAILED");
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string_view{argv[1]} == "--self-test")
    return self_test() ? 0 : 1;

  scene s;
  application app{{.mouse_motion = true}};
  auto last = std::chrono::steady_clock::now();
  const auto ticker = app.set_interval(50ms, [&] {
    const auto now = std::chrono::steady_clock::now();
    s.tick(now - last);
    last = now;
  });
  app.on_event([&](const event& e) {
    const auto* k = as<key_event>(e);
    if (k == nullptr) return false;
    if (k->is(U'q')) {
      app.quit();
      return true;
    }
    if (k->is(U' ')) {
      s.toggle_panel();
      return true;
    }
    if (k->is(U'g')) {
      s.grid = !s.grid;
      return true;
    }
    return false;
  });
  const auto result = app.run(s.root);
  ticker.cancel();
  if (!result) {
    std::println(stderr, "camouflage: {}", result.error().message());
    return 1;
  }
  return 0;
}
