module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

export module avionix.object.software_renderer;

import avionix.entity.viewport;
import avionix.object.scene_store;
import avionix.object.scene_renderer;

// CPU rendering of the same frame_description the OpenGL renderer draws,
// for terminals without a graphics context (SSH sessions, headless Linux,
// machines without OpenGL 3.3). It reproduces the GL passes: background
// gradient, lit faces with two-sided interiors, translucency, section
// planes, depth-tested edges, overlays, and screen-space handles.
//
//   frame_description + scene_store meshes
//        └─► near-plane clipping ─► triangle and line rasterization with a
//            depth buffer ─► supersample resolve ─► image_rgba8
//
// Cost is linear in covered pixels and primitives; there is no
// multithreading. Buffers are reused between calls.

export namespace avionix {

class software_renderer {
 public:
  // Renders at frame.view.extent. `supersample` > 1 renders that many
  // samples per axis and box-filters them, for smooth edges.
  [[nodiscard]] image_rgba8 render(const frame_description& frame,
                                   const scene_store& store,
                                   std::uint32_t supersample = 2) {
    const std::uint32_t out_w = frame.view.extent.width;
    const std::uint32_t out_h = frame.view.extent.height;
    ss_ = std::clamp<std::uint32_t>(supersample, 1, 4);
    w_ = out_w * ss_;
    h_ = out_h * ss_;
    const std::size_t n = std::size_t{w_} * h_;
    color_.assign(n, {});
    // Coverage: 0 where nothing is drawn over a transparent background.
    alpha_.assign(n, frame.transparent_background ? 0.0F : 1.0F);
    depth_.assign(n, std::numeric_limits<float>::infinity());
    frame_ = &frame;
    view_ = frame.view.view;
    proj_ = frame.view.projection_matrix;
    planes_.clear();
    for (const plane& p : frame.sections) {
      const vec3 nrm = normalize(transform_vector(view_, p.normal));
      planes_.push_back({nrm, p.offset + dot(nrm, translation_of(view_))});
    }

    for (std::uint32_t y = 0; y < h_ && !frame.transparent_background; ++y) {
      const float t =
          h_ > 1 ? static_cast<float>(y) / static_cast<float>(h_ - 1) : 0.0F;
      const rgb c{lerp(frame.background_top.r, frame.background_bottom.r, t),
                  lerp(frame.background_top.g, frame.background_bottom.g, t),
                  lerp(frame.background_top.b, frame.background_bottom.b, t)};
      std::fill(color_.begin() + static_cast<std::ptrdiff_t>(std::size_t{y} * w_),
                color_.begin() + static_cast<std::ptrdiff_t>(std::size_t{y + 1} * w_),
                c);
    }

    switch (frame.mode) {
      case display_mode::shaded:
      case display_mode::shaded_edges:
        faces(store, pass::opaque, 1.0F);
        faces(store, pass::translucent, 1.0F);
        edges(store, frame.mode == display_mode::shaded, true);
        break;
      case display_mode::wireframe:
        faces(store, pass::depth_only, 1.0F);
        edges(store, false, true);
        break;
      case display_mode::xray:
        faces(store, pass::translucent_all, 0.18F);
        edges(store, false, false);
        break;
    }
    for (const world_line& l : frame.lines_depth) line(l.a, l.b, l.color, true, false);
    for (const world_line& l : frame.lines_on_top)
      line(l.a, l.b, l.color, false, false);
    screen(frame.screen);
    return resolve(out_w, out_h);
  }

 private:
  struct rgb {
    float r{};
    float g{};
    float b{};
  };
  enum class pass : std::uint8_t { opaque, translucent, depth_only, translucent_all };

  struct vertex {
    vec4 clip{};
    vec3 eye{};
    vec3 normal{};
  };

  static float lerp(float a, float b, float t) { return a + (b - a) * t; }

