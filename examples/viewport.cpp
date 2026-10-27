// A conversational-CAD style workspace: the terminal shows the assembly and
// an event log, and a companion window shows the 3D viewport.
//
//   cad_document (host: owns placements and revisions)
//        │ scene_update                         ▲ transform_intent commit
//        ▼                                      │ (host validates and applies)
//   avionix::viewport ──window──► orbit, pick, drag handles, explode, capture
//        │ viewport_event
//        ▼
//   terminal widgets (assembly list, status, log)
//
// Two component instances: a grounded base plate (the host denies every
// transform) and a pin (translate and rotate only, kept on top of the
// plate by a transform filter).
//
// Terminal keys: o open/close the window, e explode, c capture, f fit,
// 1/2 select base/pin, n move the pin 10 mm along X, w display mode,
// p projection, q quit. In the window: left-drag orbits, right/middle-drag
// or Shift+left pans, the wheel zooms, click selects, drag a handle to
// move or rotate, X/Y/Z then digits and Enter for exact values, T/R/S
// handle type, F fit selection, H fit all, 1/3/7/0 standard views, W
// display mode, E explode, C capture.
//
// `viewport --self-test [dir]` drives the real window through the same
// steps without a terminal and exits non-zero on any failed check. With a
// directory it also writes the captures there as PNG files.

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

import avionix;
import avionix.viewport;

namespace {

using namespace avionix;

// ── Tessellation the host would get from its CAD kernel ─────────────────

// A box with its base on z = 0. Faces 1..6 and edges 1..12 are the host
// identities a pick reports.
mesh_data box_mesh(vec3 size) {
  mesh_data m;
  const double hx = size.x / 2, hy = size.y / 2, h = size.z;
  const std::array<vec3, 8> c{vec3{-hx, -hy, 0}, {hx, -hy, 0},  {hx, hy, 0},
                              {-hx, hy, 0},      {-hx, -hy, h}, {hx, -hy, h},
                              {hx, hy, h},       {-hx, hy, h}};
  struct face {
    std::array<int, 4> v;
    vec3 n;
  };
  const std::array<face, 6> faces{
      face{{0, 3, 2, 1}, {0, 0, -1}}, face{{4, 5, 6, 7}, {0, 0, 1}},
      face{{0, 1, 5, 4}, {0, -1, 0}}, face{{1, 2, 6, 5}, {1, 0, 0}},
      face{{2, 3, 7, 6}, {0, 1, 0}},  face{{3, 0, 4, 7}, {-1, 0, 0}}};
  for (std::uint32_t f = 0; f < faces.size(); ++f) {
    const auto base = static_cast<std::uint32_t>(m.positions.size());
    for (const int v : faces[f].v) {
      m.positions.push_back(narrow(c[static_cast<std::size_t>(v)]));
      m.normals.push_back(narrow(faces[f].n));
    }
    m.indices.insert(m.indices.end(),
                     {base, base + 1, base + 2, base, base + 2, base + 3});
    m.triangle_faces.insert(m.triangle_faces.end(), {f + 1, f + 1});
  }
  const auto corner0 = static_cast<std::uint32_t>(m.positions.size());
  for (const vec3 p : c) {
    m.positions.push_back(narrow(p));
    m.normals.push_back({0, 0, 1});
  }
  const std::array<std::array<std::uint32_t, 2>, 12> edges{{{0, 1},
                                                            {1, 2},
                                                            {2, 3},
                                                            {3, 0},
                                                            {4, 5},
                                                            {5, 6},
                                                            {6, 7},
                                                            {7, 4},
                                                            {0, 4},
                                                            {1, 5},
                                                            {2, 6},
                                                            {3, 7}}};
  for (std::uint32_t e = 0; e < edges.size(); ++e) {
    m.edge_indices.insert(m.edge_indices.end(),
                          {corner0 + edges[e][0], corner0 + edges[e][1]});
    m.segment_edges.push_back(e + 1);
  }
  return m;
}

// A cylinder on z = 0. Faces: 1 side, 2 top, 3 bottom. Edges: 1 top
// circle, 2 bottom circle.
mesh_data cylinder_mesh(double radius, double height, std::uint32_t segments = 48) {
  mesh_data m;
  const auto add = [&](vec3 p, vec3 n) {
    m.positions.push_back(narrow(p));
    m.normals.push_back(narrow(n));
    return static_cast<std::uint32_t>(m.positions.size() - 1);
  };
  std::vector<std::uint32_t> side_bottom, side_top, cap_bottom, cap_top;
  for (std::uint32_t i = 0; i < segments; ++i) {
    const double a = 2.0 * pi * i / segments;
    const vec3 d{std::cos(a), std::sin(a), 0};
    side_bottom.push_back(add(d * radius, d));
    side_top.push_back(add(d * radius + vec3{0, 0, height}, d));
    cap_bottom.push_back(add(d * radius, {0, 0, -1}));
    cap_top.push_back(add(d * radius + vec3{0, 0, height}, {0, 0, 1}));
  }
  const std::uint32_t bottom_center = add({0, 0, 0}, {0, 0, -1});
  const std::uint32_t top_center = add({0, 0, height}, {0, 0, 1});
  for (std::uint32_t i = 0; i < segments; ++i) {
    const std::uint32_t j = (i + 1) % segments;
    m.indices.insert(m.indices.end(), {side_bottom[i], side_bottom[j], side_top[j],
                                       side_bottom[i], side_top[j], side_top[i]});
    m.triangle_faces.insert(m.triangle_faces.end(), {1, 1});
    m.indices.insert(m.indices.end(), {top_center, cap_top[i], cap_top[j]});
    m.triangle_faces.push_back(2);
    m.indices.insert(m.indices.end(), {bottom_center, cap_bottom[j], cap_bottom[i]});
    m.triangle_faces.push_back(3);
    m.edge_indices.insert(m.edge_indices.end(), {side_top[i], side_top[j]});
    m.segment_edges.push_back(1);
    m.edge_indices.insert(m.edge_indices.end(), {side_bottom[i], side_bottom[j]});
    m.segment_edges.push_back(2);
  }
  return m;
}

// ── The host document ───────────────────────────────────────────────────

constexpr node_id assembly_id{1};
constexpr node_id base_id{10};
constexpr node_id pin_id{11};
constexpr mesh_id plate_mesh{100};
constexpr mesh_id pin_mesh{101};
constexpr material_id steel{200};
constexpr material_id brass{201};
constexpr double plate_top = 10.0;

struct part {
  node_id id;
  std::string name;
  mesh_id mesh;
  material_id look;
  transform3d placement;
  transform3d exploded;
  transform_permissions permissions;
};

// Stand-in for Cloak: owns placements and revisions. Avionix never sees
// this object, only the scene updates it produces.
struct cad_document {
  std::uint64_t revision{};
  std::vector<part> parts{
      {base_id, "base plate", plate_mesh, steel, {}, {}, transform_permissions::none},
      {pin_id,
       "pin",
       pin_mesh,
       brass,
       {{18, 8, plate_top}, {}, {1, 1, 1}},
       {{0, 0, 45}, {}, {1, 1, 1}},
       transform_permissions::rigid},
  };

