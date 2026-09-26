module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

export module avionix.entity.constraint;

export import avionix.entity.geometry;

// Deterministic one-dimensional layout: split a rect into consecutive
// segments along one axis. The rules are pure; widgets in
// avionix.interface.layout apply them.

export namespace avionix {

enum class direction : std::uint8_t { horizontal, vertical };

class constraint {
 public:
  enum class kind : std::uint8_t { fixed, percentage, minimum, maximum, fill };

  // Exactly `cells` cells, unless the area is too small.
  [[nodiscard]] static constexpr constraint fixed(std::uint32_t cells) noexcept {
    return {kind::fixed, cells};
  }

  // `percent` of the whole available length, rounded down. Values above
  // 100 are clamped.
  [[nodiscard]] static constexpr constraint percentage(std::uint32_t percent) noexcept {
    return {kind::percentage, std::min<std::uint32_t>(percent, 100)};
  }

  // At least `cells`, then grows like fill(1).
  [[nodiscard]] static constexpr constraint minimum(std::uint32_t cells) noexcept {
    return {kind::minimum, cells};
  }

  // Grows like fill(1), but never beyond `cells`.
  [[nodiscard]] static constexpr constraint maximum(std::uint32_t cells) noexcept {
    return {kind::maximum, cells};
  }

  // A share of the space left after fixed, percentage, and minimum
  // requests, proportional to `weight`. A weight of 0 is treated as 1.
  [[nodiscard]] static constexpr constraint fill(std::uint32_t weight = 1) noexcept {
    return {kind::fill, std::max<std::uint32_t>(weight, 1)};
  }

  [[nodiscard]] constexpr kind type() const noexcept { return kind_; }
  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }

  friend constexpr bool operator==(const constraint&,
                                   const constraint&) noexcept = default;

 private:
  constexpr constraint(kind k, std::uint32_t v) noexcept : kind_{k}, value_{v} {}

  kind kind_;
  std::uint32_t value_;
};

// Computes segment lengths for `constraints` along a line of `available`
// cells, with `spacing` cells between segments. Writes one length per
// constraint into `lengths` (which must have the same size).
//
// Rules, applied in order:
// 1. fixed, percentage, and minimum requests are granted.
// 2. If the requests exceed the space, segments are shrunk from the last
//    to the first until they fit.
// 3. Remaining space goes to fill, minimum, and maximum segments by weight
//    (fill uses its weight, the others weight 1). Integer remainders go to
//    the earliest segments. Maximum segments are capped and their excess is
//    redistributed.
//
// Complexity O(n * k) where k is the number of capped maximum segments.
constexpr void solve_lengths(std::uint32_t available,
                             std::span<const constraint> constraints,
                             std::span<std::uint32_t> lengths,
                             std::uint32_t spacing = 0) noexcept {
  const std::size_t count = std::min(constraints.size(), lengths.size());
  if (count == 0) {
    return;
  }
  const std::uint64_t gaps = std::uint64_t{spacing} * (count - 1);
  std::uint64_t space = available > gaps ? available - gaps : 0;

  std::uint64_t requested = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const auto& c = constraints[i];
    std::uint64_t base = 0;
    switch (c.type()) {
      case constraint::kind::fixed:
      case constraint::kind::minimum:
        base = c.value();
        break;
      case constraint::kind::percentage:
        base = std::uint64_t{available} * c.value() / 100;
        break;
      case constraint::kind::maximum:
      case constraint::kind::fill:
        base = 0;
        break;
    }
    lengths[i] = static_cast<std::uint32_t>(base);
    requested += base;
  }

  if (requested >= space) {
    std::uint64_t excess = requested - space;
    for (std::size_t i = count; i-- > 0 && excess > 0;) {
      const std::uint64_t cut = std::min<std::uint64_t>(lengths[i], excess);
      lengths[i] -= static_cast<std::uint32_t>(cut);
      excess -= cut;
    }
    return;
  }

  std::uint64_t remaining = space - requested;
  const auto weight_of = [](const constraint& c) -> std::uint64_t {
    switch (c.type()) {
      case constraint::kind::fill:
        return c.value();
      case constraint::kind::minimum:
      case constraint::kind::maximum:
        return 1;
      default:
        return 0;
    }
  };

  // A maximum segment is capped once it reaches its limit; capped
  // segments drop out and the pass repeats with their excess. Each pass
  // caps at least one segment or distributes everything, so the loop runs
  // at most count + 1 times.
  const auto is_capped = [&](std::size_t i) {
    return constraints[i].type() == constraint::kind::maximum &&
           lengths[i] >= constraints[i].value();
  };

  while (remaining > 0) {
    std::uint64_t total_weight = 0;
    for (std::size_t i = 0; i < count; ++i) {
      if (!is_capped(i)) {
        total_weight += weight_of(constraints[i]);
      }
    }
    if (total_weight == 0) {
      return;
    }

    std::uint64_t distributed = 0;
    for (std::size_t i = 0; i < count; ++i) {
      if (is_capped(i)) continue;
      const std::uint64_t share = remaining * weight_of(constraints[i]) / total_weight;
      lengths[i] += static_cast<std::uint32_t>(share);
      distributed += share;
    }
    std::uint64_t leftover = remaining - distributed;
    for (std::size_t i = 0; i < count && leftover > 0; ++i) {
      if (!is_capped(i) && weight_of(constraints[i]) > 0) {
        ++lengths[i];
        --leftover;
      }
    }

    remaining = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const auto& c = constraints[i];
      if (c.type() == constraint::kind::maximum && lengths[i] > c.value()) {
        remaining += lengths[i] - c.value();
        lengths[i] = c.value();
      }
    }
  }
}

// Splits `area` into one rect per constraint along `axis`. The cross axis
// is copied from `area` unchanged.
inline void split(const rect& area, direction axis,
                  std::span<const constraint> constraints, std::span<rect> out,
                  std::uint32_t spacing = 0) {
  const std::size_t count = std::min(constraints.size(), out.size());
  // Layout runs every frame; typical splits have few segments, so avoid
  // the heap for them.
  std::array<std::uint32_t, 32> small{};
  std::vector<std::uint32_t> large;
  std::span<std::uint32_t> lengths{small.data(), std::min(count, small.size())};
  if (count > small.size()) {
    large.resize(count);
    lengths = large;
  }
  const std::uint32_t available =
      axis == direction::horizontal ? area.width() : area.height();
  solve_lengths(available, constraints.first(count), lengths, spacing);

  std::int64_t cursor = axis == direction::horizontal ? area.left() : area.top();
  for (std::size_t i = 0; i < count; ++i) {
    if (axis == direction::horizontal) {
      out[i] = {{static_cast<std::int32_t>(cursor), area.top()},
                {lengths[i], area.height()}};
    } else {
      out[i] = {{area.left(), static_cast<std::int32_t>(cursor)},
                {area.width(), lengths[i]}};
    }
    cursor += lengths[i];
    cursor += spacing;
  }
}

[[nodiscard]] inline std::vector<rect> split(const rect& area, direction axis,
                                             std::span<const constraint> constraints,
                                             std::uint32_t spacing = 0) {
  std::vector<rect> out(constraints.size());
  split(area, axis, constraints, out, spacing);
  return out;
}

}  // namespace avionix
