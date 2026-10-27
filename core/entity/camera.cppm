module;

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

export module avionix.entity.camera;

export import avionix.entity.spatial;
export import avionix.entity.image;

// Viewport camera. A camera is a value in document coordinates; it holds no
// pixel sizes. The same camera rendered into a 640x480 window, a 4K window,
// or a 512x512 capture frames the same document region vertically, and the
// output aspect ratio only widens or narrows the horizontal extent.
//
//   camera (document units) ──┬─ pixel_extent ──► view_frame
//                             └─ scene bounds ──► clip planes
//
// The camera orbits `target` on a turntable: yaw turns around +Z, pitch
// tilts toward +Z, and the horizon never rolls. Pitch is limited to
// [-90°, 90°], so straight top and bottom views are valid.

export namespace avionix {

enum class projection_kind : std::uint8_t { perspective, orthographic };

struct camera {
  projection_kind projection{projection_kind::perspective};
  vec3 target{};
  // Eye-to-target distance in document units.
  double distance{200.0};
  // Radians. yaw 0 places the eye on +X; yaw -90° on -Y (front view).
  double yaw{radians(-60.0)};
  double pitch{radians(25.0)};
  double vertical_fov{radians(40.0)};
  // Document units visible from the bottom to the top edge in orthographic
  // projection.
  double ortho_height{150.0};

