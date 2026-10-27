module;

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

export module avionix.entity.viewport;

export import avionix.entity.spatial;
export import avionix.entity.scene;
export import avionix.entity.camera;
export import avionix.entity.picking;
export import avionix.entity.manipulator;
export import avionix.entity.image;
export import avionix.entity.event;
export import avionix.entity.error;

// Values that cross the viewport boundary.
//
//   native window ──viewport_input──► viewport ──viewport_event──► host
//   host ──scene_update / capture_request / settings──► viewport
//
// Inputs use framebuffer pixels: the window adapter converts window points
// with the framebuffer-to-window ratio, so picking and handles agree with
// what was drawn on high-DPI displays.
//
// Events are delivered on the UI thread, in the order they were produced.
// Each event that refers to the scene carries the scene revision it was
// computed against. A host that has moved to a newer revision should treat
// an intent tagged with an older revision as stale.

export namespace avionix {

// ── Input ────────────────────────────────────────────────────────────────

enum class pointer_button : std::uint8_t { left, middle, right };

struct pointer_moved {
  vec2 where{};
  modifiers mods{};
};
struct pointer_pressed {
  vec2 where{};
  pointer_button button{};
  modifiers mods{};
};
struct pointer_released {
  vec2 where{};
  pointer_button button{};
  modifiers mods{};
};
struct pointer_left {};
// Wheel notches; positive y scrolls away from the user (zoom in).
struct wheel_scrolled {
  vec2 where{};
  vec2 delta{};
  modifiers mods{};
};
struct framebuffer_resized {
  pixel_extent extent{};
  // Framebuffer pixels per logical pixel (1 on a 96-DPI display, 2 on a
  // typical Retina display). Scales handle sizes and hit tolerances.
  double content_scale{1.0};
};
struct window_focus {
  bool focused{};
};
struct window_close_requested {};

using viewport_input =
    std::variant<pointer_moved, pointer_pressed, pointer_released, pointer_left,
                 wheel_scrolled, key_event, framebuffer_resized, window_focus,
                 window_close_requested>;

// ── Presentation settings ────────────────────────────────────────────────

enum class display_mode : std::uint8_t {
  shaded,        // faces only
  shaded_edges,  // faces plus host edges
  wireframe,     // host edges, hidden lines removed by a depth pre-pass
  xray,          // host edges, all drawn
};

struct section_plane {
  avionix::plane value{};
  bool enabled{true};

  friend constexpr bool operator==(const section_plane&,
                                   const section_plane&) noexcept = default;
};

inline constexpr std::size_t max_section_planes = 4;

// Overlay geometry for dimensions, annotations, and construction aids.
// When `attached` names a node, points are in that node's local frame and
// follow its presented placement (including exploded views and drag
// previews); otherwise they are world coordinates.
struct overlay_line {
  vec3 a{};
  vec3 b{};
  rgba color{1, 1, 1, 1};
  node_id attached{};
};
struct overlay_marker {
  vec3 at{};
  rgba color{1, 1, 1, 1};
  node_id attached{};
  double size{8.0};  // logical pixels
};
struct overlay_set {
  std::vector<overlay_line> lines;
  std::vector<overlay_marker> markers;
  bool depth_tested{false};  // false: drawn over the model
};

// Everything a saved view needs to restore the picture. The host stores
// these; the viewport only applies them.
struct view_state {
  avionix::camera view{};
  display_mode mode{display_mode::shaded_edges};
  bool exploded{false};
  std::vector<section_plane> sections;

  friend bool operator==(const view_state&, const view_state&) = default;
};

// ── Captures ─────────────────────────────────────────────────────────────

struct capture_request {
  avionix::camera view{};  // rendered exactly; the window camera is not used
  pixel_extent extent{};   // output size in pixels, 1..8192 per side
  // When set, the capture renders only if the scene is still at this
  // revision; otherwise it completes with capture_status::stale and no
  // pixels.
  std::optional<std::uint64_t> revision{};
  std::optional<display_mode> mode{};  // default: the viewport's mode
  bool exploded{false};                // render the exploded presentation
  bool show_selection{false};          // include selection highlight
};

enum class capture_status : std::uint8_t {
  complete,
  stale,   // the scene revision no longer matched the request
  failed,  // no graphics context or a GPU error; see detail
};

// ── Output events ────────────────────────────────────────────────────────

struct viewport_opened {
  pixel_extent extent{};
  double content_scale{1.0};
  std::string renderer;  // GL_RENDERER and GL_VERSION, for diagnostics
};

enum class close_reason : std::uint8_t { user, host, graphics_error };

struct viewport_closed {
  close_reason reason{};
};

struct viewport_focus_changed {
  bool focused{};
};

// The user's selection changed. `nodes` is the full new selection.
struct selection_changed {
  std::uint64_t scene_revision{};
  std::vector<node_id> nodes;
  std::optional<pick_hit> hit;  // the click that caused it, if any
};

struct hover_changed {
  std::uint64_t scene_revision{};
  std::optional<pick_hit> hit;
};

enum class gesture_phase : std::uint8_t { preview, commit, cancel };

enum class gesture_source : std::uint8_t { pointer, keyboard, host };

// An editing request. The viewport never changes the host's physical
// transforms. A gesture produces zero or more preview events, then exactly
// one commit or one cancel, all with the same `gesture` number.
//
// `proposed` is the node's new local transform in its parent frame. On
// commit the host applies it (or not) and submits a scene update with a
// newer revision. Until that update arrives the viewport keeps drawing the
// committed proposal, so the part does not snap back while the host works.
// Any accepted update with a newer revision clears it.
struct transform_intent {
  std::uint64_t gesture{};
  std::uint64_t scene_revision{};
  gesture_phase phase{};
  gesture_source source{};
  transform_kind kind{};
  axis_constraint constraint{};
  node_id node{};
  transform3d original{};
  transform3d proposed{};
};

// Typed value for the active keyboard or pointer gesture, for display by
// the host (for example in a terminal status line).
struct numeric_entry_changed {
  std::uint64_t gesture{};
  transform_kind kind{};
  axis_constraint constraint{};
  std::string text;
};

// End of a camera gesture (orbit, pan, zoom, fit, or set_camera).
struct camera_changed {
  avionix::camera view{};
};

// A key the viewport did not use, with the window focused.
struct viewport_key {
  key_event key{};
};

struct update_applied {
  std::uint64_t revision{};
};

// A posted update failed validation or arrived stale.
struct update_rejected {
  std::uint64_t revision{};
  error reason{};
};

struct capture_completed {
  std::uint64_t request{};
  capture_status status{};
  // The revision the pixels show (complete), or the current revision
  // (stale, failed).
  std::uint64_t scene_revision{};
  avionix::camera view{};
  image_rgba8 image{};
  std::string detail;
};

struct graphics_error {
  error reason{};
};

using viewport_event =
    std::variant<viewport_opened, viewport_closed, viewport_focus_changed,
                 selection_changed, hover_changed, transform_intent,
                 numeric_entry_changed, camera_changed, viewport_key, update_applied,
                 update_rejected, capture_completed, graphics_error>;

}  // namespace avionix
