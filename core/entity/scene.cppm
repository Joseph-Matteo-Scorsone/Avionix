module;

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module avionix.entity.scene;

export import avionix.entity.spatial;
import avionix.entity.error;

// What a host submits to the 3D viewport. These are values: the viewport
// copies or moves them into its retained scene and never reads host memory
// after a call returns.
//
//   host (CAD kernel, document)                    Avionix viewport
//   ───────────────────────────                    ────────────────
//   exact B-rep, features, history                 nothing of these
//   tessellation ──mesh_data──────────────────────► retained mesh
//   component placement ──node_desc───────────────► retained node
//   face/edge identity ──triangle_faces───────────► returned in pick_hit
//                       └─segment_edges───────────► returned in pick_hit
//   revision counter ──scene_update.revision──────► stale updates rejected
//
// Identity. Every id is a host-chosen nonzero 64-bit value that stays the
// same across revisions for the same thing. Zero means "none". The viewport
// never invents ids, so a pick or an intent always names something the host
// created.

export namespace avionix {

template <typename Tag>
struct basic_id {
  std::uint64_t value{};

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
  friend constexpr bool operator==(const basic_id&, const basic_id&) noexcept = default;
  friend constexpr auto operator<=>(const basic_id&,
                                    const basic_id&) noexcept = default;
};

struct node_tag {};
struct mesh_tag {};
struct material_tag {};

// A placed thing: a component instance, an assembly, or a body.
using node_id = basic_id<node_tag>;
// Reusable tessellated geometry. Several nodes may draw the same mesh.
using mesh_id = basic_id<mesh_tag>;
using material_id = basic_id<material_tag>;

// Hasher for unordered containers keyed by an id.
struct id_hash {
  template <typename Tag>
  [[nodiscard]] std::size_t operator()(basic_id<Tag> id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value);
  }
};

// Host-defined face or edge identity carried through picking unchanged.
// no_topology marks a triangle or segment without one.
inline constexpr std::uint32_t no_topology = 0xFFFF'FFFFU;

// Tessellated geometry in the mesh's local frame, in document units.
//
// Ownership: the viewport takes this by value. Move it in to avoid a copy.
// The viewport keeps it until the mesh is removed or replaced, so it can
// rebuild GPU buffers after the window is closed and reopened and can pick
// on the CPU.
//
// Layout:
//   positions        one per vertex
//   normals          empty (flat shading is derived), or one per vertex
//   indices          triangle list, three per triangle, counter-clockwise
//                    when seen from outside
//   triangle_faces   empty, or one host face id per triangle
//   edge_indices     line list, two per segment, indexing positions
//   segment_edges    empty, or one host edge id per segment
//
// Edges are drawn as given. The viewport does not derive feature edges,
// so a host that wants B-rep edges drawn sends them here.
struct mesh_data {
  std::vector<vec3f> positions;
  std::vector<vec3f> normals;
  std::vector<std::uint32_t> indices;
  std::vector<std::uint32_t> triangle_faces;
  std::vector<std::uint32_t> edge_indices;
  std::vector<std::uint32_t> segment_edges;

  [[nodiscard]] std::size_t triangle_count() const noexcept {
    return indices.size() / 3;
  }
  [[nodiscard]] std::size_t segment_count() const noexcept {
    return edge_indices.size() / 2;
  }
};

[[nodiscard]] inline std::expected<void, error> validate(const mesh_data& mesh) {
  const auto fail = [](std::string text) {
    return std::unexpected(
        error{"validate mesh", failure::invalid_argument, std::move(text), 0});
  };
  const std::size_t vertices = mesh.positions.size();
  if (!mesh.normals.empty() && mesh.normals.size() != vertices)
    return fail(
        std::format("{} normals for {} positions", mesh.normals.size(), vertices));
  if (mesh.indices.size() % 3 != 0)
    return fail(
        std::format("{} triangle indices is not a multiple of 3", mesh.indices.size()));
  if (mesh.edge_indices.size() % 2 != 0)
    return fail(std::format("{} edge indices is not a multiple of 2",
                            mesh.edge_indices.size()));
  if (!mesh.triangle_faces.empty() &&
      mesh.triangle_faces.size() != mesh.triangle_count())
    return fail(std::format("{} face ids for {} triangles", mesh.triangle_faces.size(),
                            mesh.triangle_count()));
  if (!mesh.segment_edges.empty() && mesh.segment_edges.size() != mesh.segment_count())
    return fail(std::format("{} edge ids for {} segments", mesh.segment_edges.size(),
                            mesh.segment_count()));
  for (const std::uint32_t i : mesh.indices)
    if (i >= vertices)
      return fail(std::format("triangle index {} >= {} vertices", i, vertices));
  for (const std::uint32_t i : mesh.edge_indices)
    if (i >= vertices)
      return fail(std::format("edge index {} >= {} vertices", i, vertices));
  return {};
}

[[nodiscard]] inline aabb bounds_of(const mesh_data& mesh) noexcept {
  aabb box;
  for (const vec3f p : mesh.positions) box.extend(widen(p));
  return box;
}

// Linear RGBA in [0, 1].
struct rgba {
  float r{};
  float g{};
  float b{};
  float a{1.0F};

