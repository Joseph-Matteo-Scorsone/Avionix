module;

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module avionix.interface.equity_chart;

import avionix.entity.geometry;
import avionix.entity.color;
import avionix.entity.style;
import avionix.interface.widget;

export namespace avionix {

struct equity_point {
  std::chrono::sys_seconds time{};
  double value{};
};

// A monochrome terminal line chart. Times are UTC and spaced by elapsed
// time, not sample index. Non-finite values break the line. No graphics
// protocol or viewport is needed; each cell contains a 2x4 Braille raster.
class equity_chart final : public component {
 public:
  equity_chart() = default;
  explicit equity_chart(std::vector<equity_point> points) {
    set_points(std::move(points));
  }

  // Owns and stably sorts points by time; duplicate times draw vertical lines.
  void set_points(std::vector<equity_point> points) {
    std::stable_sort(points.begin(), points.end(),
                     [](const auto& a, const auto& b) { return a.time < b.time; });
    points_ = std::move(points);
  }
  [[nodiscard]] std::span<const equity_point> points() const noexcept {
    return points_;
  }

  void render(render_context& context) override {
    auto ink =
        context.with_style({.foreground = colors::white, .background = colors::black});
    ink.clear();
    if (ink.width() < 8 || ink.height() < 4) return;
    const equity_point* first = nullptr;
    const equity_point* last = nullptr;
    long double low = 0, high = 0;
    for (const auto& point : points_) {
      if (!std::isfinite(point.value)) continue;
      if (!first) first = &point;
      last = &point;
      low = std::min(low, static_cast<long double>(point.value));
      high = std::max(high, static_cast<long double>(point.value));
    }
    if (!first) {
      ink.draw_text({0, 0}, "No data");
      return;
    }
    if (low == high) {
      low = -1;
      high = 1;
    }
    const auto number = [](long double value) {
      const auto magnitude = std::abs(value);
      if (magnitude != 0 && (magnitude >= 1'000'000 || magnitude < 0.01L))
        return std::format("{:.1e}", value);
      return std::format("{:.2f}", value);
    };
    const auto top_label = number(high), bottom_label = number(low);
    const auto label_width =
        static_cast<std::uint32_t>(std::max(top_label.size(), bottom_label.size()));
    const auto left = label_width + 1;
    if (ink.width() <= left + 2) return;
    const auto cols = ink.width() - left;
    const auto rows = ink.height() - 2;
    const auto dot_w = static_cast<std::int32_t>(cols * 2);
    const auto dot_h = static_cast<std::int32_t>(rows * 4);
    dots_.assign(std::size_t{cols} * rows, 0);
    const auto time_value = [](auto time) {
      return static_cast<long double>(time.time_since_epoch().count());
    };
    const auto start = time_value(first->time);
    const auto duration = time_value(last->time) - start;
    const auto scale = std::max(std::abs(high), std::abs(low));
    const auto map = [&](const equity_point& p) -> position {
      const auto x = duration == 0
                         ? (dot_w - 1) / 2.0L
                         : (time_value(p.time) - start) / duration * (dot_w - 1);
      const auto y = (high / scale - static_cast<long double>(p.value) / scale) /
                     (high / scale - low / scale) * (dot_h - 1);
      return {static_cast<std::int32_t>(std::llround(x)),
              static_cast<std::int32_t>(std::llround(y))};
    };
    const auto dot = [&](position p) {
      if (p.x < 0 || p.y < 0 || p.x >= dot_w || p.y >= dot_h) return;
      static constexpr std::array<unsigned, 8> bits{0, 3, 1, 4, 2, 5, 6, 7};
      const auto cell = std::size_t{static_cast<std::uint32_t>(p.y / 4)} * cols +
                        static_cast<std::uint32_t>(p.x / 2);
      dots_[cell] |= static_cast<std::uint8_t>(
          1U << bits[static_cast<std::size_t>((p.y % 4) * 2 + p.x % 2)]);
    };
    const equity_point* previous = nullptr;
    for (const auto& point : points_) {
      if (!std::isfinite(point.value)) {
        previous = nullptr;
        continue;
      }
      auto p = map(point);
      if (previous) {
        auto from = map(*previous);
        const auto dx = std::abs(p.x - from.x), dy = -std::abs(p.y - from.y);
        const auto sx = from.x < p.x ? 1 : -1, sy = from.y < p.y ? 1 : -1;
        auto error = dx + dy;
        for (;;) {
          dot(from);
          if (from == p) break;
          const auto twice = 2 * error;
          if (twice >= dy) {
            error += dy;
            from.x += sx;
          }
          if (twice <= dx) {
            error += dx;
            from.y += sy;
          }
        }
      } else
        dot(p);
      previous = &point;
    }
    const auto zero_row = map({first->time, 0}).y / 4;
    for (std::uint32_t y = 0; y < rows; ++y) {
      ink.draw_text({static_cast<std::int32_t>(left - 1), static_cast<std::int32_t>(y)},
                    "|");
      for (std::uint32_t x = 0; x < cols; ++x) {
        const auto mask = dots_[std::size_t{y} * cols + x];
        const position at{static_cast<std::int32_t>(left + x),
                          static_cast<std::int32_t>(y)};
        if (mask) {
          const auto cp = 0x2800U + mask;
          const std::array<char, 3> utf8{static_cast<char>(0xE0U | (cp >> 12)),
                                         static_cast<char>(0x80U | ((cp >> 6) & 63U)),
                                         static_cast<char>(0x80U | (cp & 63U))};
          ink.draw_text(at, std::string_view{utf8.data(), utf8.size()});
        } else if (static_cast<std::int32_t>(y) == zero_row)
          ink.draw_text(at, "-");
      }
    }
    ink.draw_text({0, 0}, top_label);
    ink.draw_text({0, static_cast<std::int32_t>(rows - 1)}, bottom_label);
    if (zero_row > 0 && zero_row < static_cast<std::int32_t>(rows - 1))
      ink.draw_text({0, zero_row}, "0");
    const auto axis_y = static_cast<std::int32_t>(rows);
    ink.draw_text({static_cast<std::int32_t>(left - 1), axis_y},
                  "+" + std::string(cols, '-'));
    const auto label_time = [&](auto time) {
      return duration >= 86400 ? std::format("{:%Y-%m-%d}", time)
                               : std::format("{:%H:%M:%S}", time);
    };
    const auto begin_label = label_time(first->time),
               end_label = label_time(last->time);
    if (begin_label.size() <= cols)
      ink.draw_text({static_cast<std::int32_t>(left), axis_y + 1}, begin_label);
    if (duration > 0 && begin_label.size() + end_label.size() + 1 <= cols)
      ink.draw_text(
          {static_cast<std::int32_t>(ink.width() - end_label.size()), axis_y + 1},
          end_label);
  }

 private:
  std::vector<equity_point> points_;
  std::vector<std::uint8_t> dots_;
};
}  // namespace avionix
