module;

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module avionix.interface.image;

import avionix.entity.geometry;
import avionix.entity.color;
import avionix.entity.style;
import avionix.entity.terminal_graphics;
import avionix.interface.widget;

// A pixel image in a cell area.
//
//   image_rgba8 ──render──► graphics_support says Kitty/Sixel/iTerm2?
//        │                     yes: fit to the area's pixels, encode once,
//        │                          register a placement (renderer writes it)
//        │                     no:  quadrant characters, 2x2 samples and two
//        │                          colors per cell, through the ordinary diff
//        │                          renderer (a brightness ramp without color)
//
// The widget owns its image. Encoded payloads are cached until the image,
// the area, or the terminal's graphics support changes.
//
// Sixel and iTerm2 images move the cursor below themselves; an image on the
// screen's last row would scroll the screen, so on that row the image stops
// one row short and the last row stays blank.

export namespace avionix {

// Kitty image ids and renderer placement ids, unique per process.
[[nodiscard]] inline std::uint32_t next_image_id() noexcept {
  static std::atomic<std::uint32_t> counter{0};
  return ++counter;
}

class image_view final : public component {
 public:
  image_view() : id_{next_image_id()} {}
  explicit image_view(image_rgba8 image)
      : id_{next_image_id()},
        image_{std::move(image)},
        transparent_{has_transparency(image_)} {}

  // Replaces the image. Previously encoded output is discarded.
  void set_image(image_rgba8 image) {
    image_ = std::move(image);
    ++version_;
    transparent_ = has_transparency(image_);
  }
  [[nodiscard]] const image_rgba8& image() const noexcept { return image_; }

  // How the last render showed the image: a pixel protocol, or
  // graphics_protocol::none for half blocks.
  [[nodiscard]] graphics_protocol shown_as() const noexcept { return shown_as_; }

  // The pixel size an image needs to fill `cells` exactly under `support`,
  // so producers (a 3D view) can render at the right resolution. With no
  // pixel protocol, 4x8 samples per cell (the quadrant fit's input).
  // `last_row` is true when the area touches the screen's last row.
  struct target {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t rows{};  // cell rows the image covers
    bool pixels{};         // false: half blocks
  };
  [[nodiscard]] static target target_for(const graphics_support& support, size cells,
                                         bool last_row) noexcept {
    std::uint32_t rows = cells.height;
    const bool scrolls = support.protocol == graphics_protocol::sixel ||
                         support.protocol == graphics_protocol::iterm2;
    if (scrolls && last_row && rows > 0) --rows;
    if (!support.images() || rows == 0 || cells.width == 0) {
      return {cells.width * 4, cells.height * 8, cells.height, false};
    }
    const cell_pixels c = support.effective_cell();
    return {cells.width * c.width, rows * c.height, rows, true};
  }

  void render(render_context& context) override {
    const rect area = context.area();
    if (area.empty()) return;
    context.fill(bounds(area.extent));
    const graphics_support& support = context.graphics();
    const bool last_row =
        area.bottom() >= static_cast<std::int32_t>(context.buffer().height());
    const target t = target_for(support, area.extent, last_row);
    shown_as_ = graphics_protocol::none;
    if (t.pixels && !image_.pixels.empty()) {
      if (payload_version_ != version_ || payload_area_ != area ||
          payload_support_ != support) {
        payload_.clear();
        const image_rgba8 fitted = fit_image(image_, t.width, t.height);
        switch (support.protocol) {
          case graphics_protocol::kitty:
            append_kitty(payload_, fitted, id_, area.width(), t.rows);
            break;
          case graphics_protocol::sixel:
            append_sixel(payload_, fitted);
            break;
          case graphics_protocol::iterm2:
            append_iterm2(payload_, fitted, area.width(), t.rows);
            break;
          case graphics_protocol::none:
            break;
        }
        payload_version_ = version_;
        payload_area_ = area;
        payload_support_ = support;
        ++payload_serial_;
      }
      // Letterboxing also leaves transparent margins.
      const bool letterboxed = image_.width * t.height != image_.height * t.width;
      if (context.place_graphics(id_, payload_serial_, {{0, 0}, {area.width(), t.rows}},
                                 payload_, transparent_ || letterboxed)) {
        shown_as_ = support.protocol;
        return;
      }
    }
    draw_blocks(context, area.extent);
  }