  part* find(node_id id) {
    for (auto& p : parts)
      if (p.id == id) return &p;
    return nullptr;
  }

  scene_update full_scene() {
    scene_update u{++revision, {}};
    u.commands.emplace_back(clear_scene{});
    u.commands.emplace_back(put_mesh{plate_mesh, box_mesh({80, 60, plate_top})});
    u.commands.emplace_back(put_mesh{pin_mesh, cylinder_mesh(8, 40)});
    u.commands.emplace_back(
        put_material{steel, {{0.62F, 0.66F, 0.72F, 1}, {0.1F, 0.1F, 0.12F, 1}, 0.5F}});
    u.commands.emplace_back(put_material{
        brass, {{0.86F, 0.68F, 0.30F, 1}, {0.25F, 0.16F, 0.04F, 1}, 0.8F}});
    u.commands.emplace_back(put_node{assembly_id, {.name = "assembly"}});
    for (const auto& p : parts) {
      u.commands.emplace_back(put_node{p.id,
                                       {.parent = assembly_id,
                                        .mesh = p.mesh,
                                        .material = p.look,
                                        .local = p.placement,
                                        .permissions = p.permissions,
                                        .name = p.name}});
      u.commands.emplace_back(set_presentation{p.id, p.exploded});
    }
    return u;
  }

