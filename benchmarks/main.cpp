// Hot-path benchmarks.
//
// Usage: avionix-benchmarks [--quick] [filter]
//
// Each benchmark reports time per operation and, where it applies,
// allocations per operation, cells inspected, and terminal bytes generated.
// Output volume matters as much as speed: a renderer that is faster but
// emits more bytes can be slower end to end over a real terminal link.
//
// Build with -Doptimize=ReleaseFast for meaningful numbers:
//   zig build bench -Doptimize=ReleaseFast

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <new>
#include <print>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

import avionix.entity.geometry;
import avionix.entity.color;
import avionix.entity.style;
import avionix.entity.unicode;
import avionix.entity.cell;
import avionix.entity.event;
import avionix.entity.constraint;
import avionix.object.buffer;
import avionix.object.ansi_encoder;
import avionix.object.renderer;
import avionix.object.input_decoder;
import avionix.object.event_queue;
import avionix.task.dispatch;
import avionix.interface.widget;
import avionix.interface.layout;
import avionix.interface.controls;

// Allocation counting. Replacing the global allocation functions is
// permitted in any one translation unit of a program.
namespace {
std::atomic<std::uint64_t> g_allocations{0};
std::atomic<std::uint64_t> g_allocated_bytes{0};
}  // namespace

void* operator new(std::size_t n) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  g_allocated_bytes.fetch_add(n, std::memory_order_relaxed);
  if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
  throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace avionix;
using clock_type = std::chrono::steady_clock;

template <typename T>
inline void keep(T const& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

struct counters {
  std::uint64_t cells_inspected{};
  std::uint64_t terminal_bytes{};
  std::uint64_t items{};  // domain-specific units (bytes decoded, events, ...)
};

struct options {
  bool quick{};
  std::string_view filter{};
};

// Runs `body` repeatedly for about `budget`, after a warmup, and prints a
// row. `body` returns per-iteration counters.
template <typename F>
void bench(const options& opts, std::string_view name, std::string_view unit,
           F&& body) {
  if (!opts.filter.empty() && name.find(opts.filter) == std::string_view::npos) return;
  const auto budget =
      opts.quick ? std::chrono::milliseconds{50} : std::chrono::milliseconds{400};

  for (int i = 0; i < 3; ++i) keep(body());  // warmup

  counters total;
  std::uint64_t iterations = 0;
  const auto alloc_before = g_allocations.load();
  const auto start = clock_type::now();
  auto now = start;
  do {
    for (int i = 0; i < 16; ++i) {
      const counters c = body();
      total.cells_inspected += c.cells_inspected;
      total.terminal_bytes += c.terminal_bytes;
      total.items += c.items;
      ++iterations;
    }
    now = clock_type::now();
  } while (now - start < budget);
  const auto allocs = g_allocations.load() - alloc_before;

  const double ns = std::chrono::duration<double, std::nano>(now - start).count() /
                    static_cast<double>(iterations);
  const double per = 1.0 / static_cast<double>(iterations);
  std::string extra;
  if (total.cells_inspected != 0) {
    extra += std::format("  cells/op {:>9.0f}",
                         static_cast<double>(total.cells_inspected) * per);
  }
  if (total.terminal_bytes != 0) {
    extra += std::format("  term-bytes/op {:>8.0f}",
                         static_cast<double>(total.terminal_bytes) * per);
  }
  if (total.items != 0) {
    const double items_per_second = static_cast<double>(total.items) /
                                    std::chrono::duration<double>(now - start).count();
    extra += std::format("  {:>8.1f} M{}/s", items_per_second / 1e6, unit);
  }
  std::println("{:<34} {:>11.1f} ns/op  allocs/op {:>6.2f}{}", name, ns,
               static_cast<double>(allocs) * per, extra);
}

std::string sample_text(std::size_t lines) {
  std::string s;
  for (std::size_t i = 0; i < lines; ++i) {
    s += "The quick brown fox jumps over the lazy dog; ";
    s += "\xE4\xB8\xAD\xE6\x96\x87 caf\xC3\xA9 \xF0\x9F\x98\x80 ";
    s += "e\xCC\x81 flag \xF0\x9F\x87\xBA\xF0\x9F\x87\xB8\n";
  }
  return s;
}

void draw_dashboard(render_buffer& back, int frame) {
  const rect area = back.area();
  for (std::uint32_t y = 0; y < back.height(); ++y) {
    const style s{y % 2 == 0 ? colors::cyan : colors::white, {}, attribute::none};
    back.put_text({0, static_cast<std::int32_t>(y)}, "row", s, area);
    back.put_text({5, static_cast<std::int32_t>(y)},
                  "status: nominal  latency: 12ms  queue: 0  throughput: 1024/s", s,
                  area);
  }
  // A small moving element: what an interactive app typically changes.
  back.put_text({frame % 50, 3}, "[*]", {colors::yellow, {}, attribute::bold}, area);
}

}  // namespace

