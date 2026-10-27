module;

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

export module avionix.object.scene_store;

import avionix.entity.spatial;
import avionix.entity.scene;
import avionix.entity.error;

// The viewport's retained copy of the host scene.
//
//   scene_update ──validate (no mutation)──► reject: stale or invalid
//                └─apply (cannot fail)─────► records, topology, versions
//
// Ownership: the store owns every mesh_data, material, and node_desc it has
// accepted. GPU buffers are derived from it and can be rebuilt at any time,
// which is how the viewport survives closing and reopening its window.
//
// Versions: each mesh and node record carries a version that changes when
// the record is replaced or edited, so the renderer re-uploads only changed
// meshes and the viewport can tell when a node under an active drag
// changed underneath it.
//
// Threading: not synchronized. The viewport uses it on the UI thread.

export namespace avionix {

using transform_overrides = std::unordered_map<node_id, transform3d, id_hash>;

class scene_store {
 public:
  struct node_record {
    node_desc desc;
    transform3d presentation{};
    std::vector<node_id> children;
    std::uint64_t version{};
  };
  struct mesh_record {
    mesh_data data;
    aabb bounds;
    std::uint64_t version{};
  };

  // A node with its presented world matrix, in depth-first tree order.
  struct resolved_node {
    node_id id;
    const node_record* record{};
    mat4 world{};
    mat4 parent{};   // presented(parent) * explode(node)
    bool visible{};  // false when it or any ancestor is hidden
  };

  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

  // Applies `update` atomically. Fails without changing anything when the
  // revision is not newer than revision() or any command is invalid.
  [[nodiscard]] std::expected<void, error> apply(scene_update update) {
    if (update.revision <= revision_) {
      return std::unexpected(
          error{"apply scene update", failure::stale_revision,
                std::format("update revision {} is not newer than current revision {}",
                            update.revision, revision_),
                0});
    }
    if (auto valid = validate(update); !valid) return valid;
    for (auto& command : update.commands) {
      std::visit([this](auto& c) { execute(c); }, command);
    }
    rebuild_topology();
    revision_ = update.revision;
    return {};
  }

  [[nodiscard]] const node_record* node(node_id id) const {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
  }
  [[nodiscard]] const mesh_record* mesh(mesh_id id) const {
    const auto it = meshes_.find(id);
    return it == meshes_.end() ? nullptr : &it->second;
  }
  [[nodiscard]] const material* find_material(material_id id) const {
    const auto it = materials_.find(id);
    return it == materials_.end() ? nullptr : &it->second;
  }
  [[nodiscard]] const std::vector<node_id>& roots() const noexcept { return roots_; }
  // Nodes in first-insertion order.
  [[nodiscard]] const std::vector<node_id>& nodes() const noexcept { return order_; }
  [[nodiscard]] const std::unordered_map<mesh_id, mesh_record, id_hash>& meshes()
      const noexcept {
    return meshes_;
  }

  // Physical world matrix: the product of local transforms only.
  [[nodiscard]] mat4 physical_world(node_id id) const {
    mat4 m;
    for (const node_record* n = node(id); n != nullptr; n = node(n->desc.parent))
      m = n->desc.local.matrix() * m;
    return m;
  }

  // Resolves every node with exploded presentation scaled by `explode`
  // (0 = physical) and preview transforms from `overrides` replacing
  // locals. `out` is cleared first and reuses its capacity.
  void resolve(std::vector<resolved_node>& out, double explode,
               const transform_overrides* overrides) const {
    out.clear();
    out.reserve(nodes_.size());
    for (const node_id root : roots_)
      resolve_into(out, root, mat4{}, true, explode, overrides);
  }

  // presented(parent) * explode(node) for one node.
  [[nodiscard]] mat4 parent_frame(node_id id, double explode,
                                  const transform_overrides* overrides) const {
    const node_record* n = node(id);
    if (n == nullptr) return {};
    return presented(n->desc.parent, explode, overrides) * explode_matrix(*n, explode);
  }

  // Nearest ancestor-or-self with node_desc::selectable, or none.
  [[nodiscard]] node_id selection_target(node_id id) const {
    for (const node_record* n = node(id); n != nullptr;) {
      if (n->desc.selectable) return id;
      id = n->desc.parent;
      n = node(id);
    }
    return {};
  }

