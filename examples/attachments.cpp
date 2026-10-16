#include <chrono>
#include <format>
#include <future>
#include <optional>
#include <print>
#include <string>
#include <vector>
import avionix;

const char* clipboard_message(avionix::clipboard_error error) {
  using avionix::clipboard_error;
  switch (error) {
    case clipboard_error::unsupported:
      return "Clipboard images unavailable on this backend";
    case clipboard_error::no_image:
      return "No image on clipboard";
    case clipboard_error::platform_failure:
      return "Clipboard access failed; retry F2";
    case clipboard_error::malformed_data:
      return "Clipboard image data is malformed";
    case clipboard_error::unsupported_format:
      return "Clipboard image format is unsupported";
    case clipboard_error::allocation_limit:
      return "Clipboard image exceeds allocation limits";
  }
  return "Clipboard retrieval failed";
}

int main() {
  using namespace avionix;
  // Declare pending before app so any outstanding worker joins after UI teardown.
  std::optional<std::future<clipboard_image_result>> pending;
  std::vector<clipboard_image> images;
  std::vector<std::string> paths;
  application app;
  column root;
  auto& status =
      root.add(constraint::fixed(1),
               text{"Native file drops unavailable; terminal path text stays text"});
  auto& metadata = root.add(constraint::fixed(3), text{});
  root.add(constraint::fixed(2),
           text{"F2: request clipboard image | F3: remove last | F4: quit\n"
                "Paste inserts text. Paste /consume to demonstrate interception."});
  root.add(constraint::fill(), text_area{});
  const auto refresh = [&] {
    std::string value =
        std::format("{} images, {} native paths", images.size(), paths.size());
    if (!images.empty()) {
      const auto& i = images.back();
      value += std::format("\nLast image: {}x{}, RGBA8, {} bytes", i.width, i.height,
                           i.rgba8.size());
    }
    if (!paths.empty()) value += "\nLast path: " + paths.back();
    metadata.set(value);
  };
  refresh();
  app.on_event(
      [&](const event& e) {
        if (const auto* paste = as<paste_event>(e)) {
          status.set(std::format("Intercepted {} bytes of text", paste->text.size()));
          return paste->text == "/consume";
        }
        if (const auto* drop = as<file_drop_event>(e)) {
          paths.insert(paths.end(), drop->paths.begin(), drop->paths.end());
          refresh();
          return true;
        }
        const auto* k = as<key_event>(e);
        if (!k) return false;
        if (k->is(key::f2)) {
          if (!attachment_capabilities().clipboard_image)
            status.set("Clipboard images unavailable on this backend");
          else if (!pending) {
            try {
              pending.emplace(request_clipboard_image());
              status.set("Reading clipboard on worker");
            } catch (const std::exception& ex) {
              status.set(ex.what());
            }
          }
        } else if (k->is(key::f3)) {
          if (!paths.empty())
            paths.pop_back();
          else if (!images.empty())
            images.pop_back();
          refresh();
        } else if (k->is(key::f4))
          app.quit();
        else
          return false;
        return true;
      },
      true);
  auto timer = app.set_interval(std::chrono::milliseconds{30}, [&] {
    if (!pending ||
        pending->wait_for(std::chrono::seconds{0}) != std::future_status::ready)
      return;
    try {
      auto result = pending->get();
      if (result) {
        images.push_back(std::move(*result));
        status.set("Image received");
      } else {
        status.set(clipboard_message(result.error()));
      }
    } catch (const std::exception& ex) {
      status.set(ex.what());
    }
    pending.reset();
    refresh();
  });
  auto result = app.run(root);
  if (!result) {
    std::println(stderr, "{}", result.error().message());
    return 1;
  }
}
