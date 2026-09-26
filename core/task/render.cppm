module;

#include <concepts>
#include <expected>
#include <functional>
#include <string_view>
#include <utility>

export module avionix.task.render;

import avionix.entity.error;
import avionix.object.buffer;
import avionix.object.renderer;

// One frame:
//
//   clear back buffer ──draw──► back buffer ──renderer::present──► bytes
//   bytes ──terminal_output::write──► terminal (one write per frame)
//
// The output is a concept so tests can capture frames without a terminal;
// avionix.object.terminal's terminal_session satisfies it.

export namespace avionix {

template <typename T>
concept terminal_output = requires(T& output, std::string_view bytes) {
  { output.write(bytes) } -> std::same_as<std::expected<void, error>>;
};

template <typename F>
concept frame_drawer = std::invocable<F&, render_buffer&>;

template <terminal_output Output, frame_drawer Draw>
std::expected<frame_statistics, error> render_frame(renderer& target, Output& output,
                                                    Draw&& draw) {
  render_buffer& back = target.back();
  back.clear();
  std::invoke(draw, back);
  const std::string_view bytes = target.present();
  if (!bytes.empty()) {
    if (auto written = output.write(bytes); !written) {
      // The physical screen now holds an unknown partial frame.
      target.invalidate();
      return std::unexpected(std::move(written.error()));
    }
  }
  return target.last_frame();
}

}  // namespace avionix
