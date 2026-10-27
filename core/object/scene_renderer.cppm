module;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

export module avionix.object.scene_renderer;

import avionix.entity.viewport;
import avionix.entity.error;
import avionix.object.gl;
import avionix.object.scene_store;

// OpenGL 3.3 core rendering of a resolved scene.
//
//   scene_store ──sync()──► GPU mesh cache (by mesh version)
//   frame_description ──draw()──► default framebuffer
//                     └─capture()──► offscreen MSAA framebuffer ──resolve──► pixels
//
// Passes, in order: background gradient; faces (opaque, then translucent);
// edges; world overlays; screen-space handles, markers, and the axis triad.
// Faces use a polygon offset so coincident edges win the depth test, which
// keeps edges depth-correct without z-fighting.
//
// Precision: model-view matrices are composed in double and cast to float,
// so vertices reach the GPU relative to the eye.
//
// Lifetime: every GL object belongs to the context that was current at
// create(). release() must run with that context current, before the
// context is destroyed; the destructor does not touch GL.

export namespace avionix {

struct draw_item {
  mesh_id mesh;
  mat4 world{};
  material look{};
  // 0: normal, 1: selected. Hover uses an intermediate value.
  float highlight{};
};

// A screen-space colored triangle list, in framebuffer pixels.
struct screen_vertex {
  float x{};
  float y{};
  rgba color{};
};

struct world_line {
  vec3 a{};
  vec3 b{};
  rgba color{};
};

struct frame_description {
  view_frame view{};
  display_mode mode{display_mode::shaded_edges};
  rgba background_top{0.30F, 0.33F, 0.38F, 1.0F};
  rgba background_bottom{0.11F, 0.12F, 0.14F, 1.0F};
  rgba highlight{1.0F, 0.62F, 0.12F, 1.0F};
  // No background: uncovered pixels get alpha 0 so whatever is behind the
  // image (a terminal's background or background image) shows through.
  bool transparent_background{false};
  std::span<const draw_item> items{};
  std::span<const plane> sections{};
  std::span<const world_line> lines_depth{};   // depth-tested overlays
  std::span<const world_line> lines_on_top{};  // overlays drawn over the model
  std::span<const screen_vertex> screen{};     // handles, markers, triad
};

// Expands screen-space polylines and polygons into triangles.
class screen_batch {
 public:
  void clear() { vertices_.clear(); }
  [[nodiscard]] std::span<const screen_vertex> vertices() const noexcept {
    return vertices_;
  }

  void line(vec2 a, vec2 b, double width, rgba color) {
    const vec2 d = b - a;
    const double len = length(d);
    if (len < 1e-9) return;
    const vec2 n = vec2{-d.y, d.x} * (width * 0.5 / len);
    triangle(a + n, b + n, b - n, color);
    triangle(a + n, b - n, a - n, color);
  }
  void polyline(std::span<const vec2> points, bool loop, double width, rgba color) {
    if (points.size() < 2) return;
    for (std::size_t i = 0; i + 1 < points.size(); ++i)
      line(points[i], points[i + 1], width, color);
    if (loop) line(points.back(), points.front(), width, color);
  }
  void polygon(std::span<const vec2> points, rgba color) {
    for (std::size_t i = 1; i + 1 < points.size(); ++i)
      triangle(points[0], points[i], points[i + 1], color);
  }
  void triangle(vec2 a, vec2 b, vec2 c, rgba color) {
    for (const vec2 p : {a, b, c})
      vertices_.push_back({static_cast<float>(p.x), static_cast<float>(p.y), color});
  }

 private:
  std::vector<screen_vertex> vertices_;
};

class scene_renderer {
 public:
  [[nodiscard]] static std::expected<scene_renderer, error> create(
      gl::proc_lookup lookup) {
    auto table = gl::api::load(lookup);
    if (!table) return std::unexpected(std::move(table.error()));
    scene_renderer r;
    r.gl_ = *table;
    r.loaded_ = true;
    if (auto ok = r.build_programs(); !ok) {
      r.release();
      return std::unexpected(std::move(ok.error()));
    }
    r.gl_.GenVertexArrays(1, &r.stream_vao_);
    r.gl_.GenBuffers(1, &r.stream_vbo_);
    std::int32_t samples = 0;
    r.gl_.GetIntegerv(gl::max_samples, &samples);
    r.max_samples_ = samples;
    return r;
  }