  [[nodiscard]] bool clipped(vec3 eye) const {
    return std::ranges::any_of(
        planes_, [&](const plane& p) { return dot(p.normal, eye) > p.offset; });
  }

  [[nodiscard]] vertex make_vertex(const mat4& mv, const mat4& nm, vec3f p,
                                   vec3 n) const {
    const vec3 eye = transform_point(mv, widen(p));
    return {transform(proj_, {eye.x, eye.y, eye.z, 1.0}), eye,
            normalize(transform_vector(nm, n))};
  }

  // Sutherland-Hodgman against the near plane (z >= -w in clip space).
  static std::size_t clip_near(const std::array<vertex, 3>& in,
                               std::array<vertex, 4>& out) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < 3; ++i) {
      const vertex& a = in[i];
      const vertex& b = in[(i + 1) % 3];
      const double da = a.clip.z + a.clip.w;
      const double db = b.clip.z + b.clip.w;
      if (da >= 0) out[count++] = a;
      if ((da >= 0) != (db >= 0)) {
        const double t = da / (da - db);
        out[count++] = {
            {a.clip.x + (b.clip.x - a.clip.x) * t, a.clip.y + (b.clip.y - a.clip.y) * t,
             a.clip.z + (b.clip.z - a.clip.z) * t,
             a.clip.w + (b.clip.w - a.clip.w) * t},
            avionix::lerp(a.eye, b.eye, t),
            avionix::lerp(a.normal, b.normal, t)};
      }
    }
    return count;
  }

  [[nodiscard]] vec3 to_screen(const vec4& c) const {
    const double iw = 1.0 / c.w;
    return {(c.x * iw + 1.0) * 0.5 * w_, (1.0 - c.y * iw) * 0.5 * h_, c.z * iw};
  }

  [[nodiscard]] rgb shade(const material& look, float highlight, vec3 n, vec3 eye,
                          bool front) const {
    const vec3 v = normalize(-eye);
    const double interior = front ? 1.0 : 0.55;
    if (dot(n, v) < 0.0) n = -n;
    const vec3 key = normalize(vec3{0.35, 0.55, 0.75});
    const double diffuse =
        0.55 * std::max(dot(n, key), 0.0) + 0.35 * std::max(dot(n, v), 0.0);
    const vec3 h = normalize(key + v);
    const double shin = look.shininess;
    const double spec =
        std::pow(std::max(dot(n, h), 0.0), 8.0 + (96.0 - 8.0) * shin) * 0.3 * shin;
    const double mix = highlight * 0.55;
    const rgba hl = frame_->highlight;
    const auto channel = [&](float base, float hi) {
      const double c = (base + (hi - base) * mix) * interior;
      return static_cast<float>(std::min(1.0, c * (0.22 + diffuse) + spec));
    };
    return {channel(look.base_color.r, hl.r), channel(look.base_color.g, hl.g),
            channel(look.base_color.b, hl.b)};
  }

  // Straight-alpha "over": the result's coverage is a + d(1 - a), and its
  // color is the coverage-weighted mix, so translucent geometry over a
  // transparent background keeps its own color.
  void blend(std::size_t i, rgb c, float alpha) {
    rgb& d = color_[i];
    const float da = alpha_[i];
    const float out = alpha + da * (1.0F - alpha);
    if (out <= 0.0F) return;
    const float keep = da * (1.0F - alpha);
    d = {(c.r * alpha + d.r * keep) / out, (c.g * alpha + d.g * keep) / out,
         (c.b * alpha + d.b * keep) / out};
    alpha_[i] = out;
  }

  void triangle(const std::array<vertex, 3>& tri, const material& look, float highlight,
                pass mode, float alpha) {
    std::array<vertex, 4> poly{};
    const std::size_t count = clip_near(tri, poly);
    for (std::size_t k = 1; k + 1 < count; ++k)
      raster(poly[0], poly[k], poly[k + 1], look, highlight, mode, alpha);
  }

  void raster(const vertex& a, const vertex& b, const vertex& c, const material& look,
              float highlight, pass mode, float alpha) {
    const vec3 sa = to_screen(a.clip);
    const vec3 sb = to_screen(b.clip);
    const vec3 sc = to_screen(c.clip);
    const double area = (sb.x - sa.x) * (sc.y - sa.y) - (sb.y - sa.y) * (sc.x - sa.x);
    if (std::abs(area) < 1e-12) return;
    const bool front = area < 0.0;  // counter-clockwise in a y-down image
    const auto x0 = static_cast<std::int64_t>(
        std::max(0.0, std::floor(std::min({sa.x, sb.x, sc.x}))));
    const auto x1 = static_cast<std::int64_t>(
        std::min<double>(w_ - 1, std::ceil(std::max({sa.x, sb.x, sc.x}))));
    const auto y0 = static_cast<std::int64_t>(
        std::max(0.0, std::floor(std::min({sa.y, sb.y, sc.y}))));
    const auto y1 = static_cast<std::int64_t>(
        std::min<double>(h_ - 1, std::ceil(std::max({sa.y, sb.y, sc.y}))));
    const double iwa = 1.0 / a.clip.w;
    const double iwb = 1.0 / b.clip.w;
    const double iwc = 1.0 / c.clip.w;
    // Faces sit slightly behind edges, like the GL polygon offset.
    constexpr double face_bias = 2e-5;
    for (std::int64_t y = y0; y <= y1; ++y) {
      for (std::int64_t x = x0; x <= x1; ++x) {
        const double px = static_cast<double>(x) + 0.5;
        const double py = static_cast<double>(y) + 0.5;
        double l0 = (sb.x - px) * (sc.y - py) - (sb.y - py) * (sc.x - px);
        double l1 = (sc.x - px) * (sa.y - py) - (sc.y - py) * (sa.x - px);
        double l2 = (sa.x - px) * (sb.y - py) - (sa.y - py) * (sb.x - px);
        l0 /= area;
        l1 /= area;
        l2 /= area;
        if (l0 < 0 || l1 < 0 || l2 < 0) continue;
        const double z = l0 * sa.z + l1 * sb.z + l2 * sc.z + face_bias;
        if (z < -1.0 || z > 1.0) continue;
        const std::size_t i =
            static_cast<std::size_t>(y) * w_ + static_cast<std::size_t>(x);
        const bool writes_depth = mode == pass::opaque || mode == pass::depth_only;
        if (z >= depth_[i]) continue;
        // Perspective-correct attributes.
        const double q0 = l0 * iwa;
        const double q1 = l1 * iwb;
        const double q2 = l2 * iwc;
        const double qs = q0 + q1 + q2;
        const vec3 eye = (a.eye * q0 + b.eye * q1 + c.eye * q2) / qs;
        if (clipped(eye)) continue;
        if (writes_depth) depth_[i] = static_cast<float>(z);
        if (mode == pass::depth_only) continue;
        const vec3 n = normalize((a.normal * q0 + b.normal * q1 + c.normal * q2) / qs);
        const rgb col = shade(look, highlight, n, eye, front);
        if (mode == pass::opaque) {
          color_[i] = col;
          alpha_[i] = 1.0F;
        } else {
          blend(i, col, alpha);
        }
      }
    }
  }

  void faces(const scene_store& store, pass mode, float alpha_scale) {
    for (const draw_item& item : frame_->items) {
      const auto* m = store.mesh(item.mesh);
      if (m == nullptr) continue;
      const float alpha = item.look.base_color.a * alpha_scale;
      const bool translucent = alpha < 0.999F;
      if (mode == pass::opaque && translucent) continue;
      if (mode == pass::translucent && !translucent) continue;
      const mat4 mv = view_ * item.world;
      const mat4 nm = transpose(inverse(mv));
      const mesh_data& d = m->data;
      for (std::size_t t = 0; t < d.triangle_count(); ++t) {
        const std::array<std::uint32_t, 3> idx{d.indices[t * 3], d.indices[t * 3 + 1],
                                               d.indices[t * 3 + 2]};
        const vec3 face =
            normalize(cross(widen(d.positions[idx[1]]) - widen(d.positions[idx[0]]),
                            widen(d.positions[idx[2]]) - widen(d.positions[idx[0]])));
        std::array<vertex, 3> tri{};
        for (std::size_t k = 0; k < 3; ++k) {
          const vec3 n = d.normals.empty() ? face : widen(d.normals[idx[k]]);
          tri[k] = make_vertex(mv, nm, d.positions[idx[k]], n);
        }
        triangle(tri, item.look, item.highlight, mode, alpha);
      }
    }
  }

  void edges(const scene_store& store, bool selected_only, bool depth_tested) {
    for (const draw_item& item : frame_->items) {
      if (selected_only && item.highlight < 0.99F) continue;
      const auto* m = store.mesh(item.mesh);
      if (m == nullptr) continue;
      const rgba color =
          item.highlight >= 0.99F ? frame_->highlight : item.look.edge_color;
      const mesh_data& d = m->data;
      const auto seg = [&](std::uint32_t i, std::uint32_t j) {
        line(transform_point(item.world, widen(d.positions[i])),
             transform_point(item.world, widen(d.positions[j])), color, depth_tested,
             true);
      };
      if (!d.edge_indices.empty()) {
        for (std::size_t s = 0; s < d.segment_count(); ++s)
          seg(d.edge_indices[s * 2], d.edge_indices[s * 2 + 1]);
      } else if (frame_->mode == display_mode::wireframe ||
                 frame_->mode == display_mode::xray) {
        for (std::size_t t = 0; t < d.triangle_count(); ++t)
          for (std::size_t k = 0; k < 3; ++k)
            seg(d.indices[t * 3 + k], d.indices[t * 3 + (k + 1) % 3]);
      }
    }
  }

  // A one-sample-wide world-space line (thicker when supersampled, like
  // GL's 1 px lines after the resolve).
  void line(vec3 wa, vec3 wb, rgba color, bool depth_tested, bool sectioned) {
    vec3 ea = transform_point(view_, wa);
    vec3 eb = transform_point(view_, wb);
    vec4 ca = transform(proj_, {ea.x, ea.y, ea.z, 1.0});
    vec4 cb = transform(proj_, {eb.x, eb.y, eb.z, 1.0});
    const double da = ca.z + ca.w;
    const double db = cb.z + cb.w;
    if (da < 0 && db < 0) return;
    if ((da < 0) != (db < 0)) {
      const double t = da / (da - db);
      const vec4 cm{ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t,
                    ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t};
      const vec3 em = avionix::lerp(ea, eb, t);
      if (da < 0) {
        ca = cm;
        ea = em;
      } else {
        cb = cm;
        eb = em;
      }
    }
    const vec3 sa = to_screen(ca);
    const vec3 sb = to_screen(cb);
    const double steps = std::max({std::abs(sb.x - sa.x), std::abs(sb.y - sa.y), 1.0});
    const auto count = static_cast<std::int64_t>(std::ceil(steps));
    const rgb c{color.r, color.g, color.b};
    for (std::int64_t k = 0; k <= count; ++k) {
      const double t = static_cast<double>(k) / static_cast<double>(count);
      const double x = sa.x + (sb.x - sa.x) * t;
      const double y = sa.y + (sb.y - sa.y) * t;
      if (x < 0 || y < 0 || x >= w_ || y >= h_) continue;
      const double z = sa.z + (sb.z - sa.z) * t;
      const std::size_t i =
          static_cast<std::size_t>(y) * w_ + static_cast<std::size_t>(x);
      if (depth_tested && z > depth_[i] + 1e-5) continue;
      if (sectioned) {
        const double iwa = 1.0 / ca.w;
        const double iwb = 1.0 / cb.w;
        const double q = (1 - t) * iwa + t * iwb;
        if (clipped((ea * ((1 - t) * iwa) + eb * (t * iwb)) / q)) continue;
      }
      blend(i, c, color.a);
    }
  }

  void screen(std::span<const screen_vertex> vertices) {
    const auto s = static_cast<double>(ss_);
    for (std::size_t v = 0; v + 2 < vertices.size(); v += 3) {
      const vec2 a{vertices[v].x * s, vertices[v].y * s};
      const vec2 b{vertices[v + 1].x * s, vertices[v + 1].y * s};
      const vec2 c{vertices[v + 2].x * s, vertices[v + 2].y * s};
      const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
      if (std::abs(area) < 1e-12) continue;
      const rgba col = vertices[v].color;
      const auto x0 = static_cast<std::int64_t>(
          std::max(0.0, std::floor(std::min({a.x, b.x, c.x}))));
      const auto x1 = static_cast<std::int64_t>(
          std::min<double>(w_ - 1, std::ceil(std::max({a.x, b.x, c.x}))));
      const auto y0 = static_cast<std::int64_t>(
          std::max(0.0, std::floor(std::min({a.y, b.y, c.y}))));
      const auto y1 = static_cast<std::int64_t>(
          std::min<double>(h_ - 1, std::ceil(std::max({a.y, b.y, c.y}))));
      for (std::int64_t y = y0; y <= y1; ++y) {
        for (std::int64_t x = x0; x <= x1; ++x) {
          const double px = static_cast<double>(x) + 0.5;
          const double py = static_cast<double>(y) + 0.5;
          const double l0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
          const double l1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
          if (l0 < 0 || l1 < 0 || l0 + l1 > 1) continue;
          blend(static_cast<std::size_t>(y) * w_ + static_cast<std::size_t>(x),
                {col.r, col.g, col.b}, col.a);
        }
      }
    }
  }

  [[nodiscard]] image_rgba8 resolve(std::uint32_t out_w, std::uint32_t out_h) const {
    image_rgba8 out{out_w, out_h,
                    std::vector<std::uint8_t>(std::size_t{out_w} * out_h * 4)};
    const float samples = static_cast<float>(ss_ * ss_);
    for (std::uint32_t y = 0; y < out_h; ++y) {
      for (std::uint32_t x = 0; x < out_w; ++x) {
        // Coverage-weighted average: uncovered samples do not darken the
        // color, they only lower the alpha.
        rgb sum{};
        float coverage = 0.0F;
        for (std::uint32_t sy = 0; sy < ss_; ++sy) {
          for (std::uint32_t sx = 0; sx < ss_; ++sx) {
            const std::size_t i = std::size_t{y * ss_ + sy} * w_ + (x * ss_ + sx);
            const rgb& c = color_[i];
            const float a = alpha_[i];
            sum = {sum.r + c.r * a, sum.g + c.g * a, sum.b + c.b * a};
            coverage += a;
          }
        }
        const std::size_t o = (std::size_t{y} * out_w + x) * 4;
        const auto byte = [](float v) {
          return static_cast<std::uint8_t>(
              std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
        };
        const float norm = coverage > 0.0F ? 1.0F / coverage : 0.0F;
        out.pixels[o] = byte(sum.r * norm);
        out.pixels[o + 1] = byte(sum.g * norm);
        out.pixels[o + 2] = byte(sum.b * norm);
        out.pixels[o + 3] = byte(coverage / samples);
      }
    }
    return out;
  }

  std::vector<rgb> color_;
  std::vector<float> alpha_;
  std::vector<float> depth_;
  std::vector<plane> planes_;
  const frame_description* frame_{};
  mat4 view_{};
  mat4 proj_{};
  std::uint32_t w_{};
  std::uint32_t h_{};
  std::uint32_t ss_{1};
};

}  // namespace avionix
