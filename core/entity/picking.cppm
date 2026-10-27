module;

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>

export module avionix.entity.picking;

export import avionix.entity.scene;
export import avionix.entity.camera;

// CPU picking against what is drawn. Callers resolve each visible, pickable
// node to a pick_target with its presented world matrix, so a pick in an
// exploded view or during a drag preview hits the geometry where the user
// sees it.
//
//   pixel ──ray_through──► world ray ──per target: bounds test,
//                                      local-space ray/triangle,
//                                      section-plane test──► nearest face
//   pixel ──project segments──► screen distance ≤ tolerance,
//                               not behind the nearest face──► nearest edge
//
// Cost is linear in the triangles and segments of targets whose bounds the
// ray crosses. There is no acceleration structure yet.

export namespace avionix {

struct pick_target {
  node_id node;
  mesh_id mesh;
  const mesh_data* data{};  // borrowed for the duration of pick()
  mat4 world{};
  mat4 inverse_world{};
  aabb world_bounds{};
};

struct pick_options {
  // Geometry on the removed side of any plane is not hit.
  std::span<const plane> section_planes{};
  // Maximum distance from the pointer to an edge, in framebuffer pixels.
  double edge_tolerance{6.0};
  bool edges{true};
};

// What a pick found. `node` is the drawn node, not the selection target.
// `face` and `edge` are the host ids from mesh_data, or no_topology.
// `triangle` and `segment` index the mesh's triangles and segments.
struct pick_hit {
  node_id node;
  mesh_id mesh;
  std::uint32_t triangle{no_topology};
  std::uint32_t face{no_topology};
  std::uint32_t segment{no_topology};
  std::uint32_t edge{no_topology};
  vec3 world_point{};
  // Ray parameter of the hit; smaller is closer.
  double distance{};

  friend constexpr bool operator==(const pick_hit&, const pick_hit&) noexcept = default;
};

namespace picking_detail {

[[nodiscard]] inline bool ray_box(const ray& r, const aabb& box,
                                  double& t_near) noexcept {
  if (box.empty()) return false;
  double lo = r.t_min;
  double hi = std::numeric_limits<double>::infinity();
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const double o = r.origin[axis];
    const double d = r.direction[axis];
    if (std::abs(d) < 1e-15) {
      if (o < box.min[axis] || o > box.max[axis]) return false;
      continue;
    }
    double t0 = (box.min[axis] - o) / d;
    double t1 = (box.max[axis] - o) / d;
    if (t0 > t1) std::swap(t0, t1);
    lo = std::max(lo, t0);
    hi = std::min(hi, t1);
    if (lo > hi) return false;
  }
  t_near = lo;
  return true;
}

// Möller–Trumbore, two-sided. Returns the ray parameter or NaN.
[[nodiscard]] inline double ray_triangle(vec3 o, vec3 d, vec3 a, vec3 b,
                                         vec3 c) noexcept {
  const vec3 e1 = b - a;
  const vec3 e2 = c - a;
  const vec3 p = cross(d, e2);
  const double det = dot(e1, p);
  if (std::abs(det) < 1e-300) return std::numeric_limits<double>::quiet_NaN();
  const double inv = 1.0 / det;
  const vec3 s = o - a;
  const double u = dot(s, p) * inv;
  if (u < 0.0 || u > 1.0) return std::numeric_limits<double>::quiet_NaN();
  const vec3 q = cross(s, e1);
  const double v = dot(d, q) * inv;
  if (v < 0.0 || u + v > 1.0) return std::numeric_limits<double>::quiet_NaN();
  return dot(e2, q) * inv;
}

[[nodiscard]] inline bool clipped(std::span<const plane> planes, vec3 p) noexcept {
  return std::ranges::any_of(
      planes, [&](const plane& s) { return s.signed_distance(p) > 0.0; });
}

}  // namespace picking_detail

[[nodiscard]] inline std::optional<pick_hit> pick(std::span<const pick_target> targets,
                                                  const view_frame& frame, vec2 pixel,
                                                  const pick_options& options = {}) {
  using namespace picking_detail;
  const ray r = ray_through(frame, pixel);
  std::optional<pick_hit> face_hit;
  double best = std::numeric_limits<double>::infinity();

  for (const pick_target& target : targets) {
    if (target.data == nullptr) continue;
    double box_t = 0.0;
    if (!ray_box(r, target.world_bounds, box_t) || box_t > best) continue;
    // The ray parameter is preserved by an affine map, so t found in the
    // local frame is the world-space t.
    const vec3 lo = transform_point(target.inverse_world, r.origin);
    const vec3 ld = transform_vector(target.inverse_world, r.direction);
    const mesh_data& mesh = *target.data;
    for (std::size_t tri = 0; tri < mesh.triangle_count(); ++tri) {
      const vec3 a = widen(mesh.positions[mesh.indices[tri * 3]]);
      const vec3 b = widen(mesh.positions[mesh.indices[tri * 3 + 1]]);
      const vec3 c = widen(mesh.positions[mesh.indices[tri * 3 + 2]]);
      const double t = ray_triangle(lo, ld, a, b, c);
      if (!(t >= r.t_min) || !(t < best)) continue;
      const vec3 world = r.at(t);
      if (clipped(options.section_planes, world)) continue;
      best = t;
      const auto index = static_cast<std::uint32_t>(tri);
      face_hit =
          pick_hit{target.node,
                   target.mesh,
                   index,
                   mesh.triangle_faces.empty() ? no_topology : mesh.triangle_faces[tri],
                   no_topology,
                   no_topology,
                   world,
                   t};
    }
  }

  if (!options.edges) return face_hit;

  std::optional<pick_hit> edge_hit;
  double best_screen = options.edge_tolerance;
  for (const pick_target& target : targets) {
    if (target.data == nullptr) continue;
    const mesh_data& mesh = *target.data;
    for (std::size_t seg = 0; seg < mesh.segment_count(); ++seg) {
      const vec3 a = transform_point(target.world,
                                     widen(mesh.positions[mesh.edge_indices[seg * 2]]));
      const vec3 b = transform_point(
          target.world, widen(mesh.positions[mesh.edge_indices[seg * 2 + 1]]));
      const auto pa = project(frame, a);
      const auto pb = project(frame, b);
      if (!pa || !pb) continue;
      const vec2 sa{pa->x, pa->y};
      const vec2 sb{pb->x, pb->y};
      const vec2 ab = sb - sa;
      const double len2 = dot(ab, ab);
      const double s =
          len2 > 0.0 ? std::clamp(dot(pixel - sa, ab) / len2, 0.0, 1.0) : 0.0;
      const double screen = length(pixel - (sa + ab * s));
      if (screen > best_screen) continue;
      const vec3 world = lerp(a, b, s);
      if (clipped(options.section_planes, world)) continue;
      const double t = dot(world - r.origin, r.direction);
      const double slack =
          units_per_pixel(frame, world) * (options.edge_tolerance + 1.0);
      if (face_hit && t > face_hit->distance + slack) continue;
      best_screen = screen;
      pick_hit hit{target.node,
                   target.mesh,
                   no_topology,
                   no_topology,
                   static_cast<std::uint32_t>(seg),
                   mesh.segment_edges.empty() ? no_topology : mesh.segment_edges[seg],
                   world,
                   t};
      if (face_hit && face_hit->node == target.node) {
        hit.triangle = face_hit->triangle;
        hit.face = face_hit->face;
      }
      edge_hit = hit;
    }
  }
  return edge_hit ? edge_hit : face_hit;
}

}  // namespace avionix
