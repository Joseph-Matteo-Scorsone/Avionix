module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

export module avionix.entity.manipulator;

export import avionix.entity.scene;
export import avionix.entity.camera;

// Move, rotate, and scale handles as pure geometry and math.
//
//   node placement ──► manipulator_frame (origin, axes, parent frame)
//        │
//        ├─► gizmo_shapes(): screen-space polylines and polygons that the
//        │   renderer draws and hit_handle() tests, so what is drawn is
//        │   exactly what is clickable
//        │
//        └─► begin_drag() / drag_to() / numeric_transform(): the proposed
//            local transform for a pointer position or a typed value
//
// Every result is a proposed *physical* local transform in the node's
// parent frame. Handles are placed on the presented (possibly exploded)
// geometry, and world-space deltas are mapped back through the presented
// parent frame, so dragging an exploded part moves its physical placement
// by the amount the user sees.
//
// Rotation pivots on the node's local origin. Scale is along the node's own
// axes. Permissions remove axes before any math runs; the host's filter can
// still adjust or reject each proposal afterwards.

export namespace avionix {

enum class transform_kind : std::uint8_t { translate, rotate, scale };

// x, y, z: one gizmo axis. xy, yz, zx: the plane of two axes (translate) or
// both axes together (scale). view: the view plane (translate) or all axes
// uniformly (scale). Rotation uses x, y, z, or view (about the view
// direction).
enum class axis_constraint : std::uint8_t { x, y, z, xy, yz, zx, view };

// Orientation of translate and rotate handles. parent: the frame of
// node_desc::local.translation, which permissions refer to. world: document
// axes. local: the node's rotated axes. Scale always uses local axes.
enum class transform_space : std::uint8_t { parent, world, local };

struct snap_settings {
  bool enabled{false};  // Ctrl inverts this during a drag
  double translate_step{1.0};
  double rotate_step{radians(15.0)};
  double scale_step{0.1};
};

struct manipulator_frame {
  vec3 origin{};  // presented world position of the node origin
  std::array<vec3, 3> axes{axis_x, axis_y, axis_z};  // unit world directions
  mat4 parent_presented{};  // presented(parent) * explode(node)
  mat4 parent_presented_inverse{};
  transform3d local{};  // physical local transform at gesture start
  transform_permissions permissions{transform_permissions::all};
  transform_space space{transform_space::parent};
};

[[nodiscard]] inline manipulator_frame make_frame(const mat4& parent_presented,
                                                  const transform3d& local,
                                                  transform_permissions permissions,
                                                  transform_space space,
                                                  transform_kind kind) noexcept {
  manipulator_frame f;
  f.parent_presented = parent_presented;
  f.parent_presented_inverse = inverse(parent_presented);
  f.local = local;
  f.permissions = permissions;
  f.space = kind == transform_kind::scale ? transform_space::local : space;
  f.origin = transform_point(parent_presented, local.translation);
  const std::array<vec3, 3> basis{axis_x, axis_y, axis_z};
  for (std::size_t i = 0; i < 3; ++i) {
    switch (f.space) {
      case transform_space::world:
        f.axes[i] = basis[i];
        break;
      case transform_space::parent:
        f.axes[i] = normalize(transform_vector(parent_presented, basis[i]));
        break;
      case transform_space::local:
        f.axes[i] = normalize(
            transform_vector(parent_presented, rotate(local.rotation, basis[i])));
        break;
    }
    if (f.axes[i] == vec3{}) f.axes[i] = basis[i];
  }
  return f;
}

[[nodiscard]] constexpr std::array<bool, 3> constraint_axes(
    axis_constraint c) noexcept {
  switch (c) {
    case axis_constraint::x:
      return {true, false, false};
    case axis_constraint::y:
      return {false, true, false};
    case axis_constraint::z:
      return {false, false, true};
    case axis_constraint::xy:
      return {true, true, false};
    case axis_constraint::yz:
      return {false, true, true};
    case axis_constraint::zx:
      return {true, false, true};
    case axis_constraint::view:
      return {true, true, true};
  }
  return {};
}

[[nodiscard]] constexpr transform_permissions axis_permission(
    transform_kind kind, std::size_t axis) noexcept {
  constexpr std::array<transform_permissions, 3> t{transform_permissions::translate_x,
                                                   transform_permissions::translate_y,
                                                   transform_permissions::translate_z};
  constexpr std::array<transform_permissions, 3> r{transform_permissions::rotate_x,
                                                   transform_permissions::rotate_y,
                                                   transform_permissions::rotate_z};
  constexpr std::array<transform_permissions, 3> s{transform_permissions::scale_x,
                                                   transform_permissions::scale_y,
                                                   transform_permissions::scale_z};
  return kind == transform_kind::translate
             ? t[axis]
             : (kind == transform_kind::rotate ? r[axis] : s[axis]);
}

// Whether a handle may be offered. For translate in world or local space,
// and for rotation in any space but parent, the axis may not line up with
// a permitted parent axis; such handles are offered when any component of
// that kind is permitted, and drag results are masked or rejected.
[[nodiscard]] inline bool constraint_permitted(const manipulator_frame& f,
                                               transform_kind kind,
                                               axis_constraint c) noexcept {
  const auto axes = constraint_axes(c);
  const bool aligned =
      f.space == transform_space::parent || kind == transform_kind::scale;
  if (kind == transform_kind::rotate && c == axis_constraint::view)
    return allows(f.permissions, transform_permissions::rotate);
  if (kind == transform_kind::rotate &&
      (c == axis_constraint::xy || c == axis_constraint::yz ||
       c == axis_constraint::zx))
    return false;
  bool any = false;
  for (std::size_t i = 0; i < 3; ++i) {
    if (!axes[i]) continue;
    const bool ok = aligned ? allows(f.permissions, axis_permission(kind, i))
                            : allows_any(f.permissions, axis_permission(kind, 0) |
                                                            axis_permission(kind, 1) |
                                                            axis_permission(kind, 2));
    if (aligned && !ok) return false;
    any = any || ok;
  }
  return any;
}

// Screen-space handle geometry, in framebuffer pixels.
struct gizmo_shape {
  transform_kind kind{};
  axis_constraint constraint{};
  std::vector<vec2> points;
  bool loop{false};    // polyline closes back to the first point
  bool filled{false};  // convex polygon instead of a polyline
  rgba color{};
};

struct gizmo_style {
  double length{90.0};    // logical pixels from origin to axis tip
  double tolerance{7.0};  // logical pixels for hit testing
  double scale{1.0};      // framebuffer pixels per logical pixel
};

[[nodiscard]] constexpr rgba axis_color(std::size_t axis) noexcept {
  constexpr std::array<rgba, 3> colors{rgba{0.90F, 0.22F, 0.24F, 1.0F},
                                       rgba{0.30F, 0.78F, 0.30F, 1.0F},
                                       rgba{0.25F, 0.48F, 0.95F, 1.0F}};
  return colors[axis];
}
inline constexpr rgba view_handle_color{0.85F, 0.85F, 0.85F, 1.0F};
inline constexpr rgba active_handle_color{1.0F, 0.85F, 0.15F, 1.0F};

[[nodiscard]] inline std::vector<gizmo_shape> gizmo_shapes(const manipulator_frame& f,
                                                           const view_frame& view,
                                                           transform_kind kind,
                                                           const gizmo_style& style) {
  std::vector<gizmo_shape> shapes;
  const auto origin_screen = project(view, f.origin);
  if (!origin_screen) return shapes;
  const vec2 o{origin_screen->x, origin_screen->y};
  const double len = units_per_pixel(view, f.origin) * style.length * style.scale;
  const auto screen = [&](vec3 world) -> std::optional<vec2> {
    const auto p = project(view, world);
    if (!p) return std::nullopt;
    return vec2{p->x, p->y};
  };
  const auto square = [&](vec2 center, double half) {
    return std::vector<vec2>{center + vec2{-half, -half}, center + vec2{half, -half},
                             center + vec2{half, half}, center + vec2{-half, half}};
  };
  constexpr std::array<axis_constraint, 3> single{
      axis_constraint::x, axis_constraint::y, axis_constraint::z};
  constexpr std::array<axis_constraint, 3> planes{
      axis_constraint::yz, axis_constraint::zx, axis_constraint::xy};

  if (kind == transform_kind::rotate) {
    for (std::size_t i = 0; i < 3; ++i) {
      if (!constraint_permitted(f, kind, single[i])) continue;
      const vec3 u = f.axes[(i + 1) % 3];
      const vec3 v = f.axes[(i + 2) % 3];
      gizmo_shape ring{kind, single[i], {}, true, false, axis_color(i)};
      constexpr int segments = 64;
      for (int s = 0; s < segments; ++s) {
        const double a = 2.0 * pi * s / segments;
        if (auto p =
                screen(f.origin + (u * std::cos(a) + v * std::sin(a)) * (len * 0.85)))
          ring.points.push_back(*p);
      }
      if (ring.points.size() == segments) shapes.push_back(std::move(ring));
    }
    if (constraint_permitted(f, kind, axis_constraint::view)) {
      gizmo_shape outer{kind,  axis_constraint::view, {}, true,
                        false, view_handle_color};
      constexpr int segments = 64;
      const double r = style.length * style.scale * 1.0;
      for (int s = 0; s < segments; ++s) {
        const double a = 2.0 * pi * s / segments;
        outer.points.push_back(o + vec2{std::cos(a), std::sin(a)} * r);
      }
      shapes.push_back(std::move(outer));
    }
    return shapes;
  }

  for (std::size_t i = 0; i < 3; ++i) {
    if (!constraint_permitted(f, kind, single[i])) continue;
    const auto tip = screen(f.origin + f.axes[i] * len);
    if (!tip) continue;
    shapes.push_back({kind, single[i], {o, *tip}, false, false, axis_color(i)});
    const vec2 dir = *tip - o;
    const double dl = length(dir);
    if (dl < 1e-6) continue;
    const vec2 d = dir * (1.0 / dl);
    const vec2 n{-d.y, d.x};
    const double head = 9.0 * style.scale;
    if (kind == transform_kind::translate) {
      shapes.push_back({kind,
                        single[i],
                        {*tip + d * (head * 1.6), *tip + n * head, *tip - n * head},
                        false,
                        true,
                        axis_color(i)});
    } else {
      shapes.push_back(
          {kind, single[i], square(*tip, head * 0.7), false, true, axis_color(i)});
    }
  }
  if (kind == transform_kind::translate) {
    for (std::size_t i = 0; i < 3; ++i) {
      if (!constraint_permitted(f, kind, planes[i])) continue;
      const vec3 u = f.axes[(i + 1) % 3] * (len * 0.28);
      const vec3 v = f.axes[(i + 2) % 3] * (len * 0.28);
      std::vector<vec2> quad;
      for (const vec3 corner :
           {f.origin + u * 0.6 + v * 0.6, f.origin + u * 1.4 + v * 0.6,
            f.origin + u * 1.4 + v * 1.4, f.origin + u * 0.6 + v * 1.4}) {
        if (auto p = screen(corner)) quad.push_back(*p);
      }
      rgba c = axis_color(i);
      c.a = 0.55F;
      if (quad.size() != 4) continue;
      // A plane seen nearly edge-on cannot be dragged meaningfully; omit it.
      double twice_area = 0.0;
      for (std::size_t k = 0; k < 4; ++k)
        twice_area += quad[k].x * quad[(k + 1) % 4].y - quad[(k + 1) % 4].x * quad[k].y;
      if (std::abs(twice_area) * 0.5 < 30.0 * style.scale * style.scale) continue;
      shapes.push_back({kind, planes[i], quad, false, true, c});
    }
  }
  if (constraint_permitted(f, kind, axis_constraint::view)) {
    shapes.push_back({kind, axis_constraint::view, square(o, 6.0 * style.scale), true,
                      false, view_handle_color});
  }
  return shapes;
}

namespace manipulator_detail {

[[nodiscard]] inline double segment_distance(vec2 p, vec2 a, vec2 b) noexcept {
  const vec2 ab = b - a;
  const double len2 = dot(ab, ab);
  const double t = len2 > 0.0 ? std::clamp(dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
  return length(p - (a + ab * t));
}

[[nodiscard]] inline double polygon_area(const std::vector<vec2>& poly) noexcept {
  double twice = 0.0;
  for (std::size_t i = 0; i < poly.size(); ++i) {
    const vec2 a = poly[i];
    const vec2 b = poly[(i + 1) % poly.size()];
    twice += a.x * b.y - b.x * a.y;
  }
  return std::abs(twice) * 0.5;
}

// False for degenerate polygons, such as a plane handle seen edge-on.
[[nodiscard]] inline bool inside_convex(vec2 p,
                                        const std::vector<vec2>& poly) noexcept {
  if (poly.size() < 3 || polygon_area(poly) < 1.0) return false;
  int sign = 0;
  for (std::size_t i = 0; i < poly.size(); ++i) {
    const vec2 a = poly[i];
    const vec2 b = poly[(i + 1) % poly.size()];
    const double c = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
    const int s = c > 0.0 ? 1 : (c < 0.0 ? -1 : 0);
    if (s == 0) continue;
    if (sign == 0) sign = s;
    if (s != sign) return false;
  }
  return true;
}

// Parameter along the line origin + axis * s closest to the ray, or nullopt
// when the axis is nearly parallel to the view ray.
[[nodiscard]] inline std::optional<double> axis_parameter(const ray& r, vec3 origin,
                                                          vec3 axis) noexcept {
  const vec3 w0 = origin - r.origin;
  const double b = dot(axis, r.direction);
  const double c = dot(r.direction, r.direction);
  const double d = dot(axis, w0);
  const double e = dot(r.direction, w0);
  const double denom = c - b * b;
  if (denom < 1e-4 * c) return std::nullopt;
  return (b * e - c * d) / denom;
}

[[nodiscard]] inline double snap(double value, double step, bool on) noexcept {
  return on && step > 0.0 ? std::round(value / step) * step : value;
}

}  // namespace manipulator_detail

// The handle under `pixel`, if any. Filled shapes win when the pointer is
// inside them; otherwise the nearest polyline within tolerance wins.
[[nodiscard]] inline std::optional<axis_constraint> hit_handle(
    const std::vector<gizmo_shape>& shapes, vec2 pixel, const gizmo_style& style) {
  using namespace manipulator_detail;
  for (const gizmo_shape& s : shapes)
    if (s.filled && inside_convex(pixel, s.points)) return s.constraint;
  std::optional<axis_constraint> best;
  double best_distance = style.tolerance * style.scale;
  for (const gizmo_shape& s : shapes) {
    const std::size_t n = s.points.size();
    for (std::size_t i = 0; i + 1 < n || (s.loop && i < n); ++i) {
      const double d = segment_distance(pixel, s.points[i], s.points[(i + 1) % n]);
      if (d <= best_distance) {
        best_distance = d;
        best = s.constraint;
      }
    }
  }
  return best;
}

// State of one pointer drag on a handle.
struct drag_state {
  transform_kind kind{};
  axis_constraint constraint{};
  manipulator_frame frame{};
  vec2 start_pixel{};
  vec2 origin_pixel{};
  vec3 start_point{};        // translate: start hit on the constraint
  double start_parameter{};  // translate/scale along an axis
  double angle{};            // rotate: accumulated, unwrapped
  double last_screen_angle{};
  vec3 rotation_axis{};  // rotate: world axis
  transform3d last{};    // last proposal, kept when a step is degenerate
};

namespace manipulator_detail {

[[nodiscard]] inline vec3 constraint_normal(const manipulator_frame& f,
                                            axis_constraint c,
                                            const view_frame& view) noexcept {
  switch (c) {
    case axis_constraint::xy:
      return f.axes[2];
    case axis_constraint::yz:
      return f.axes[0];
    case axis_constraint::zx:
      return f.axes[1];
    default:
      return -view.forward;
  }
}

[[nodiscard]] inline std::size_t single_axis(axis_constraint c) noexcept {
  return c == axis_constraint::y ? 1 : (c == axis_constraint::z ? 2 : 0);
}

[[nodiscard]] inline bool is_single(axis_constraint c) noexcept {
  return c == axis_constraint::x || c == axis_constraint::y || c == axis_constraint::z;
}

// Applies a world-space translation delta. Components the permissions deny
// are removed in the parent frame.
[[nodiscard]] inline transform3d translated(const manipulator_frame& f,
                                            vec3 world_delta,
                                            const snap_settings& snaps,
                                            bool snap_on) noexcept {
  vec3 d = transform_vector(f.parent_presented_inverse, world_delta);
  for (std::size_t i = 0; i < 3; ++i) {
    if (!allows(f.permissions, axis_permission(transform_kind::translate, i)))
      d[i] = 0.0;
  }
  transform3d out = f.local;
  out.translation = f.local.translation + d;
  if (snap_on) {
    for (std::size_t i = 0; i < 3; ++i) {
      if (d[i] != 0.0)
        out.translation[i] = snap(out.translation[i], snaps.translate_step, true);
    }
  }
  return out;
}

// Rotation about a world axis through the node origin. Returns nullopt
// when the axis is not a permitted parent-frame axis.
[[nodiscard]] inline std::optional<transform3d> rotated(const manipulator_frame& f,
                                                        vec3 world_axis,
                                                        double angle) noexcept {
  const vec3 a = normalize(transform_vector(f.parent_presented_inverse, world_axis));
  if (a == vec3{}) return std::nullopt;
  if (!allows(f.permissions, transform_permissions::rotate)) {
    bool ok = false;
    for (std::size_t i = 0; i < 3; ++i) {
      if (std::abs(a[i]) > 1.0 - 1e-6 &&
          allows(f.permissions, axis_permission(transform_kind::rotate, i)))
        ok = true;
    }
    if (!ok) return std::nullopt;
  }
  transform3d out = f.local;
  out.rotation = normalize(axis_angle(a, angle) * f.local.rotation);
  return out;
}

[[nodiscard]] inline transform3d scaled(const manipulator_frame& f, axis_constraint c,
                                        double factor) noexcept {
  factor = std::max(factor, 1e-4);
  const auto axes = constraint_axes(c);
  transform3d out = f.local;
  for (std::size_t i = 0; i < 3; ++i) {
    if (axes[i] && allows(f.permissions, axis_permission(transform_kind::scale, i)))
      out.scale[i] = f.local.scale[i] * factor;
  }
  return out;
}

}  // namespace manipulator_detail

// Starts a drag. Returns nullopt when the constraint is not permitted or
// the pointer ray does not meet the constraint (for example a plane seen
// edge-on).
[[nodiscard]] inline std::optional<drag_state> begin_drag(const manipulator_frame& f,
                                                          transform_kind kind,
                                                          axis_constraint c,
                                                          const view_frame& view,
                                                          vec2 pixel) {
  using namespace manipulator_detail;
  if (!constraint_permitted(f, kind, c)) return std::nullopt;
  const auto origin_screen = project(view, f.origin);
  if (!origin_screen) return std::nullopt;
  drag_state s;
  s.kind = kind;
  s.constraint = c;
  s.frame = f;
  s.start_pixel = pixel;
  s.origin_pixel = {origin_screen->x, origin_screen->y};
  s.last = f.local;
  const ray r = ray_through(view, pixel);
  if (kind == transform_kind::translate) {
    if (is_single(c)) {
      const auto t = axis_parameter(r, f.origin, f.axes[single_axis(c)]);
      if (!t) return std::nullopt;
      s.start_parameter = *t;
    } else {
      const double t =
          intersect(r, plane_through(f.origin, constraint_normal(f, c, view)));
      if (!std::isfinite(t)) return std::nullopt;
      s.start_point = r.at(t);
    }
  } else if (kind == transform_kind::rotate) {
    s.rotation_axis =
        c == axis_constraint::view ? -view.forward : f.axes[single_axis(c)];
    const vec2 d = pixel - s.origin_pixel;
    s.last_screen_angle = std::atan2(-d.y, d.x);
  } else {
    if (is_single(c)) {
      const auto t = axis_parameter(r, f.origin, f.axes[single_axis(c)]);
      if (!t || std::abs(*t) < 1e-9) return std::nullopt;
      s.start_parameter = *t;
    } else {
      s.start_parameter = length(pixel - s.origin_pixel);
      if (s.start_parameter < 1.0) return std::nullopt;
    }
  }
  return s;
}

// The proposed local transform for the pointer at `pixel`. A degenerate
// step (axis turned parallel to the view) repeats the last proposal.
[[nodiscard]] inline transform3d drag_to(drag_state& s, const view_frame& view,
                                         vec2 pixel, const snap_settings& snaps,
                                         bool invert_snap) {
  using namespace manipulator_detail;
  const bool snap_on = snaps.enabled != invert_snap;
  const manipulator_frame& f = s.frame;
  const ray r = ray_through(view, pixel);
  switch (s.kind) {
    case transform_kind::translate: {
      if (is_single(s.constraint)) {
        const vec3 axis = f.axes[single_axis(s.constraint)];
        const auto t = axis_parameter(r, f.origin, axis);
        if (!t) return s.last;
        double amount = *t - s.start_parameter;
        if (snap_on && f.space != transform_space::parent)
          amount = snap(amount, snaps.translate_step, true);
        s.last = translated(f, axis * amount, snaps,
                            snap_on && f.space == transform_space::parent);
      } else {
        const double t = intersect(
            r, plane_through(f.origin, constraint_normal(f, s.constraint, view)));
        if (!std::isfinite(t)) return s.last;
        s.last = translated(f, r.at(t) - s.start_point, snaps, snap_on);
      }
      return s.last;
    }
    case transform_kind::rotate: {
      const vec2 d = pixel - s.origin_pixel;
      const double toward = dot(s.rotation_axis, -view.forward);
      double step = 0.0;
      if (std::abs(toward) < 0.15 || length(d) < 4.0) {
        // Edge-on ring: horizontal pointer motion turns it.
        step = -(pixel.x - s.start_pixel.x) * 0.01 - s.angle;
        s.angle += step;
      } else {
        const double a = std::atan2(-d.y, d.x);
        step = std::remainder(a - s.last_screen_angle, 2.0 * pi);
        s.last_screen_angle = a;
        s.angle += toward > 0.0 ? step : -step;
      }
      const double angle = snap(s.angle, snaps.rotate_step, snap_on);
      if (auto out = rotated(f, s.rotation_axis, angle)) s.last = *out;
      return s.last;
    }
    case transform_kind::scale: {
      double factor = 1.0;
      if (is_single(s.constraint)) {
        const auto t = axis_parameter(r, f.origin, f.axes[single_axis(s.constraint)]);
        if (!t) return s.last;
        factor = *t / s.start_parameter;
      } else {
        factor = length(pixel - s.origin_pixel) / s.start_parameter;
      }
      factor = snap(factor, snaps.scale_step, snap_on);
      s.last = scaled(f, s.constraint, factor);
      return s.last;
    }
  }
  return s.last;
}

// A transform from a typed value: document units along one axis for
// translate, degrees about one axis for rotate, a factor for scale.
// Returns nullopt when the constraint does not take a single value
// (translate in a plane) or is not permitted.
[[nodiscard]] inline std::optional<transform3d> numeric_transform(
    const manipulator_frame& f, transform_kind kind, axis_constraint c, double value,
    const view_frame& view) {
  using namespace manipulator_detail;
  if (!constraint_permitted(f, kind, c)) return std::nullopt;
  switch (kind) {
    case transform_kind::translate:
      if (!is_single(c)) return std::nullopt;
      return translated(f, f.axes[single_axis(c)] * value, {}, false);
    case transform_kind::rotate: {
      const vec3 axis =
          c == axis_constraint::view ? -view.forward : f.axes[single_axis(c)];
      if (c != axis_constraint::view && !is_single(c)) return std::nullopt;
      return rotated(f, axis, radians(value));
    }
    case transform_kind::scale:
      if (!(value > 0.0)) return std::nullopt;
      return scaled(f, c, value);
  }
  return std::nullopt;
}

}  // namespace avionix
