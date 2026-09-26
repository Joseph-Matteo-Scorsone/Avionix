module;

#include <concepts>
#include <cstddef>
#include <functional>
#include <variant>
#include <vector>

export module avionix.task.dispatch;

import avionix.entity.event;
import avionix.object.event_queue;

// Delivers drained queue entries on the UI thread:
//
//   event_queue::drain ──entries──► dispatch ──event──► handler
//                                       └──callback──► invoke
//
// dispatch owns nothing. It decides order and coalescing: only the newest
// resize in a batch is delivered, because older sizes are already stale and
// each resize forces a full repaint.

export namespace avionix {

template <typename... F>
struct overloaded : F... {
  using F::operator()...;
};

template <typename H>
concept event_handler = requires(H& handler, const event& value) { handler(value); };

struct dispatch_result {
  std::size_t events{};
  std::size_t callbacks{};
  std::size_t coalesced{};
};

template <event_handler H>
dispatch_result dispatch(std::vector<queue_entry>& entries, H& handler) {
  dispatch_result result;

  std::size_t last_resize = entries.size();
  for (std::size_t i = entries.size(); i-- > 0;) {
    const auto* e = std::get_if<event>(&entries[i]);
    if (e != nullptr && std::holds_alternative<resize_event>(*e)) {
      last_resize = i;
      break;
    }
  }

  for (std::size_t i = 0; i < entries.size(); ++i) {
    std::visit(
        overloaded{
            [&](const event& value) {
              if (std::holds_alternative<resize_event>(value) && i != last_resize) {
                ++result.coalesced;
                return;
              }
              handler(value);
              ++result.events;
            },
            [&](ui_callback& callback) {
              if (callback) {
                std::invoke(callback);
              }
              ++result.callbacks;
            },
        },
        entries[i]);
  }
  entries.clear();
  return result;
}

}  // namespace avionix