 private:
  [[nodiscard]] static bool has_transparency(const image_rgba8& image) noexcept {
    for (std::size_t i = 3; i < image.pixels.size(); i += 4)
      if (image.pixels[i] < 255) return true;
    return false;
  }

  // Quadrant characters: each cell is split 2x2 and drawn with the best of
  // the 16 quadrant patterns and two colors (foreground for the set
  // quadrants, background for the rest). Half blocks (▀ ▄) are two of
  // those patterns, so this is never worse than half blocks and doubles
  // the horizontal detail of edges.
  void draw_blocks(render_context& context, size cells) {
    if (image_.pixels.empty()) return;
    if (blocks_version_ != version_ || blocks_extent_ != cells) {
      // Fit at the cell's real shape (twice as tall as wide), 4x8 samples
      // per cell, then average 2x4 samples per quadrant.
      const image_rgba8 big = fit_image(image_, cells.width * 4, cells.height * 8);
      blocks_ = {cells.width * 2, cells.height * 2, {}};
      blocks_.pixels.resize(std::size_t{blocks_.width} * blocks_.height * 4);
      for (std::uint32_t y = 0; y < blocks_.height; ++y) {
        for (std::uint32_t x = 0; x < blocks_.width; ++x) {
          const auto p = box_average(big, x * 2, y * 4, x * 2 + 2, y * 4 + 4);
          std::copy(
              p.begin(), p.end(),
              blocks_.pixels.begin() + static_cast<std::ptrdiff_t>(
                                           (std::size_t{y} * blocks_.width + x) * 4));
        }
      }
      blocks_version_ = version_;
      blocks_extent_ = cells;
    }
    const style base = context.base_style();
    const bool mono = context.graphics().colors == color_depth::monochrome;
    // Indexed by pattern bits: top-left 1, top-right 2, bottom-left 4,
    // bottom-right 8.
    static constexpr std::array<std::string_view, 16> glyphs{
        " ", "▘", "▝", "▀", "▖", "▌", "▞", "▛", "▗", "▚", "▐", "▜", "▄", "▙", "▟", "█"};
    constexpr std::string_view ramp = " .:-=+*#%@";
    for (std::uint32_t y = 0; y < cells.height; ++y) {
      for (std::uint32_t x = 0; x < cells.width; ++x) {
        std::array<std::array<std::uint8_t, 4>, 4> q{
            blocks_.at(x * 2, y * 2), blocks_.at(x * 2 + 1, y * 2),
            blocks_.at(x * 2, y * 2 + 1), blocks_.at(x * 2 + 1, y * 2 + 1)};
        std::uint32_t opaque = 0;
        for (std::size_t i = 0; i < 4; ++i)
          if (q[i][3] >= 128) opaque |= 1U << i;
        if (opaque == 0) continue;
        const position at{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
        if (mono) {
          // Without color, one brightness character per cell.
          double luma = 0.0;
          for (const auto& p : q)
            luma += (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) * (p[3] / 255.0);
          const auto level = static_cast<std::size_t>(
              std::lround(luma / (4.0 * 255.0) * (ramp.size() - 1)));
          context.draw_text_styled(at, ramp.substr(level, 1), base);
          continue;
        }
        // Transparent quadrants keep the cell background; only opaque
        // ones take part in the fit.
        std::uint32_t best_pattern = opaque;
        std::array<double, 3> best_fg{};
        std::array<double, 3> best_bg{};
        double best_error = -1.0;
        const bool partial = opaque != 15;
        // The full block comes first so uniform cells keep it on ties.
        static constexpr std::array<std::uint32_t, 15> order{
            15, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
        for (const std::uint32_t pattern : order) {
          if (partial && pattern != opaque) continue;
          std::array<double, 3> fg{};
          std::array<double, 3> bg{};
          double nf = 0.0;
          double nb = 0.0;
          for (std::size_t i = 0; i < 4; ++i) {
            auto& acc = (pattern >> i) & 1U ? fg : bg;
            ((pattern >> i) & 1U ? nf : nb) += 1.0;
            for (std::size_t c = 0; c < 3; ++c) acc[c] += q[i][c];
          }
          for (std::size_t c = 0; c < 3; ++c) {
            fg[c] /= nf;
            if (nb > 0.0) bg[c] /= nb;
          }
          double error = 0.0;
          for (std::size_t i = 0; i < 4; ++i) {
            const auto& m = (pattern >> i) & 1U ? fg : bg;
            for (std::size_t c = 0; c < 3; ++c)
              error += (q[i][c] - m[c]) * (q[i][c] - m[c]);
          }
          if (best_error < 0.0 || error < best_error - 1e-9) {
            best_error = error;
            best_pattern = pattern;
            best_fg = fg;
            best_bg = bg;
          }
        }
        const auto channel = [](double v) {
          return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
        };
        style s = base;
        s.foreground =
            color::rgb(channel(best_fg[0]), channel(best_fg[1]), channel(best_fg[2]));
        if (best_pattern != 15)
          s.background =
              color::rgb(channel(best_bg[0]), channel(best_bg[1]), channel(best_bg[2]));
        if (partial) s.background = base.background;
        context.draw_text_styled(at, glyphs[best_pattern], s);
      }
    }
  }

  std::uint32_t id_;
  image_rgba8 image_{};
  std::uint64_t version_{1};
  bool transparent_{};
  graphics_protocol shown_as_{graphics_protocol::none};

  std::string payload_;
  std::uint64_t payload_version_{};
  std::uint64_t payload_serial_{};
  rect payload_area_{};
  graphics_support payload_support_{};

  image_rgba8 blocks_{};
  std::uint64_t blocks_version_{};
  size blocks_extent_{};
};

// A wallpaper behind a child widget.
//
//   Kitty graphics (Kitty, Ghostty, WezTerm): the image is placed under the
//   text and under cells with a background color, so it shows wherever the
//   child leaves the terminal's default background, the way a terminal's
//   own background image does.
//
//   Sixel, iTerm2, and character cells: terminals cannot put pixels under
//   text there, so nothing is drawn behind text. Widgets that draw their own
//   pixels (viewport_panel with a transparent background) call
//   pixels_behind() and composite the wallpaper themselves. For a
//   full-window wallpaper in those terminals, configure the terminal's own
//   background image and keep the UI's backgrounds default.
//
// The image is scaled to cover the area (cropped, not letterboxed) and
// darkened by dim() so text stays readable.
class backdrop final : public component {
 public:
  backdrop() : id_{next_image_id()} {}
  explicit backdrop(image_rgba8 image)
      : id_{next_image_id()}, image_{std::move(image)} {}

  template <typename T>
    requires std::derived_from<std::remove_cvref_t<T>, component> ||
             widget<std::remove_cvref_t<T>>
  std::remove_cvref_t<T>& set_child(T&& child) {
    auto made = make_component(std::forward<T>(child));
    auto& ref = made.value;
    child_ = std::move(made.owner);
    return ref;
  }

  // Replaces the wallpaper; an empty image removes it.
  void set_image(image_rgba8 image) {
    image_ = std::move(image);
    ++version_;
  }
  [[nodiscard]] const image_rgba8& image() const noexcept { return image_; }

  // 0 shows the image as is; 1 makes it black.
  void set_dim(double dim) {
    dim_ = std::clamp(dim, 0.0, 1.0);
    ++version_;
  }
  [[nodiscard]] double dim() const noexcept { return dim_; }

  // Whether the last frame placed the image under the text (Kitty).
  [[nodiscard]] bool under_text() const noexcept { return under_text_; }
  // Changes whenever pixels_behind() would return different pixels.
  [[nodiscard]] std::uint64_t version() const noexcept {
    return version_ * 1'000'003ULL + static_cast<std::uint64_t>(area_.width()) * 4099U +
           area_.height();
  }

  // The wallpaper pixels behind `cells` (absolute cell coordinates), at
  // `size` pixels, as the full-area wallpaper would show them. Empty when
  // there is no image or the backdrop has not been drawn yet.
  [[nodiscard]] image_rgba8 pixels_behind(rect cells, pixel_extent size) const {
    image_rgba8 out;
    if (image_.pixels.empty() || area_.empty() || size.empty()) return out;
    out = {size.width, size.height,
           std::vector<std::uint8_t>(std::size_t{size.width} * size.height * 4)};
    // In cell widths: the area is cols x rows cells, each cell_aspect_
    // times taller than wide.
    const double full_w = area_.width();
    const double full_h = area_.height() * cell_aspect_;
    const double cx = cells.left() - area_.left();
    const double cy = (cells.top() - area_.top()) * cell_aspect_;
    const double sw = static_cast<double>(cells.width()) / size.width;
    const double sh = cells.height() * cell_aspect_ / size.height;
    const double keep = 1.0 - dim_;
    for (std::uint32_t y = 0; y < size.height; ++y) {
      for (std::uint32_t x = 0; x < size.width; ++x) {
        auto p = sample_cover(image_, full_w, full_h, cx + (x + 0.5) * sw,
                              cy + (y + 0.5) * sh);
        const std::size_t o = (std::size_t{y} * size.width + x) * 4;
        for (std::size_t c = 0; c < 3; ++c)
          out.pixels[o + c] = static_cast<std::uint8_t>(std::lround(p[c] * keep));
        out.pixels[o + 3] = 255;
      }
    }
    return out;
  }

  void render(render_context& context) override {
    area_ = context.area();
    under_text_ = false;
    const graphics_support& support = context.graphics();
    const cell_pixels shape = support.effective_cell();
    const double aspect = static_cast<double>(shape.height) / shape.width;
    if (aspect != cell_aspect_) {
      cell_aspect_ = aspect;
      ++version_;
    }
    if (support.protocol == graphics_protocol::kitty && support.images() &&
        !image_.pixels.empty() && !area_.empty()) {
      if (payload_version_ != version_ || payload_area_ != area_ ||
          payload_support_ != support) {
        const cell_pixels cell = support.effective_cell();
        const pixel_extent size{area_.width() * cell.width,
                                area_.height() * cell.height};
        payload_.clear();
        append_kitty(payload_, pixels_behind(area_, size), id_, area_.width(),
                     area_.height(), kitty_under_backgrounds);
        payload_version_ = version_;
        payload_area_ = area_;
        payload_support_ = support;
        ++payload_serial_;
      }
      under_text_ =
          context.place_graphics(id_, payload_serial_, bounds(area_.extent), payload_);
    }
    if (child_) child_->render_in(context);
  }

  void children(std::vector<component*>& out) override {
    if (child_) out.push_back(child_.get());
  }

 private:
  std::uint32_t id_;
  image_rgba8 image_{};
  std::unique_ptr<component> child_;
  double dim_{0.35};
  double cell_aspect_{2.0};
  std::uint64_t version_{1};
  rect area_{};
  bool under_text_{};
  std::string payload_;
  std::uint64_t payload_version_{};
  std::uint64_t payload_serial_{};
  rect payload_area_{};
  graphics_support payload_support_{};
};

}  // namespace avionix