  scene_renderer(scene_renderer&& other) noexcept { *this = std::move(other); }
  scene_renderer& operator=(scene_renderer&& other) noexcept {
    if (this == &other) return *this;
    gl_ = other.gl_;
    loaded_ = std::exchange(other.loaded_, false);
    meshes_ = std::move(other.meshes_);
    mesh_program_ = std::exchange(other.mesh_program_, {});
    line_program_ = std::exchange(other.line_program_, {});
    color_program_ = std::exchange(other.color_program_, {});
    screen_program_ = std::exchange(other.screen_program_, {});
    stream_vao_ = std::exchange(other.stream_vao_, 0);
    stream_vbo_ = std::exchange(other.stream_vbo_, 0);
    max_samples_ = other.max_samples_;
    return *this;
  }
  scene_renderer(const scene_renderer&) = delete;
  scene_renderer& operator=(const scene_renderer&) = delete;
  ~scene_renderer() = default;

  // Deletes every GL object. Requires the creating context to be current.
  void release() {
    if (!loaded_) return;
    for (auto& [id, m] : meshes_) destroy(m);
    meshes_.clear();
    for (program* p :
         {&mesh_program_, &line_program_, &color_program_, &screen_program_}) {
      if (p->id != 0) gl_.DeleteProgram(p->id);
      *p = {};
    }
    if (stream_vbo_ != 0) gl_.DeleteBuffers(1, &stream_vbo_);
    if (stream_vao_ != 0) gl_.DeleteVertexArrays(1, &stream_vao_);
    stream_vbo_ = stream_vao_ = 0;
    loaded_ = false;
  }

  [[nodiscard]] std::string description() const {
    return std::format("{} ({}), OpenGL {}", gl_.string(gl::renderer),
                       gl_.string(gl::vendor), gl_.string(gl::version));
  }

  // Uploads new and changed meshes and frees removed ones.
  void sync(const scene_store& store) {
    std::erase_if(meshes_, [&](auto& entry) {
      const auto* record = store.mesh(entry.first);
      if (record != nullptr && record->version == entry.second.version) return false;
      destroy(entry.second);
      return true;
    });
    for (const auto& [id, record] : store.meshes()) {
      if (!meshes_.contains(id))
        meshes_.emplace(id, upload(record.data, record.version));
    }
  }

  // Draws into the default framebuffer of the current context.
  [[nodiscard]] std::expected<void, error> draw(const frame_description& frame) {
    gl_.BindFramebuffer(gl::framebuffer, 0);
    render(frame);
    return check("draw viewport frame");
  }

