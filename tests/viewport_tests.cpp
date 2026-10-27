// 3D viewport: math, camera, scene revisions, picking, handles, and the
// viewport's interaction rules. Everything here runs with the window
// closed; one test renders through a hidden OpenGL context and reports a
// skip when no display or OpenGL 3.3 is available.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <print>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

import avionix.entity.spatial;
import avionix.entity.scene;
import avionix.entity.camera;
import avionix.entity.picking;
import avionix.entity.manipulator;
import avionix.entity.image;
import avionix.entity.viewport;
import avionix.entity.event;
import avionix.entity.error;
import avionix.object.event_queue;
import avionix.object.scene_store;
import avionix.interface.viewport;
import avionix.interface.widget;
import avionix.interface.viewport_panel;
import avionix.object.buffer;
import avionix.object.renderer;
import avionix.entity.geometry;
import avionix.entity.terminal_graphics;
import avionix_tests.check;

using namespace avionix;
using avionix_tests::check;
using avionix_tests::check_equal;

namespace {

bool near(double a, double b, double eps = 1e-9) { return std::abs(a - b) <= eps; }
bool near(vec3 a, vec3 b, double eps = 1e-9) {
  return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

// A unit-ish cube on z = 0 with face ids 1..6 (two triangles each) and four
// top edges with ids 1..4.
mesh_data cube(double s = 10.0) {
  mesh_data m;
  const double h = s / 2;
  const std::vector<vec3> c{{-h, -h, 0}, {h, -h, 0}, {h, h, 0}, {-h, h, 0},
                            {-h, -h, s}, {h, -h, s}, {h, h, s}, {-h, h, s}};
  for (const vec3 p : c) m.positions.push_back(narrow(p));
  m.indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
               1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7};
  m.triangle_faces = {1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6};
  m.edge_indices = {4, 5, 5, 6, 6, 7, 7, 4};
  m.segment_edges = {1, 2, 3, 4};
  return m;
}

constexpr node_id root{1};
constexpr node_id a_id{2};
constexpr node_id b_id{3};
constexpr mesh_id cube_mesh{10};

// Two instances of one mesh under an assembly root.
scene_update two_instances(std::uint64_t revision) {
  scene_update u{revision, {}};
  u.commands.emplace_back(put_mesh{cube_mesh, cube()});
  u.commands.emplace_back(put_node{root, {.name = "root"}});
  u.commands.emplace_back(put_node{
      a_id, {.parent = root, .mesh = cube_mesh, .local = {{-20, 0, 0}}, .name = "a"}});
  u.commands.emplace_back(put_node{
      b_id, {.parent = root, .mesh = cube_mesh, .local = {{20, 0, 0}}, .name = "b"}});
  u.commands.emplace_back(set_presentation{b_id, {{0, 0, 30}}});
  return u;
}

camera front_camera() {
  camera c;
  c = oriented(c, standard_view::front);
  c.target = {0, 0, 5};
  c.distance = 120;
  return c;
}

const avionix_tests::suite spatial{
    "spatial",
    {
        {"inverse_round_trips",
         [] {
           const transform3d t{{3, -4, 5}, axis_angle({1, 2, 3}, 0.7), {2, 3, 4}};
           const mat4 m = t.matrix();
           const vec3 p{1, 2, 3};
           check(near(transform_point(inverse(m), transform_point(m, p)), p, 1e-9));
         }},
        {"quaternion_rotates_right_handed",
         [] {
           check(near(rotate(axis_angle(axis_z, pi / 2), axis_x), axis_y, 1e-12));
           check(near(transform_vector(rotation(axis_angle(axis_x, pi / 2)), axis_y),
                      axis_z, 1e-12));
         }},
        {"transformed_bounds_cover_corners",
         [] {
           aabb box;
           box.extend(vec3{-1, -1, -1});
           box.extend(vec3{1, 1, 1});
           const aabb r = transformed(box, rotation(axis_angle(axis_z, pi / 4)));
           check(near(r.max.x, std::sqrt(2.0), 1e-9));
         }},
    }};

const avionix_tests::suite camera_suite{
    "camera",
    {
        {"ray_and_projection_agree",
         [] {
           const camera c = front_camera();
           for (const auto projection :
                {projection_kind::perspective, projection_kind::orthographic}) {
             const camera cp = with_projection(c, projection);
             const view_frame f = frame_for(cp, {800, 600}, {});
             const vec3 world{7, 3, 12};
             const auto px = project(f, world);
             check(px.has_value());
             const ray r = ray_through(f, {px->x, px->y});
             const vec3 to = world - r.origin;
             const double t = dot(to, r.direction);
             check(near(r.at(t), world, 1e-6),
                   "ray passes through the projected point");
           }
         }},
        {"front_view_axes",
         [] {
           const view_frame f = frame_for(front_camera(), {800, 600}, {});
           check(near(f.right, axis_x, 1e-12) && near(f.up, axis_z, 1e-12) &&
                 near(f.forward, axis_y, 1e-12));
           const auto right_point = project(f, {10, 0, 5});
           check(right_point && right_point->x > 400.0,
                 "+X is to the right in the front view");
         }},
        {"top_view_is_valid",
         [] {
           const view_frame f =
               frame_for(oriented(front_camera(), standard_view::top), {800, 600}, {});
           check(near(f.forward, -axis_z, 1e-12) && near(f.up, axis_y, 1e-9),
                 "top view looks down with +Y up");
         }},
        {"fit_contains_bounds_at_any_aspect",
         [] {
           aabb box;
           box.extend(vec3{-50, -10, 0});
           box.extend(vec3{50, 10, 30});
           for (const pixel_extent e :
                {pixel_extent{1600, 400}, pixel_extent{400, 1600},
                 pixel_extent{512, 512}}) {
             for (const auto projection :
                  {projection_kind::perspective, projection_kind::orthographic}) {
               camera c = oriented(camera{}, standard_view::isometric);
               c.projection = projection;
               c = fit(c, box, e.aspect());
               const view_frame f = frame_for(c, e, box);
               bool inside = true;
               for (std::uint32_t i = 0; i < 8; ++i) {
                 const auto p = project(f, {(i & 1U) ? box.max.x : box.min.x,
                                            (i & 2U) ? box.max.y : box.min.y,
                                            (i & 4U) ? box.max.z : box.min.z});
                 inside = inside && p && p->x >= 0 && p->x <= e.width && p->y >= 0 &&
                          p->y <= e.height && p->z >= -1 && p->z <= 1;
               }
               check(inside, "every corner is on screen and inside the depth range");
             }
           }
         }},
        {"vertical_framing_is_independent_of_output_size",
         [] {
           const camera c = front_camera();
           const auto small = project(frame_for(c, {320, 240}, {}), {0, 0, 25});
           const auto large = project(frame_for(c, {1920, 1440}, {}), {0, 0, 25});
           const auto wide = project(frame_for(c, {2000, 240}, {}), {0, 0, 25});
           check(small && large && wide);
           check(near(small->y / 240.0, large->y / 1440.0, 1e-9) &&
                 near(small->y / 240.0, wide->y / 240.0, 1e-9));
         }},
        {"zoom_keeps_the_cursor_point",
         [] {
           for (const auto projection :
                {projection_kind::perspective, projection_kind::orthographic}) {
             const camera c = with_projection(front_camera(), projection);
             const view_frame f = frame_for(c, {800, 600}, {});
             const vec2 cursor{600, 150};
             const ray r = ray_through(f, cursor);
             const vec3 anchor = r.at(intersect(r, plane_through(c.target, f.forward)));
             const camera z = zoom(c, f, 0.5, cursor);
             const auto p = project(frame_for(z, {800, 600}, {}), anchor);
             check(p && near(p->x, cursor.x, 1e-6) && near(p->y, cursor.y, 1e-6));
           }
         }},
        {"projection_switch_keeps_target_scale",
         [] {
           const camera c = front_camera();
           const camera o = with_projection(c, projection_kind::orthographic);
           const double a = units_per_pixel(frame_for(c, {800, 600}, {}), c.target);
           const double b = units_per_pixel(frame_for(o, {800, 600}, {}), c.target);
           check(near(a, b, 1e-9));
         }},
        {"orbit_clamps_pitch",
         [] {
           const camera c = orbit(front_camera(), 0.0, 10.0);
           check(near(c.pitch, pi / 2));
         }},
    }};

const avionix_tests::suite store_suite{
    "scene_store",
    {
        {"applies_and_reuses_meshes",
         [] {
           scene_store s;
           check(s.apply(two_instances(1)).has_value());
           check_equal(s.revision(), std::uint64_t{1});
           check(s.node(a_id)->desc.mesh == s.node(b_id)->desc.mesh);
           check_equal(s.meshes().size(), std::size_t{1});
           check_equal(s.roots().size(), std::size_t{1});
           check_equal(s.node(root)->children.size(), std::size_t{2});
         }},
        {"rejects_stale_revisions",
         [] {
           scene_store s;
           check(s.apply(two_instances(5)).has_value());
           auto stale = s.apply(scene_update{5, {set_visibility{a_id, false}}});
           check(!stale && stale.error().kind == failure::stale_revision);
           check(s.node(a_id)->desc.visible, "nothing applied");
           auto older = s.apply(scene_update{3, {}});
           check(!older && older.error().kind == failure::stale_revision);
         }},
        {"invalid_updates_change_nothing",
         [] {
           scene_store s;
           check(s.apply(two_instances(1)).has_value());
           // Valid first command, invalid second: the first must not apply.
           auto missing_parent =
               s.apply(scene_update{2,
                                    {set_visibility{a_id, false},
                                     put_node{node_id{9}, {.parent = node_id{99}}}}});
           check(!missing_parent &&
                 missing_parent.error().kind == failure::invalid_argument);
           check(s.node(a_id)->desc.visible && s.revision() == 1);
           auto dangling = s.apply(scene_update{2, {remove_mesh{cube_mesh}}});
           check(!dangling, "mesh still referenced");
           auto cycle = s.apply(scene_update{2, {put_node{root, {.parent = a_id}}}});
           check(!cycle, "parent cycle");
           mesh_data bad = cube();
           bad.indices.push_back(400);
           bad.indices.push_back(0);
           bad.indices.push_back(1);
           auto out_of_range =
               s.apply(scene_update{2, {put_mesh{mesh_id{11}, std::move(bad)}}});
           check(!out_of_range, "index out of range");
           auto orphan = s.apply(scene_update{2, {remove_node{root}}});
           check(!orphan, "children left without a parent");
           check(s
                     .apply(scene_update{
                         2, {remove_node{a_id}, remove_node{b_id}, remove_node{root}}})
                     .has_value());
         }},
        {"presentation_is_separate_from_physical",
         [] {
           scene_store s;
           check(s.apply(two_instances(1)).has_value());
           const mat4 physical = s.physical_world(b_id);
           std::vector<scene_store::resolved_node> out;
           s.resolve(out, 1.0, nullptr);
           mat4 presented{};
           for (const auto& r : out)
             if (r.id == b_id) presented = r.world;
           check(near(translation_of(physical), {20, 0, 0}));
           check(near(translation_of(presented), {20, 0, 30}));
           s.resolve(out, 0.5, nullptr);
           for (const auto& r : out)
             if (r.id == b_id) check(near(translation_of(r.world), {20, 0, 15}));
           check(s.physical_world(b_id) == physical);
         }},
        {"versions_change_on_edit",
         [] {
           scene_store s;
           check(s.apply(two_instances(1)).has_value());
           const auto mesh_version = s.mesh(cube_mesh)->version;
           const auto node_version = s.node(a_id)->version;
           check(s.apply(scene_update{2, {set_transform{a_id, {{1, 2, 3}}}}})
                     .has_value());
           check(s.node(a_id)->version != node_version);
           check_equal(s.mesh(cube_mesh)->version, mesh_version);
           check(s.apply(scene_update{3, {put_mesh{cube_mesh, cube(4)}}}).has_value());
           check(s.mesh(cube_mesh)->version != mesh_version);
         }},
        {"selection_target_walks_to_selectable_ancestor",
         [] {
           scene_store s;
           check(s.apply(two_instances(1)).has_value());
           check(s.apply(scene_update{2,
                                      {put_node{a_id,
                                                {.parent = root,
                                                 .mesh = cube_mesh,
                                                 .selectable = false}}}})
                     .has_value());
           check(s.selection_target(a_id) == root);
           check(s.selection_target(b_id) == b_id);
         }},
    }};

std::vector<pick_target> targets_of(const scene_store& s, double explode,
                                    std::vector<scene_store::resolved_node>& resolved) {
  s.resolve(resolved, explode, nullptr);
  std::vector<pick_target> out;
  for (const auto& r : resolved) {
    if (!r.record->desc.mesh) continue;
    const auto* m = s.mesh(r.record->desc.mesh);
    out.push_back({r.id, r.record->desc.mesh, &m->data, r.world, inverse(r.world),
                   transformed(m->bounds, r.world)});
  }
  return out;
}

const avionix_tests::suite picking_suite{
    "picking",
    {
        {"reports_host_face_ids",
         [] {
           scene_store s;
           check(s.apply(two_instances(1)).has_value());
           std::vector<scene_store::resolved_node> resolved;
           const auto targets = targets_of(s, 0.0, resolved);
           const view_frame f = frame_for(front_camera(), {800, 600}, {});
           const auto p = project(f, {20, -5, 5});
           const auto hit = pick(targets, f, {p->x, p->y}, {.edges = false});
           check(hit && hit->node == b_id);
           check(hit && hit->face == 3, "front face of the cube is face 3");
           check(hit && near(hit->world_point, {20, -5, 5}, 1e-6));
         }},
        {"reports_host_edge_ids",
         [] {
           scene_store s;
           check(s.apply(two_instances(1)).has_value());
           std::vector<scene_store::resolved_node> resolved;
           const auto targets = targets_of(s, 0.0, resolved);
           const view_frame f = frame_for(front_camera(), {800, 600}, {});
           // The top front edge of b, from (15,-5,10) to (25,-5,10), is edge 1.
           const auto p = project(f, {20, -5, 10});
           const auto hit = pick(targets, f, {p->x + 1, p->y + 2});
           check(hit && hit->node == b_id && hit->edge == 1);
         }},
        {"misses_report_nothing",
         [] {
           scene_store s;
           check(s.apply(two_instances(1)).has_value());
           std::vector<scene_store::resolved_node> resolved;
           const auto targets = targets_of(s, 0.0, resolved);
           check(!pick(targets, frame_for(front_camera(), {800, 600}, {}), {5, 5})
                      .has_value());
         }},
        {"exploded_geometry_is_hit_where_drawn",
         [] {
           scene_store s;
           check(s.apply(two_instances(1)).has_value());
           std::vector<scene_store::resolved_node> resolved;
           const auto targets = targets_of(s, 1.0, resolved);
           const view_frame f = frame_for(front_camera(), {800, 600}, {});
           const auto drawn = project(f, {20, -5, 35});
           check(pick(targets, f, {drawn->x, drawn->y}, {.edges = false}).has_value());
           const auto physical = project(f, {20, -5, 5});
           check(!pick(targets, f, {physical->x, physical->y}, {.edges = false})
                      .has_value());
         }},
        {"section_planes_remove_hits",
         [] {
           scene_store s;
           check(s.apply(two_instances(1)).has_value());
           std::vector<scene_store::resolved_node> resolved;
           const auto targets = targets_of(s, 0.0, resolved);
           const view_frame f = frame_for(front_camera(), {800, 600}, {});
           const auto p = project(f, {20, -5, 5});
           // Keep y <= 0: the front face at y = -5 survives.
           const std::vector<plane> keep_front{plane{{0, 1, 0}, 0.0}};
           const auto kept = pick(targets, f, {p->x, p->y},
                                  {.section_planes = keep_front, .edges = false});
           check(kept && kept->face == 3);
           // Keep y >= 0: the front face is cut away and the back face is hit from
           // inside.
           const std::vector<plane> keep_back{plane{{0, -1, 0}, 0.0}};
           const auto cut = pick(targets, f, {p->x, p->y},
                                 {.section_planes = keep_back, .edges = false});
           check(cut && cut->face == 5 && cut->world_point.y > 4.9);
         }},
        {"orthographic_rays_hit_behind_the_eye_plane",
         [] {
           scene_store s;
           check(s.apply(two_instances(1)).has_value());
           std::vector<scene_store::resolved_node> resolved;
           const auto targets = targets_of(s, 0.0, resolved);
           camera c = with_projection(front_camera(), projection_kind::orthographic);
           c.distance = 1.0;  // the eye sits inside the scene's depth range
           const view_frame f = frame_for(c, {800, 600}, {});
           const auto p = project(f, {20, 0, 5});
           check(p && pick(targets, f, {p->x, p->y}, {.edges = false}).has_value());
         }},
    }};

const avionix_tests::suite manipulator_suite{
    "manipulator",
    {
        {"axis_drag_moves_along_the_axis_only",
         [] {
           const view_frame view = frame_for(
               oriented(front_camera(), standard_view::isometric), {800, 600}, {});
           const manipulator_frame f =
               make_frame(mat4{}, {{0, 0, 5}}, transform_permissions::all,
                          transform_space::parent, transform_kind::translate);
           const auto o = project(view, f.origin);
           const auto x = project(view, f.origin + axis_x * 10.0);
           const vec2 dir = (vec2{x->x, x->y} - vec2{o->x, o->y}) *
                            (1.0 / length(vec2{x->x, x->y} - vec2{o->x, o->y}));
           const vec2 start = vec2{o->x, o->y} + dir * 50.0;
           auto drag = begin_drag(f, transform_kind::translate, axis_constraint::x,
                                  view, start);
           check(drag.has_value());
           const transform3d t =
               drag_to(*drag, view, start + dir * 40.0 + vec2{0, 15}, {}, false);
           check(t.translation.x > 1.0 && near(t.translation.y, 0.0) &&
                 near(t.translation.z, 5.0));
         }},
        {"permissions_mask_components",
         [] {
           const view_frame view = frame_for(front_camera(), {800, 600}, {});
           const manipulator_frame f =
               make_frame(mat4{}, {}, transform_permissions::translate_x,
                          transform_space::parent, transform_kind::translate);
           check(
               constraint_permitted(f, transform_kind::translate, axis_constraint::x));
           check(
               !constraint_permitted(f, transform_kind::translate, axis_constraint::z));
           check(!constraint_permitted(f, transform_kind::rotate, axis_constraint::z));
           check(!numeric_transform(f, transform_kind::translate, axis_constraint::z, 5,
                                    view));
           const auto shapes = gizmo_shapes(f, view, transform_kind::translate, {});
           for (const auto& s : shapes)
             check(s.constraint == axis_constraint::x, "only the X handle is offered");
         }},
        {"numeric_values",
         [] {
           const view_frame view = frame_for(front_camera(), {800, 600}, {});
           const manipulator_frame f =
               make_frame(mat4{}, {{1, 2, 3}}, transform_permissions::all,
                          transform_space::parent, transform_kind::rotate);
           const auto r = numeric_transform(f, transform_kind::rotate,
                                            axis_constraint::z, 90, view);
           check(r && near(rotate(r->rotation, axis_x), axis_y, 1e-12) &&
                 near(r->translation, {1, 2, 3}));
           const auto s = numeric_transform(f, transform_kind::scale,
                                            axis_constraint::xy, 2, view);
           check(s && near(s->scale, {2, 2, 1}));
           check(!numeric_transform(f, transform_kind::scale, axis_constraint::x, -1,
                                    view),
                 "negative scale refused");
           check(!numeric_transform(f, transform_kind::translate, axis_constraint::xy,
                                    3, view),
                 "plane takes no single value");
         }},
        {"world_delta_maps_through_rotated_parent",
         [] {
           // Parent rotated 90° about Z: world +X is parent -Y.
           const mat4 parent = rotation(axis_angle(axis_z, pi / 2));
           const view_frame view = frame_for(front_camera(), {800, 600}, {});
           const manipulator_frame f =
               make_frame(parent, {}, transform_permissions::all,
                          transform_space::world, transform_kind::translate);
           const auto t = numeric_transform(f, transform_kind::translate,
                                            axis_constraint::x, 4, view);
           check(t && near(t->translation, {0, -4, 0}, 1e-12));
         }},
        {"snapping",
         [] {
           const view_frame view = frame_for(front_camera(), {800, 600}, {});
           const manipulator_frame f =
               make_frame(mat4{}, {}, transform_permissions::all,
                          transform_space::parent, transform_kind::translate);
           const auto o = project(view, f.origin);
           const vec2 start{o->x + 50, o->y};
           auto drag = begin_drag(f, transform_kind::translate, axis_constraint::x,
                                  view, start);
           const transform3d t =
               drag_to(*drag, view, start + vec2{37, 0}, {.translate_step = 5.0}, true);
           check(near(std::fmod(t.translation.x, 5.0), 0.0, 1e-9));
         }},
        {"handles_hit_where_drawn",
         [] {
           const view_frame view = frame_for(front_camera(), {800, 600}, {});
           const manipulator_frame f =
               make_frame(mat4{}, {{0, 0, 5}}, transform_permissions::all,
                          transform_space::parent, transform_kind::rotate);
           const auto shapes = gizmo_shapes(f, view, transform_kind::rotate, {});
           for (const auto& s : shapes) {
             if (s.constraint != axis_constraint::y) continue;
             const auto hit = hit_handle(shapes, s.points[5], {});
             check(hit && *hit == axis_constraint::y,
                   "a point on the Y ring hits the Y ring");
           }
         }},
    }};

const avionix_tests::suite image_suite{
    "image",
    {
        {"png_structure",
         [] {
           image_rgba8 img{2, 1, {255, 0, 0, 255, 0, 255, 0, 255}};
           const auto png = encode_png(img);
           check(png.size() > 33 && png[0] == 0x89 && png[1] == 'P' && png[12] == 'I' &&
                 png[13] == 'H');
           // IEND chunk with its standard CRC ends every PNG.
           const std::vector<std::uint8_t> iend{0,   0,   0,    0,    'I',  'E',
                                                'N', 'D', 0xAE, 0x42, 0x60, 0x82};
           check(std::equal(iend.begin(), iend.end(), png.end() - 12));
         }},
    }};

// ── Viewport interaction, window closed ────────────────────────────────

struct harness {
  viewport view{{.hover = false}};
  std::vector<viewport_event> events;

