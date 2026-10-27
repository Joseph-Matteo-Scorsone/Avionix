module;

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

export module avionix.entity.spatial;

// Document-space math for the 3D viewport.
//
// Conventions, shared by every viewport module:
//
//   - Right-handed coordinates. +Z is up, +X is right in the front view,
//     +Y points away from the viewer in the front view.
//   - Lengths are document units chosen by the host (see scene_settings).
//     The viewport never converts units and never derives a length from
//     window size.
//   - Angles are radians.
//   - Transforms, cameras, and picking use double. Vertex storage uses
//     float (vec3f) because meshes are expressed in their local frame, where
//     float precision is adequate. Model-view matrices are composed in
//     double before the cast to float, so large world coordinates do not
//     jitter.
//   - mat4 is column-major: element (row r, column c) is m[c * 4 + r], and
//     vectors are columns (p' = M * p).

export namespace avionix {

inline constexpr double pi = 3.14159265358979323846;

[[nodiscard]] constexpr double radians(double degrees) noexcept {
  return degrees * pi / 180.0;
}
[[nodiscard]] constexpr double degrees(double rad) noexcept { return rad * 180.0 / pi; }

struct vec2 {
  double x{};
  double y{};

  friend constexpr bool operator==(const vec2&, const vec2&) noexcept = default;
  friend constexpr vec2 operator+(vec2 a, vec2 b) noexcept {
    return {a.x + b.x, a.y + b.y};
  }
  friend constexpr vec2 operator-(vec2 a, vec2 b) noexcept {
    return {a.x - b.x, a.y - b.y};
  }
  friend constexpr vec2 operator*(vec2 a, double s) noexcept {
    return {a.x * s, a.y * s};
  }
};

[[nodiscard]] constexpr double dot(vec2 a, vec2 b) noexcept {
  return a.x * b.x + a.y * b.y;
}
[[nodiscard]] inline double length(vec2 a) noexcept { return std::sqrt(dot(a, a)); }

struct vec3 {
  double x{};
  double y{};
  double z{};

  friend constexpr bool operator==(const vec3&, const vec3&) noexcept = default;
  friend constexpr vec3 operator+(vec3 a, vec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
  }
  friend constexpr vec3 operator-(vec3 a, vec3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
  }
  friend constexpr vec3 operator-(vec3 a) noexcept { return {-a.x, -a.y, -a.z}; }
  friend constexpr vec3 operator*(vec3 a, double s) noexcept {
    return {a.x * s, a.y * s, a.z * s};
  }
  friend constexpr vec3 operator*(double s, vec3 a) noexcept { return a * s; }
  friend constexpr vec3 operator/(vec3 a, double s) noexcept {
    return {a.x / s, a.y / s, a.z / s};
  }
  constexpr vec3& operator+=(vec3 b) noexcept { return *this = *this + b; }
  constexpr vec3& operator-=(vec3 b) noexcept { return *this = *this - b; }