  // Renders offscreen at frame.view.extent and returns top-down RGBA rows.
  [[nodiscard]] std::expected<image_rgba8, error> capture(
      const frame_description& frame, int samples = 4) {
    const auto w = static_cast<gl::sizei>(frame.view.extent.width);
    const auto h = static_cast<gl::sizei>(frame.view.extent.height);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
      return std::unexpected(
          error{"capture viewport", failure::invalid_argument,
                std::format("output size {}x{} is outside 1..8192", w, h), 0});
    }
    samples = std::clamp(samples, 0, max_samples_);
    std::array<gl::uint, 2> fbo{};
    std::array<gl::uint, 3> rbo{};
    gl_.GenFramebuffers(2, fbo.data());
    gl_.GenRenderbuffers(3, rbo.data());
    const auto cleanup = [&] {
      gl_.BindFramebuffer(gl::framebuffer, 0);
      gl_.DeleteRenderbuffers(3, rbo.data());
      gl_.DeleteFramebuffers(2, fbo.data());
    };
    // fbo[0]: multisampled color + depth; fbo[1]: single-sample color.
    gl_.BindFramebuffer(gl::framebuffer, fbo[0]);
    gl_.BindRenderbuffer(gl::renderbuffer_target, rbo[0]);
    gl_.RenderbufferStorageMultisample(gl::renderbuffer_target, samples, gl::rgba8, w,
                                       h);
    gl_.FramebufferRenderbuffer(gl::framebuffer, gl::color_attachment0,
                                gl::renderbuffer_target, rbo[0]);
    gl_.BindRenderbuffer(gl::renderbuffer_target, rbo[1]);
    gl_.RenderbufferStorageMultisample(gl::renderbuffer_target, samples,
                                       gl::depth24_stencil8, w, h);
    gl_.FramebufferRenderbuffer(gl::framebuffer, gl::depth_stencil_attachment,
                                gl::renderbuffer_target, rbo[1]);
    if (gl_.CheckFramebufferStatus(gl::framebuffer) != gl::framebuffer_complete) {
      cleanup();
      return std::unexpected(error{
          "capture viewport", failure::graphics_failed,
          std::format("{}x{} framebuffer with {} samples is incomplete", w, h, samples),
          0});
    }
    render(frame);
    gl_.BindFramebuffer(gl::framebuffer, fbo[1]);
    gl_.BindRenderbuffer(gl::renderbuffer_target, rbo[2]);
    gl_.RenderbufferStorageMultisample(gl::renderbuffer_target, 0, gl::rgba8, w, h);
    gl_.FramebufferRenderbuffer(gl::framebuffer, gl::color_attachment0,
                                gl::renderbuffer_target, rbo[2]);
    gl_.BindFramebuffer(gl::read_framebuffer, fbo[0]);
    gl_.BindFramebuffer(gl::draw_framebuffer, fbo[1]);
    gl_.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, gl::color_buffer_bit, gl::nearest);
    gl_.BindFramebuffer(gl::read_framebuffer, fbo[1]);
    image_rgba8 image{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), {}};
    image.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    gl_.PixelStorei(gl::pack_alignment, 1);
    gl_.ReadPixels(0, 0, w, h, gl::rgba, gl::unsigned_byte, image.pixels.data());
    cleanup();
    if (auto ok = check("capture viewport"); !ok)
      return std::unexpected(std::move(ok.error()));
    // Blending into a transparent target leaves antialiased edge colors
    // multiplied by their coverage; divide it back out so edges do not get
    // a dark fringe when shown over another background.
    if (frame.transparent_background) unpremultiply(image);
    // GL rows run bottom to top.
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    for (std::size_t y = 0; y < static_cast<std::size_t>(h) / 2; ++y) {
      std::swap_ranges(
          image.pixels.begin() + static_cast<std::ptrdiff_t>(y * stride),
          image.pixels.begin() + static_cast<std::ptrdiff_t>((y + 1) * stride),
          image.pixels.begin() + static_cast<std::ptrdiff_t>(
                                     (static_cast<std::size_t>(h) - 1 - y) * stride));
    }
    return image;
  }

 private:
  scene_renderer() = default;

  static void unpremultiply(image_rgba8& image) {
    for (std::size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
      const std::uint32_t a = image.pixels[i + 3];
      if (a == 0 || a == 255) continue;
      for (std::size_t c = 0; c < 3; ++c)
        image.pixels[i + c] = static_cast<std::uint8_t>(
            std::min<std::uint32_t>(255, (image.pixels[i + c] * 255U + a / 2) / a));
    }
  }

  struct gpu_mesh {
    std::uint64_t version{};
    gl::uint face_vao{};
    gl::uint face_vbo{};
    gl::uint face_ebo{};
    gl::sizei face_count{};  // indices (indexed) or vertices (expanded)
    bool indexed{};
    gl::uint edge_vao{};
    gl::uint edge_vbo{};
    gl::uint edge_ebo{};
    gl::sizei edge_count{};
  };

  struct program {
    gl::uint id{};
    std::int32_t model_view{-1};
    std::int32_t projection{-1};
    std::int32_t normal_matrix{-1};
    std::int32_t planes{-1};
    std::int32_t color{-1};
    std::int32_t highlight{-1};
    std::int32_t highlight_amount{-1};
    std::int32_t shininess{-1};
    std::int32_t extent{-1};
  };

  static constexpr const char* mesh_vs = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
uniform mat4 u_model_view;
uniform mat4 u_projection;
uniform mat4 u_normal_matrix;
uniform vec4 u_planes[4];
out vec3 v_normal;
out vec3 v_view;
void main() {
  vec4 p = u_model_view * vec4(a_position, 1.0);
  v_view = p.xyz;
  v_normal = mat3(u_normal_matrix) * a_normal;
  for (int i = 0; i < 4; ++i) gl_ClipDistance[i] = u_planes[i].w - dot(u_planes[i].xyz, p.xyz);
  gl_Position = u_projection * p;
})";

  static constexpr const char* mesh_fs = R"(#version 330 core