  harness() {
    view.on_event([this](const viewport_event& e) { events.push_back(e); });
    (void)view.submit(two_instances(1));
    view.set_camera(front_camera());
    view.simulate(framebuffer_resized{{800, 600}, 1.0});
  }

  vec2 screen(vec3 world) { return *view.project(world); }
  void click(vec2 at, modifiers mods = {}) {
    view.simulate(pointer_pressed{at, pointer_button::left, mods});
    view.simulate(pointer_released{at, pointer_button::left, mods});
  }
  template <typename T>
  std::vector<T> all() const {
    std::vector<T> out;
    for (const auto& e : events)
      if (const auto* v = std::get_if<T>(&e)) out.push_back(*v);
    return out;
  }
  // Drags the X arrow of the selected node by `pixels`.
  void drag_x(double pixels, bool release = true) {
    const node_id id = view.selection().front();
    // Handles sit on the presented placement.
    const vec3 origin =
        transform_point(view.scene().parent_frame(id, view.explode_factor(), nullptr),
                        view.scene().node(id)->desc.local.translation);
    const vec2 o = screen(origin);
    const vec2 grab = o + vec2{60, 0};
    view.simulate(pointer_pressed{grab, pointer_button::left, {}});
    for (int i = 1; i <= 4; ++i)
      view.simulate(pointer_moved{grab + vec2{pixels * i / 4.0, 0}, {}});
    if (release)
      view.simulate(pointer_released{grab + vec2{pixels, 0}, pointer_button::left, {}});
  }
};

const avionix_tests::suite viewport_suite{
    "viewport",
    {
        {"click_selects_and_reports_the_hit",
         [] {
           harness h;
           h.click(h.screen({-20, -5, 5}));
           const auto changes = h.all<selection_changed>();
           check(changes.size() == 1 && changes[0].nodes == std::vector{a_id});
           check(!changes.empty() && changes[0].hit && changes[0].hit->face == 3 &&
                 changes[0].scene_revision == 1);
           h.click(h.screen({20, -5, 5}), modifiers::shift);
           check(h.view.selection() == std::vector{a_id, b_id}, "shift adds");
           h.click({5, 5});
           check(h.view.selection().empty(), "empty click clears");
         }},
        {"drag_orbits_without_selecting",
         [] {
           harness h;
           const camera before = h.view.camera_value();
           const vec2 at = h.screen({-20, -5, 5});
           h.view.simulate(pointer_pressed{at, pointer_button::left, {}});
           h.view.simulate(pointer_moved{at + vec2{40, 0}, {}});
           h.view.simulate(
               pointer_released{at + vec2{40, 0}, pointer_button::left, {}});
           check(h.view.camera_value().yaw != before.yaw && h.view.selection().empty());
         }},
        {"handle_drag_previews_then_commits_once",
         [] {
           harness h;
           h.view.select({b_id});
           h.drag_x(70);
           const auto intents = h.all<transform_intent>();
           std::size_t previews = 0, commits = 0;
           std::uint64_t gesture = 0;
           for (const auto& i : intents) {
             previews += i.phase == gesture_phase::preview;
             commits += i.phase == gesture_phase::commit;
             check(gesture == 0 || i.gesture == gesture, "one gesture number");
             gesture = i.gesture;
           }
           check(previews >= 1 && commits == 1);
           check(!intents.empty() && intents.back().phase == gesture_phase::commit &&
                 intents.back().proposed.translation.x > 20.5 &&
                 intents.back().original.translation.x == 20.0);
           check(h.view.scene().node(b_id)->desc.local.translation.x == 20.0,
                 "the viewport never edits the host scene");
           // The committed proposal stays drawn until the host answers: the
           // old position is empty.
           check(!h.view.pick(h.screen({20, -5, 5})).has_value());
           const double proposed = intents.back().proposed.translation.x;
           (void)h.view.submit(
               scene_update{2, {set_transform{b_id, intents.back().proposed}}});
           check(h.view.scene().node(b_id)->desc.local.translation.x == proposed);
         }},
        {"escape_cancels_and_restores",
         [] {
           harness h;
           h.view.select({b_id});
           h.drag_x(40, false);
           h.view.simulate(key_event{key::escape, 0, {}});
           const auto intents = h.all<transform_intent>();
           check(!intents.empty() && intents.back().phase == gesture_phase::cancel);
           std::size_t commits = 0;
           for (const auto& i : intents) commits += i.phase == gesture_phase::commit;
           check_equal(commits, std::size_t{0});
           // Releasing after cancel does not commit.
           h.view.simulate(pointer_released{{0, 0}, pointer_button::left, {}});
           check(h.all<transform_intent>().back().phase == gesture_phase::cancel);
         }},
        {"keyboard_numeric_entry",
         [] {
           harness h;
           h.view.select({b_id});
           for (const key_event k :
                {key_event{key::character, U'z', {}},
                 key_event{key::character, U'1', {}},
                 key_event{key::character, U'2', {}},
                 key_event{key::character, U'.', {}},
                 key_event{key::character, U'5', {}}, key_event{key::enter, 0, {}}})
             h.view.simulate(k);
           const auto intents = h.all<transform_intent>();
           check(!intents.empty() && intents.back().phase == gesture_phase::commit &&
                 near(intents.back().proposed.translation.z, 12.5) &&
                 intents.back().source == gesture_source::keyboard);
           const auto entries = h.all<numeric_entry_changed>();
           check(!entries.empty() && entries.back().text == "12.5");
         }},
        {"filter_and_permissions",
         [] {
           harness h;
           (void)h.view.submit(
               scene_update{2,
                            {put_node{a_id,
                                      {.parent = root,
                                       .mesh = cube_mesh,
                                       .local = {{-20, 0, 0}},
                                       .permissions = transform_permissions::none}}}});
           h.view.select({a_id});
           check(!h.view.propose_transform(transform_kind::translate,
                                           axis_constraint::x, 5));
           h.view.select({b_id});
           h.view.set_transform_filter(
               [](const transform_intent& i) -> std::optional<transform3d> {
                 transform3d t = i.proposed;
                 t.translation.x = std::min(t.translation.x, 22.0);
                 return t;
               });
           check(h.view.propose_transform(transform_kind::translate, axis_constraint::x,
                                          10));
           check(h.all<transform_intent>().empty(), "events wait for the loop");
           h.view.process();
           const auto intents = h.all<transform_intent>();
           check(intents.size() == 2 && intents[0].phase == gesture_phase::preview &&
                 intents[1].phase == gesture_phase::commit &&
                 near(intents[1].proposed.translation.x, 22.0));
         }},
        {"host_update_cancels_gesture_on_that_node",
         [] {
           harness h;
           h.view.select({b_id});
           h.drag_x(30, false);
           check(h.view.gesture_active());
           (void)h.view.submit(scene_update{2, {set_transform{b_id, {{0, 40, 0}}}}});
           h.view.process();
           check(!h.view.gesture_active() &&
                 h.all<transform_intent>().back().phase == gesture_phase::cancel);
         }},
        {"unrelated_update_keeps_gesture",
         [] {
           harness h;
           h.view.select({b_id});
           h.drag_x(30, false);
           (void)h.view.submit(scene_update{2, {set_visibility{a_id, false}}});
           check(h.view.gesture_active());
           h.view.simulate(pointer_released{{0, 0}, pointer_button::left, {}});
           check(h.all<transform_intent>().back().phase == gesture_phase::commit);
         }},
        {"removed_nodes_leave_the_selection",
         [] {
           harness h;
           h.view.select({a_id, b_id});
           (void)h.view.submit(scene_update{2, {remove_node{a_id}}});
           h.view.process();
           check(h.view.selection() == std::vector{b_id});
           check(!h.all<selection_changed>().empty());
         }},
        {"explode_keeps_physical_and_picks_presented",
         [] {
           harness h;
           const mat4 physical = h.view.scene().physical_world(b_id);
           h.view.set_exploded(true, false);
           check(h.view.explode_factor() == 1.0 &&
                 h.view.scene().physical_world(b_id) == physical);
           h.click(h.screen({20, -5, 35}));
           check(h.view.selection() == std::vector{b_id});
           // A drag while exploded proposes a physical transform.
           h.drag_x(40);
           const auto i = h.all<transform_intent>().back();
           check(i.phase == gesture_phase::commit &&
                 near(i.proposed.translation.z, 0.0) &&
                 i.proposed.translation.x > 20.5);
         }},
        {"posted_updates_apply_in_order_and_reject_stale",
         [] {
           harness h;
           std::jthread worker{[&] {
             h.view.post_update(scene_update{2, {set_visibility{a_id, false}}});
             h.view.post_update(scene_update{2, {set_visibility{b_id, false}}});
           }};
           worker.join();
           h.view.process();
           check(h.all<update_applied>().size() == 1 &&
                 h.all<update_rejected>().size() == 1);
           check(!h.view.scene().node(a_id)->desc.visible &&
                 h.view.scene().node(b_id)->desc.visible);
           check(h.all<update_rejected>().front().reason.kind ==
                 failure::stale_revision);
         }},
        {"stale_capture_is_rejected_without_rendering",
         [] {
           harness h;
           const auto id = h.view.request_capture({front_camera(),
                                                   {64, 64},
                                                   std::uint64_t{0},
                                                   std::nullopt,
                                                   false,
                                                   false});
           check(h.all<capture_completed>().empty(),
                 "never completes inside the request");
           h.view.process();
           const auto done = h.all<capture_completed>();
           check(done.size() == 1 && done[0].request == id &&
                 done[0].status == capture_status::stale &&
                 done[0].image.pixels.empty() && done[0].scene_revision == 1);
         }},
        {"capture_renders_offscreen",
         [] {
           harness h;
           camera c = front_camera();
           const auto id = h.view.request_capture(
               {c, {160, 120}, std::uint64_t{1}, display_mode::shaded, false, false});
           h.view.process();
           const auto done = h.all<capture_completed>();
           check(done.size() == 1 && done[0].request == id);
           if (done.empty()) return;
           if (done[0].status == capture_status::failed) {
             std::println("    SKIP capture_renders_offscreen: {}", done[0].detail);
             return;
           }
           check(done[0].status == capture_status::complete &&
                 done[0].image.width == 160 && done[0].image.height == 120 &&
                 done[0].view == c);
           // The cube at x = 20 is lit; the background corner is not cube colored.
           const auto px = project(frame_for(c, {160, 120}, {}), {20, -5, 5});
           const auto object = done[0].image.at(static_cast<std::uint32_t>(px->x),
                                                static_cast<std::uint32_t>(px->y));
           const auto corner = done[0].image.at(1, 1);
           int diff = 0;
           for (std::size_t i = 0; i < 3; ++i)
             diff += std::abs(int{object[i]} - int{corner[i]});
           check(diff > 60, "object pixels differ from the background");
         }},
        {"saved_view_round_trips",
         [] {
           harness h;
           h.view.set_display_mode(display_mode::wireframe);
           (void)h.view.set_section_planes({{plane{{1, 0, 0}, 0}, true}});
           const view_state saved = h.view.view();
           h.view.set_display_mode(display_mode::shaded);
           h.view.set_standard_view(standard_view::top);
           (void)h.view.set_section_planes({});
           h.view.restore(saved);
           check(h.view.view() == saved);
           check(!h.view.set_section_planes(std::vector<section_plane>(5)).has_value(),
                 "too many planes");
         }},
        {"submit_from_another_thread_is_refused",
         [] {
           harness h;
           std::optional<failure> kind;
           std::jthread worker{[&] {
             auto r = h.view.submit(scene_update{2, {}});
             if (!r) kind = r.error().kind;
           }};
           worker.join();
           check(kind == failure::wrong_thread && h.view.revision() == 1);
         }},
    }};

// A native loop that records waits and wakes, standing in for the window.
struct fake_loop final : native_loop {
  std::atomic<int> wakes{0};
  std::atomic<int> waits{0};
  void wait_until(std::chrono::steady_clock::time_point deadline) override {
    ++waits;
    while (wakes.load() == 0 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
  }
  void wake() noexcept override { ++wakes; }
};

const avionix_tests::suite native_loop_suite{
    "native_loop",
    {
        {"drain_waits_in_the_loop_and_producers_wake_it",
         [] {
           using namespace std::chrono_literals;
           event_queue q;
           fake_loop loop;
           q.set_native_loop(&loop);
           std::jthread producer{[&] {
             std::this_thread::sleep_for(20ms);
             q.push(event{user_event{7, 0}});
           }};
           std::vector<queue_entry> out;
           const auto start = std::chrono::steady_clock::now();
           std::size_t n = 0;
           while (n == 0 && std::chrono::steady_clock::now() - start < 2s)
             n = q.drain(out, std::chrono::steady_clock::now() + 1s, {});
           check_equal(n, std::size_t{1});
           check(loop.waits.load() >= 1 && loop.wakes.load() >= 1);
           check(std::chrono::steady_clock::now() - start < 1s,
                 "woken before the deadline");
           q.set_native_loop(nullptr);
           q.push(event{user_event{8, 0}});
           check_equal(loop.wakes.load(), 1);
         }},
    }};

int channel_difference(const image_rgba8& a, const image_rgba8& b) {
  long long total = 0;
  for (std::size_t i = 0; i < a.pixels.size(); i += 4)
    for (std::size_t c = 0; c < 3; ++c)
      total += std::abs(int{a.pixels[i + c]} - int{b.pixels[i + c]});
  return static_cast<int>(total / static_cast<long long>(a.pixels.size() / 4 * 3));
}

struct panel_frame {
  render_buffer buffer;
  std::vector<graphics_placement> placements;
};

panel_frame draw_panel(viewport_panel& panel, graphics_support support, rect area,
                       size screen) {
  panel_frame result{render_buffer{screen}, {}};
  frame_state frame;
  frame.graphics = support;
  render_context whole{result.buffer, result.buffer.area(), frame};
  render_context ctx = whole.child(area);
  panel.render(ctx);
  result.placements = std::move(frame.placements);
  return result;
}

const avionix_tests::suite terminal_suite{
    "viewport_terminal",
    {
        {"cpu_renderer_matches_opengl",
         [] {
           harness cpu;
           cpu.view.select({b_id});
           viewport gpu_view{{.hover = false}};
           (void)gpu_view.submit(two_instances(1));
           gpu_view.set_camera(front_camera());
           gpu_view.simulate(framebuffer_resized{{800, 600}, 1.0});
           gpu_view.select({b_id});
           viewport soft{{.hover = false, .software_only = true}};
           (void)soft.submit(two_instances(1));
           soft.set_camera(front_camera());
           soft.simulate(framebuffer_resized{{800, 600}, 1.0});
           soft.select({b_id});
           const auto c = soft.render_view({200, 150});
           check(c.has_value() && soft.view_renderer() == "cpu");
           if (!c) return;
           // The cube is drawn: its center differs from the background corner.
           const auto px =
               project(frame_for(front_camera(), {200, 150}, {}), {-20, -5, 5});
           const auto object = c->at(static_cast<std::uint32_t>(px->x),
                                     static_cast<std::uint32_t>(px->y));
           const auto corner = c->at(1, 1);
           int diff = 0;
           for (std::size_t i = 0; i < 3; ++i)
             diff += std::abs(int{object[i]} - int{corner[i]});
           check(diff > 60, "object differs from background");
           // The selected cube is tinted toward the highlight (orange: red > blue).
           const auto sel =
               project(frame_for(front_camera(), {200, 150}, {}), {20, -5, 5});
           const auto tinted = c->at(static_cast<std::uint32_t>(sel->x),
                                     static_cast<std::uint32_t>(sel->y));
           check(tinted[0] > tinted[2] + 20, "selection highlight");
           const auto g = gpu_view.render_view({200, 150});
           if (!g || gpu_view.view_renderer() != "opengl") {
             std::println("    SKIP OpenGL comparison: no context");
             return;
           }
           const int d = channel_difference(*c, *g);
           check(d < 10, "CPU and OpenGL pictures agree on average");
           std::println("    note: mean CPU/OpenGL channel difference {}", d);
         }},
        {"cpu_renderer_honors_section_planes",
         [] {
           viewport soft{{.hover = false, .axis_triad = false, .software_only = true}};
           (void)soft.submit(two_instances(1));
           soft.set_camera(front_camera());
           soft.simulate(framebuffer_resized{{200, 150}, 1.0});
           const auto px =
               project(frame_for(front_camera(), {200, 150}, {}), {-20, -5, 5});
           const auto before = soft.render_view({200, 150});
           // Remove everything with x > 0: cube b vanishes, cube a stays.
           (void)soft.set_section_planes({{plane{{1, 0, 0}, 0.0}, true}});
           const auto after = soft.render_view({200, 150});
           const auto b_px =
               project(frame_for(front_camera(), {200, 150}, {}), {20, -5, 5});
           check(before && after);
           if (!before || !after) return;
           const auto x = static_cast<std::uint32_t>(px->x);
           const auto y = static_cast<std::uint32_t>(px->y);
           check(before->at(x, y) == after->at(x, y), "kept side unchanged");
           const auto bx = static_cast<std::uint32_t>(b_px->x);
           const auto by = static_cast<std::uint32_t>(b_px->y);
           check(after->at(bx, by) == after->at(bx, 2) || after->at(bx, by)[0] < 120,
                 "cut side shows background");
         }},
        {"transparent_background",
         [] {
           const auto check_image = [](const image_rgba8& img, std::string_view which) {
             const auto object =
                 project(frame_for(front_camera(), {200, 150}, {}), {-20, -5, 5});
             check(img.at(1, 1)[3] == 0, "background is transparent");
             check(img.at(static_cast<std::uint32_t>(object->x),
                          static_cast<std::uint32_t>(object->y))[3] == 255,
                   "the model is opaque");
             // Partly covered edge pixels keep the model's brightness instead
             // of fading toward black.
             int darkest = 255;
             for (std::size_t i = 0; i < img.pixels.size(); i += 4) {
               const int a = img.pixels[i + 3];
               if (a >= 64 && a < 192)
                 darkest = std::min(
                     darkest, std::max({int{img.pixels[i]}, int{img.pixels[i + 1]},
                                        int{img.pixels[i + 2]}}));
             }
             check(darkest > 40, "no dark fringe on antialiased edges");
             std::println("    note: {} darkest partial-alpha pixel {}", which,
                          darkest);
           };
           for (const bool software : {true, false}) {
             viewport v{{.hover = false,
                         .axis_triad = false,
                         .software_only = software,
                         .transparent_background = true}};
             (void)v.submit(two_instances(1));
             v.set_camera(front_camera());
             v.set_surface({200, 150}, 1.0);
             // Faces only: drawn edges are dark by design.
             v.set_display_mode(display_mode::shaded);
             const auto img = v.render_view({200, 150});
             check(img.has_value());
             if (!img) continue;
             if (!software && v.view_renderer() != "opengl") {
               std::println("    SKIP OpenGL transparency: no context");
               continue;
             }
             check_image(*img, software ? "cpu" : "opengl");
           }
         }},
        {"panel_places_a_sixel_and_becomes_the_surface",
         [] {
           harness h;
           viewport_panel panel{h.view};
           const graphics_support sixel{graphics_protocol::sixel, {10, 20}};
           const auto frame = draw_panel(panel, sixel, {{2, 1}, {30, 12}}, {60, 20});
           check(frame.placements.size() == 1 &&
                 panel.shown_as() == graphics_protocol::sixel);
           check(panel.picture_size() == pixel_extent{300, 240});
           check(h.view.extent() == pixel_extent{300, 240},
                 "the panel is the drawing surface");
           check(!frame.placements.empty() &&
                 frame.placements[0].payload.starts_with(std::string{char{27}} + "P"));
           // Unchanged view: the same payload version.
           const auto again = draw_panel(panel, sixel, {{2, 1}, {30, 12}}, {60, 20});
           check(!again.placements.empty() &&
                 again.placements[0].version == frame.placements[0].version);
           check(!frame.placements[0].transparent, "the gradient background is opaque");
           h.view.set_transparent_background(true);
           const auto clear = draw_panel(panel, sixel, {{2, 1}, {30, 12}}, {60, 20});
           check(!clear.placements.empty() && clear.placements[0].transparent &&
                     clear.placements[0].version != frame.placements[0].version,
                 "transparent pictures are flagged");
         }},
        {"panel_drag_reuses_pixels_and_restores_quality",
         [] {
           harness h;
           viewport_panel panel{h.view};
           const graphics_support sixel{graphics_protocol::sixel, {10, 20}};
           const rect area{{0, 0}, {30, 12}};
           (void)draw_panel(panel, sixel, area, {40, 16});
           const auto* pixels = panel.picture().pixels.data();
           const auto extent = h.view.extent();
           event_context ctx;
           (void)panel.on_event(
               mouse_event{mouse_button::left, mouse_action::press, {3, 3}}, ctx);
           (void)panel.on_event(
               mouse_event{mouse_button::left, mouse_action::drag, {8, 3}}, ctx);
           const auto drag = draw_panel(panel, sixel, area, {40, 16});
           check(!drag.placements.empty());
           check(drag.placements[0].payload.find("#63;2;100;100;100") !=
                     std::string_view::npos,
                 "drag uses the fast 64-color palette");
           check(panel.picture().pixels.data() == pixels,
                 "pixel allocation is recycled");
           check(h.view.extent() == extent,
                 "drag keeps surface size and picking coordinates");
           const auto version = drag.placements[0].version;
           (void)panel.on_event(
               mouse_event{mouse_button::none, mouse_action::release, {8, 3}}, ctx);
           const auto settled = draw_panel(panel, sixel, area, {40, 16});
           check(settled.placements[0].version != version,
                 "release refreshes the adaptive palette");
           check(settled.placements[0].payload.find("#63;2;100;100;100") ==
                     std::string_view::npos,
                 "release restores the adaptive palette");
         }},
        {"panel_clicks_select_through_cells",
         [] {
           harness h;
           viewport_panel panel{h.view};
           const rect area{{0, 0}, {40, 15}};
           (void)draw_panel(panel, {graphics_protocol::sixel, {10, 20}}, area,
                            {40, 16});
           // Cube b's front face center in surface pixels, then in cells.
           const auto px = h.view.project({20, -5, 5});
           check(px.has_value());
           if (!px) return;
           const position cell{static_cast<std::int32_t>(px->x / 10.0),
                               static_cast<std::int32_t>(px->y / 20.0)};
           event_context ctx;
           check(panel.on_event(event{mouse_event{
                                    mouse_button::left, mouse_action::press, cell, {}}},
                                ctx) == event_result::handled);
           check(ctx.pointer_captured(), "the panel captures the drag");
           (void)panel.on_event(
               event{mouse_event{mouse_button::none, mouse_action::release, cell, {}}},
               ctx);
           check(h.view.selection() == std::vector{b_id});
           // A drag orbits.
           const camera before = h.view.camera_value();
           (void)panel.on_event(
               event{mouse_event{mouse_button::left, mouse_action::press, {5, 5}, {}}},
               ctx);
           (void)panel.on_event(
               event{mouse_event{mouse_button::left, mouse_action::drag, {12, 5}, {}}},
               ctx);
           (void)panel.on_event(
               event{
                   mouse_event{mouse_button::left, mouse_action::release, {12, 5}, {}}},
               ctx);
           check(h.view.camera_value().yaw != before.yaw);
           // Wheel zooms.
           const double distance = h.view.camera_value().distance;
           (void)panel.on_event(
               event{mouse_event{
                   mouse_button::wheel_up, mouse_action::press, {20, 7}, {}}},
               ctx);
           check(h.view.camera_value().distance < distance);
         }},
        {"panel_keys",
         [] {
           harness h;
           viewport_panel panel{h.view};
           event_context ctx;
           check(panel.on_event(event{key_event{key::character, U'q', {}}}, ctx) ==
                     event_result::ignored,
                 "application keys pass through");
           check(panel.on_event(event{key_event{key::character, U'7', {}}}, ctx) ==
                 event_result::handled);
           check(near(h.view.camera_value().pitch, pi / 2), "top view");
         }},
        {"panel_falls_back_to_half_blocks",
         [] {
           harness h;
           viewport_panel panel{h.view};
           const auto frame = draw_panel(panel, {}, {{0, 0}, {40, 12}}, {40, 13});
           check(frame.placements.empty() &&
                 panel.shown_as() == graphics_protocol::none);
           check(panel.picture_size() == pixel_extent{160, 96}, "4x8 samples per cell");
           std::size_t drawn = 0;
           for (std::uint32_t y = 0; y < 12; ++y)
             for (std::uint32_t x = 0; x < 40; ++x)
               drawn +=
                   frame.buffer
                       .at({static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)})
                       .appearance()
                       .foreground.type() == color::kind::rgb;
           check(drawn > 400, "the picture is drawn in colored cells");
         }},
    }};

}  // namespace
