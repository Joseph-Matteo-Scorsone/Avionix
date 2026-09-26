// A counter driven by keys and by a background worker.
//
// The worker never touches widgets. It posts a callback, and the callback
// runs on the UI thread between frames:
//
//   worker thread ──app.post──► event queue ──► UI thread ──► text::set
//
// Keys: + / - change the count, r resets, q quits.

#include <chrono>
#include <format>
#include <print>
#include <stop_token>
#include <thread>

import avionix;

int main() {
  avionix::application app;

  int count = 0;
  int ticks = 0;

  avionix::column root;
  root.add(avionix::constraint::fixed(1),
           avionix::text{"Counter",
                         {.foreground = avionix::colors::bright_cyan,
                          .add = avionix::attribute::bold}});
  auto& value = root.add(avionix::constraint::fixed(1), avionix::text{});
  auto& clock = root.add(avionix::constraint::fixed(1), avionix::text{});
  root.add(avionix::constraint::fill(), avionix::spacer{});
  root.add(avionix::constraint::fixed(1),
           avionix::text{"+/- change  r reset  q quit",
                         {.foreground = avionix::colors::bright_black}});

  const auto refresh = [&] {
    value.set(std::format("count: {}", count));
    clock.set(std::format("worker ticks: {}", ticks));
  };
  refresh();

  app.on_event([&](const avionix::event& e) {
    const auto* key = avionix::as<avionix::key_event>(e);
    if (key == nullptr) return false;
    if (key->is(U'+') || key->is(U'=')) {
      ++count;
    } else if (key->is(U'-')) {
      --count;
    } else if (key->is(U'r')) {
      count = 0;
    } else if (key->is(U'q')) {
      app.quit();
    } else {
      return false;
    }
    refresh();
    return true;
  });

  std::jthread worker{[&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::seconds{1});
      app.post([&] {
        ++ticks;
        refresh();
      });
    }
  }};

  auto result = app.run(root);
  worker.request_stop();
  if (!result) {
    std::println(stderr, "counter: {}", result.error().message());
    return 1;
  }
  std::println("final count: {}", count);
  return 0;
}