in vec3 v_normal;
in vec3 v_view;
uniform vec4 u_color;
uniform vec4 u_highlight;
uniform float u_highlight_amount;
uniform float u_shininess;
out vec4 frag;
void main() {
  vec3 n = normalize(v_normal);
  vec3 v = normalize(-v_view);
  // Two-sided: interior faces revealed by a section plane are darkened.
  float interior = gl_FrontFacing ? 1.0 : 0.55;
  if (dot(n, v) < 0.0) n = -n;
  vec3 key = normalize(vec3(0.35, 0.55, 0.75));
  float diffuse = 0.55 * max(dot(n, key), 0.0) + 0.35 * max(dot(n, v), 0.0);
  vec3 h = normalize(key + v);
  float spec = pow(max(dot(n, h), 0.0), mix(8.0, 96.0, u_shininess)) * 0.3 * u_shininess;
  vec3 base = mix(u_color.rgb, u_highlight.rgb, u_highlight_amount * 0.55) * interior;
  frag = vec4(base * (0.22 + diffuse) + vec3(spec), u_color.a);
})";

  static constexpr const char* line_vs = R"(#version 330 core
layout(location = 0) in vec3 a_position;
uniform mat4 u_model_view;
uniform mat4 u_projection;
uniform vec4 u_planes[4];
void main() {
  vec4 p = u_model_view * vec4(a_position, 1.0);
  for (int i = 0; i < 4; ++i) gl_ClipDistance[i] = u_planes[i].w - dot(u_planes[i].xyz, p.xyz);
  gl_Position = u_projection * p;
})";

  static constexpr const char* line_fs = R"(#version 330 core
uniform vec4 u_color;
out vec4 frag;
void main() { frag = u_color; })";

  static constexpr const char* color_vs = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_color;
uniform mat4 u_model_view;
uniform mat4 u_projection;
out vec4 v_color;
void main() {
  v_color = a_color;
  gl_Position = u_projection * (u_model_view * vec4(a_position, 1.0));
})";

  static constexpr const char* screen_vs = R"(#version 330 core
layout(location = 0) in vec2 a_pixel;
layout(location = 1) in vec4 a_color;
uniform vec2 u_extent;
out vec4 v_color;
void main() {
  v_color = a_color;
  gl_Position = vec4(a_pixel.x / u_extent.x * 2.0 - 1.0, 1.0 - a_pixel.y / u_extent.y * 2.0, 0.0, 1.0);
})";

  static constexpr const char* color_fs = R"(#version 330 core
