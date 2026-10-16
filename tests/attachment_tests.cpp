#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <variant>
#include <vector>
import avionix;
import avionix.object.event_queue;
import avionix.object.input_decoder;
import avionix_tests.check;
using namespace avionix;
using avionix_tests::check;
namespace {
struct paste_observer final : component {
  int calls{};
  bool focusable() const noexcept override { return true; }
  void render(render_context&) override {}
  event_result on_event(const event& e, event_context&) override {
    if (holds<paste_event>(e)) ++calls;
    return event_result::ignored;
  }
};
std::vector<std::byte> dib() {
  std::vector<std::byte> b(48);
  b[0] = std::byte{40};
  b[4] = std::byte{2};
  b[8] = std::byte{1};
  b[12] = std::byte{1};
  b[14] = std::byte{24};
  b[40] = std::byte{10};
  b[42] = std::byte{20};
  return b;
}
const avionix_tests::suite attachments{
    "attachments",
    {{"unhandled_paste_bubbles_once",
      [] {
        column root;
        auto& child = root.add(constraint::fill(), paste_observer{});
        application app;
        int early = 0, late = 0;
        app.on_event(
            [&](const event&) {
              ++early;
              return false;
            },
            true);
        app.on_event([&](const event&) {
          ++late;
          return false;
        });
        app.simulate(root, paste_event{"text"});
        check(child.calls == 1 && early == 1 && late == 1);
      }},
     {"paste_consumed_or_delivered_once",
      [] {
        text_area area;
        application app;
        int early = 0, late = 0;
        bool consume = true;
        app.on_event(
            [&](const event& e) {
              if (holds<paste_event>(e)) ++early;
              return consume;
            },
            true);
        app.on_event([&](const event&) {
          ++late;
          return false;
        });
        app.simulate(area, paste_event{"abc"});
        check(area.value().empty());
        consume = false;
        app.simulate(area, paste_event{"abc"});
        check(area.value() == "abc");
        check(early == 2 && late == 0);
        app.simulate(area, key_event{key::character, U'x'});
        check(area.value() == "abcx");
        app.simulate(area, key_event{key::left, 0, modifiers::shift});
        consume = true;
        app.simulate(area, paste_event{"ignored"});
        check(area.value() == "abcx");
        consume = false;
        app.simulate(area, paste_event{"Y"});
        check(area.value() == "abcY");
      }},
     {"owned_unicode_multiple_paths",
      [] {
        event_queue queue;
        file_drop_event source{{"C:/outside/\xe7\x8c\xab.png", "/tmp/screen shot.png"}};
        queue.push(event{source});
        source.paths[0] = "changed";
        std::vector<queue_entry> entries;
        queue.try_drain(entries);
        const auto& drop = std::get<file_drop_event>(std::get<event>(entries[0]));
        check(drop.paths.size() == 2 && drop.paths[0] == "C:/outside/\xe7\x8c\xab.png");
        check(drop.paths[1] == "/tmp/screen shot.png");
      }},
     {"path_text_stays_paste",
      [] {
        input_decoder decoder;
        std::vector<event> events;
        std::string input = "\x1b[200~/outside/\xe7\x8c\xab.png\x1b[201~";
        decoder.feed(input, events);
        input.assign("changed");
        check(events.size() == 1);
        check(as<paste_event>(events[0])->text == "/outside/\xe7\x8c\xab.png");
      }},
     {"dib_pixels_and_ownership",
      [] {
        auto bytes = dib();
        auto result = decode_clipboard_dib(bytes);
        check(result.has_value());
        bytes.clear();
        check(result->width == 2 && result->height == 1 && result->stride == 8);
        check(result->rgba8[0] == std::byte{20} && result->rgba8[2] == std::byte{10});
        check(result->rgba8[3] == std::byte{255});
      }},
     {"malformed_and_limits",
      [] {
        auto bytes = dib();
        check(decode_clipboard_dib(bytes, {47, 100}).error() ==
              clipboard_error::allocation_limit);
        check(decode_clipboard_dib(bytes, {1024, 1}).error() ==
              clipboard_error::allocation_limit);
        bytes.resize(47);
        check(decode_clipboard_dib(bytes).error() == clipboard_error::malformed_data);
        bytes = dib();
        bytes[8] = std::byte{0};
        check(decode_clipboard_dib(bytes).error() == clipboard_error::malformed_data);
        bytes = dib();
        bytes[16] = std::byte{1};
        check(decode_clipboard_dib(bytes).error() ==
              clipboard_error::unsupported_format);
        bytes = dib();
        bytes[7] = std::byte{127};
        check(decode_clipboard_dib(bytes).error() == clipboard_error::allocation_limit);
        bytes = dib();
        bytes[32] = std::byte{255};
        check(decode_clipboard_dib(bytes).error() == clipboard_error::malformed_data);
      }},
     {"dib_orientation_stride_and_alpha",
      [] {
        auto bytes = dib();
        bytes.resize(56);
        bytes[8] = std::byte{2};
        bytes[48] = std::byte{30};
        auto bottom = decode_clipboard_dib(bytes);
        check(bottom && bottom->rgba8[2] == std::byte{30} &&
              bottom->rgba8[10] == std::byte{10});
        bytes[8] = std::byte{254};
        bytes[9] = std::byte{255};
        bytes[10] = std::byte{255};
        bytes[11] = std::byte{255};
        auto top = decode_clipboard_dib(bytes);
        check(top && top->rgba8[2] == std::byte{10} && top->rgba8[10] == std::byte{30});
        bytes = dib();
        bytes[14] = std::byte{32};
        bytes[43] = std::byte{7};
        auto rgba = decode_clipboard_dib(bytes);
        check(rgba && rgba->rgba8[3] == std::byte{255});
        bytes[8] = std::byte{0};
        bytes[11] = std::byte{128};
        check(decode_clipboard_dib(bytes).error() == clipboard_error::malformed_data);
      }},
     {"backend_capabilities", [] {
        check(!attachment_capabilities().native_file_drop);
#if !defined(_WIN32)
        check(!attachment_capabilities().clipboard_image);
        check(request_clipboard_image().get().error() == clipboard_error::unsupported);
#else
    check(attachment_capabilities().clipboard_image);
#endif
      }}}};
}  // namespace
