// A wallpaper behind the TUI, a PNG logo panel, and a 3D model floating
// over the wallpaper.
//
//   backdrop (JPEG wallpaper)
//     └─ column
//          header
//          row ── logo (PNG, image_view) and status
//              └─ Model: viewport_panel, transparent background
//          footer
//
// Where the wallpaper shows depends on the terminal:
//
//   Kitty, Ghostty, WezTerm   behind everything: the image sits under the
//                             text, like a terminal background image
//   Windows Terminal, iTerm2, behind the 3D model: these terminals cannot
//   Zed, others               draw pixels under text, so the Model panel
//                             composites the wallpaper itself. For a
//                             full-window wallpaper, set the terminal's own
//                             background image (the status panel says how).
//
// Usage: both [wallpaper] [logo] [--quit-after ms] [--self-test dir]
// Defaults: assets/QuantHood.jpg and assets/s.png from the working
// directory (zig build run-viewport_background runs from the repository root).
//
// Keys: b wallpaper on/off, d brightness, i swap images, t model background,
// plus the viewport's camera and handle keys in the Model panel. q quits.

// std::getenv is deprecated by the Windows CRT; this read-only use is fine.
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

import avionix;
import avionix.viewport;

namespace {

using namespace avionix;

mesh_data box_mesh(vec3 size) {
  mesh_data m;
  const double hx = size.x / 2, hy = size.y / 2, h = size.z;
  const std::array<vec3, 8> c{vec3{-hx, -hy, 0}, {hx, -hy, 0},  {hx, hy, 0},
                              {-hx, hy, 0},      {-hx, -hy, h}, {hx, -hy, h},
                              {hx, hy, h},       {-hx, hy, h}};
  const std::array<std::array<int, 4>, 6> faces{{{0, 3, 2, 1},
                                                 {4, 5, 6, 7},
                                                 {0, 1, 5, 4},
                                                 {1, 2, 6, 5},
                                                 {2, 3, 7, 6},
                                                 {3, 0, 4, 7}}};
  for (std::uint32_t f = 0; f < 6; ++f) {
    const auto base = static_cast<std::uint32_t>(m.positions.size());
    for (const int v : faces[f])
      m.positions.push_back(narrow(c[static_cast<std::size_t>(v)]));
    m.indices.insert(m.indices.end(),
                     {base, base + 1, base + 2, base, base + 2, base + 3});
    m.triangle_faces.insert(m.triangle_faces.end(), {f + 1, f + 1});
  }
  const auto corners = static_cast<std::uint32_t>(m.positions.size());
  for (const vec3 p : c) m.positions.push_back(narrow(p));
  for (const auto& e : std::array<std::array<std::uint32_t, 2>, 12>{{{0, 1},
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
                                                                     {3, 7}}})
    m.edge_indices.insert(m.edge_indices.end(), {corners + e[0], corners + e[1]});
  return m;
}

mesh_data cylinder_mesh(double radius, double height, std::uint32_t segments = 48) {
  mesh_data m;
  const auto add = [&](vec3 p, vec3 n) {
    m.positions.push_back(narrow(p));
    m.normals.push_back(narrow(n));
    return static_cast<std::uint32_t>(m.positions.size() - 1);
  };
  std::vector<std::uint32_t> sb, st, cb, ct;
  for (std::uint32_t i = 0; i < segments; ++i) {
    const double a = 2.0 * pi * i / segments;
    const vec3 d{std::cos(a), std::sin(a), 0};
    sb.push_back(add(d * radius, d));
    st.push_back(add(d * radius + vec3{0, 0, height}, d));
    cb.push_back(add(d * radius, {0, 0, -1}));
    ct.push_back(add(d * radius + vec3{0, 0, height}, {0, 0, 1}));
  }
  const std::uint32_t bottom = add({0, 0, 0}, {0, 0, -1});
  const std::uint32_t top = add({0, 0, height}, {0, 0, 1});
  for (std::uint32_t i = 0; i < segments; ++i) {
    const std::uint32_t j = (i + 1) % segments;
    m.indices.insert(m.indices.end(), {sb[i], sb[j], st[j], sb[i], st[j], st[i], top,
                                       ct[i], ct[j], bottom, cb[j], cb[i]});
    m.edge_indices.insert(m.edge_indices.end(), {st[i], st[j], sb[i], sb[j]});
  }
  return m;
}

scene_update model() {
  scene_update u{1, {}};
  u.commands.emplace_back(put_mesh{mesh_id{1}, box_mesh({80, 60, 10})});
  u.commands.emplace_back(put_mesh{mesh_id{2}, cylinder_mesh(8, 40)});
  u.commands.emplace_back(put_material{
      material_id{1}, {{0.70F, 0.74F, 0.80F, 1}, {0.1F, 0.1F, 0.12F, 1}, 0.5F}});
  u.commands.emplace_back(put_material{
      material_id{2}, {{0.90F, 0.70F, 0.30F, 1}, {0.25F, 0.16F, 0.04F, 1}, 0.8F}});
  u.commands.emplace_back(put_node{
      node_id{10}, {.mesh = mesh_id{1}, .material = material_id{1}, .name = "plate"}});
  u.commands.emplace_back(put_node{node_id{11},
                                   {.mesh = mesh_id{2},
                                    .material = material_id{2},
                                    .local = {{18, 8, 10}},
                                    .name = "pin"}});
  return u;
}

std::optional<image_rgba8> load_or_report(const std::filesystem::path& path,
                                          std::string& note) {
  auto image = load_image(path);
  if (!image) {
    note += image.error().message() + "\n";
    return std::nullopt;
  }
  return std::move(*image);
}

std::string write_png(const image_rgba8& image, const std::string& path) {
  const auto bytes = encode_png(image);
  std::ofstream out{path, std::ios::binary};
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return out ? path : std::string{"(write failed)"};
}

// The widget tree, shared by the interactive run and the self-test.
struct workspace {
  viewport view{
      {.title = "Avionix wallpaper", .hover = false, .transparent_background = true}};
  backdrop wall;
  column* body{};
  text* status{};
  image_view* logo{};
  viewport_panel* panel{};
  std::optional<image_rgba8> wallpaper_image;
  std::optional<image_rgba8> logo_image;
  std::filesystem::path wallpaper_path;
  bool wallpaper_on{true};
  std::string load_note;