  [[nodiscard]] constexpr double operator[](std::size_t i) const noexcept {
    return i == 0 ? x : (i == 1 ? y : z);
  }
  [[nodiscard]] constexpr double& operator[](std::size_t i) noexcept {
    return i == 0 ? x : (i == 1 ? y : z);
  }
};

[[nodiscard]] constexpr vec3 hadamard(vec3 a, vec3 b) noexcept {
  return {a.x * b.x, a.y * b.y, a.z * b.z};
}
[[nodiscard]] constexpr double dot(vec3 a, vec3 b) noexcept {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] constexpr vec3 cross(vec3 a, vec3 b) noexcept {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline double length(vec3 a) noexcept { return std::sqrt(dot(a, a)); }
// Returns the zero vector for a zero-length input instead of NaN.
[[nodiscard]] inline vec3 normalize(vec3 a) noexcept {
  const double len = length(a);
  return len > 0.0 ? a / len : vec3{};
}
[[nodiscard]] constexpr vec3 lerp(vec3 a, vec3 b, double t) noexcept {
  return a + (b - a) * t;
}

inline constexpr vec3 axis_x{1, 0, 0};
inline constexpr vec3 axis_y{0, 1, 0};
inline constexpr vec3 axis_z{0, 0, 1};

// Vertex storage. Meshes use float to halve upload size; see the
// conventions at the top of this module.
struct vec3f {
  float x{};
  float y{};
  float z{};

  friend constexpr bool operator==(const vec3f&, const vec3f&) noexcept = default;
};

[[nodiscard]] constexpr vec3 widen(vec3f v) noexcept { return {v.x, v.y, v.z}; }
[[nodiscard]] constexpr vec3f narrow(vec3 v) noexcept {
  return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

struct vec4 {
  double x{};
  double y{};
  double z{};
  double w{};

  friend constexpr bool operator==(const vec4&, const vec4&) noexcept = default;
};

// Unit quaternion for rotations. Identity by default.
struct quat {
  double w{1};
  double x{};
  double y{};
  double z{};

  friend constexpr bool operator==(const quat&, const quat&) noexcept = default;

  // Hamilton product: (a * b) applies b first, then a.
  friend constexpr quat operator*(quat a, quat b) noexcept {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
  }
};

[[nodiscard]] inline quat axis_angle(vec3 axis, double angle) noexcept {
  const vec3 n = normalize(axis);
  const double s = std::sin(angle * 0.5);
  return {std::cos(angle * 0.5), n.x * s, n.y * s, n.z * s};
}
[[nodiscard]] constexpr quat conjugate(quat q) noexcept {
  return {q.w, -q.x, -q.y, -q.z};
}
[[nodiscard]] inline quat normalize(quat q) noexcept {
  const double len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  return len > 0.0 ? quat{q.w / len, q.x / len, q.y / len, q.z / len} : quat{};
}
[[nodiscard]] constexpr vec3 rotate(quat q, vec3 v) noexcept {
  const vec3 u{q.x, q.y, q.z};
  const vec3 t = cross(u, v) * 2.0;
  return v + t * q.w + cross(u, t);
}
// Shortest-arc spherical interpolation.
[[nodiscard]] inline quat slerp(quat a, quat b, double t) noexcept {
  double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
  if (d < 0.0) {
    b = {-b.w, -b.x, -b.y, -b.z};
    d = -d;
  }
  if (d > 0.9995) {
    return normalize(quat{a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t,
                          a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t});
  }
  const double theta = std::acos(d);
  const double sa = std::sin((1.0 - t) * theta) / std::sin(theta);
  const double sb = std::sin(t * theta) / std::sin(theta);
  return {a.w * sa + b.w * sb, a.x * sa + b.x * sb, a.y * sa + b.y * sb,
          a.z * sa + b.z * sb};
}

struct mat4 {
  std::array<double, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  friend constexpr bool operator==(const mat4&, const mat4&) noexcept = default;

  [[nodiscard]] constexpr double at(std::size_t row, std::size_t col) const noexcept {
    return m[col * 4 + row];
  }
  constexpr double& at(std::size_t row, std::size_t col) noexcept {
    return m[col * 4 + row];
  }

  friend constexpr mat4 operator*(const mat4& a, const mat4& b) noexcept {
    mat4 r;
    for (std::size_t c = 0; c < 4; ++c) {
      for (std::size_t row = 0; row < 4; ++row) {
        double sum = 0.0;
        for (std::size_t k = 0; k < 4; ++k) sum += a.at(row, k) * b.at(k, c);
        r.at(row, c) = sum;
      }
    }
    return r;
  }
};

[[nodiscard]] constexpr vec3 transform_point(const mat4& a, vec3 p) noexcept {
  const double w = a.at(3, 0) * p.x + a.at(3, 1) * p.y + a.at(3, 2) * p.z + a.at(3, 3);
  const vec3 r{a.at(0, 0) * p.x + a.at(0, 1) * p.y + a.at(0, 2) * p.z + a.at(0, 3),
               a.at(1, 0) * p.x + a.at(1, 1) * p.y + a.at(1, 2) * p.z + a.at(1, 3),
               a.at(2, 0) * p.x + a.at(2, 1) * p.y + a.at(2, 2) * p.z + a.at(2, 3)};
  return (w != 0.0 && w != 1.0) ? r / w : r;
}
[[nodiscard]] constexpr vec3 transform_vector(const mat4& a, vec3 v) noexcept {
  return {a.at(0, 0) * v.x + a.at(0, 1) * v.y + a.at(0, 2) * v.z,
          a.at(1, 0) * v.x + a.at(1, 1) * v.y + a.at(1, 2) * v.z,
          a.at(2, 0) * v.x + a.at(2, 1) * v.y + a.at(2, 2) * v.z};
}
[[nodiscard]] constexpr vec4 transform(const mat4& a, vec4 v) noexcept {
  return {a.at(0, 0) * v.x + a.at(0, 1) * v.y + a.at(0, 2) * v.z + a.at(0, 3) * v.w,
          a.at(1, 0) * v.x + a.at(1, 1) * v.y + a.at(1, 2) * v.z + a.at(1, 3) * v.w,
          a.at(2, 0) * v.x + a.at(2, 1) * v.y + a.at(2, 2) * v.z + a.at(2, 3) * v.w,
          a.at(3, 0) * v.x + a.at(3, 1) * v.y + a.at(3, 2) * v.z + a.at(3, 3) * v.w};
}
[[nodiscard]] constexpr vec3 translation_of(const mat4& a) noexcept {
  return {a.at(0, 3), a.at(1, 3), a.at(2, 3)};
}

[[nodiscard]] constexpr mat4 translation(vec3 t) noexcept {
  mat4 r;
  r.at(0, 3) = t.x;
  r.at(1, 3) = t.y;
  r.at(2, 3) = t.z;
  return r;
}
[[nodiscard]] constexpr mat4 scaling(vec3 s) noexcept {
  mat4 r;
  r.at(0, 0) = s.x;
  r.at(1, 1) = s.y;
  r.at(2, 2) = s.z;
  return r;
}
[[nodiscard]] constexpr mat4 rotation(quat q) noexcept {
  mat4 r;
  const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  r.at(0, 0) = 1 - 2 * (yy + zz);
  r.at(0, 1) = 2 * (xy - wz);
  r.at(0, 2) = 2 * (xz + wy);
  r.at(1, 0) = 2 * (xy + wz);
  r.at(1, 1) = 1 - 2 * (xx + zz);
  r.at(1, 2) = 2 * (yz - wx);
  r.at(2, 0) = 2 * (xz - wy);
  r.at(2, 1) = 2 * (yz + wx);
  r.at(2, 2) = 1 - 2 * (xx + yy);
  return r;
}

[[nodiscard]] constexpr mat4 transpose(const mat4& a) noexcept {
  mat4 r;
  for (std::size_t row = 0; row < 4; ++row)
    for (std::size_t c = 0; c < 4; ++c) r.at(row, c) = a.at(c, row);
  return r;
}

// General inverse by cofactor expansion. Returns identity for a singular
// matrix; callers that can produce one (zero scale) check determinant().
[[nodiscard]] constexpr mat4 inverse(const mat4& a) noexcept {
  const auto& m = a.m;
  std::array<double, 16> inv{};
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
           m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
           m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
           m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
            m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
           m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
           m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
           m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
            m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
           m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
           m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
            m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
            m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
           m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
           m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
            m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
            m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (det == 0.0) return mat4{};
  mat4 r;
  for (std::size_t i = 0; i < 16; ++i) r.m[i] = inv[i] / det;
  return r;
}

// OpenGL clip conventions: NDC depth in [-1, 1], camera looks down -Z in
// eye space.
[[nodiscard]] inline mat4 perspective(double vertical_fov, double aspect,
                                      double near_plane, double far_plane) noexcept {
  const double f = 1.0 / std::tan(vertical_fov * 0.5);
  mat4 r{};
  r.m.fill(0.0);
  r.at(0, 0) = f / aspect;
  r.at(1, 1) = f;
  r.at(2, 2) = (far_plane + near_plane) / (near_plane - far_plane);
  r.at(2, 3) = 2.0 * far_plane * near_plane / (near_plane - far_plane);
  r.at(3, 2) = -1.0;
  return r;
}
[[nodiscard]] constexpr mat4 orthographic(double half_width, double half_height,
                                          double near_plane,
                                          double far_plane) noexcept {
  mat4 r;
  r.at(0, 0) = 1.0 / half_width;
  r.at(1, 1) = 1.0 / half_height;
  r.at(2, 2) = -2.0 / (far_plane - near_plane);
  r.at(2, 3) = -(far_plane + near_plane) / (far_plane - near_plane);
  return r;
}

// Translation, rotation, scale. The matrix is T * R * S: scale in the local
// frame, then rotate, then translate into the parent frame. Non-uniform scale
// is applied along local axes, so composing a rotated child under a
// non-uniformly scaled parent can shear; the viewport only produces
// non-uniform scale on the node being edited.
struct transform3d {
  vec3 translation{};
  quat rotation{};
  vec3 scale{1, 1, 1};

  friend constexpr bool operator==(const transform3d&,
                                   const transform3d&) noexcept = default;

  [[nodiscard]] constexpr mat4 matrix() const noexcept {
    return avionix::translation(translation) * avionix::rotation(rotation) *
           avionix::scaling(scale);
  }
};

[[nodiscard]] inline transform3d interpolate(const transform3d& a, const transform3d& b,
                                             double t) noexcept {
  return {lerp(a.translation, b.translation, t), slerp(a.rotation, b.rotation, t),
          lerp(a.scale, b.scale, t)};
}

// Axis-aligned bounds. Default-constructed bounds are empty and absorb the
// first point extended into them.
struct aabb {
  vec3 min{std::numeric_limits<double>::infinity(),
           std::numeric_limits<double>::infinity(),
           std::numeric_limits<double>::infinity()};
  vec3 max{-std::numeric_limits<double>::infinity(),
           -std::numeric_limits<double>::infinity(),
           -std::numeric_limits<double>::infinity()};

  [[nodiscard]] constexpr bool empty() const noexcept {
    return min.x > max.x || min.y > max.y || min.z > max.z;
  }
  constexpr void extend(vec3 p) noexcept {
    min = {p.x < min.x ? p.x : min.x, p.y < min.y ? p.y : min.y,
           p.z < min.z ? p.z : min.z};
    max = {p.x > max.x ? p.x : max.x, p.y > max.y ? p.y : max.y,
           p.z > max.z ? p.z : max.z};
  }
  constexpr void extend(const aabb& other) noexcept {
    if (other.empty()) return;
    extend(other.min);
    extend(other.max);
  }
  [[nodiscard]] constexpr vec3 center() const noexcept { return (min + max) * 0.5; }
  [[nodiscard]] constexpr vec3 extent() const noexcept { return max - min; }
  [[nodiscard]] inline double radius() const noexcept {
    return empty() ? 0.0 : length(max - min) * 0.5;
  }
};

// Bounds of the eight transformed corners.
[[nodiscard]] constexpr aabb transformed(const aabb& box, const mat4& m) noexcept {
  aabb out;
  if (box.empty()) return out;
  for (std::uint32_t i = 0; i < 8; ++i) {
    out.extend(transform_point(
        m, {(i & 1U) ? box.max.x : box.min.x, (i & 2U) ? box.max.y : box.min.y,
            (i & 4U) ? box.max.z : box.min.z}));
  }
  return out;
}

// A pick ray. Points are origin + direction * t for t >= t_min. Perspective
// rays start at the eye (t_min = 0); orthographic rays extend behind the
// image plane (t_min = -infinity) because the orthographic near plane may
// lie behind the eye position.
struct ray {
  vec3 origin{};
  vec3 direction{0, 0, -1};
  double t_min{0.0};

  [[nodiscard]] constexpr vec3 at(double t) const noexcept {
    return origin + direction * t;
  }
};

// Points p with dot(normal, p) == offset. A section plane keeps the side
// where dot(normal, p) <= offset and removes the rest.
struct plane {
  vec3 normal{0, 0, 1};
  double offset{};

  friend constexpr bool operator==(const plane&, const plane&) noexcept = default;

  [[nodiscard]] constexpr double signed_distance(vec3 p) const noexcept {
    return dot(normal, p) - offset;
  }
};

[[nodiscard]] inline plane plane_through(vec3 point, vec3 normal) noexcept {
  const vec3 n = normalize(normal);
  return {n, dot(n, point)};
}

// Ray parameter where the ray meets the plane, or NaN when parallel.
[[nodiscard]] inline double intersect(const ray& r, const plane& p) noexcept {
  const double denom = dot(p.normal, r.direction);
  if (std::abs(denom) < 1e-12) return std::numeric_limits<double>::quiet_NaN();
  return (p.offset - dot(p.normal, r.origin)) / denom;
}

}  // namespace avionix