in vec4 v_color;
out vec4 frag;
void main() { frag = v_color; })";

  [[nodiscard]] std::expected<gl::uint, error> compile(gl::enum_t kind,
                                                       const char* source,
                                                       std::string_view name) {
    const gl::uint shader = gl_.CreateShader(kind);
    gl_.ShaderSource(shader, 1, &source, nullptr);
    gl_.CompileShader(shader);
    std::int32_t ok = 0;
    gl_.GetShaderiv(shader, gl::compile_status, &ok);
    if (ok == 0) {
      std::int32_t len = 0;
      gl_.GetShaderiv(shader, gl::info_log_length, &len);
      std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
      gl_.GetShaderInfoLog(shader, len, nullptr, log.data());
      gl_.DeleteShader(shader);
      return std::unexpected(error{"compile viewport shader", failure::graphics_failed,
                                   std::format("{}: {}", name, log.c_str()), 0});
    }
    return shader;
  }

  [[nodiscard]] std::expected<program, error> link(const char* vs, const char* fs,
                                                   std::string_view name) {
    auto v = compile(gl::vertex_shader, vs, name);
    if (!v) return std::unexpected(std::move(v.error()));
    auto f = compile(gl::fragment_shader, fs, name);
    if (!f) {
      gl_.DeleteShader(*v);
      return std::unexpected(std::move(f.error()));
    }
    program p;
    p.id = gl_.CreateProgram();
    gl_.AttachShader(p.id, *v);
    gl_.AttachShader(p.id, *f);
    gl_.LinkProgram(p.id);
    gl_.DeleteShader(*v);
    gl_.DeleteShader(*f);
    std::int32_t ok = 0;
    gl_.GetProgramiv(p.id, gl::link_status, &ok);
    if (ok == 0) {
      std::int32_t len = 0;
      gl_.GetProgramiv(p.id, gl::info_log_length, &len);
      std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
      gl_.GetProgramInfoLog(p.id, len, nullptr, log.data());
      gl_.DeleteProgram(p.id);
      return std::unexpected(error{"link viewport shader", failure::graphics_failed,
                                   std::format("{}: {}", name, log.c_str()), 0});
    }
    p.model_view = gl_.GetUniformLocation(p.id, "u_model_view");
    p.projection = gl_.GetUniformLocation(p.id, "u_projection");
    p.normal_matrix = gl_.GetUniformLocation(p.id, "u_normal_matrix");
    p.planes = gl_.GetUniformLocation(p.id, "u_planes");
    p.color = gl_.GetUniformLocation(p.id, "u_color");
    p.highlight = gl_.GetUniformLocation(p.id, "u_highlight");
    p.highlight_amount = gl_.GetUniformLocation(p.id, "u_highlight_amount");
    p.shininess = gl_.GetUniformLocation(p.id, "u_shininess");
    p.extent = gl_.GetUniformLocation(p.id, "u_extent");
    return p;
  }

  [[nodiscard]] std::expected<void, error> build_programs() {
    auto m = link(mesh_vs, mesh_fs, "mesh");
    if (!m) return std::unexpected(std::move(m.error()));
    mesh_program_ = *m;
    auto l = link(line_vs, line_fs, "edge");
    if (!l) return std::unexpected(std::move(l.error()));
    line_program_ = *l;
    auto c = link(color_vs, color_fs, "overlay");
    if (!c) return std::unexpected(std::move(c.error()));
    color_program_ = *c;
    auto s = link(screen_vs, color_fs, "screen");
    if (!s) return std::unexpected(std::move(s.error()));
    screen_program_ = *s;
    return {};
  }

  [[nodiscard]] std::expected<void, error> check(std::string_view operation) const {
    const gl::enum_t e = gl_.GetError();
    if (e == gl::no_error) return {};
    while (gl_.GetError() != gl::no_error) {
    }
    return std::unexpected(error{
        operation == "draw viewport frame" ? "draw viewport frame" : "capture viewport",
        failure::graphics_failed, std::format("OpenGL error 0x{:04X}", e),
        static_cast<std::int64_t>(e)});
  }

  void destroy(gpu_mesh& m) {
    for (gl::uint* b : {&m.face_vbo, &m.face_ebo, &m.edge_vbo, &m.edge_ebo})
      if (*b != 0) gl_.DeleteBuffers(1, b);
    for (gl::uint* a : {&m.face_vao, &m.edge_vao})
      if (*a != 0) gl_.DeleteVertexArrays(1, a);
    m = {};
  }

  // Interleaved position and normal, 6 floats per vertex.
  void vertex_layout() const {
    gl_.EnableVertexAttribArray(0);
    gl_.VertexAttribPointer(0, 3, gl::float_type, gl::false_value, 24, nullptr);
    gl_.EnableVertexAttribArray(1);
    gl_.VertexAttribPointer(1, 3, gl::float_type, gl::false_value, 24,
                            reinterpret_cast<const void*>(std::uintptr_t{12}));
  }

  template <typename T>
  void buffer(gl::enum_t target, gl::uint& id, const std::vector<T>& data) const {
    gl_.GenBuffers(1, &id);
    gl_.BindBuffer(target, id);
    gl_.BufferData(target, static_cast<gl::sizeiptr>(data.size() * sizeof(T)),
                   data.data(), gl::static_draw);
  }

  [[nodiscard]] gpu_mesh upload(const mesh_data& mesh, std::uint64_t version) const {
    gpu_mesh g;
    g.version = version;
    std::vector<float> vertices;
    const auto push = [&](vec3f p, vec3f n) {
      vertices.insert(vertices.end(), {p.x, p.y, p.z, n.x, n.y, n.z});
    };
    if (!mesh.normals.empty()) {
      g.indexed = true;
      vertices.reserve(mesh.positions.size() * 6);
      for (std::size_t i = 0; i < mesh.positions.size(); ++i)
        push(mesh.positions[i], mesh.normals[i]);
    } else {
      // Flat shading: one face normal per triangle, vertices duplicated.
      vertices.reserve(mesh.indices.size() * 6);
      for (std::size_t t = 0; t < mesh.triangle_count(); ++t) {
        const vec3f a = mesh.positions[mesh.indices[t * 3]];
        const vec3f b = mesh.positions[mesh.indices[t * 3 + 1]];
        const vec3f c = mesh.positions[mesh.indices[t * 3 + 2]];
        const vec3f n =
            narrow(normalize(cross(widen(b) - widen(a), widen(c) - widen(a))));
        push(a, n);
        push(b, n);
        push(c, n);
      }
    }
    gl_.GenVertexArrays(1, &g.face_vao);
    gl_.BindVertexArray(g.face_vao);
    buffer(gl::array_buffer, g.face_vbo, vertices);
    vertex_layout();
    if (g.indexed) {
      buffer(gl::element_array_buffer, g.face_ebo, mesh.indices);
      g.face_count = static_cast<gl::sizei>(mesh.indices.size());
    } else {
      g.face_count = static_cast<gl::sizei>(mesh.indices.size());
    }

    if (!mesh.edge_indices.empty()) {
      gl_.GenVertexArrays(1, &g.edge_vao);
      gl_.BindVertexArray(g.edge_vao);
      if (g.indexed) {
        gl_.BindBuffer(gl::array_buffer, g.face_vbo);
      } else {
        std::vector<float> positions;
        positions.reserve(mesh.positions.size() * 6);
        for (const vec3f p : mesh.positions)
          positions.insert(positions.end(), {p.x, p.y, p.z, 0.0F, 0.0F, 1.0F});
        buffer(gl::array_buffer, g.edge_vbo, positions);
      }
      vertex_layout();
      buffer(gl::element_array_buffer, g.edge_ebo, mesh.edge_indices);
      g.edge_count = static_cast<gl::sizei>(mesh.edge_indices.size());
    }
    gl_.BindVertexArray(0);
    return g;
  }

  static std::array<float, 16> to_float(const mat4& m) {
    std::array<float, 16> out{};
    for (std::size_t i = 0; i < 16; ++i) out[i] = static_cast<float>(m.m[i]);
    return out;
  }

  void set_common(const program& p, const mat4& model_view,
                  const frame_description& frame,
                  const std::array<float, 16>& planes) const {
    const auto mv = to_float(model_view);
    gl_.UniformMatrix4fv(p.model_view, 1, gl::false_value, mv.data());
    const auto proj = to_float(frame.view.projection_matrix);
    gl_.UniformMatrix4fv(p.projection, 1, gl::false_value, proj.data());
    if (p.planes >= 0) gl_.Uniform4fv(p.planes, 4, planes.data());
  }

  // Section planes in eye space: dot(n_eye, p_eye) <= offset + dot(n_eye, t).
  static std::array<float, 16> eye_planes(const frame_description& frame) {
    std::array<float, 16> out{};
    const mat4& v = frame.view.view;
    const vec3 t = translation_of(v);
    for (std::size_t i = 0; i < max_section_planes; ++i) {
      if (i >= frame.sections.size()) {
        // Never clips: offset is far beyond any distance.
        out[i * 4 + 3] = 3.0e38F;
        continue;
      }
      const vec3 n = normalize(transform_vector(v, frame.sections[i].normal));
      const double offset = frame.sections[i].offset + dot(n, t);
      out[i * 4] = static_cast<float>(n.x);
      out[i * 4 + 1] = static_cast<float>(n.y);
      out[i * 4 + 2] = static_cast<float>(n.z);
      out[i * 4 + 3] = static_cast<float>(offset);
    }
    return out;
  }

  static void color4(std::array<float, 4>& out, rgba c) { out = {c.r, c.g, c.b, c.a}; }

  void draw_faces(const frame_description& frame, const std::array<float, 16>& planes,
                  bool translucent_pass, float alpha_scale) const {
    gl_.UseProgram(mesh_program_.id);
    std::array<float, 4> highlight{};
    color4(highlight, frame.highlight);
    gl_.Uniform4fv(mesh_program_.highlight, 1, highlight.data());
    for (const draw_item& item : frame.items) {
      const auto it = meshes_.find(item.mesh);
      if (it == meshes_.end() || it->second.face_count == 0) continue;
      const float alpha = item.look.base_color.a * alpha_scale;
      if ((alpha < 0.999F) != translucent_pass) continue;
      const mat4 mv = frame.view.view * item.world;
      set_common(mesh_program_, mv, frame, planes);
      const auto nm = to_float(transpose(inverse(mv)));
      gl_.UniformMatrix4fv(mesh_program_.normal_matrix, 1, gl::false_value, nm.data());
      std::array<float, 4> color{};
      color4(color, item.look.base_color);
      color[3] = alpha;
      gl_.Uniform4fv(mesh_program_.color, 1, color.data());
      gl_.Uniform1f(mesh_program_.highlight_amount, item.highlight);
      gl_.Uniform1f(mesh_program_.shininess, item.look.shininess);
      gl_.BindVertexArray(it->second.face_vao);
      if (it->second.indexed) {
        gl_.DrawElements(gl::triangles, it->second.face_count, gl::unsigned_int,
                         nullptr);
      } else {
        gl_.DrawArrays(gl::triangles, 0, it->second.face_count);
      }
    }
  }

  void draw_edges(const frame_description& frame, const std::array<float, 16>& planes,
                  bool selected_only) const {
    gl_.UseProgram(line_program_.id);
    for (const draw_item& item : frame.items) {
      if (selected_only && item.highlight < 0.99F) continue;
      const auto it = meshes_.find(item.mesh);
      if (it == meshes_.end()) continue;
      set_common(line_program_, frame.view.view * item.world, frame, planes);
      std::array<float, 4> color{};
      color4(color, item.highlight >= 0.99F ? frame.highlight : item.look.edge_color);
      gl_.Uniform4fv(line_program_.color, 1, color.data());
      if (it->second.edge_count > 0) {
        gl_.BindVertexArray(it->second.edge_vao);
        gl_.DrawElements(gl::lines, it->second.edge_count, gl::unsigned_int, nullptr);
      } else if (frame.mode == display_mode::wireframe ||
                 frame.mode == display_mode::xray) {
        // No host edges: show the tessellation instead of nothing.
        gl_.PolygonMode(gl::front_and_back, gl::line);
        gl_.BindVertexArray(it->second.face_vao);
        if (it->second.indexed) {
          gl_.DrawElements(gl::triangles, it->second.face_count, gl::unsigned_int,
                           nullptr);
        } else {
          gl_.DrawArrays(gl::triangles, 0, it->second.face_count);
        }
        gl_.PolygonMode(gl::front_and_back, gl::fill);
      }
    }
  }

  void stream(std::span<const float> data, std::span<const std::int32_t> sizes) const {
    gl_.BindVertexArray(stream_vao_);
    gl_.BindBuffer(gl::array_buffer, stream_vbo_);
    gl_.BufferData(gl::array_buffer, static_cast<gl::sizeiptr>(data.size_bytes()),
                   data.data(), gl::stream_draw);
    std::int32_t stride = 0;
    for (const std::int32_t s : sizes) stride += s;
    std::uintptr_t offset = 0;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
      gl_.EnableVertexAttribArray(static_cast<gl::uint>(i));
      gl_.VertexAttribPointer(static_cast<gl::uint>(i), sizes[i], gl::float_type,
                              gl::false_value, stride * 4,
                              reinterpret_cast<const void*>(offset));
      offset += static_cast<std::uintptr_t>(sizes[i]) * 4;
    }
  }

  void draw_world_lines(const frame_description& frame,
                        std::span<const world_line> lines) {
    if (lines.empty()) return;
    // Eye-relative positions with a rotation-only view keep precision for
    // large document coordinates.
    std::vector<float> data;
    data.reserve(lines.size() * 14);
    const vec3 eye = frame.view.eye;
    for (const world_line& l : lines) {
      for (const vec3 p : {l.a - eye, l.b - eye}) {
        data.insert(data.end(), {static_cast<float>(p.x), static_cast<float>(p.y),
                                 static_cast<float>(p.z), l.color.r, l.color.g,
                                 l.color.b, l.color.a});
      }
    }
    mat4 rotation_only = frame.view.view;
    rotation_only.at(0, 3) = rotation_only.at(1, 3) = rotation_only.at(2, 3) = 0.0;
    gl_.UseProgram(color_program_.id);
    const std::array<float, 16> no_planes{};
    set_common(color_program_, rotation_only, frame, no_planes);
    constexpr std::array<std::int32_t, 2> sizes{3, 4};
    stream(data, sizes);
    gl_.DrawArrays(gl::lines, 0, static_cast<gl::sizei>(lines.size() * 2));
  }

  void draw_screen(const frame_description& frame,
                   std::span<const screen_vertex> vertices) {
    if (vertices.empty()) return;
    std::vector<float> data;
    data.reserve(vertices.size() * 6);
    for (const screen_vertex& v : vertices)
      data.insert(data.end(), {v.x, v.y, v.color.r, v.color.g, v.color.b, v.color.a});
    gl_.UseProgram(screen_program_.id);
    gl_.Uniform2f(screen_program_.extent, static_cast<float>(frame.view.extent.width),
                  static_cast<float>(frame.view.extent.height));
    constexpr std::array<std::int32_t, 2> sizes{2, 4};
    stream(data, sizes);
    gl_.DrawArrays(gl::triangles, 0, static_cast<gl::sizei>(vertices.size()));
  }

  void render(const frame_description& frame) {
    const auto w = static_cast<gl::sizei>(frame.view.extent.width);
    const auto h = static_cast<gl::sizei>(frame.view.extent.height);
    gl_.Viewport(0, 0, w, h);
    gl_.ColorMask(gl::true_value, gl::true_value, gl::true_value, gl::true_value);
    gl_.DepthMask(gl::true_value);
    if (frame.transparent_background) {
      gl_.ClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    } else {
      gl_.ClearColor(frame.background_bottom.r, frame.background_bottom.g,
                     frame.background_bottom.b, 1.0F);
    }
    gl_.ClearDepth(1.0);
    gl_.Clear(gl::color_buffer_bit | gl::depth_buffer_bit);
    gl_.Disable(gl::cull_face);
    gl_.Enable(gl::multisample);
    gl_.Enable(gl::blend);
    // Colors blend by source alpha; destination alpha accumulates coverage
    // (a + d(1 - a)), which a transparent capture needs.
    gl_.BlendFuncSeparate(gl::src_alpha, gl::one_minus_src_alpha, gl::one,
                          gl::one_minus_src_alpha);

    // Background gradient.
    gl_.Disable(gl::depth_test);
    if (!frame.transparent_background) {
      screen_batch bg;
      const double fw = w;
      const double fh = h;
      bg.triangle({0, 0}, {fw, 0}, {fw, fh}, frame.background_top);
      bg.triangle({0, 0}, {fw, fh}, {0, fh}, frame.background_top);
      // Bottom vertices take the bottom color.
      auto v = std::vector<screen_vertex>{bg.vertices().begin(), bg.vertices().end()};
      for (screen_vertex& p : v)
        if (p.y > 0.5F) p.color = frame.background_bottom;
      draw_screen(frame, v);
    }

    const auto planes = eye_planes(frame);
    const std::size_t clip_count = std::min(frame.sections.size(), max_section_planes);
    for (std::size_t i = 0; i < max_section_planes; ++i) {
      if (i < clip_count) {
        gl_.Enable(gl::clip_distance0 + static_cast<gl::enum_t>(i));
      } else {
        gl_.Disable(gl::clip_distance0 + static_cast<gl::enum_t>(i));
      }
    }

    gl_.Enable(gl::depth_test);
    gl_.DepthFunc(gl::less);
    switch (frame.mode) {
      case display_mode::shaded:
      case display_mode::shaded_edges:
        gl_.Enable(gl::polygon_offset_fill);
        gl_.PolygonOffset(1.0F, 1.0F);
        draw_faces(frame, planes, false, 1.0F);
        gl_.DepthMask(gl::false_value);
        draw_faces(frame, planes, true, 1.0F);
        gl_.DepthMask(gl::true_value);
        gl_.Disable(gl::polygon_offset_fill);
        gl_.DepthFunc(gl::lequal);
        draw_edges(frame, planes, frame.mode == display_mode::shaded);
        break;
      case display_mode::wireframe:
        // Depth-only pre-pass hides lines behind faces.
        gl_.Enable(gl::polygon_offset_fill);
        gl_.PolygonOffset(1.0F, 1.0F);
        gl_.ColorMask(gl::false_value, gl::false_value, gl::false_value,
                      gl::false_value);
        draw_faces(frame, planes, false, 1.0F);
        draw_faces(frame, planes, true, 1.0F);
        gl_.ColorMask(gl::true_value, gl::true_value, gl::true_value, gl::true_value);
        gl_.Disable(gl::polygon_offset_fill);
        gl_.DepthFunc(gl::lequal);
        draw_edges(frame, planes, false);
        break;
      case display_mode::xray:
        gl_.DepthMask(gl::false_value);
        draw_faces(frame, planes, true, 0.18F);
        gl_.Disable(gl::depth_test);
        draw_edges(frame, planes, false);
        gl_.DepthMask(gl::true_value);
        break;
    }
    for (std::size_t i = 0; i < max_section_planes; ++i)
      gl_.Disable(gl::clip_distance0 + static_cast<gl::enum_t>(i));

    gl_.Enable(gl::depth_test);
    gl_.DepthFunc(gl::lequal);
    draw_world_lines(frame, frame.lines_depth);
    gl_.Disable(gl::depth_test);
    draw_world_lines(frame, frame.lines_on_top);
    draw_screen(frame, frame.screen);
    gl_.BindVertexArray(0);
    gl_.UseProgram(0);
  }

  gl::api gl_{};
  bool loaded_{};
  std::unordered_map<mesh_id, gpu_mesh, id_hash> meshes_;
  program mesh_program_{};
  program line_program_{};
  program color_program_{};
  program screen_program_{};
  gl::uint stream_vao_{};
  gl::uint stream_vbo_{};
  int max_samples_{};
};

}  // namespace avionix
