module;

#include <algorithm>
#include <cstdint>

export module avionix.task.resize;

import avionix.entity.geometry;
import avionix.object.renderer;

// Applies a terminal size change to the render state. The renderer resizes
// both buffers and schedules a full repaint; widgets see the new size on the
// next draw because layout is recomputed from the buffer extent every frame.

export namespace avionix {

// Terminals can report 0x0 while a window is minimized or being created.
// Rendering needs at least one cell, so sizes are clamped.
[[nodiscard]] constexpr size clamp_terminal_size(size requested) noexcept {
  return {std::max<std::uint32_t>(requested.width, 1),
          std::max<std::uint32_t>(requested.height, 1)};
}

// Returns true when the renderer changed size.
bool apply_resize(renderer& target, size requested) {
  const size next = clamp_terminal_size(requested);
  if (next == target.extent()) {
    return false;
  }
  target.resize(next);
  return true;
}

}  // namespace avionix
