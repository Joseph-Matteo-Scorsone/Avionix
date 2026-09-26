// A multi-panel layout: selectable list, progress bars updated by a worker,
// a text input, and a status line. Tab moves focus between the list and the
// input; the mouse selects list items; resizing the terminal relays out.
//
//   ┌ header ─────────────────────────────┐
//   │ list │ progress bars                │
//   │      │ log                          │
//   │      │ input                        │
//   └ status ─────────────────────────────┘

#include <chrono>
#include <cstddef>
#include <format>
#include <print>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

import avionix;

int main() {
  using namespace avionix;
  application app;

  column root;
  root.add(constraint::fixed(1), text{" Avionix dashboard",
                                      {.foreground = colors::black,
                                       .background = colors::cyan,
                                       .add = attribute::bold}});

  auto& body = root.add(constraint::fill(), row{1});

  auto& tasks_panel = body.add(constraint::percentage(30), block{"Tasks"});
  auto& tasks =
      tasks_panel.set_child(list_view{{"render", "layout", "input", "unicode", "diff",
                                       "encode", "dispatch", "resize"}});

  auto& right = body.add(constraint::fill(), column{});
  auto& progress_panel = right.add(constraint::fixed(6), block{"Progress"});
  auto& bars = progress_panel.set_child(column{});
  std::vector<progress_bar*> progress;
  for (int i = 0; i < 4; ++i) {
    progress.push_back(&bars.add(constraint::fixed(1), progress_bar{0.0}));
  }

  auto& log_panel = right.add(constraint::fill(), block{"Log"});
  auto& log = log_panel.set_child(text{});
  log.wrap(true);
  std::vector<std::string> lines;
  const auto append_log = [&](std::string line) {
    lines.push_back(std::move(line));
    if (lines.size() > 50) lines.erase(lines.begin());
    std::string joined;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
      joined += *it;
      joined += '\n';
    }
    log.set(std::move(joined));
  };

  auto& input_panel = right.add(constraint::fixed(3), block{"Command"});
  auto& input = input_panel.set_child(text_input{"type and press Enter"});

  auto& status = root.add(constraint::fixed(1), text{});
  const auto update_status = [&] {
    const size s = app.screen_size();
    status.set(
        std::format(" {}x{}  Tab focus  Enter select  Ctrl+C quit", s.width, s.height));
    status.set_style({.foreground = colors::bright_black});
  };

  tasks.on_activate([&](std::size_t index, const std::string& name) {
    append_log(std::format("selected task {} ({})", index, name));
  });
  input.on_submit([&](const std::string& command) {
    if (command == "quit") {
      app.quit();
      return;
    }
    append_log(std::format("> {}", command));
    input.set_value("");
  });
  app.on_event([&](const event& e) {
    if (holds<resize_event>(e)) update_status();
    return false;
  });
  update_status();
  append_log("started; type 'quit' to exit");

  std::jthread worker{[&](std::stop_token stop) {
    std::vector<double> values(progress.size(), 0.0);
    std::size_t step = 0;
    while (!stop.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
      ++step;
      for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] += 0.005 * static_cast<double>(i + 1);
        if (values[i] > 1.0) values[i] = 0.0;
      }
      // Copy the values into the callback; the worker never touches
      // the widgets itself.
      app.post([&, values, step] {
        for (std::size_t i = 0; i < values.size(); ++i) progress[i]->set(values[i]);
        if (step % 50 == 0) append_log(std::format("worker heartbeat {}", step / 50));
      });
    }
  }};

  auto result = app.run(root);
  worker.request_stop();
  worker.join();
  if (!result) {
    std::println(stderr, "dashboard: {}", result.error().message());
    return 1;
  }
  return 0;
}