int main(int argc, char** argv) {
  options opts;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--quick") {
      opts.quick = true;
    } else {
      opts.filter = arg;
    }
  }

  constexpr size screen{200, 60};
  std::println("Avionix benchmarks, screen {}x{}{}", screen.width, screen.height,
               opts.quick ? " (quick)" : "");

  // Buffer operations.
  {
    render_buffer b{screen};
    bench(opts, "buffer.clear", "", [&] {
      b.clear();
      keep(b.cells().data());
      return counters{};
    });
    render_buffer r{screen};
    bool toggle = false;
    bench(opts, "buffer.resize", "", [&] {
      toggle = !toggle;
      r.resize(toggle ? size{201, 61} : screen);
      keep(r.cells().data());
      return counters{};
    });
    render_buffer g{screen};
    bench(opts, "buffer.glyph_insertion", "cell", [&] {
      const rect area = g.area();
      for (std::int32_t y = 0; y < 60; ++y) {
        for (std::int32_t x = 0; x < 200; ++x) {
          g.put_grapheme({x, y}, "x", 1, {}, area);
        }
      }
      return counters{.items = screen.area()};
    });
    const std::string text = sample_text(60);
    render_buffer t{screen};
    bench(opts, "buffer.text_rendering", "B", [&] {
      std::int32_t y = 0;
      std::size_t start = 0;
      while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        t.put_text({0, y++}, std::string_view{text}.substr(start, end - start), {},
                   t.area());
        start = end + 1;
      }
      return counters{.items = text.size()};
    });
    const std::string ascii(200, 'a');
    bench(opts, "buffer.text_rendering_ascii", "B", [&] {
      for (std::int32_t y = 0; y < 60; ++y) t.put_text({0, y}, ascii, {}, t.area());
      return counters{.items = ascii.size() * 60};
    });
  }

  // Diffing.
  {
    render_buffer front{screen};
    render_buffer back{screen};
    draw_dashboard(front, 0);
    draw_dashboard(back, 0);
    std::vector<changed_run> runs;
    runs.reserve(1024);
    bench(opts, "diff.identical", "", [&] {
      runs.clear();
      const auto s = collect_changed_runs(front, back, runs);
      return counters{.cells_inspected = s.cells_inspected};
    });
    draw_dashboard(back, 7);
    bench(opts, "diff.small_change", "", [&] {
      runs.clear();
      const auto s = collect_changed_runs(front, back, runs);
      return counters{.cells_inspected = s.cells_inspected};
    });
    render_buffer full{screen};
    for (std::uint32_t y = 0; y < screen.height; ++y) {
      for (std::uint32_t x = 0; x < screen.width; x += 2) {
        full.put_grapheme({static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)},
                          "#", 1, {}, full.area());
      }
    }
    bench(opts, "diff.changed_runs_dense", "", [&] {
      runs.clear();
      const auto s = collect_changed_runs(front, full, runs);
      return counters{.cells_inspected = s.cells_inspected, .items = runs.size()};
    });
  }

  // Encoding and presentation.
  {
    ansi_encoder e;
    bench(opts, "ansi.encode_full_screen", "cell", [&] {
      e.clear();
      e.forget_state();
      for (std::int32_t y = 0; y < 60; ++y) {
        e.move_to({0, y});
        for (std::int32_t x = 0; x < 200; ++x) {
          e.set_style({x % 3 == 0 ? colors::red : colors::green, {}, attribute::none});
          e.write_glyph("x", 1);
        }
      }
      return counters{.terminal_bytes = e.size(), .items = screen.area()};
    });

    renderer r{color_depth::truecolor, true};
    r.resize(screen);
    int frame = 0;
    bench(opts, "present.full_repaint", "", [&] {
      r.invalidate();
      r.back().clear();
      draw_dashboard(r.back(), frame);
      const auto bytes = r.present();
      return counters{.cells_inspected = r.last_frame().cells_inspected,
                      .terminal_bytes = bytes.size()};
    });
    bench(opts, "present.animated_frame", "", [&] {
      r.back().clear();
      draw_dashboard(r.back(), ++frame);
      const auto bytes = r.present();
      return counters{.cells_inspected = r.last_frame().cells_inspected,
                      .terminal_bytes = bytes.size()};
    });
    bench(opts, "present.idle_frame", "", [&] {
      r.back().clear();
      draw_dashboard(r.back(), frame);
      const auto bytes = r.present();
      return counters{.cells_inspected = r.last_frame().cells_inspected,
                      .terminal_bytes = bytes.size()};
    });
  }

  // Layout.
  {
    std::vector<constraint> cs;
    for (std::size_t i = 0; i < 12; ++i) {
      cs.push_back(i % 3 == 0   ? constraint::fixed(3)
                   : i % 3 == 1 ? constraint::fill(2)
                                : constraint::maximum(10));
    }
    bench(opts, "layout.solve_12", "", [&] {
      std::array<std::uint32_t, 12> lengths{};
      solve_lengths(200, cs, lengths);
      keep(lengths);
      return counters{};
    });
    column root;
    for (int i = 0; i < 6; ++i) {
      auto& r = root.add(constraint::fill(), row{});
      for (int j = 0; j < 6; ++j) r.add(constraint::fill(), text{"cell"});
    }
    render_buffer b{screen};
    frame_state frame;
    bench(opts, "layout.render_6x6_tree", "", [&] {
      render_context ctx{b, b.area(), frame};
      root.render_in(ctx);
      return counters{};
    });
  }

  // Unicode.
  {
    const std::string ascii(4096, 'a');
    bench(opts, "unicode.width_ascii", "B", [&] {
      keep(display_width(ascii));
      return counters{.items = ascii.size()};
    });
    const std::string mixed = sample_text(64);
    bench(opts, "unicode.width_mixed", "B", [&] {
      keep(display_width(mixed));
      return counters{.items = mixed.size()};
    });
    bench(opts, "unicode.graphemes_mixed", "B", [&] {
      std::size_t count = 0;
      for (const auto g : graphemes(mixed)) count += g.width;
      keep(count);
      return counters{.items = mixed.size()};
    });
  }

  // Input decoding.
  {
    std::string input;
    for (int i = 0; i < 64; ++i) {
      input += "hello";
      input += "\x1b[A\x1b[1;5C\x1b[<0;10;5M\x1b[<0;10;5m";
      input += "\xC3\xA9\x1b[3~\r";
    }
    input += "\x1b[200~pasted text body\x1b[201~";
    input_decoder d;
    std::vector<event> events;
    events.reserve(1024);
    bench(opts, "input.decode_mixed", "B", [&] {
      events.clear();
      d.feed(input, events);
      return counters{.items = input.size()};
    });
  }

  // Event dispatch.
  {
    event_queue q;
    std::vector<queue_entry> entries;
    entries.reserve(1024);
    std::uint64_t sum = 0;
    auto handler = [&](const event& e) {
      if (const auto* u = std::get_if<user_event>(&e)) sum += u->value;
    };
    bench(opts, "dispatch.push_drain_1000", "event", [&] {
      for (std::uint64_t i = 0; i < 1000; ++i) q.push(event{user_event{0, i}});
      q.try_drain(entries);
      dispatch(entries, handler);
      keep(sum);
      return counters{.items = 1000};
    });
  }
  return 0;
}