  friend constexpr bool operator==(const camera&, const camera&) noexcept = default;
};

enum class standard_view : std::uint8_t {
  front,
  back,
  left,
  right,
  top,
  bottom,
  isometric
};

[[nodiscard]] inline camera oriented(camera c, standard_view view) noexcept {
  switch (view) {
    case standard_view::front:
      c.yaw = radians(-90.0), c.pitch = 0.0;
      break;
    case standard_view::back:
      c.yaw = radians(90.0), c.pitch = 0.0;
      break;
    case standard_view::left:
      c.yaw = radians(180.0), c.pitch = 0.0;
      break;
    case standard_view::right:
      c.yaw = 0.0, c.pitch = 0.0;
      break;
    case standard_view::top:
      c.yaw = radians(-90.0), c.pitch = radians(90.0);
      break;
    case standard_view::bottom:
      c.yaw = radians(-90.0), c.pitch = radians(-90.0);
      break;
    case standard_view::isometric:
      c.yaw = radians(-45.0), c.pitch = std::atan(1.0 / std::sqrt(2.0));
      break;
  }
  return c;
}

// Unit vector from the target toward the eye.
[[nodiscard]] inline vec3 eye_direction(const camera& c) noexcept {
  return {std::cos(c.pitch) * std::cos(c.yaw), std::cos(c.pitch) * std::sin(c.yaw),
          std::sin(c.pitch)};
}
[[nodiscard]] inline vec3 eye_position(const camera& c) noexcept {
  return c.target + eye_direction(c) * c.distance;
}

// Everything derived from a camera for one output size. Rebuild it when
// the camera, the output size, or the scene bounds change.
struct view_frame {
  pixel_extent extent{};
  projection_kind projection{};
  vec3 eye{};
  vec3 right{1, 0, 0};
  vec3 up{0, 0, 1};
  vec3 forward{0, 1, 0};
  double near_plane{0.1};
  double far_plane{1000.0};
  double vertical_fov{};
  double ortho_height{};
  mat4 view{};
  mat4 projection_matrix{};
  mat4 view_projection{};
};

// Clip planes that enclose `scene` with a small margin. Perspective near
// planes stay positive and no closer than far / 10^5 to keep depth
// precision; orthographic near planes may be negative.
[[nodiscard]] inline view_frame frame_for(const camera& c, pixel_extent extent,
                                          const aabb& scene) noexcept {
  view_frame f;
  f.extent = extent;
  f.projection = c.projection;
  f.vertical_fov = c.vertical_fov;
  f.ortho_height = c.ortho_height;
  const vec3 back = eye_direction(c);
  f.eye = c.target + back * c.distance;
  f.right = {-std::sin(c.yaw), std::cos(c.yaw), 0.0};
  f.up = cross(back, f.right);
  f.forward = -back;

  f.view.at(0, 0) = f.right.x, f.view.at(0, 1) = f.right.y, f.view.at(0, 2) = f.right.z;
  f.view.at(1, 0) = f.up.x, f.view.at(1, 1) = f.up.y, f.view.at(1, 2) = f.up.z;
  f.view.at(2, 0) = back.x, f.view.at(2, 1) = back.y, f.view.at(2, 2) = back.z;
  f.view.at(0, 3) = -dot(f.right, f.eye);
  f.view.at(1, 3) = -dot(f.up, f.eye);
  f.view.at(2, 3) = -dot(back, f.eye);

  double near_plane = c.distance * 0.01;
  double far_plane = c.distance * 100.0;
  if (!scene.empty()) {
    const double r = std::max(scene.radius(), 1e-9) * 1.02;
    const double dc = dot(scene.center() - f.eye, f.forward);
    far_plane = dc + r;
    near_plane = dc - r;
  }
  if (c.projection == projection_kind::perspective) {
    far_plane = std::max(far_plane, 1e-6);
    near_plane = std::max(near_plane, far_plane * 1e-5);
  }
  if (far_plane - near_plane < 1e-9) far_plane = near_plane + 1e-3;
  f.near_plane = near_plane;
  f.far_plane = far_plane;

  const double aspect = extent.aspect();
  if (c.projection == projection_kind::perspective) {
    f.projection_matrix = perspective(c.vertical_fov, aspect, near_plane, far_plane);
  } else {
    const double hh = c.ortho_height * 0.5;
    f.projection_matrix = orthographic(hh * aspect, hh, near_plane, far_plane);
  }
  f.view_projection = f.projection_matrix * f.view;
  return f;
}

// Pixel coordinates are continuous framebuffer coordinates: (0, 0) is the
// top-left corner of the top-left pixel and (width, height) the opposite
// corner.
[[nodiscard]] inline ray ray_through(const view_frame& f, vec2 pixel) noexcept {
  const double w = std::max(1.0, static_cast<double>(f.extent.width));
  const double h = std::max(1.0, static_cast<double>(f.extent.height));
  const double nx = 2.0 * pixel.x / w - 1.0;
  const double ny = 1.0 - 2.0 * pixel.y / h;
  const double aspect = f.extent.aspect();
  if (f.projection == projection_kind::perspective) {
    const double t = std::tan(f.vertical_fov * 0.5);
    return {f.eye, normalize(f.forward + f.right * (nx * t * aspect) + f.up * (ny * t)),
            0.0};
  }
  const double hh = f.ortho_height * 0.5;
  return {f.eye + f.right * (nx * hh * aspect) + f.up * (ny * hh), f.forward,
          -std::numeric_limits<double>::infinity()};
}

// Framebuffer pixel position and NDC depth of a world point, or nullopt
// when the point is behind a perspective eye.
[[nodiscard]] inline std::optional<vec3> project(const view_frame& f,
                                                 vec3 world) noexcept {
  const vec4 clip = transform(f.view_projection, {world.x, world.y, world.z, 1.0});
  if (clip.w <= 1e-12) return std::nullopt;
  const double nx = clip.x / clip.w;
  const double ny = clip.y / clip.w;
  return vec3{(nx + 1.0) * 0.5 * f.extent.width, (1.0 - ny) * 0.5 * f.extent.height,
              clip.z / clip.w};
}

// Document units covered by one framebuffer pixel at `world`'s depth.
[[nodiscard]] inline double units_per_pixel(const view_frame& f, vec3 world) noexcept {
  const double h = std::max(1.0, static_cast<double>(f.extent.height));
  if (f.projection == projection_kind::orthographic) return f.ortho_height / h;
  const double depth = std::max(dot(world - f.eye, f.forward), 1e-9);
  return 2.0 * depth * std::tan(f.vertical_fov * 0.5) / h;
}

// Turntable orbit. Angles in radians; positive yaw turns the eye
// counter-clockwise seen from +Z.
[[nodiscard]] inline camera orbit(camera c, double delta_yaw,
                                  double delta_pitch) noexcept {
  c.yaw = std::remainder(c.yaw + delta_yaw, 2.0 * pi);
  c.pitch = std::clamp(c.pitch + delta_pitch, -pi * 0.5, pi * 0.5);
  return c;
}

// Moves the target so the scene follows a pointer drag of `delta` pixels.
[[nodiscard]] inline camera pan(camera c, const view_frame& f, vec2 delta) noexcept {
  const double upp = units_per_pixel(f, c.target);
  c.target = c.target - f.right * (delta.x * upp) + f.up * (delta.y * upp);
  return c;
}

// Zooms by `factor` (< 1 moves closer) and keeps the document point under
// `pixel` fixed on screen.
[[nodiscard]] inline camera zoom(camera c, const view_frame& f, double factor,
                                 vec2 pixel) noexcept {
  factor = std::clamp(factor, 1e-3, 1e3);
  const ray r = ray_through(f, pixel);
  const double t = intersect(r, plane_through(c.target, f.forward));
  const vec3 anchor = std::isfinite(t) ? r.at(t) : c.target;
  c.target = anchor + (c.target - anchor) * factor;
  if (c.projection == projection_kind::perspective) {
    c.distance = std::max(c.distance * factor, 1e-6);
  } else {
    c.ortho_height = std::max(c.ortho_height * factor, 1e-9);
  }
  return c;
}

// Switches projection and keeps the apparent size of objects at the
// target.
[[nodiscard]] inline camera with_projection(camera c, projection_kind kind) noexcept {
  if (kind == c.projection) return c;
  const double t = 2.0 * std::tan(c.vertical_fov * 0.5);
  if (kind == projection_kind::orthographic) {
    c.ortho_height = c.distance * t;
  } else {
    c.distance = std::max(c.ortho_height / t, 1e-6);
  }
  c.projection = kind;
  return c;
}

// Frames `box` for an output with aspect ratio `aspect`, keeping the
// orientation. `margin` > 1 leaves space around the bounds. Empty bounds
// leave the camera unchanged.
[[nodiscard]] inline camera fit(camera c, const aabb& box, double aspect,
                                double margin = 1.15) noexcept {
  if (box.empty()) return c;
  aspect = aspect > 0.0 ? aspect : 1.0;
  const double r = std::max(box.radius(), 1e-6) * margin;
  c.target = box.center();
  const double half_v = c.vertical_fov * 0.5;
  const double half_h = std::atan(std::tan(half_v) * aspect);
  c.distance = r / std::sin(std::min(half_v, half_h));
  c.ortho_height = 2.0 * r * std::max(1.0, 1.0 / aspect);
  return c;
}

}  // namespace avionix
