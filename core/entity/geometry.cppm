module;

#include <algorithm>
#include <cstdint>

export module avionix.entity.geometry;

// Terminal geometry. Coordinates are cell units with the origin at the top
// left. `position` is signed so partially off-screen regions can be expressed
// before clipping; `size` is unsigned because extents are never negative.

export namespace avionix {

struct position {
  std::int32_t x{};
  std::int32_t y{};

  friend constexpr bool operator==(const position&, const position&) noexcept = default;

  friend constexpr position operator+(position a, position b) noexcept {
    return {a.x + b.x, a.y + b.y};
  }

  friend constexpr position operator-(position a, position b) noexcept {
    return {a.x - b.x, a.y - b.y};
  }
};

struct size {
  std::uint32_t width{};
  std::uint32_t height{};

  friend constexpr bool operator==(const size&, const size&) noexcept = default;

  [[nodiscard]] constexpr std::uint64_t area() const noexcept {
    return std::uint64_t{width} * height;
  }

  [[nodiscard]] constexpr bool empty() const noexcept {
    return width == 0 || height == 0;
  }
};

struct rect {
  position origin{};
  size extent{};

  friend constexpr bool operator==(const rect&, const rect&) noexcept = default;

  [[nodiscard]] constexpr std::int32_t left() const noexcept { return origin.x; }
  [[nodiscard]] constexpr std::int32_t top() const noexcept { return origin.y; }

  // One past the last column/row. Computed in 64 bits and saturated so a
  // rect near INT32_MAX cannot overflow.
  [[nodiscard]] constexpr std::int32_t right() const noexcept {
    return saturate(std::int64_t{origin.x} + extent.width);
  }

  [[nodiscard]] constexpr std::int32_t bottom() const noexcept {
    return saturate(std::int64_t{origin.y} + extent.height);
  }

  [[nodiscard]] constexpr std::uint32_t width() const noexcept { return extent.width; }
  [[nodiscard]] constexpr std::uint32_t height() const noexcept {
    return extent.height;
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return extent.empty(); }

  [[nodiscard]] constexpr bool contains(position p) const noexcept {
    return p.x >= left() && p.x < right() && p.y >= top() && p.y < bottom();
  }

  [[nodiscard]] constexpr bool contains(const rect& other) const noexcept {
    return other.empty() || (other.left() >= left() && other.right() <= right() &&
                             other.top() >= top() && other.bottom() <= bottom());
  }

  [[nodiscard]] constexpr rect translated(position offset) const noexcept {
    return {origin + offset, extent};
  }

  // Shrinks each side by the given amount, collapsing to an empty rect at
  // the original origin when the margins exceed the extent.
  [[nodiscard]] constexpr rect inset(std::uint32_t left_margin,
                                     std::uint32_t top_margin,
                                     std::uint32_t right_margin,
                                     std::uint32_t bottom_margin) const noexcept {
    const std::uint64_t horizontal = std::uint64_t{left_margin} + right_margin;
    const std::uint64_t vertical = std::uint64_t{top_margin} + bottom_margin;
    if (horizontal >= extent.width || vertical >= extent.height) {
      return {origin, {}};
    }
    return {
        {saturate(std::int64_t{origin.x} + left_margin),
         saturate(std::int64_t{origin.y} + top_margin)},
        {extent.width - static_cast<std::uint32_t>(horizontal),
         extent.height - static_cast<std::uint32_t>(vertical)},
    };
  }

  [[nodiscard]] constexpr rect inset(std::uint32_t margin) const noexcept {
    return inset(margin, margin, margin, margin);
  }

 private:
  static constexpr std::int32_t saturate(std::int64_t value) noexcept {
    return static_cast<std::int32_t>(
        std::clamp<std::int64_t>(value, INT32_MIN, INT32_MAX));
  }
};

// Intersection of two rects. Returns an empty rect positioned at the clamped
// origin when they do not overlap.
[[nodiscard]] constexpr rect intersect(const rect& a, const rect& b) noexcept {
  const std::int32_t left = std::max(a.left(), b.left());
  const std::int32_t top = std::max(a.top(), b.top());
  const std::int32_t right = std::min(a.right(), b.right());
  const std::int32_t bottom = std::min(a.bottom(), b.bottom());
  if (right <= left || bottom <= top) {
    return {{left, top}, {}};
  }
  return {{left, top},
          {static_cast<std::uint32_t>(std::int64_t{right} - left),
           static_cast<std::uint32_t>(std::int64_t{bottom} - top)}};
}

[[nodiscard]] constexpr rect bounds(size extent) noexcept { return {{0, 0}, extent}; }

}  // namespace avionix
