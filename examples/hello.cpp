// A greeting with application-controlled shortcut help.
// Press F1 for help, Esc to close it, or q to quit.

#include <print>
#include <utility>

import avionix;

int main() {
  avionix::application app;
  avionix::layer root;
  avionix::block frame{"Avionix", avionix::border_kind::rounded};
  auto& greeting = frame.set_child(
      avionix::text{"Hello from Avionix.\nPress F1 for help or q to quit."});
  greeting.align(avionix::alignment::center);
  root.add(std::move(frame));
  root.add(avionix::shortcut_help{{{"F1", "Show help"},
                                    {"Esc", "Close help"},
                                    {"q", "Quit"}}});
  bool help_visible = false;
  root.set_visible(1, help_visible);

  app.on_event([&](const avionix::event& e) {
    if (const auto* key = avionix::as<avionix::key_event>(e)) {
      if (key->is(avionix::key::f1)) {
        help_visible = true;
        root.set_visible(1, true);
        return true;
      }
      if (help_visible && key->is(avionix::key::escape)) {
        help_visible = false;
        root.set_visible(1, false);
        return true;
      }
      if (key->is(U'q')) {
        app.quit();
        return true;
      }
    }
    return false;
  });

  if (auto result = app.run(root); !result) {
    std::println(stderr, "hello: {}", result.error().message());
    return 1;
  }
  return 0;
}