  workspace(const std::filesystem::path& wallpaper,
            const std::filesystem::path& logo_path)
      : wallpaper_path{wallpaper} {
    wallpaper_image = load_or_report(wallpaper, load_note);
    logo_image = load_or_report(logo_path, load_note);
    (void)view.submit(model());
    view.set_standard_view(standard_view::isometric);
    view.select({node_id{11}});

    auto& root = wall.set_child(column{});
    body = &root;
    root.add(constraint::fixed(1), text{" Avionix wallpaper example",
                                        {.foreground = colors::black,
                                         .background = colors::cyan,
                                         .add = attribute::bold}});
    auto& main = root.add(constraint::fill(), row{1});
    auto& left = main.add(constraint::percentage(28), column{});
    auto& logo_block = left.add(constraint::fixed(12), block{"Scorsone Enterprises"});
    logo = &logo_block.set_child(image_view{logo_image.value_or(image_rgba8{})});
    auto& status_block = left.add(constraint::fill(), block{"Status"});
    status = &status_block.set_child(text{});
    status->wrap(true);
    auto& model_block = main.add(constraint::fill(), block{"Model"});
    panel = &model_block.set_child(viewport_panel{view});
    panel->set_backdrop(&wall);
    root.add(constraint::fixed(1),
             text{" b wallpaper  d brightness  i swap images  t model background  drag "
                  "orbit  wheel zoom  q quit",
                  {.foreground = colors::bright_black}});
    if (wallpaper_image) wall.set_image(*wallpaper_image);
  }

  void toggle_wallpaper() {
    wallpaper_on = !wallpaper_on;
    wall.set_image(wallpaper_on && wallpaper_image ? *wallpaper_image : image_rgba8{});
  }
  void cycle_dim() { wall.set_dim(wall.dim() > 0.0 ? 0.0 : 0.35); }
  void swap_images() {
    std::swap(wallpaper_image, logo_image);
    if (wallpaper_on) wall.set_image(wallpaper_image.value_or(image_rgba8{}));
    logo->set_image(logo_image.value_or(image_rgba8{}));
  }