  friend constexpr bool operator==(const rgba&, const rgba&) noexcept = default;
};

struct material {
  rgba base_color{0.72F, 0.74F, 0.78F, 1.0F};
  rgba edge_color{0.08F, 0.09F, 0.11F, 1.0F};
  // 0 is matte, 1 is a tight highlight.
  float shininess{0.35F};

  friend constexpr bool operator==(const material&, const material&) noexcept = default;
};

// Which components of a node's local transform the user may change. The
// viewport hides and ignores handles for denied axes. Translation and
// rotation axes are the parent frame's axes; scale axes are the node's own.
enum class transform_permissions : std::uint16_t {
  none = 0,
  translate_x = 1U << 0U,
  translate_y = 1U << 1U,
  translate_z = 1U << 2U,
  rotate_x = 1U << 3U,
  rotate_y = 1U << 4U,
  rotate_z = 1U << 5U,
  scale_x = 1U << 6U,
  scale_y = 1U << 7U,
  scale_z = 1U << 8U,
  translate = translate_x | translate_y | translate_z,
  rotate = rotate_x | rotate_y | rotate_z,
  scale = scale_x | scale_y | scale_z,
  rigid = translate | rotate,
  all = translate | rotate | scale,
};

[[nodiscard]] constexpr transform_permissions operator|(
    transform_permissions a, transform_permissions b) noexcept {
  return static_cast<transform_permissions>(static_cast<std::uint16_t>(a) |
                                            static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr transform_permissions operator&(
    transform_permissions a, transform_permissions b) noexcept {
  return static_cast<transform_permissions>(static_cast<std::uint16_t>(a) &
                                            static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr bool allows(transform_permissions set,
                                    transform_permissions flag) noexcept {
  return (set & flag) == flag && flag != transform_permissions::none;
}
[[nodiscard]] constexpr bool allows_any(transform_permissions set,
                                        transform_permissions flags) noexcept {
  return (set & flags) != transform_permissions::none;
}

// A node in the assembly tree.
//
// Physical placement is `local` in the parent's frame. world(node) =
// world(parent) * local(node). Exploded views never change `local`; see
// set_presentation.
struct node_desc {
  node_id parent{};        // none: a root
  mesh_id mesh{};          // none: a grouping node such as an assembly
  material_id material{};  // none: the default material
  transform3d local{};
  bool visible{true};   // hides this node and its subtree
  bool pickable{true};  // false: rays pass through its mesh
  // Clicking geometry selects the nearest ancestor-or-self that is
  // selectable. Mark parts of a rigid component unselectable to select the
  // component instead.
  bool selectable{true};
  transform_permissions permissions{transform_permissions::all};
  std::string name{};  // shown in diagnostics; not interpreted

  friend bool operator==(const node_desc&, const node_desc&) = default;
};

// Scene update commands. Commands in one update apply in order.
struct put_mesh {
  mesh_id id;
  mesh_data data;
};
struct remove_mesh {
  mesh_id id;
};
struct put_material {
  material_id id;
  avionix::material value;
};
struct remove_material {
  material_id id;
};
// Creates or replaces a node. Replacing keeps its presentation transform.
struct put_node {
  node_id id;
  node_desc desc;
};
// Removes a node. Its children must be removed or reparented in the same
// update.
struct remove_node {
  node_id id;
};
struct set_transform {
  node_id id;
  transform3d local;
};
struct set_visibility {
  node_id id;
  bool visible{true};
};
// Exploded-view placement, separate from physical placement. The
// presentation transform sits between the parent and the node:
//
//   presented(node) = presented(parent) * lerp(identity, presentation, f) * local(node)
//
// where f is the viewport's explode factor in [0, 1]. f = 0 shows the
// physical assembly; f = 1 shows the full exploded layout. Physical
// queries and transform intents always use local(node).
struct set_presentation {
  node_id id;
  transform3d presentation;
};
// Removes every node, mesh, and material.
struct clear_scene {};

using scene_command = std::variant<put_mesh, remove_mesh, put_material, remove_material,
                                   put_node, remove_node, set_transform, set_visibility,
                                   set_presentation, clear_scene>;

// One atomic change to the retained scene.
//
// `revision` is the host's document revision after this change. It must be
// greater than the viewport's current revision. An update with an equal or
// older revision is rejected with failure::stale_revision and nothing in it
// applies, so an update computed on a worker against an older document can
// never overwrite a newer one. Invalid commands (bad mesh, missing parent,
// cycle, dangling mesh reference) reject the whole update the same way.
struct scene_update {
  std::uint64_t revision{};
  std::vector<scene_command> commands;
};

// Length unit of document coordinates. The viewport reports it back and
// uses it only for labels; it never scales geometry.
enum class length_unit : std::uint8_t {
  unitless,
  millimeter,
  centimeter,
  meter,
  inch,
  foot
};

[[nodiscard]] constexpr std::string_view symbol(length_unit unit) noexcept {
  switch (unit) {
    case length_unit::unitless:
      return "";
    case length_unit::millimeter:
      return "mm";
    case length_unit::centimeter:
      return "cm";
    case length_unit::meter:
      return "m";
    case length_unit::inch:
      return "in";
    case length_unit::foot:
      return "ft";
  }
  return "";
}

}  // namespace avionix