  // Applies a committed intent if it was made against the current revision
  // and respects the document's rules.
  std::optional<scene_update> apply(const transform_intent& intent, std::string& why) {
    part* p = find(intent.node);
    if (p == nullptr) {
      why = "unknown part";
      return std::nullopt;
    }
    if (intent.scene_revision != revision) {
      why = std::format("intent made at revision {}, document is at {}",
                        intent.scene_revision, revision);
      return std::nullopt;
    }
    if (!allows_any(p->permissions, transform_permissions::all)) {
      why = std::format("{} is grounded", p->name);
      return std::nullopt;
    }
    p->placement = intent.proposed;
    return scene_update{++revision, {set_transform{p->id, p->placement}}};
  }
};

// The host's rule for the pin: it stays on top of the plate and is not
// scaled. Runs before each preview.
std::optional<transform3d> keep_on_plate(const transform_intent& intent) {
  if (intent.node != pin_id) return intent.proposed;
  transform3d t = intent.proposed;
  t.translation.z = std::max(t.translation.z, plate_top);
  t.scale = {1, 1, 1};
  return t;
}

std::string write_image(const image_rgba8& image, const std::string& path) {
  const auto bytes = encode_png(image);
  std::ofstream out{path, std::ios::binary};
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return out ? path : std::string{"(write failed)"};
}

std::string write_png(const capture_completed& c, std::string_view stem) {
  const std::string path =
      std::format("{}-r{}-{}.png", stem, c.scene_revision, c.request);
  const auto bytes = encode_png(c.image);
  std::ofstream out{path, std::ios::binary};
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return out ? path : std::string{"(write failed)"};
}

// Decodes the Sixel this example's panel sends, to check what a Sixel
// terminal will display. Handles the subset append_sixel produces.
image_rgba8 decode_sixel(std::string_view data) {
  image_rgba8 img;
  const auto number = [&](std::size_t& i) {
    std::uint32_t v = 0;
    while (i < data.size() && data[i] >= '0' && data[i] <= '9')
      v = v * 10 + static_cast<std::uint32_t>(data[i++] - '0');
    return v;
  };
  std::size_t i = data.find('"');
  if (i == std::string_view::npos) return img;
  ++i;
  number(i), ++i, number(i), ++i;
  img.width = number(i);
  ++i;
  img.height = number(i);
  img.pixels.assign(std::size_t{img.width} * img.height * 4, 0);
  std::vector<std::array<std::uint8_t, 3>> palette(256);
  std::uint32_t color = 0, x = 0, band = 0;
  while (i < data.size()) {
    const char c = data[i];
    if (c == '#') {
      ++i;
      color = number(i);
      if (i < data.size() && data[i] == ';') {
        ++i, number(i), ++i;
        std::array<std::uint32_t, 3> pct{};
        for (std::size_t k = 0; k < 3; ++k) {
          pct[k] = number(i);
          if (k < 2) ++i;
        }
        for (std::size_t k = 0; k < 3; ++k)
          palette[color][k] =
              static_cast<std::uint8_t>(std::lround(pct[k] * 255.0 / 100.0));
      }
      continue;
    }
    std::uint32_t repeat = 1;
    if (c == '!') {
      ++i;
      repeat = number(i);
    }
    const char ch = data[i++];
    if (ch == '$') {
      x = 0;
    } else if (ch == '-') {
      x = 0;
      band += 6;
    } else if (ch >= 63 && ch <= 126) {
      const auto bits = static_cast<std::uint32_t>(ch - 63);
      for (std::uint32_t r = 0; r < repeat; ++r, ++x) {
        for (std::uint32_t row = 0; row < 6; ++row) {
          if ((bits & (1U << row)) == 0 || x >= img.width || band + row >= img.height)
            continue;
          const std::size_t o = (std::size_t{band + row} * img.width + x) * 4;
          img.pixels[o] = palette[color][0];
          img.pixels[o + 1] = palette[color][1];
          img.pixels[o + 2] = palette[color][2];
          img.pixels[o + 3] = 255;
        }
      }
    } else if (ch == 27) {
      break;
    }
  }
  return img;
}

std::string describe(const transform3d& t) {
  return std::format("({:.1f}, {:.1f}, {:.1f})", t.translation.x, t.translation.y,
                     t.translation.z);
}

std::string_view phase_name(gesture_phase p) {
  switch (p) {
    case gesture_phase::preview:
      return "preview";
    case gesture_phase::commit:
      return "commit";
    case gesture_phase::cancel:
      return "cancel";
  }
  return "?";
}

// ── Scripted check against the real window ──────────────────────────────

struct self_test {
  viewport view{{.title = "Avionix viewport self-test", .width = 900, .height = 640}};
  cad_document doc;
  std::string output_dir;
  std::vector<viewport_event> events;
  int failures{};