  [[nodiscard]] bool is_ancestor(node_id ancestor, node_id id) const {
    for (const node_record* n = node(id); n != nullptr; n = node(n->desc.parent))
      if (n->desc.parent == ancestor) return true;
    return false;
  }

 private:
  [[nodiscard]] static mat4 explode_matrix(const node_record& n, double explode) {
    if (explode <= 0.0) return {};
    return interpolate(transform3d{}, n.presentation, std::min(explode, 1.0)).matrix();
  }

  [[nodiscard]] mat4 presented(node_id id, double explode,
                               const transform_overrides* overrides) const {
    const node_record* n = node(id);
    if (n == nullptr) return {};
    return presented(n->desc.parent, explode, overrides) * explode_matrix(*n, explode) *
           local_of(id, *n, overrides).matrix();
  }

  [[nodiscard]] static const transform3d& local_of(
      node_id id, const node_record& n, const transform_overrides* overrides) {
    if (overrides != nullptr) {
      if (const auto it = overrides->find(id); it != overrides->end())
        return it->second;
    }
    return n.desc.local;
  }

  void resolve_into(std::vector<resolved_node>& out, node_id id,
                    const mat4& parent_world, bool parent_visible, double explode,
                    const transform_overrides* overrides) const {
    const node_record& n = nodes_.at(id);
    const mat4 parent = parent_world * explode_matrix(n, explode);
    const mat4 world = parent * local_of(id, n, overrides).matrix();
    const bool visible = parent_visible && n.desc.visible;
    out.push_back({id, &n, world, parent, visible});
    for (const node_id child : n.children)
      resolve_into(out, child, world, visible, explode, overrides);
  }

  [[nodiscard]] static bool finite(const transform3d& t) {
    const auto ok = [](double v) { return std::isfinite(v); };
    return ok(t.translation.x) && ok(t.translation.y) && ok(t.translation.z) &&
           ok(t.rotation.w) && ok(t.rotation.x) && ok(t.rotation.y) &&
           ok(t.rotation.z) && ok(t.scale.x) && ok(t.scale.y) && ok(t.scale.z) &&
           t.scale.x != 0.0 && t.scale.y != 0.0 && t.scale.z != 0.0;
  }