  void refresh(const graphics_support& support) {
    std::string where;
    if (!wallpaper_on || !wallpaper_image) {
      where = "off";
    } else if (support.protocol == graphics_protocol::kitty) {
      where = "behind everything (the terminal layers it under the text)";
    } else {
      where = "behind the 3D model. This terminal cannot draw pixels under text";
      if (std::getenv("WT_SESSION") != nullptr) {
        where += std::format(
            ". For the whole window, set your Windows Terminal profile's "
            "backgroundImage to {}",
            std::filesystem::absolute(wallpaper_path).string());
      }
    }
    status->set(std::format(
        "{}terminal images  {}\nmodel           {} via {}{}\nwallpaper       "
        "{}\nbrightness      {:.0f}%",
        load_note, name(support.protocol), name(panel->shown_as()),
        view.view_renderer(), view.transparent_background() ? ", transparent" : "",
        where, (1.0 - wall.dim()) * 100));
  }
};

int run(const std::filesystem::path& wallpaper, const std::filesystem::path& logo,
        int quit_after_ms) {
  application app{{.mouse_motion = true}};
  workspace ws{wallpaper, logo};
  ws.view.attach(app);
  ws.view.on_event([&](const viewport_event&) { ws.refresh(app.graphics()); });
  app.on_event(
      [&](const event& e) {
        const auto* k = std::get_if<key_event>(&e);
        if (k == nullptr || k->code != key::character || k->mods != modifiers::none) {
          ws.refresh(app.graphics());
          return false;
        }
        switch (k->character) {
          case U'q':
            app.quit();
            break;
          case U'b':
            ws.toggle_wallpaper();
            break;
          case U'd':
            ws.cycle_dim();
            break;
          case U'i':
            ws.swap_images();
            break;
          case U't':
            ws.view.set_transparent_background(!ws.view.transparent_background());
            break;
          default:
            return false;
        }
        ws.refresh(app.graphics());
        return true;
      },
      true);
  app.post([&] {
    ws.view.fit_scene();
    ws.refresh(app.graphics());
  });
  // Refresh once the terminal has answered the image queries.
  auto status_timer = app.set_interval(std::chrono::milliseconds{500},
                                       [&] { ws.refresh(app.graphics()); });
  std::optional<interval_handle> quit_timer;
  if (quit_after_ms > 0)
    quit_timer =
        app.set_interval(std::chrono::milliseconds{quit_after_ms}, [&] { app.quit(); });
  auto result = app.run(ws.wall);
  status_timer.cancel();
  ws.view.detach();
  if (!result) {
    std::println(stderr, "both example: {}", result.error().message());
    return 1;
  }
  return 0;
}

// Renders the whole screen headlessly as each kind of terminal would get it.
int self_test(const std::filesystem::path& wallpaper, const std::filesystem::path& logo,
              const std::string& dir) {
  int failures = 0;
  const auto check = [&](bool ok, std::string_view what) {
    std::println("  {} {}", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
  };
  std::println("both self-test");
  workspace ws{wallpaper, logo};
  check(ws.wallpaper_image && ws.wallpaper_image->width == 1024 &&
            ws.wallpaper_image->height == 1024,
        "JPEG wallpaper decodes (1024x1024)");
  check(ws.logo_image && ws.logo_image->width == 500 && ws.logo_image->height == 500,
        "PNG logo decodes (500x500)");
  if (!ws.wallpaper_image || !ws.logo_image) {
    std::println("  {}", ws.load_note);
    return 1;
  }

  const auto frame_with = [&](graphics_support support) {
    render_buffer screen{{160, 48}};
    frame_state frame;
    frame.graphics = support;
    render_context whole{screen, screen.area(), frame};
    ws.wall.render_in(whole);
    ws.view.fit_scene();
    // Second pass: the panel rendered once with the fitted camera.
    frame = {};
    frame.graphics = support;
    render_context again{screen, screen.area(), frame};
    ws.wall.render_in(again);
    return frame.placements;
  };

  // Kitty: the wallpaper is one placement under everything.
  const auto kitty = frame_with({graphics_protocol::kitty, {10, 20}});
  check(ws.wall.under_text(), "kitty: wallpaper placed under the text");
  bool under = false;
  bool model_transparent = false;
  for (const auto& p : kitty) {
    under = under || (p.area == rect{{0, 0}, {160, 48}} &&
                      p.payload.find("z=-1073741825") != std::string_view::npos);
    model_transparent = model_transparent || (p.transparent && p.area.width() < 160);
  }
  check(under, "kitty: full-screen placement below cell backgrounds");
  check(model_transparent, "kitty: the model picture stays transparent over it");
  check(kitty.size() == 3, "kitty: wallpaper, logo, and model placements");

  // Sixel (Windows Terminal): the model panel composites the wallpaper.
  const auto sixel = frame_with({graphics_protocol::sixel, {10, 20}});
  check(!ws.wall.under_text(), "sixel: nothing under the text");
  const image_rgba8& picture = ws.panel->picture();
  const auto corner = picture.at(2, 2);
  check(corner[3] == 255, "sixel: model picture is opaque after compositing");
  // The corner must be wallpaper, not the gradient or black.
  const auto wall_px = ws.wall.pixels_behind({{0, 0}, {160, 48}}, {1600, 960});
  check(picture.width > 0 && !(corner[0] == 0 && corner[1] == 0 && corner[2] == 0),
        "sixel: wallpaper visible behind the model");
  check(sixel.size() == 2, "sixel: logo and model placements, no wallpaper placement");
  if (!dir.empty()) {
    std::println("       wrote {}",
                 write_png(picture, dir + "/wallpaper-model-sixel.png"));
    std::println("       wrote {}", write_png(wall_px, dir + "/wallpaper-full.png"));
  }

  // Character cells (Zed and similar): same compositing, drawn in cells.
  const auto cells = frame_with({});
  check(cells.empty() && ws.panel->shown_as() == graphics_protocol::none,
        "cells: no placements, drawn in characters");

  std::println("{} failed", failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> paths;
  int quit_after = 0;
  std::optional<std::string> test_dir;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--quit-after" && i + 1 < argc) {
      quit_after = std::atoi(argv[++i]);
    } else if (arg == "--self-test") {
      test_dir = (i + 1 < argc && argv[i + 1][0] != '-') ? std::string{argv[++i]}
                                                         : std::string{};
    } else {
      paths.push_back(arg);
    }
  }
  const std::filesystem::path wallpaper =
      paths.size() > 0 ? paths[0] : "assets/QuantHood.jpg";
  const std::filesystem::path logo = paths.size() > 1 ? paths[1] : "assets/s.png";
  if (test_dir) return self_test(wallpaper, logo, *test_dir);
  return run(wallpaper, logo, quit_after);
}