  void check(bool ok, std::string_view what) {
    std::println("  {} {}", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
  }

  void pump_for(std::chrono::milliseconds total) {
    const auto end = std::chrono::steady_clock::now() + total;
    while (std::chrono::steady_clock::now() < end)
      view.pump(std::chrono::milliseconds{5});
  }

  template <typename T>
  std::vector<T> take() {
    std::vector<T> out;
    std::vector<viewport_event> rest;
    for (auto& e : events) {
      if (auto* v = std::get_if<T>(&e)) {
        out.push_back(std::move(*v));
      } else {
        rest.push_back(std::move(e));
      }
    }
    events = std::move(rest);
    return out;
  }

  std::optional<capture_completed> capture(capture_request request) {
    const std::uint64_t id = view.request_capture(std::move(request));
    for (int i = 0; i < 200; ++i) {
      view.pump(std::chrono::milliseconds{5});
      for (auto& c : take<capture_completed>())
        if (c.request == id) return c;
    }
    return std::nullopt;
  }

  void handle_commits() {
    view.process();
    for (const auto& intent : take<transform_intent>()) {
      if (intent.phase != gesture_phase::commit) continue;
      std::string why;
      if (auto update = doc.apply(intent, why)) (void)view.submit(std::move(*update));
    }
  }

  static bool differs(const image_rgba8& a, std::uint32_t x, std::uint32_t y,
                      const image_rgba8& b) {
    const auto p = a.at(x, y);
    const auto q = b.at(x, y);
    int d = 0;
    for (std::size_t i = 0; i < 3; ++i) d += std::abs(int{p[i]} - int{q[i]});
    return d > 30;
  }

  int run() {
    view.on_event([this](const viewport_event& e) { events.push_back(e); });
    view.set_transform_filter(keep_on_plate);
    std::println("viewport self-test");
    if (auto opened = view.open(); !opened) {
      std::println("  FAIL open: {}", opened.error().message());
      return 2;
    }
    pump_for(std::chrono::milliseconds{100});
    check(!take<viewport_opened>().empty(), "window opened");

    // Stage 1: two solids, camera, selection.
    check(view.submit(doc.full_scene()).has_value(),
          "scene with two component instances accepted");
    check(!view.submit(scene_update{doc.revision, {}}).has_value(),
          "update at the same revision is rejected");
    view.set_standard_view(standard_view::isometric);
    view.fit_scene();
    pump_for(std::chrono::milliseconds{150});
    const pixel_extent extent = view.extent();
    const vec2 center{extent.width / 2.0, extent.height / 2.0};

    const camera before_orbit = view.camera_value();
    view.simulate(pointer_pressed{center, pointer_button::left, {}});
    for (int i = 1; i <= 10; ++i)
      view.simulate(pointer_moved{center + vec2{i * 8.0, i * 2.0}, {}});
    view.simulate(pointer_released{center + vec2{80, 20}, pointer_button::left, {}});
    pump_for(std::chrono::milliseconds{50});
    check(view.camera_value().yaw != before_orbit.yaw, "left drag orbits the camera");
    check(!take<camera_changed>().empty(), "orbit reports camera_changed");
    check(view.selection().empty(), "orbit drag does not select");
    view.set_standard_view(standard_view::isometric);
    view.fit_scene();
    pump_for(std::chrono::milliseconds{50});

    const vec3 pin_mid =
        transform_point(view.scene().physical_world(pin_id), {0, 0, 20});
    const auto pin_px = view.project(pin_mid);
    check(pin_px.has_value(), "pin projects into the window");
    if (!pin_px) return 1;
    view.simulate(pointer_pressed{*pin_px, pointer_button::left, {}});
    view.simulate(pointer_released{*pin_px, pointer_button::left, {}});
    pump_for(std::chrono::milliseconds{30});
    const auto selections = take<selection_changed>();
    check(selections.size() == 1 && selections[0].nodes == std::vector{pin_id},
          "click selects the pin");
    check(!selections.empty() && selections[0].hit &&
              selections[0].hit->face != no_topology,
          "pick reports a host face id");

    // Stage 2: manipulation with the pointer on the X handle.
    const transform3d start = doc.find(pin_id)->placement;
    const auto origin_px =
        view.project(translation_of(view.scene().physical_world(pin_id)));
    const auto x_px = view.project(translation_of(view.scene().physical_world(pin_id)) +
                                   vec3{5, 0, 0});
    if (origin_px && x_px) {
      const vec2 dir = (*x_px - *origin_px) * (1.0 / length(*x_px - *origin_px));
      const vec2 grab = *origin_px + dir * (60.0 * view.content_scale());
      view.simulate(pointer_moved{grab, {}});
      view.simulate(pointer_pressed{grab, pointer_button::left, {}});
      for (int i = 1; i <= 8; ++i)
        view.simulate(pointer_moved{grab + dir * (i * 6.0), {}});
      view.simulate(pointer_released{grab + dir * 48.0, pointer_button::left, {}});
    }
    const auto intents = take<transform_intent>();
    std::size_t previews = 0, commits = 0, cancels = 0;
    for (const auto& i : intents) {
      previews += i.phase == gesture_phase::preview;
      commits += i.phase == gesture_phase::commit;
      cancels += i.phase == gesture_phase::cancel;
    }
    check(previews > 0, "handle drag emits previews");
    check(commits == 1 && cancels == 0, "handle drag ends with exactly one commit");
    check(doc.find(pin_id)->placement == start, "previews do not change the document");
    for (const auto& i : intents) events.push_back(i);
    handle_commits();
    const transform3d moved = doc.find(pin_id)->placement;
    check(moved.translation.x > start.translation.x + 1.0 &&
              moved.translation.y == start.translation.y,
          "commit moves the pin along X only");
    check(view.scene().node(pin_id)->desc.local == moved,
          "host update is what the viewport shows");

    // Keyboard numeric entry: Y axis, exactly -5.
    for (const key_event k :
         {key_event{key::character, U'y', {}}, key_event{key::character, U'-', {}},
          key_event{key::character, U'5', {}}, key_event{key::enter, 0, {}}})
      view.simulate(k);
    handle_commits();
    check(std::abs(doc.find(pin_id)->placement.translation.y -
                   (moved.translation.y - 5.0)) < 1e-9,
          "typed value moves exactly -5 along Y");

    // Host restriction: the filter keeps the pin on the plate.
    view.propose_transform(transform_kind::translate, axis_constraint::z, -100);
    handle_commits();
    check(std::abs(doc.find(pin_id)->placement.translation.z - plate_top) < 1e-9,
          "filter clamps the pin to the plate top");
    view.select({base_id});
    check(!view.propose_transform(transform_kind::translate, axis_constraint::x, 5),
          "grounded base plate refuses transforms");
    view.select({pin_id});
    view.propose_transform(transform_kind::scale, axis_constraint::x, 2);
    view.process();
    check(take<transform_intent>().empty(), "scale is not offered for the pin");

    // A worker's update computed against an old revision is rejected.
    std::jthread worker{[&, stale = doc.revision - 1] {
      view.post_update(scene_update{stale, {set_visibility{pin_id, false}}});
    }};
    worker.join();
    pump_for(std::chrono::milliseconds{30});
    check(
        take<update_rejected>().size() == 1 && view.scene().node(pin_id)->desc.visible,
        "stale posted update is rejected");

    // Exploded view.
    const mat4 physical = view.scene().physical_world(pin_id);
    view.set_exploded(true);
    pump_for(std::chrono::milliseconds{450});
    check(view.explode_factor() == 1.0, "explode animation completes");
    check(view.scene().physical_world(pin_id) == physical,
          "exploding leaves physical placement alone");
    const auto exploded_px =
        view.project(transform_point(physical, {0, 0, 20}) + vec3{0, 0, 45});
    if (exploded_px) {
      view.simulate(pointer_pressed{*exploded_px, pointer_button::left, {}});
      view.simulate(pointer_released{*exploded_px, pointer_button::left, {}});
    }
    check(view.selection() == std::vector{pin_id},
          "exploded pin is picked where it is drawn");
    take<selection_changed>();

    // Captures.
    capture_request request{view.camera_value(), {640, 480}, doc.revision,
                            std::nullopt,        true,       false};
    const auto exploded_shot = capture(request);
    check(exploded_shot && exploded_shot->status == capture_status::complete &&
              exploded_shot->image.width == 640 &&
              exploded_shot->scene_revision == doc.revision,
          "exploded capture completes at the requested size and revision");
    if (exploded_shot && !output_dir.empty())
      std::println("       wrote {}",
                   write_png(*exploded_shot, output_dir + "/viewport-exploded"));
    request.exploded = false;
    const auto assembled_shot = capture(request);
    check(assembled_shot && assembled_shot->status == capture_status::complete,
          "assembled capture completes");
    if (exploded_shot && assembled_shot) {
      bool any = false;
      for (std::uint32_t y = 0; y < 480 && !any; y += 4)
        for (std::uint32_t x = 0; x < 640 && !any; x += 4)
          any = differs(exploded_shot->image, x, y, assembled_shot->image);
      check(any, "exploded and assembled captures differ");
      if (!output_dir.empty())
        std::println("       wrote {}",
                     write_png(*assembled_shot, output_dir + "/viewport-assembled"));
    }
    request.revision = doc.revision - 1;
    const auto stale_shot = capture(request);
    check(stale_shot && stale_shot->status == capture_status::stale &&
              stale_shot->image.pixels.empty(),
          "capture for an old revision is reported stale without pixels");

    // Close and reopen.
    const std::uint64_t revision = view.revision();
    view.close();
    pump_for(std::chrono::milliseconds{30});
    check(!view.is_open() && !take<viewport_closed>().empty(), "window closes");
    check(view.revision() == revision && view.scene().node(pin_id) != nullptr &&
              view.selection() == std::vector{pin_id},
          "scene and selection survive close");
    request.revision = revision;
    const auto closed_shot = capture(request);
    check(closed_shot && closed_shot->status == capture_status::complete,
          "capture works while closed");
    check(view.open().has_value(), "window reopens");
    pump_for(std::chrono::milliseconds{100});
    const auto reopened_shot = capture(request);
    check(reopened_shot && reopened_shot->status == capture_status::complete,
          "capture works after reopening");
    if (closed_shot && reopened_shot && assembled_shot)
      check(closed_shot->image.pixels.size() == assembled_shot->image.pixels.size(),
            "captures keep explicit dimensions");

    // Terminal section: the same viewport drawn by a viewport_panel.
    {
      viewport_panel panel{view};
      render_buffer screen{{100, 40}};
      const auto draw = [&] {
        frame_state frame;
        frame.graphics = {graphics_protocol::sixel, {10, 20}};
        render_context whole{screen, screen.area(), frame};
        render_context area = whole.child({{0, 0}, {80, 30}});
        panel.render(area);
        return frame.placements.size();
      };
      check(draw() == 1 && panel.shown_as() == graphics_protocol::sixel,
            "terminal panel mirrors the open window");
      view.close();
      pump_for(std::chrono::milliseconds{20});
      check(draw() == 1 && view.extent() == pixel_extent{800, 600},
            "with the window closed the panel is the drawing surface");
      {
        // What a Sixel terminal shows: decode the payload the panel sent.
        frame_state frame;
        frame.graphics = {graphics_protocol::sixel, {10, 20}};
        render_context whole{screen, screen.area(), frame};
        render_context area = whole.child({{0, 0}, {80, 30}});
        panel.render(area);
        if (!frame.placements.empty()) {
          const image_rgba8 shown = decode_sixel(frame.placements[0].payload);
          const auto direct = view.render_view({800, 600});
          check(shown.width == 800 && shown.height == 600,
                "Sixel decodes at the panel's pixel size");
          if (direct) {
            long long total = 0;
            for (std::size_t k = 0; k < shown.pixels.size(); k += 4)
              for (std::size_t c = 0; c < 3; ++c)
                total +=
                    std::abs(int{shown.pixels[k + c]} - int{direct->pixels[k + c]});
            const double mean = static_cast<double>(total) /
                                static_cast<double>(shown.pixels.size() / 4 * 3);
            std::println("       sixel mean channel error {:.2f} of 255, {} bytes",
                         mean, frame.placements[0].payload.size());
            check(mean < 6.0, "Sixel colors stay close to the render");
          }
          if (!output_dir.empty())
            std::println("       wrote {}",
                         write_image(shown, output_dir + "/panel-sixel.png"));
        }
      }
      view.fit_scene();
      const auto gpu = view.render_view({800, 600});
      check(gpu.has_value() && view.view_renderer() == "opengl",
            "panel picture rendered with OpenGL");
      viewport soft{{.hover = false, .software_only = true}};
      cad_document copy = doc;
      copy.revision = 0;
      (void)soft.submit(copy.full_scene());
      soft.set_surface({800, 600}, view.content_scale());
      soft.set_camera(view.camera_value());
      soft.select(view.selection());
      const auto cpu = soft.render_view({800, 600});
      check(cpu.has_value() && soft.view_renderer() == "cpu",
            "panel picture rendered on the CPU");
      // Half blocks: rebuild a picture from the cells' colors (one cell is
      // a top and a bottom pixel) to check what a plain terminal shows.
      {
        render_buffer cells{{100, 40}};
        frame_state frame;
        render_context whole{cells, cells.area(), frame};
        render_context area = whole.child({{0, 0}, {80, 30}});
        panel.render(area);
        check(frame.placements.empty() && panel.shown_as() == graphics_protocol::none,
              "without a pixel protocol the panel draws quadrant characters");
        // Each cell becomes 2x2 preview pixels: quadrants set in the glyph
        // take the foreground, the rest the background.
        static constexpr std::array<std::string_view, 16> glyphs{
            " ",      "\u2598", "\u259D", "\u2580", "\u2596", "\u258C",
            "\u259E", "\u259B", "\u2597", "\u259A", "\u2590", "\u259C",
            "\u2584", "\u2599", "\u259F", "\u2588"};
        image_rgba8 preview{160, 60, std::vector<std::uint8_t>(160 * 60 * 4, 255)};
        std::size_t colored = 0;
        for (std::int32_t y = 0; y < 30; ++y) {
          for (std::int32_t x = 0; x < 80; ++x) {
            const cell& c = cells.at({x, y});
            std::uint32_t pattern = 0;
            for (std::uint32_t k = 0; k < 16; ++k)
              if (c.text() == glyphs[k]) pattern = k;
            const color fg = c.appearance().foreground;
            const color bg = c.appearance().background;
            colored += fg.type() == color::kind::rgb;
            for (std::uint32_t quadrant = 0; quadrant < 4; ++quadrant) {
              const color v = (pattern >> quadrant) & 1U ? fg : bg;
              const std::size_t px = static_cast<std::size_t>(x) * 2 + (quadrant & 1U);
              const std::size_t py = static_cast<std::size_t>(y) * 2 + (quadrant >> 1U);
              const std::size_t o = (py * 160 + px) * 4;
              preview.pixels[o] = v.red();
              preview.pixels[o + 1] = v.green();
              preview.pixels[o + 2] = v.blue();
            }
          }
        }
        check(colored > 2000, "character cells carry 24-bit colors");
        if (!output_dir.empty())
          std::println("       wrote {}",
                       write_image(preview, output_dir + "/panel-blocks.png"));
        view.set_surface({800, 600}, 1.0);
      }
      // Transparent background: the model over whatever the terminal shows.
      view.set_transparent_background(true);
      soft.set_transparent_background(true);
      const auto clear_gpu = view.render_view({800, 600});
      const auto clear_cpu = soft.render_view({800, 600});
      check(clear_gpu && clear_gpu->at(2, 2)[3] == 0 && clear_cpu &&
                clear_cpu->at(2, 2)[3] == 0,
            "transparent background renders with alpha 0 around the model");
      if (clear_gpu && clear_cpu && !output_dir.empty()) {
        std::println(
            "       wrote {}",
            write_image(*clear_gpu, output_dir + "/panel-transparent-opengl.png"));
        std::println(
            "       wrote {}",
            write_image(*clear_cpu, output_dir + "/panel-transparent-cpu.png"));
      }
      view.set_transparent_background(false);
      if (gpu && cpu && !output_dir.empty()) {
        std::println("       wrote {}",
                     write_image(*gpu, output_dir + "/panel-opengl.png"));
        std::println("       wrote {}",
                     write_image(*cpu, output_dir + "/panel-cpu.png"));
      }
    }

    view.close();
    std::println("{} failed", failures);
    return failures == 0 ? 0 : 1;
  }
};

// ── Interactive workspace ───────────────────────────────────────────────

int run_workspace(bool open_window_at_start, int quit_after_ms) {
  // Mouse motion lets the 3D section highlight what is under the pointer.
  application app{{.mouse_motion = true}};
  viewport view{{.title = "Avionix viewport"}};
  cad_document doc;

  column root;
  root.add(constraint::fixed(1), text{" Avionix CAD workspace",
                                      {.foreground = colors::black,
                                       .background = colors::cyan,
                                       .add = attribute::bold}});
  auto& body = root.add(constraint::fill(), row{1});
  auto& left = body.add(constraint::percentage(22), column{});
  auto& tree_panel = left.add(constraint::fixed(5), block{"Assembly"});
  auto& tree =
      tree_panel.set_child(list_view{{"assembly", "  base plate (grounded)", "  pin"}});
  auto& state_panel = left.add(constraint::fill(), block{"Viewport"});
  auto& model_panel = body.add(constraint::fill(), block{"Model"});
  auto& panel = model_panel.set_child(viewport_panel{view});
  auto& right = body.add(constraint::percentage(26), column{});
  auto& state = state_panel.set_child(text{});
  state.wrap(true);
  auto& log_panel = right.add(constraint::fill(), block{"Events"});
  auto& log = log_panel.set_child(text{});
  log.wrap(true);
  root.add(constraint::fixed(1),
           text{" drag orbit  right-drag pan  wheel zoom  click select  t/r/s handles  "
                "x/y/z+digits move  o window  b background  e explode  c capture  1/2 "
                "select  n +10  w mode  p proj  q quit",
                {.foreground = colors::bright_black}});

  std::vector<std::string> lines;
  const auto note = [&](std::string line) {
    lines.push_back(std::move(line));
    if (lines.size() > 60) lines.erase(lines.begin());
    std::string joined;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) joined += *it + '\n';
    log.set(std::move(joined));
  };
  std::string numeric;
  std::string last_capture = "none";
  std::string window_note;
  const auto refresh = [&] {
    std::string selected;
    for (const node_id id : view.selection())
      if (const auto* n = view.scene().node(id))
        selected += (selected.empty() ? "" : ", ") + n->desc.name;
    const auto& cam = view.camera_value();
    state.set(std::format(
        "shown as   {} via {}{}\nwindow     {}{}\nrevision   {}\nselection  {}\npin    "
        "    {}\nexploded   {}\ncamera     {}  yaw {:.0f}°  pitch {:.0f}°\nentry      "
        "{}\ncapture    {}",
        name(panel.shown_as()), view.view_renderer(),
        view.transparent_background() ? ", transparent" : "",
        view.is_open() ? "open" : "closed", window_note, view.revision(),
        selected.empty() ? "none" : selected, describe(doc.find(pin_id)->placement),
        view.exploded() ? "yes" : "no",
        cam.projection == projection_kind::perspective ? "perspective" : "orthographic",
        degrees(cam.yaw), degrees(cam.pitch), numeric.empty() ? "-" : numeric,
        last_capture));
  };