  // Replays the update over id-only copies of the scene to check every
  // reference in the final state. The real records are untouched.
  [[nodiscard]] std::expected<void, error> validate(const scene_update& update) const {
    struct shape {
      node_id parent;
      mesh_id mesh;
      material_id mat;
    };
    std::unordered_map<node_id, shape, id_hash> nodes;
    std::unordered_set<mesh_id, id_hash> meshes;
    std::unordered_set<material_id, id_hash> materials;
    nodes.reserve(nodes_.size());
    for (const auto& [id, n] : nodes_)
      nodes.emplace(id, shape{n.desc.parent, n.desc.mesh, n.desc.material});
    for (const auto& [id, m] : meshes_) meshes.insert(id);
    for (const auto& [id, m] : materials_) materials.insert(id);

    std::size_t index = 0;
    const auto fail = [&](std::string text) {
      return std::unexpected(error{
          "apply scene update", failure::invalid_argument,
          std::format("revision {}, command {}: {}", update.revision, index, text), 0});
    };
    for (; index < update.commands.size(); ++index) {
      const auto& command = update.commands[index];
      if (const auto* c = std::get_if<put_mesh>(&command)) {
        if (!c->id) return fail("mesh id 0");
        if (auto ok = avionix::validate(c->data); !ok) return fail(ok.error().context);
        meshes.insert(c->id);
      } else if (const auto* rm = std::get_if<remove_mesh>(&command)) {
        if (meshes.erase(rm->id) == 0)
          return fail(std::format("no mesh {}", rm->id.value));
      } else if (const auto* pm = std::get_if<put_material>(&command)) {
        if (!pm->id) return fail("material id 0");
        materials.insert(pm->id);
      } else if (const auto* rmat = std::get_if<remove_material>(&command)) {
        if (materials.erase(rmat->id) == 0)
          return fail(std::format("no material {}", rmat->id.value));
      } else if (const auto* pn = std::get_if<put_node>(&command)) {
        if (!pn->id) return fail("node id 0");
        if (pn->desc.parent == pn->id)
          return fail(std::format("node {} is its own parent", pn->id.value));
        if (!finite(pn->desc.local))
          return fail(std::format("node {} transform is not finite or has zero scale",
                                  pn->id.value));
        nodes[pn->id] = shape{pn->desc.parent, pn->desc.mesh, pn->desc.material};
      } else if (const auto* rn = std::get_if<remove_node>(&command)) {
        if (nodes.erase(rn->id) == 0)
          return fail(std::format("no node {}", rn->id.value));
      } else if (const auto* st = std::get_if<set_transform>(&command)) {
        if (!nodes.contains(st->id))
          return fail(std::format("no node {}", st->id.value));
        if (!finite(st->local))
          return fail(std::format("node {} transform is not finite or has zero scale",
                                  st->id.value));
      } else if (const auto* sv = std::get_if<set_visibility>(&command)) {
        if (!nodes.contains(sv->id))
          return fail(std::format("no node {}", sv->id.value));
      } else if (const auto* sp = std::get_if<set_presentation>(&command)) {
        if (!nodes.contains(sp->id))
          return fail(std::format("no node {}", sp->id.value));
        if (!finite(sp->presentation))
          return fail(std::format("node {} presentation is not finite", sp->id.value));
      } else if (std::holds_alternative<clear_scene>(command)) {
        nodes.clear();
        meshes.clear();
        materials.clear();
      }
    }

    index = update.commands.size();
    for (const auto& [id, s] : nodes) {
      if (s.parent && !nodes.contains(s.parent))
        return fail(
            std::format("node {} parent {} does not exist", id.value, s.parent.value));
      if (s.mesh && !meshes.contains(s.mesh))
        return fail(
            std::format("node {} mesh {} does not exist", id.value, s.mesh.value));
      if (s.mat && !materials.contains(s.mat))
        return fail(
            std::format("node {} material {} does not exist", id.value, s.mat.value));
      std::size_t steps = 0;
      for (node_id p = s.parent; p; p = nodes.at(p).parent) {
        if (p == id || ++steps > nodes.size())
          return fail(std::format("node {} is part of a parent cycle", id.value));
      }
    }
    return {};
  }

  void execute(put_mesh& c) {
    auto& record = meshes_[c.id];
    record.bounds = bounds_of(c.data);
    record.data = std::move(c.data);
    record.version = ++version_counter_;
  }
  void execute(remove_mesh& c) { meshes_.erase(c.id); }
  void execute(put_material& c) { materials_[c.id] = c.value; }
  void execute(remove_material& c) { materials_.erase(c.id); }
  void execute(put_node& c) {
    auto [it, inserted] = nodes_.try_emplace(c.id);
    if (inserted) order_.push_back(c.id);
    it->second.desc = std::move(c.desc);
    it->second.version = ++version_counter_;
  }
  void execute(remove_node& c) {
    nodes_.erase(c.id);
    std::erase(order_, c.id);
  }
  void execute(set_transform& c) {
    auto& n = nodes_.at(c.id);
    n.desc.local = c.local;
    n.version = ++version_counter_;
  }
  void execute(set_visibility& c) {
    auto& n = nodes_.at(c.id);
    n.desc.visible = c.visible;
    n.version = ++version_counter_;
  }
  void execute(set_presentation& c) { nodes_.at(c.id).presentation = c.presentation; }
  void execute(clear_scene&) {
    nodes_.clear();
    meshes_.clear();
    materials_.clear();
    order_.clear();
  }

  void rebuild_topology() {
    roots_.clear();
    for (auto& [id, n] : nodes_) n.children.clear();
    for (const node_id id : order_) {
      const node_id parent = nodes_.at(id).desc.parent;
      if (parent) {
        nodes_.at(parent).children.push_back(id);
      } else {
        roots_.push_back(id);
      }
    }
  }

  std::unordered_map<node_id, node_record, id_hash> nodes_;
  std::unordered_map<mesh_id, mesh_record, id_hash> meshes_;
  std::unordered_map<material_id, material, id_hash> materials_;
  std::vector<node_id> order_;
  std::vector<node_id> roots_;
  std::uint64_t revision_{};
  std::uint64_t version_counter_{};
};

}  // namespace avionix
