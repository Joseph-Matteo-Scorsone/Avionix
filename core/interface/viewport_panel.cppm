module;

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

export module avionix.interface.viewport_panel;

import avionix.entity.geometry;
import avionix.entity.event;
import avionix.entity.terminal_graphics;
import avionix.entity.viewport;
import avionix.interface.widget;
import avionix.interface.image;
import avionix.interface.viewport;

// The 3D viewport as a section of the terminal UI.
//
//   viewport ──render_view(panel pixels)──► image_view ──► Kitty / Sixel /
//      ▲                                                   iTerm2 / half blocks
//      └── terminal mouse (cells → surface pixels) and keys ── viewport_panel
//
// The panel borrows the viewport; the viewport must outlive it. It works on
// every platform and terminal: pixel protocols where the terminal has one,
// half-block characters everywhere else, OpenGL where a context can be
// created, and the CPU renderer otherwise.
//
// Surface: with the companion window closed, the panel is the viewport's
// drawing surface, so picks and handles use the panel's pixels. With the
// window open, the panel mirrors the window's view at the window's aspect
// ratio (letterboxed) and maps its input into window pixels, so both stay
// interactive.
//
// Pointer precision is one terminal cell. Hover needs
// application_options::mouse_motion.

export namespace avionix {

class viewport_panel final : public component {
 public:
  explicit viewport_panel(viewport& view) : view_{&view} {}

  [[nodiscard]] bool focusable() const noexcept override { return true; }

  // How the last frame was shown: a pixel protocol, or none for half
  // blocks.
  [[nodiscard]] graphics_protocol shown_as() const noexcept {
    return image_.shown_as();
  }
  // Pixel size of the last rendered picture.
  [[nodiscard]] pixel_extent picture_size() const noexcept { return picture_; }
  // The last picture handed to the terminal, after any compositing.
  [[nodiscard]] const image_rgba8& picture() const noexcept { return image_.image(); }

  // Shows `wallpaper` behind the model when the viewport's background is
  // transparent. Where the terminal layers the wallpaper itself (Kitty) the
  // picture stays transparent; elsewhere the panel composites the matching
  // part of the wallpaper into its picture. Borrowed; nullptr removes it.
  void set_backdrop(const backdrop* wallpaper) noexcept {
    backdrop_ = wallpaper;
    drawn_backdrop_ = ~std::uint64_t{0};
  }

  void render(render_context& context) override {
    const rect area = context.area();
    area_ = area;
    if (area.empty()) return;
    const graphics_support& support = context.graphics();
    const bool last_row =
        area.bottom() >= static_cast<std::int32_t>(context.buffer().height());
    const image_view::target t = image_view::target_for(support, area.extent, last_row);
    rows_ = t.rows;
    // Half blocks are box filtered from four samples per output pixel.
    pixel_extent room = t.pixels ? pixel_extent{t.width, t.height}
                                 : pixel_extent{area.width() * 4, area.height() * 8};
    pixel_extent size = room;
    if (view_->is_open()) {
      // Mirror the window at its aspect ratio.
      const double aspect = view_->extent().aspect();
      const double fit_w = std::min<double>(room.width, room.height * aspect);
      size = {std::max<std::uint32_t>(1, static_cast<std::uint32_t>(fit_w)),
              std::max<std::uint32_t>(1, static_cast<std::uint32_t>(fit_w / aspect))};
    } else {
      // Logical pixels assume a 16 px tall cell.
      const double cell_h =
          static_cast<double>(room.height) / std::max<std::uint32_t>(1, t.rows);
      view_->set_surface(size, std::max(0.25, cell_h / 16.0));
    }
    room_ = room;
    // Composite only where the terminal cannot put the wallpaper under the
    // picture, and only when the picture's pixels line up with the cells
    // (not when letterboxing a window's aspect ratio).
    const bool composite = backdrop_ != nullptr && view_->transparent_background() &&
                           !backdrop_->under_text() && !view_->is_open();
    const std::uint64_t backdrop_state = composite ? backdrop_->version() : 0;
    if (size != picture_ || view_->changes() != drawn_changes_ ||
        view_->is_open() != drawn_open_ || backdrop_state != drawn_backdrop_) {
      if (auto image = view_->render_view(size)) {
        if (composite) over_backdrop(*image, {area.origin, {area.width(), t.rows}});
        image_.set_image(std::move(*image));
        picture_ = size;
        drawn_changes_ = view_->changes();
        drawn_open_ = view_->is_open();
        drawn_backdrop_ = backdrop_state;
      }
    }
    image_.render(context);
  }

