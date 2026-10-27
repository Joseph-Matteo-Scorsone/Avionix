// Wallpaper with overlaid text and a click-and-drag brightness slider.
// Usage: background [image]. b toggles the image; q quits.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

import avionix;

namespace {
using namespace avionix;

class wallpaper_controls final : public component {
 public:
  explicit wallpaper_controls(backdrop& wall) : wall_{wall} {}
  bool focusable() const noexcept override { return true; }

  void render(render_context& context) override {
    // Pixel protocols other than Kitty cannot put an image beneath text.
    // Draw the same wallpaper in cells there, then overlay ordinary text.
    if (context.graphics().protocol != graphics_protocol::kitty) {
      const auto version = wall_.version();
      if (version != cached_version_ || extent_ != context.extent()) {
        preview_.set_image(wall_.pixels_behind(
            context.area(), {context.width() * 4, context.height() * 8}));
        cached_version_ = version;
        extent_ = context.extent();
      }
      frame_state frame;
      frame.graphics.colors = context.graphics().colors;
      render_context cells{context.buffer(), context.area(), frame};
      preview_.render_in(cells);
    }
    const auto text_over = [&](position at, std::string_view text) {
      // All labels are ASCII; keep each cell's wallpaper background color.
      for (const char ch : text) {
        const position absolute = at + context.area().origin;
        if (context.clip().contains(absolute)) {
          auto appearance = context.buffer().at(absolute).appearance();
          appearance.foreground = colors::white;
          context.draw_text_styled(at, std::string_view{&ch, 1}, appearance);
        }
        ++at.x;
      }
    };
    text_over({2, 1}, "Avionix wallpaper example");
    text_over({2, 3}, "Text over the background image");
    text_over({2, 4}, "Click or drag the brightness bar below.");
    text_over({2, 6}, std::format("Brightness: {:.0f}%", (1.0 - wall_.dim()) * 100));
    const auto width =
        std::min<std::uint32_t>(36, context.width() > 4 ? context.width() - 4 : 0);
    track_ =
        width >= 2 && context.height() > 7
            ? rect{{context.area().left() + 2, context.area().top() + 7}, {width, 1}}
            : rect{};
    if (!track_.empty()) {
      std::string bar(width, '-');
      const auto knob =
          static_cast<std::size_t>(std::lround((1.0 - wall_.dim()) * (width - 1)));
      bar[knob] = 'O';
      text_over({2, 7}, bar);
    }
    if (context.height() > 10)
      text_over({2, static_cast<std::int32_t>(context.height()) - 2},
                "b image on/off   q quit");
  }

  event_result on_event(const event& e, event_context& context) override {
    const auto* mouse = std::get_if<mouse_event>(&e);
    if (!mouse) return event_result::ignored;
    if (mouse->action == mouse_action::press && mouse->button == mouse_button::left &&
        track_.contains(mouse->where)) {
      dragging_ = true;
      context.capture_pointer();
      context.request_focus(*this);
    } else if (!dragging_) {
      return event_result::ignored;
    }
    if (mouse->action == mouse_action::press || mouse->action == mouse_action::drag ||
        mouse->action == mouse_action::move || mouse->action == mouse_action::release) {
      if (track_.width() >= 2) {
        const double brightness = std::clamp(
            static_cast<double>(mouse->where.x - track_.left()) / (track_.width() - 1),
            0.0, 1.0);
        wall_.set_dim(1.0 - brightness);
      }
      if (mouse->action == mouse_action::release) dragging_ = false;
      return event_result::handled;
    }
    return event_result::ignored;
  }

 private:
  backdrop& wall_;
  image_view preview_;
  rect track_{};
  bool dragging_{};
  std::uint64_t cached_version_{};
  avionix::size extent_{};
};
}  // namespace

int main(int argc, char** argv) {
  using namespace avionix;
  const bool self_test = argc > 1 && std::string_view{argv[1]} == "--self-test";
  const std::filesystem::path path =
      argc > 1 && !self_test ? argv[1] : "assets/QuantHood.jpg";
  auto image = load_image(path);
  if (!image) {
    std::println(stderr, "background: {}", image.error().message());
    return 1;
  }
  backdrop wall{*image};
  auto& controls = wall.set_child(wallpaper_controls{wall});
  if (self_test) {
    render_buffer buffer{{80, 24}};
    frame_state frame;
    render_context render{buffer, buffer.area(), frame};
    wall.render_in(render);
    bool ok = buffer.find("Text over the background image").has_value();
    const auto send = [&](mouse_action action, position where) {
      event_context context;
      const auto handled =
          controls.on_event(mouse_event{mouse_button::left, action, where}, context);
      return handled == event_result::handled &&
             (action != mouse_action::press || context.pointer_captured());
    };
    ok = send(mouse_action::press, {2, 7}) && wall.dim() == 1.0 && ok;
    ok = send(mouse_action::drag, {100, 7}) && wall.dim() == 0.0 && ok;
    ok = send(mouse_action::release, {37, 7}) && wall.dim() == 0.0 && ok;
    event_context idle;
    ok = controls.on_event(mouse_event{mouse_button::left, mouse_action::drag, {2, 7}},
                           idle) == event_result::ignored &&
         ok;
    wall.render_in(render);
    ok = buffer.find("Brightness: 100%").has_value() && ok;
    std::println("background overlay and slider self-test: {}",
                 ok ? "passed" : "FAILED");
    return ok ? 0 : 1;
  }
  application app{{.mouse_motion = true}};
  bool enabled = true;
  app.on_event([&](const event& e) {
    const auto* k = std::get_if<key_event>(&e);
    if (!k || k->mods != modifiers::none) return false;
    if (k->is(U'q'))
      app.quit();
    else if (k->is(U'b')) {
      enabled = !enabled;
      wall.set_image(enabled ? *image : image_rgba8{});
    } else
      return false;
    return true;
  });
  if (auto result = app.run(wall); !result) {
    std::println(stderr, "background: {}", result.error().message());
    return 1;
  }
  return 0;
}
