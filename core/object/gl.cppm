module;

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

// OpenGL entry points use __stdcall on 32-bit Windows and the platform
// default everywhere else.
#if defined(_WIN32) && !defined(_WIN64)
#define AVIONIX_GL_API __stdcall
#else
#define AVIONIX_GL_API
#endif

export module avionix.object.gl;

import avionix.entity.error;

// The OpenGL 3.3 core functions the viewport uses, loaded at runtime.
//
//   native_window ──proc address──► gl_api::load ──► function table
//
// Avionix does not include system GL headers or link an OpenGL import
// library: the window adapter supplies the platform's proc-address lookup
// and every function is resolved here. A missing entry point fails load()
// with the function name, which is the actionable detail on drivers that
// advertise 3.3 but are incomplete.
//
// Lifetime: a gl_api is valid only while the context it was loaded from is
// current on the calling thread.

export namespace avionix::gl {

using enum_t = std::uint32_t;
using bitfield = std::uint32_t;
using uint = std::uint32_t;
using sizei = std::int32_t;
using sizeiptr = std::ptrdiff_t;
using intptr = std::ptrdiff_t;

inline constexpr enum_t no_error = 0;
inline constexpr enum_t lines = 0x0001;
inline constexpr enum_t triangles = 0x0004;
inline constexpr enum_t less = 0x0201;
inline constexpr enum_t lequal = 0x0203;
inline constexpr enum_t always = 0x0207;
inline constexpr enum_t one = 0x0001;
inline constexpr enum_t src_alpha = 0x0302;
inline constexpr enum_t one_minus_src_alpha = 0x0303;
inline constexpr enum_t cull_face = 0x0B44;
inline constexpr enum_t depth_test = 0x0B71;
inline constexpr enum_t blend = 0x0BE2;
inline constexpr enum_t pack_alignment = 0x0D05;
inline constexpr enum_t front_and_back = 0x0408;
inline constexpr enum_t line = 0x1B01;
inline constexpr enum_t fill = 0x1B02;
inline constexpr enum_t unsigned_byte = 0x1401;
inline constexpr enum_t unsigned_int = 0x1405;
inline constexpr enum_t float_type = 0x1406;
inline constexpr enum_t rgba = 0x1908;
inline constexpr enum_t vendor = 0x1F00;
inline constexpr enum_t renderer = 0x1F01;
inline constexpr enum_t version = 0x1F02;
inline constexpr enum_t nearest = 0x2600;
inline constexpr enum_t clip_distance0 = 0x3000;
inline constexpr enum_t polygon_offset_fill = 0x8037;
inline constexpr enum_t rgba8 = 0x8058;
inline constexpr enum_t multisample = 0x809D;
inline constexpr enum_t depth24_stencil8 = 0x88F0;
inline constexpr enum_t array_buffer = 0x8892;
inline constexpr enum_t element_array_buffer = 0x8893;
inline constexpr enum_t stream_draw = 0x88E0;
inline constexpr enum_t static_draw = 0x88E4;
inline constexpr enum_t fragment_shader = 0x8B30;
inline constexpr enum_t vertex_shader = 0x8B31;
inline constexpr enum_t compile_status = 0x8B81;
inline constexpr enum_t link_status = 0x8B82;
inline constexpr enum_t info_log_length = 0x8B84;
inline constexpr enum_t read_framebuffer = 0x8CA8;
inline constexpr enum_t draw_framebuffer = 0x8CA9;
inline constexpr enum_t framebuffer_complete = 0x8CD5;
inline constexpr enum_t color_attachment0 = 0x8CE0;
inline constexpr enum_t depth_stencil_attachment = 0x821A;
inline constexpr enum_t framebuffer = 0x8D40;
inline constexpr enum_t renderbuffer_target = 0x8D41;
inline constexpr enum_t max_samples = 0x8D57;
inline constexpr bitfield depth_buffer_bit = 0x00000100;
inline constexpr bitfield color_buffer_bit = 0x00004000;
inline constexpr unsigned char false_value = 0;
inline constexpr unsigned char true_value = 1;

// Signature of the loader the window adapter supplies.
using proc = void (*)();
using proc_lookup = proc (*)(const char* name);

// X-macro: return type, name, parameters. The member is the GL name without
// its "gl" prefix.
#define AVIONIX_GL_FUNCTIONS(X)                                                    \
  X(const unsigned char*, GetString, (enum_t))                                     \
  X(enum_t, GetError, ())                                                          \
  X(void, GetIntegerv, (enum_t, std::int32_t*))                                    \
  X(void, Viewport, (std::int32_t, std::int32_t, sizei, sizei))                    \
  X(void, ClearColor, (float, float, float, float))                                \
  X(void, ClearDepth, (double))                                                    \
  X(void, Clear, (bitfield))                                                       \
  X(void, Enable, (enum_t))                                                        \
  X(void, Disable, (enum_t))                                                       \
  X(void, DepthFunc, (enum_t))                                                     \
  X(void, DepthMask, (unsigned char))                                              \
  X(void, ColorMask, (unsigned char, unsigned char, unsigned char, unsigned char)) \
  X(void, BlendFunc, (enum_t, enum_t))                                             \
  X(void, BlendFuncSeparate, (enum_t, enum_t, enum_t, enum_t))                     \
  X(void, PolygonOffset, (float, float))                                           \
  X(void, PolygonMode, (enum_t, enum_t))                                           \
  X(void, PixelStorei, (enum_t, std::int32_t))                                     \
  X(void, ReadPixels,                                                              \
    (std::int32_t, std::int32_t, sizei, sizei, enum_t, enum_t, void*))             \
  X(void, Finish, ())                                                              \
  X(void, GenBuffers, (sizei, uint*))                                              \
  X(void, DeleteBuffers, (sizei, const uint*))                                     \
  X(void, BindBuffer, (enum_t, uint))                                              \
  X(void, BufferData, (enum_t, sizeiptr, const void*, enum_t))                     \
  X(void, GenVertexArrays, (sizei, uint*))                                         \
  X(void, DeleteVertexArrays, (sizei, const uint*))                                \
  X(void, BindVertexArray, (uint))                                                 \
  X(void, EnableVertexAttribArray, (uint))                                         \
  X(void, VertexAttribPointer,                                                     \
    (uint, std::int32_t, enum_t, unsigned char, sizei, const void*))               \
  X(uint, CreateShader, (enum_t))                                                  \
  X(void, ShaderSource, (uint, sizei, const char* const*, const std::int32_t*))    \
  X(void, CompileShader, (uint))                                                   \
  X(void, GetShaderiv, (uint, enum_t, std::int32_t*))                              \
  X(void, GetShaderInfoLog, (uint, sizei, sizei*, char*))                          \
  X(void, DeleteShader, (uint))                                                    \
  X(uint, CreateProgram, ())                                                       \
  X(void, AttachShader, (uint, uint))                                              \
  X(void, BindAttribLocation, (uint, uint, const char*))                           \
  X(void, LinkProgram, (uint))                                                     \
  X(void, GetProgramiv, (uint, enum_t, std::int32_t*))                             \
  X(void, GetProgramInfoLog, (uint, sizei, sizei*, char*))                         \
  X(void, DeleteProgram, (uint))                                                   \
  X(void, UseProgram, (uint))                                                      \
  X(std::int32_t, GetUniformLocation, (uint, const char*))                         \
  X(void, Uniform1i, (std::int32_t, std::int32_t))                                 \
  X(void, Uniform1f, (std::int32_t, float))                                        \
  X(void, Uniform2f, (std::int32_t, float, float))                                 \
  X(void, Uniform4fv, (std::int32_t, sizei, const float*))                         \
  X(void, UniformMatrix4fv, (std::int32_t, sizei, unsigned char, const float*))    \
  X(void, DrawArrays, (enum_t, std::int32_t, sizei))                               \
  X(void, DrawElements, (enum_t, sizei, enum_t, const void*))                      \
  X(void, GenFramebuffers, (sizei, uint*))                                         \
  X(void, DeleteFramebuffers, (sizei, const uint*))                                \
  X(void, BindFramebuffer, (enum_t, uint))                                         \
  X(void, FramebufferRenderbuffer, (enum_t, enum_t, enum_t, uint))                 \
  X(enum_t, CheckFramebufferStatus, (enum_t))                                      \
  X(void, GenRenderbuffers, (sizei, uint*))                                        \
  X(void, DeleteRenderbuffers, (sizei, const uint*))                               \
  X(void, BindRenderbuffer, (enum_t, uint))                                        \
  X(void, RenderbufferStorageMultisample, (enum_t, sizei, enum_t, sizei, sizei))   \
  X(void, BlitFramebuffer,                                                         \
    (std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t,         \
     std::int32_t, std::int32_t, std::int32_t, bitfield, enum_t))

struct api {
#define AVIONIX_GL_MEMBER(ret, name, params) ret(AVIONIX_GL_API* name) params{};
  AVIONIX_GL_FUNCTIONS(AVIONIX_GL_MEMBER)
#undef AVIONIX_GL_MEMBER

  // Resolves every function through `lookup`. Requires a current context.
  [[nodiscard]] static std::expected<api, error> load(proc_lookup lookup) {
    api table;
    std::string missing;
#define AVIONIX_GL_LOAD(ret, name, params)                                        \
  table.name = reinterpret_cast<ret(AVIONIX_GL_API*) params>(lookup("gl" #name)); \
  if (table.name == nullptr)                                                      \
    missing += (missing.empty() ? "gl" : ", gl") + std::string{#name};
    AVIONIX_GL_FUNCTIONS(AVIONIX_GL_LOAD)
#undef AVIONIX_GL_LOAD
    if (!missing.empty()) {
      return std::unexpected(error{"load OpenGL functions",
                                   failure::graphics_unavailable,
                                   "the OpenGL driver does not provide " + missing +
                                       "; OpenGL 3.3 core is required",
                                   0});
    }
    return table;
  }

  [[nodiscard]] std::string string(enum_t name) const {
    const unsigned char* s = GetString(name);
    return s == nullptr ? std::string{} : std::string{reinterpret_cast<const char*>(s)};
  }
};

}  // namespace avionix::gl