  const auto open_window = [&] {
    if (auto opened = view.open(); !opened) {
      window_note = " (unavailable)";
      note(std::format("viewport: {}", opened.error().message()));
    } else {
      window_note.clear();
    }
  };
  const auto capture_now = [&] {
    const auto e = view.extent();
    const auto id =
        view.request_capture({view.camera_value(),
                              {std::max(e.width, 1U), std::max(e.height, 1U)},
                              view.revision(),
                              std::nullopt,
                              view.exploded(),
                              true});
    note(std::format("capture {} requested at revision {}", id, view.revision()));
  };
  const auto select_from_tree = [&](std::size_t index) {
    const std::array<node_id, 3> ids{assembly_id, base_id, pin_id};
    view.select({ids[std::min<std::size_t>(index, 2)]});
    refresh();
  };

  view.attach(app);
  view.set_transform_filter(keep_on_plate);
  view.on_event([&](const viewport_event& e) {
    std::visit(
        [&](const auto& v) {
          using T = std::remove_cvref_t<decltype(v)>;
          if constexpr (std::is_same_v<T, viewport_opened>) {
            note(std::format("window opened: {}", v.renderer));
          } else if constexpr (std::is_same_v<T, viewport_closed>) {
            note(v.reason == close_reason::user
                     ? "window closed by the user (o reopens)"
                     : "window closed");
          } else if constexpr (std::is_same_v<T, selection_changed>) {
            if (!v.nodes.empty())
              tree.select_without_callback(
                  v.nodes.front() == pin_id ? 2 : (v.nodes.front() == base_id ? 1 : 0));
            if (v.hit)
              note(std::format(
                  "picked node {} face {} edge {} at revision {}", v.hit->node.value,
                  v.hit->face == no_topology ? -1 : static_cast<long long>(v.hit->face),
                  v.hit->edge == no_topology ? -1 : static_cast<long long>(v.hit->edge),
                  v.scene_revision));
          } else if constexpr (std::is_same_v<T, transform_intent>) {
            if (v.phase == gesture_phase::preview) return;
            numeric.clear();
            note(std::format("gesture {} {} -> {}", v.gesture, phase_name(v.phase),
                             describe(v.proposed)));
            if (v.phase != gesture_phase::commit) return;
            std::string why;
            if (auto update = doc.apply(v, why)) {
              if (auto ok = view.submit(std::move(*update)); !ok)
                note(ok.error().message());
            } else {
              note(std::format("host rejected: {}", why));
              view.discard_pending();
            }
          } else if constexpr (std::is_same_v<T, numeric_entry_changed>) {
            numeric = std::format(
                "{} {} {}", v.kind == transform_kind::rotate ? "rotate" : "move",
                "xyz"[std::min<std::size_t>(static_cast<std::size_t>(v.constraint), 2)],
                v.text.empty() ? "_" : v.text);
          } else if constexpr (std::is_same_v<T, capture_completed>) {
            if (v.status == capture_status::complete) {
              last_capture = write_png(v, "viewport-capture");
              note(std::format("capture {} saved {}", v.request, last_capture));
            } else {
              note(std::format("capture {} not used: {}", v.request, v.detail));
            }
          } else if constexpr (std::is_same_v<T, viewport_key>) {
            if (v.key.is(U'e')) view.set_exploded(!view.exploded());
            if (v.key.is(U'c')) capture_now();
          } else if constexpr (std::is_same_v<T, graphics_error>) {
            note(v.reason.message());
          } else if constexpr (std::is_same_v<T, update_rejected>) {
            note(v.reason.message());
          }
        },
        e);
    refresh();
  });