  event_result on_event(const event& value, event_context& context) override {
    if (const auto* m = as<mouse_event>(value)) return mouse(*m, context);
    if (const auto* k = as<key_event>(value)) {
      if (!view_->wants_key(*k)) return event_result::ignored;
      view_->simulate(*k);
      return event_result::handled;
    }
    return event_result::ignored;
  }

 private:
  // Straight-alpha "over" onto the wallpaper pixels behind `cells`.
  void over_backdrop(image_rgba8& image, rect cells) const {
    const image_rgba8 under =
        backdrop_->pixels_behind(cells, {image.width, image.height});
    if (under.pixels.size() != image.pixels.size()) return;
    for (std::size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
      const std::uint32_t a = image.pixels[i + 3];
      for (std::size_t c = 0; c < 3; ++c)
        image.pixels[i + c] = static_cast<std::uint8_t>(
            (image.pixels[i + c] * a + under.pixels[i + c] * (255U - a) + 127U) / 255U);
      image.pixels[i + 3] = 255;
    }
  }

  // Terminal cell to surface pixel (the cell's center). The picture is
  // fitted into `room_` preserving aspect, so undo that fit first.
  [[nodiscard]] std::optional<vec2> to_surface(position where) const {
    if (area_.empty() || picture_.empty() || room_.empty()) return std::nullopt;
    const double cols = area_.width();
    const double rows = std::max<std::uint32_t>(1, rows_);
    const double rx = (where.x - area_.left() + 0.5) / cols * room_.width;
    const double ry = (where.y - area_.top() + 0.5) / rows * room_.height;
    const double scale = std::min(static_cast<double>(room_.width) / picture_.width,
                                  static_cast<double>(room_.height) / picture_.height);
    const double ox = (room_.width - picture_.width * scale) * 0.5;
    const double oy = (room_.height - picture_.height * scale) * 0.5;
    const double px = (rx - ox) / scale;
    const double py = (ry - oy) / scale;
    // Picture pixels to surface pixels (they differ when mirroring a window).
    const pixel_extent surface = view_->extent();
    return vec2{px * surface.width / picture_.width,
                py * surface.height / picture_.height};
  }

  event_result mouse(const mouse_event& m, event_context& context) {
    const auto at = to_surface(m.where);
    if (!at) return event_result::ignored;
    const auto button = [](mouse_button b) -> std::optional<pointer_button> {
      switch (b) {
        case mouse_button::left:
          return pointer_button::left;
        case mouse_button::middle:
          return pointer_button::middle;
        case mouse_button::right:
          return pointer_button::right;
        default:
          return std::nullopt;
      }
    };
    switch (m.action) {
      case mouse_action::press:
        if (m.button == mouse_button::wheel_up ||
            m.button == mouse_button::wheel_down) {
          view_->simulate(wheel_scrolled{
              *at, {0.0, m.button == mouse_button::wheel_up ? 1.0 : -1.0}, m.mods});
          return event_result::handled;
        }
        if (const auto b = button(m.button)) {
          context.request_focus(*this);
          context.capture_pointer();
          down_ = *b;
          view_->simulate(pointer_moved{*at, m.mods});
          view_->simulate(pointer_pressed{*at, *b, m.mods});
          return event_result::handled;
        }
        return event_result::ignored;
      case mouse_action::drag:
      case mouse_action::move:
        view_->simulate(pointer_moved{*at, m.mods});
        return event_result::handled;
      case mouse_action::release: {
        // Terminals often report releases without the button.
        const pointer_button b = button(m.button).value_or(down_);
        view_->simulate(pointer_released{*at, b, m.mods});
        return event_result::handled;
      }
    }
    return event_result::ignored;
  }

  viewport* view_;
  image_view image_{};
  rect area_{};
  pixel_extent room_{};
  pixel_extent picture_{};
  std::uint32_t rows_{};
  std::uint64_t drawn_changes_{};
  bool drawn_open_{};
  const backdrop* backdrop_{};
  std::uint64_t drawn_backdrop_{};
  pointer_button down_{pointer_button::left};
};

}  // namespace avionix
