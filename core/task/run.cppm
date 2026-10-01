module;

#include <algorithm>
#include <chrono>
#include <concepts>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

export module avionix.task.run;

import avionix.entity.geometry;
import avionix.entity.color;
import avionix.entity.event;
import avionix.entity.error;
import avionix.object.terminal;
import avionix.object.terminal_capabilities;
import avionix.object.buffer;
import avionix.object.renderer;
import avionix.object.event_queue;
import avionix.object.input_reader;
import avionix.task.dispatch;
import avionix.task.render;
import avionix.task.resize;

// The event loop. run() creates the objects for one session, drives them,
// and destroys them in reverse order before returning:
//
//   terminal_session ─┬─► input_reader thread ──► event_queue
//                     │                               │
//                     │         UI thread: drain ◄────┘
//                     │              │
//                     │         dispatch ──► driver.handle
//                     │              │
//                     └◄── write ◄── render_frame ◄── driver.draw
//
// The loop owns nothing long-lived; the terminal, renderer, and reader are
// locals whose lifetimes end with run(). The queue belongs to the caller so
// other threads can post to it before and during the run.

export namespace avionix {

struct run_options {
  terminal_options terminal{};
  input_reader_options input{};
  // Minimum time between frames. Input arriving faster than this is
  // batched into the next frame.
  std::chrono::milliseconds frame_interval{16};
  // Overrides detected color depth.
  std::optional<color_depth> colors{};
};

// What the loop drives. All calls happen on the UI thread.
template <typename D>
concept frame_driver = requires(D& driver, const D& const_driver, const event& value,
                                render_buffer& back) {
  driver.handle(value);
  driver.draw(back);
  { const_driver.running() } -> std::convertible_to<bool>;
  { const_driver.needs_redraw() } -> std::convertible_to<bool>;
  { const_driver.cursor() } -> std::convertible_to<std::optional<position>>;
  { driver.take_output() } -> std::same_as<std::string>;
};

template <frame_driver D>
std::expected<void, error> run(event_queue& queue, D& driver,
                               const run_options& options) {
  const terminal_capabilities caps = detect_capabilities(read_environment());

  terminal_options terminal_opts = options.terminal;
  terminal_opts.alternate_screen =
      terminal_opts.alternate_screen && caps.alternate_screen;
  terminal_opts.mouse = terminal_opts.mouse && caps.mouse;
  terminal_opts.bracketed_paste = terminal_opts.bracketed_paste && caps.bracketed_paste;
  terminal_opts.focus_events = terminal_opts.focus_events && caps.focus_events;

  auto session = terminal_session::open(terminal_opts);
  if (!session) {
    return std::unexpected(std::move(session.error()));
  }

  auto initial = session->query_size();
  if (!initial) {
    return std::unexpected(std::move(initial.error()));
  }
  const size start_size = clamp_terminal_size(*initial);

  renderer screen{options.colors.value_or(caps.colors), caps.synchronized_output};
  apply_resize(screen, start_size);
  driver.handle(event{resize_event{start_size}});

  std::expected<void, error> outcome{};
  {
    // Declared after the session so it is destroyed (and joined) first.
    input_reader reader{*session, queue, start_size, options.input};

    using clock = std::chrono::steady_clock;
    std::vector<queue_entry> entries;
    entries.reserve(64);
    auto last_frame = clock::time_point{};
    const std::stop_token never_stop{};

    const auto handle = [&](const event& value) {
      if (const auto* r = std::get_if<resize_event>(&value)) {
        apply_resize(screen, r->extent);
      }
      driver.handle(value);
    };

    while (driver.running()) {
      const auto now = clock::now();
      if constexpr (requires { driver.tick(now); }) driver.tick(now);
      if (!driver.running()) break;
      if (driver.needs_redraw() && now - last_frame >= options.frame_interval) {
        auto frame = render_frame(screen, *session, [&](render_buffer& back) {
          driver.draw(back);
          // Text inputs place the cursor while drawing, so the
          // request is read after draw and before present.
          screen.set_cursor(driver.cursor());
        });
        if (!frame) {
          outcome = std::unexpected(std::move(frame.error()));
          break;
        }
        last_frame = now;
      }

      auto deadline = driver.needs_redraw() ? last_frame + options.frame_interval
                                            : clock::now() + std::chrono::hours{1};
      if constexpr (requires { driver.next_deadline(); })
        deadline = std::min(deadline, driver.next_deadline());
      queue.drain(entries, deadline, never_stop);
      dispatch(entries, handle);
      if (std::string output = driver.take_output(); !output.empty()) {
        if (auto written = session->write(output); !written) {
          outcome = std::unexpected(std::move(written.error()));
          break;
        }
      }

      if (queue.closed() && reader.failed()) {
        outcome = std::unexpected(
            error{"run event loop", failure::io_failed, "terminal input closed", 0});
        break;
      }
    }
  }

  if (auto restored = session->restore(); !restored && outcome) {
    return restored;
  }
  return outcome;
}

}  // namespace avionix
