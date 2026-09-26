// The smallest Avionix program: a bordered, centered greeting.
// Press q or Ctrl+C to quit.

#include <print>

import avionix;

int main() {
  avionix::application app;

  avionix::block frame{"Avionix", avionix::border_kind::rounded};
  auto& greeting =
      frame.set_child(avionix::text{"Hello from Avionix.\nPress q to quit."});
  greeting.align(avionix::alignment::center);

  app.on_event([&](const avionix::event& e) {
    if (const auto* key = avionix::as<avionix::key_event>(e); key && key->is(U'q')) {
      app.quit();
      return true;
    }
    return false;
  });

  if (auto result = app.run(frame); !result) {
    std::println(stderr, "hello: {}", result.error().message());
    return 1;
  }
  return 0;
}