  if (auto ok = view.submit(doc.full_scene()); !ok) note(ok.error().message());
  view.set_standard_view(standard_view::isometric);
  view.fit_scene();
  if (open_window_at_start) {
    open_window();
    view.fit_scene();
  }
  // Runs after the first frame, when the panel has set the surface size.
  app.post([&] {
    view.fit_scene();
    view.select({pin_id});
  });
  // For unattended checks: quit by itself.
  std::optional<interval_handle> quit_timer;
  if (quit_after_ms > 0)
    quit_timer =
        app.set_interval(std::chrono::milliseconds{quit_after_ms}, [&] { app.quit(); });

  tree.on_select(
      [&](std::size_t index, const std::string&) { select_from_tree(index); });
  app.on_event(
      [&](const event& e) {
        const auto* k = std::get_if<key_event>(&e);
        if (k == nullptr || k->code != key::character || k->mods != modifiers::none)
          return false;
        switch (k->character) {
          case U'q':
            app.quit();
            break;
          case U'o':
            if (view.is_open()) {
              view.close();
            } else {
              open_window();
            }
            break;
          case U'e':
            view.set_exploded(!view.exploded());
            break;
          case U'c':
            capture_now();
            break;
          case U'f':
            view.fit_selection();
            break;
          case U'1':
            select_from_tree(1);
            break;
          case U'2':
            select_from_tree(2);
            break;
          case U'n':
            view.select({pin_id});
            view.propose_transform(transform_kind::translate, axis_constraint::x, 10);
            break;
          case U'w':
            view.set_display_mode(
                static_cast<display_mode>((static_cast<int>(view.mode()) + 1) % 4));
            break;
          case U'b':
            view.set_transparent_background(!view.transparent_background());
            break;
          case U'p':
            view.set_projection(view.camera_value().projection ==
                                        projection_kind::perspective
                                    ? projection_kind::orthographic
                                    : projection_kind::perspective);
            break;
          default:
            return false;
        }
        refresh();
        return true;
      },
      true);
  refresh();

  auto result = app.run(root);
  view.detach();
  view.close();
  if (!result) {
    std::println(stderr, "viewport example: {}", result.error().message());
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool window = false;
  int quit_after = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--window") window = true;
    if (arg == "--quit-after" && i + 1 < argc) quit_after = std::atoi(argv[++i]);
  }
  if (argc > 1 && std::string_view{argv[1]} == "--self-test") {
    self_test test;
    if (argc > 2) test.output_dir = argv[2];
    return test.run();
  }
  return run_workspace(window, quit_after);
}
