# 3D viewport

`import avionix.viewport;` adds an interactive 3D viewport in a companion graphical window. The terminal UI keeps running in the same process and on the same thread. The viewport renders tessellated geometry that a host submits, handles camera, picking, selection, and manipulation handles, and reports edits as typed intents. It never edits the host's document.

```cpp
import avionix;
import avionix.viewport;

avionix::application app;
avionix::viewport view;
view.attach(app);                       // share the terminal's event loop
view.on_event([&](const avionix::viewport_event& e) { /* intents, captures */ });
(void)view.submit(scene);               // tessellated meshes and component placements
if (auto opened = view.open(); !opened) // the terminal UI works without it
  log(opened.error().message());
app.run(root);
```

`examples/viewport.cpp` is a complete program: `zig build run-viewport` for the interactive workspace, `zig build viewport-check` for the scripted check against a real window. `viewport --self-test <dir>` runs the same check and writes its captures to `<dir>` as PNG files.

## Ownership

```text
Cloak (host)                                   Avionix viewport
────────────                                   ────────────────
exact B-rep, features, history                 none of these
modeling operations, document storage          none of these
revisions and undo                             reads the revision number only
tessellation ──scene_update──────────────────► scene_store (retained copy)
face and edge identity ──mesh_data ids───────► returned unchanged in pick_hit
                                               camera, selection, hover
                                               handles, gestures, snapping
                                               exploded presentation
                                               rendering and captures
transform_intent ◄────────────────────────────── drag, keyboard, or numeric edit
validates, applies, submits newer revision ──► clears the drawn proposal
```

The viewport contains no CAD kernel and no conversation or document storage. Every id it reports is an id the host created.

Module graph, following the project's layering:

```text
lib/viewport.cppm (avionix.viewport)
        │
        ▼
interface.viewport ──► interface.application (shares its loop)
        │
        ├──► object.native_window  GLFW window, context, input translation
        ├──► object.scene_renderer OpenGL 3.3 core passes and captures
        ├──► object.gl             runtime-loaded GL function table
        ├──► object.scene_store    retained scene, revision checks
        └──► entity.viewport, manipulator, picking, camera, scene, spatial, image
```

`object.native_window` is the only module that includes GLFW. No GLFW or OpenGL type appears in the public API.

## Backend choice

The viewport uses GLFW 3.4 for windows, input, and context creation, and OpenGL 3.3 core for rendering.

| Concern                 | Decision                                                                                                                                                                                      |
| ----------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| License                 | GLFW is zlib/libpng. Compatible with Avionix's MIT license; no attribution in binaries required.                                                                                              |
| Distribution            | GLFW is vendored in `libs/glfw` at 3.4 (commit `7b6aead9`), compiled from source into `avionix_viewport`. No runtime DLL ships. OpenGL comes from the system driver.                          |
| Footprint               | About 1.8 MB of C source. GLFW loads `opengl32.dll`, X11, and other system libraries at runtime, so the link needs only `gdi32`, `user32`, and `shell32` on Windows, and frameworks on macOS. |
| Build                   | Zig compiles the C and Objective-C sources with its bundled Clang; CMake compiles them with the selected compiler. No generated files, no package manager.                                    |
| GL loading              | `object.gl` resolves 59 functions through GLFW's proc-address lookup. No system GL headers and no loader library. A missing function names itself in the error.                               |
| Terminal-only consumers | Unaffected. The viewport is a separate module and library; `import avionix;` links neither GLFW nor OpenGL.                                                                                   |

Alternatives considered:

- SDL3 with its GPU API: zlib, more capable, but ten times the source and a heavier build. The viewport needs one window and simple passes.
- sokol_app and sokol_gfx: small, but sokol_app owns the main loop and keeps global state, which conflicts with closing and reopening the window while the terminal loop runs.
- WebGPU (Dawn or wgpu-native): modern, but large prebuilt binaries or a Rust toolchain.
- Vulkan or Metal directly: three backends to write and maintain.

OpenGL 3.3 core runs on every desktop GPU from the last 12 years, on Mesa's software renderer, and on macOS (deprecated there, but still shipped). The renderer is small enough to port to another API later; nothing outside `object.scene_renderer` and `object.gl` touches GL.

## Supported platforms

| Platform                      | Status                                                                                                                                                                                               |
| ----------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Windows 10 1809+ x86_64       | Built and tested: 280 unit tests, `viewport-check` against a real window on an NVIDIA RTX 2070 Super (OpenGL 3.3.0, driver 581.95), and the PTY smoke test with the window open.                     |
| Linux x86_64, X11 or XWayland | Build rules written, not tested. Needs X11 development headers (`libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev`) and a native build. Wayland-native windows are not compiled in. |
| macOS 11+                     | Build rules written, not tested. The UI thread must be the process main thread.                                                                                                                      |

`-Dviewport` defaults to on for Windows targets and for native builds, and off when cross-compiling to Linux or macOS, because those headers come from the target system. `-Dviewport=false` builds the terminal library alone.

When no display or OpenGL 3.3 context is available, `open()` returns `failure::graphics_unavailable` with advice in the context text, for example to set `DISPLAY`, use X forwarding, update the driver, or use `LIBGL_ALWAYS_SOFTWARE=1`. The scene, selection, and camera keep working without a window, and `simulate()` drives interaction without one.

## Coordinates and units

- Right-handed. +Z is up. In the front view +X points right and +Y points away from the viewer.
- Lengths are document units. `viewport_options::unit` records which unit for labels; the viewport never scales geometry.
- Angles in the API are radians, except typed and numeric rotation values, which are degrees.
- Transforms, cameras, and picking use `double`. Vertex positions use `float` in each mesh's local frame. Model-view matrices are composed in `double` before the cast, so large world coordinates do not jitter.
- `mat4` is column-major and multiplies column vectors.
- `transform3d` is translation, rotation (unit quaternion), and scale, applied as `T * R * S`.
- Pixel coordinates are framebuffer pixels with the origin at the top-left corner. On high-DPI displays the window adapter converts window points to framebuffer pixels, so picks, handles, and captures agree.

Document coordinates are independent of the window. A camera holds a target, distance, yaw, pitch, field of view, and orthographic height, all in document units or radians. The same camera renders the same vertical extent at any output size; the aspect ratio only widens or narrows the horizontal extent.

## Scene submission

`scene_update` carries a revision and a list of commands that apply in order:

| Command                                              | Effect                                                              |
| ---------------------------------------------------- | ------------------------------------------------------------------- |
| `put_mesh{id, mesh_data}`                            | Adds or replaces reusable geometry.                                 |
| `remove_mesh{id}`                                    | Fails if a node still uses it.                                      |
| `put_material{id, material}` / `remove_material{id}` | Base color, edge color, shininess. Alpha below 1 draws translucent. |
| `put_node{id, node_desc}`                            | Adds or replaces a component instance, assembly, or body.           |
| `remove_node{id}`                                    | Children must be removed or reparented in the same update.          |
| `set_transform{id, local}`                           | Physical placement in the parent frame.                             |
| `set_visibility{id, visible}`                        | Hides the node and its subtree.                                     |
| `set_presentation{id, transform}`                    | Exploded placement; see below.                                      |
| `clear_scene{}`                                      | Removes everything.                                                 |

`node_desc` holds the parent, mesh, material, local transform, visibility, `pickable`, `selectable`, `permissions`, and a name for diagnostics. Several nodes may reference the same mesh; the GPU holds one copy.

Updates are atomic. The store validates the whole update against id-only copies of the scene before changing anything: mesh index ranges, parent existence, cycles, references to removed meshes or materials, and non-finite or zero-scale transforms. A failed update returns `failure::invalid_argument` and leaves the scene untouched.

### Geometry ownership and lifetime

- `submit()` and `post_update()` take the update by value. Move meshes in to avoid copying. The viewport never reads host memory after the call returns.
- The viewport keeps each `mesh_data` until it is removed or replaced. It needs the CPU copy to rebuild GPU buffers after the window closes and reopens, to pick on the CPU, and to render captures with a hidden context.
- GPU buffers are derived state. `scene_renderer::sync` uploads meshes whose version changed and frees removed ones. Closing the window frees all GPU resources; the retained scene survives.
- `view.scene()` returns a read-only reference valid until the next update or destruction.

### Revisions

- Each update's `revision` must be greater than the current revision; revisions start above 0.
- An update with an equal or older revision fails with `failure::stale_revision` and nothing applies. A worker that tessellated against revision 7 cannot overwrite revision 8.
- Every event that depends on the scene carries the revision it was computed against: `selection_changed`, `hover_changed`, `transform_intent`, and `capture_completed`. A host at a newer revision should treat an older intent as stale. The example rejects such commits and calls `discard_pending()`.

## Threading and event delivery

- The UI thread is the thread that constructs the viewport. Every member except `post_update()` must run on it. `submit()`, `open()`, and `pump()` check and return `failure::wrong_thread` otherwise.
- GLFW requires window creation, destruction, and event processing on one thread, and on macOS that thread must be the process main thread. Run `application::run` there.
- `post_update()` is safe from any thread. Updates apply on the UI thread in posting order and report `update_applied` or `update_rejected`.
- Workers that need anything else post a callback with `application::post`.

Loop sharing:

```text
application::run ──drain()──► queue has entries? ──yes──► dispatch
                                   │ no
                                   ▼
                      viewport native loop: process input, updates,
                      captures, render; then wait in the window system
                      until a window event, a wake, or the deadline
                                   ▲
terminal input thread, post(), notify(), post_update() ──wake──┘
```

`attach(app)` installs the viewport as the application's native loop. The event queue calls `native_loop::wake()` under its mutex for every push and post, and GLFW's empty event ends the wait, so no wake is lost between the empty check and the wait. Terminal input latency with the window open is the same as without it. During an explode animation the wait is capped at 16 ms.

Without an application, call `view.pump(max_wait)` in a loop.

Events are delivered in production order to the handler set with `on_event`, on the UI thread, from the loop or `pump()`, never from inside the call that caused them. For example `propose_transform()` returns before its preview and commit are delivered. A handler may call any viewport member, including `submit`, `close`, and `request_capture`, but must not destroy the viewport. After delivery the attached application redraws, so handlers can update terminal widgets directly.

## Camera

| Input                                 | Action                                                                       |
| ------------------------------------- | ---------------------------------------------------------------------------- |
| Left drag                             | Orbit around the target (turntable, Z stays up)                              |
| Right or middle drag, Shift+left drag | Pan                                                                          |
| Wheel                                 | Zoom toward the point under the pointer                                      |
| `F`                                   | Fit selection (or everything)                                                |
| `H`, Home                             | Fit everything                                                               |
| `P`, `5`                              | Toggle perspective and orthographic, keeping the apparent size at the target |
| `1`, `3`, `7`, `0`                    | Front, right, top, isometric                                                 |
| `W`                                   | Cycle shaded with edges, wireframe, x-ray, shaded                            |

API: `camera_value`, `set_camera`, `set_projection`, `set_standard_view`, `fit_scene`, `fit_selection`. `camera_changed` reports the end of each camera gesture, not every frame.

Clip planes follow the scene bounds every frame, with the perspective near plane kept at least far/10^5 for depth precision.

## Rendering

Passes: background gradient, opaque faces, translucent faces, edges, world overlays, then screen-space handles, markers, and the axis triad.

- Faces use a polygon offset so host edges on the same surface win the depth test without z-fighting.
- `shaded_edges` draws host edges. `wireframe` draws edges after a depth-only pass, so hidden lines are removed. `xray` draws translucent faces and all edges. Meshes without host edges show their tessellation in wireframe modes.
- Without normals, the renderer derives flat normals per triangle.
- The selected node and its subtree are tinted with the highlight color and draw their edges in it, in every mode. Hover tints lightly.
- The window uses 4x MSAA when the driver allows it and vsync.

## Picking

Picking is on the CPU against what is drawn: presented placement (exploded or not), drag previews, visibility, `pickable`, and section planes.

```cpp
std::optional<avionix::pick_hit> hit = view.pick(pixel);
// hit->node      the drawn node
// hit->mesh      its mesh
// hit->triangle  triangle index in that mesh, or no_topology
// hit->face      mesh_data::triangle_faces[triangle], or no_topology
// hit->segment   segment index, or no_topology
// hit->edge      mesh_data::segment_edges[segment], or no_topology
// hit->world_point, hit->distance
```

Mapping contract for face and edge identity:

- The host fills `triangle_faces` with one host face id per triangle and `segment_edges` with one host edge id per edge segment. Any `uint32` except `0xFFFFFFFF` (`no_topology`) is valid; Avionix does not interpret it.
- Ids are meaningful only together with the mesh and the revision in the event. If a mesh changes in a later revision, the host maps old ids through its own history or ignores the pick.
- An edge wins over a face when the pointer is within 6 logical pixels of an edge that is not hidden behind the nearest face. When both belong to the same node, the hit carries both ids.
- Clicking selects the nearest ancestor-or-self with `selectable = true`. Shift or Ctrl toggles membership. Clicking empty space clears the selection. `select()` sets the selection from the host and emits no event, so mirroring it into a terminal tree does not loop.

Picking is linear in the triangles of meshes whose bounds the ray crosses. Very large meshes will need an acceleration structure (see the gaps below).

## Manipulation

A single selected node shows handles for the current kind: `T` translate (axis arrows, plane squares, view-plane square), `R` rotate (axis rings, view ring), `S` scale (axis handles and a uniform center). `set_transform_space` chooses parent (default), world, or local axes; scale always uses local axes.

A gesture is one of:

- Pointer: press a handle, drag, release.
- Keyboard: press `X`, `Y`, or `Z` (Shift for the plane without that axis). The pointer moves the node along the constraint and typed digits set an exact value. Enter or left click commits; Escape or right click cancels. During any gesture, `X`, `Y`, `Z` switch the constraint and digits, `.`, `-`, and Backspace edit the value.
- Host: `propose_transform(kind, constraint, value)` from a terminal command or an AI tool call.

Event sequence for one gesture, all with the same `gesture` number and `scene_revision`:

```text
transform_intent{preview} × 0..n   proposed local transform, drawn, not applied
transform_intent{commit}  × 1      or
transform_intent{cancel}  × 1
```

A gesture that ends without a change emits cancel, not commit. `numeric_entry_changed` reports the typed text for display in the terminal. Ctrl during a drag inverts `snap_settings::enabled` (default steps: 1 unit, 15 degrees, 0.1).

`proposed` is the node's new local transform in its parent frame. World deltas are mapped through the presented parent frame, so dragging an exploded part moves its physical placement by the amount the user sees.

After a commit the viewport keeps drawing the proposal until the next accepted update (from any source) or `discard_pending()`, so the part does not jump back while the host works. An accepted update that changes the dragged node or any ancestor cancels an active gesture.

Host restrictions:

- `node_desc::permissions` removes axes before any math runs. Translation and rotation axes are the parent frame's; scale axes are the node's. Denied handles are not drawn and denied components are masked from drags. `transform_permissions::none` grounds a part.
- `set_transform_filter(fn)` sees every preview proposal. It returns an adjusted transform (clamp, snap to a feature, zero a component) or `nullopt` to keep the previous proposal.

## Exploded views

Each node may have a presentation transform, set with `set_presentation`, in addition to its physical `local` transform:

```text
physical(node)  = physical(parent)  * local(node)
presented(node) = presented(parent) * lerp(identity, presentation(node), f) * local(node)
```

`f` is the explode factor. `set_exploded(true)` animates it from 0 to 1 over `explode_duration`. Physical transforms, `scene().physical_world()`, and intents never include the presentation. Picking and handles operate on the presented picture. Toggling cancels an active gesture. The host chooses presentation offsets; Avionix does not compute an explode layout.

## Captures

```cpp
std::uint64_t id = view.request_capture({
    .view = camera,             // explicit camera, independent of the window's
    .extent = {1024, 768},      // explicit output size, 1..8192 per side
    .revision = doc.revision,   // render only if the scene is still at this revision
    .mode = std::nullopt,       // default: the viewport's display mode
    .exploded = false,
    .show_selection = false,
});
// later, on the UI thread:
// capture_completed{request = id, status, scene_revision, view, image, detail}
```

- Completion arrives as an event from a later loop iteration, never inside `request_capture`.
- `complete`: `image` holds top-down RGBA8 rows at exactly `extent`, and `scene_revision` is the revision the pixels show. `encode_png(image)` returns PNG bytes.
- `stale`: `revision` was given and the scene had moved on before the capture ran. No pixels are produced, so an obsolete picture cannot be mistaken for the current one.
- `failed`: no graphics context or a GPU error; `detail` says which.
- Captures render the committed host scene: no drag previews, pending proposals, handles, hover, or axis triad. Overlays and section planes are included.
- Captures render offscreen with 4x MSAA. With the window closed, the viewport creates a hidden OpenGL context for the duration of the capture.

## The viewport inside the terminal UI

`viewport_panel` shows the viewport as a section of the TUI. It is an ordinary component, so it goes in a `row`, `column`, or `block` like any widget. The companion window is optional.

```cpp
avionix::application app{{.mouse_motion = true}};  // motion enables hover
avionix::viewport view;
view.attach(app);
avionix::row body{1};
body.add(avionix::constraint::percentage(25), tree);
body.add(avionix::constraint::fill(), avionix::viewport_panel{view});
```

`examples/viewport.cpp` uses it by default; `viewport --window` also opens the companion window.

### How the picture reaches the terminal

```text
viewport::render_view(panel pixels)
   │  OpenGL: the window's context, or a hidden one kept for the viewport's life
   │  CPU:    software_renderer when no OpenGL 3.3 context exists (SSH, headless)
   ▼
image_view ──► Kitty graphics   Kitty, Ghostty, WezTerm
           ──► Sixel            Windows Terminal 1.22+, foot, xterm, mlterm, Konsole, VTE, iTerm2
           ──► iTerm2 images    iTerm2
           ──► quadrant cells   every other terminal: 2x2 samples, best two colors per cell
           ──► brightness ramp  monochrome terminals (NO_COLOR, dumb)
```

| Platform | Typical terminals                                                           | What the panel uses                                                                      |
| -------- | --------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------- |
| Windows  | Windows Terminal 1.22+                                                      | Sixel (verified on 1.24: DA1 lists 4, cell size 10x20 px through ConPTY)                 |
| Windows  | classic console host, editor terminals (Zed, VS Code without image support) | quadrant cells                                                                           |
| macOS    | iTerm2 / Kitty, Ghostty, WezTerm                                            | iTerm2 images / Kitty graphics                                                           |
| macOS    | Terminal.app                                                                | quadrant cells                                                                           |
| Linux    | Kitty, Ghostty, WezTerm                                                     | Kitty graphics                                                                           |
| Linux    | foot, xterm, Konsole, VTE terminals with Sixel                              | Sixel                                                                                    |
| any      | tmux, screen                                                                | quadrant cells (passthrough not implemented)                                             |
| any      | SSH session                                                                 | same as the local terminal; rendering falls back to the CPU when the host has no display |

Detection, in `object.terminal_capabilities` and `task.run`:

1. `AVIONIX_GRAPHICS=kitty|sixel|iterm2|blocks` or `application_options::graphics` decides outright.
2. The environment identifies Kitty, Ghostty, WezTerm, and iTerm2, and rules out multiplexers and the Linux console.
3. Otherwise the event loop sends primary device attributes (`CSI c`). Feature 4 in the reply selects Sixel; any other reply keeps character cells.
4. The cell size comes from `TIOCGWINSZ` on POSIX, or from `CSI 16 t` (needed on Windows, where the console API has no pixel size). It is queried again after each resize.

The input decoder recognizes both replies wherever they arrive in the stream and the loop consumes them; applications never see them (`terminal_reply`). Until a reply arrives the panel shows character cells, then switches.

### Drawing rules

- The renderer writes images after the cell runs, inside the same synchronized update. It re-sends an image only when its pixels or area change, after a full repaint, or (Sixel and iTerm2, which live in the cells) when a cell run overwrote part of it. Vanished or moved Sixel and iTerm2 images have their old cells repainted; Kitty images are deleted by id, and all Kitty images are deleted when the session ends.
- The panel re-renders only when the viewport's `changes()` counter moves (camera, selection, hover, scene, handles, animation) or its size changes. Encoded payloads are cached.
- Without a pixel protocol, each cell is fitted with one of the 16 quadrant characters (U+2580-259F, including the half blocks) and two 24-bit colors, from 4x8 rendered samples. This needs a font with the Block Elements range, which common terminal fonts have. It is a preview: an 80x30 panel resolves about 160x60 samples. For the full picture use a terminal with Sixel or Kitty graphics, or the companion window.
- Sixel uses a palette of up to 256 colors fitted to each image (median cut over a 15-bit histogram) with 3/4-strength Floyd-Steinberg diffusion. On the example scene the decoded Sixel is within 1.3/255 of the render on average. Kitty and iTerm2 are full color.
- Windows Terminal reports a 10x20 pixel cell for Sixel and scales images to the actual font. With display scaling above 100% or a large font, each image pixel covers more than one screen pixel; Sixel cannot send more detail per cell than the terminal reports.
- Sixel and iTerm2 move the cursor below an image, so on the screen's last row the image stops one row short to avoid scrolling. Kitty images do not move the cursor.
- Widgets drawn over a pixel panel (popups) are not supported: terminals cannot crop an image to cells, and Kitty images sit above text.

### Transparent backgrounds

`viewport_options::transparent_background` or `set_transparent_background(true)` drops the gradient in terminal panels and captures, so the terminal's own background shows around the model: its color, or a background image configured in the terminal (Windows Terminal `backgroundImage`, iTerm2, Kitty `background_image`, WezTerm, Ghostty, Konsole). The companion window always draws the gradient. The example toggles it with `b`.

- Both renderers produce real coverage: OpenGL accumulates destination alpha and un-premultiplies the read-back pixels; the CPU renderer composites with straight alpha and resolves supersamples by coverage. Antialiased silhouettes keep the model's color instead of fading to black.
- Kitty and iTerm2 show partial transparency. Sixel has no partial alpha: pixels below 50% coverage are left to the terminal and the rest are drawn opaque, so silhouettes are not antialiased against the terminal background.
- Sixel and iTerm2 leave transparent pixels as they were. When a transparent image changes, the renderer first repaints its cells to clear the previous picture, then draws the new one in the same synchronized update.
- Character-cell fallback: fully transparent cells keep the terminal background; partly covered cells draw only their covered quadrants.

### Wallpapers

`backdrop` (in `import avionix;`) puts an image behind a child widget, scaled to cover the area and dimmed for readability (`set_dim`, default 35%). `load_image(path)` and `decode_image(bytes)` read PNG and JPEG through the vendored stb_image (PNG and JPEG only, 16384 px per side, 256 MiB decoded at most).

| Terminal                              | Where the wallpaper shows                                                                                                                                                                |
| ------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Kitty, Ghostty, WezTerm               | behind everything: a Kitty placement below text and below colored cell backgrounds, exactly like a terminal background image                                                             |
| Windows Terminal, iTerm2, Zed, others | behind the 3D model: these terminals cannot draw pixels under text, so `viewport_panel::set_backdrop()` composites the matching part of the wallpaper into the transparent model picture |

For a whole-window wallpaper in Windows Terminal, set the profile's `backgroundImage` to the same file and keep the model's background transparent; the example's status panel prints the absolute path. `examples/both.cpp` (`zig build run-viewport_background`) uses `assets/QuantHood.jpg` as the wallpaper and `assets/s.png` as a logo, and `both --self-test <dir>` checks the Kitty layering and the composited Sixel picture without a terminal.

### Input

- Mouse cells map to the center of the corresponding surface pixel block. Left drag orbits, right or middle drag pans, the wheel zooms, a click selects, and handles drag. Precision is one cell; terminals' pixel-precise mouse mode (1016) is not used yet.
- The panel captures the pointer on press, so drags keep working outside it and do not start a text selection.
- Keys reach the viewport when the panel is focused and the viewport uses them (`viewport::wants_key`): camera keys, `T`/`R`/`S`, `X`/`Y`/`Z` with digits, Enter and Escape during a gesture. Other keys go to the application.

### Surface

- Window closed: the panel is the drawing surface. It calls `viewport::set_surface(pixels, scale)`, so picks, handles, and fits use the panel's resolution.
- Window open: the panel mirrors the window's view at the window's aspect ratio, letterboxed, and maps its input into window pixels. Both stay interactive.
- Explode animations tick through an application interval when no window is open.

## Window lifecycle

- `open()` creates the window and context, loads GL, compiles shaders, and uploads the retained scene. Errors are `graphics_unavailable` (no display, no OpenGL 3.3 core) or `graphics_failed` (shader or framebuffer failure), each with an actionable context string. Opening twice does nothing.
- `close()` cancels an active gesture, detaches from the loop, frees GPU resources, destroys the window, and emits `viewport_closed{host}`. The scene, camera, selection, and settings remain for the next `open()`.
- The window's close button emits `viewport_closed{user}` and closes the same way. A GL error during drawing emits `graphics_error` and closes with `close_reason::graphics_error`.
- The first open window initializes GLFW and the last one to close terminates it, so a closed viewport leaves no window-system state or helper threads behind.
- The destructor closes without emitting events and detaches from the application.
- Resize and DPI changes update the framebuffer size and content scale. Handle sizes, hit tolerances, and line widths scale with the content scale. The camera does not change.
- Focus: `focus_on_open` controls whether opening steals focus. `viewport_focus_changed` reports focus. Losing focus cancels a pointer gesture. Keys go to whichever window has focus; keys the viewport does not use arrive as `viewport_key` so the host can bind shortcuts in both windows.

## Extension points

| Capability                 | What exists                                                                                                                                                  | Host builds on it                                                            |
| -------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------- |
| Section planes             | `set_section_planes`, up to 4, clipped in the shader and in picking. Interior faces show darkened.                                                           | Cap faces, plane handles, and named sections.                                |
| Dimensions and annotations | `set_overlay(layer, overlay_set)` with world or node-attached lines and screen-size markers, depth-tested or on top. `project(world)` gives label positions. | Measurement logic, leader layout, label text in the terminal or the window.  |
| Assembly tree              | `scene()` exposes `roots()`, `nodes()`, `node(id)->children`, names, and versions. `select()` and `selection_changed` sync both ways.                        | Tree widget in the terminal (the example shows a flat list).                 |
| Property editing           | Typed intents and `propose_transform`; the host submits property changes as updates.                                                                         | Property panels and validation.                                              |
| Saved views                | `view()` returns `view_state` (camera, display mode, exploded flag, sections); `restore()` applies it.                                                       | Naming, persistence, thumbnails via `request_capture` with the saved camera. |

## Not implemented

- Terminal panel: pixel-precise mouse (mode 1016), reduced resolution while dragging over slow links, rendering above the reported cell size for Kitty and iTerm2 on high-DPI screens, tmux/screen passthrough, and partially covered pixel panels.
- Building `avionix_viewport` on Linux needs X11 headers even when the program will only use the terminal panel and the CPU renderer at runtime.

- Text rendering in the window. Overlays draw lines and markers; dimension values and annotation text are not drawn in 3D.
- Section caps. Cut solids show their interior instead of a filled cap.
- Box or lasso selection, and selecting individual faces or edges as the selection itself. Picks report faces and edges; selection is node-level.
- Pivot editing. Rotation and scale pivot on the node's local origin.
- An acceleration structure for picking, level of detail, and instancing in one draw call. Each node is one draw.
- Camera animation for fit and standard views (they jump).
- Wayland-native windows (XWayland works) and any untested platform behavior on Linux and macOS.
- Touch, pen, and 3D mouse input.
- Windows modal move/resize: while the user holds the window's border or title bar, Windows runs a modal loop on the UI thread. The viewport repaints during the drag, but terminal input is queued until the drag ends.
- Shadows, PBR materials, textures, and anti-aliased line widths above one pixel for host edges.

## Verification

| Command                    | Covers                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| -------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `zig build test`           | 303 tests, including viewport and terminal-image tests: math, camera framing at several aspect ratios, zoom-to-cursor, revision rejection and atomic updates, picking ids and section planes, handle math and permissions, gesture sequencing, posted updates from a thread, stale captures, saved views, the queue's native-loop wake, one offscreen render (skipped with a message when no OpenGL 3.3 is available), the CPU renderer against OpenGL, section planes on the CPU, protocol detection rules, Sixel/Kitty/iTerm2 encoding, renderer image placement and erasure, `image_view` in every mode, and the panel's Sixel, half-block, click, drag, wheel, and key paths. |
| `zig build viewport-check` | The example's `--self-test` against a real window: orbit, pick with face ids, handle drag with one commit, typed values, filter and permissions, stale worker update, explode without physical change, picks in the exploded view, captures (exploded, assembled, stale, window closed, after reopen), close and reopen with the scene retained, then the terminal panel: mirroring the window, becoming the surface, OpenGL and CPU pictures, half-block cell colors. `viewport --self-test <dir>` writes `panel-opengl.png`, `panel-cpu.png`, and `panel-blocks.png`.                                                                                                           |
| `zig build smoke`          | Runs the viewport example under ConPTY with its window open; the terminal must render, accept `e` and `q`, and exit cleanly.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |

### Drag performance

The terminal panel recycles its RGBA pixel buffer, reuses OpenGL capture targets until the size or sample count changes, and caches the sampled wallpaper. Interactive frame vectors retain capacity. Sixel uses the fast fixed 64-color palette without dithering while the pointer is held. Release restores the adaptive palette with dithering. Encoder buffers retain capacity, and the existing 16 ms frame interval targets approximately 60 FPS; terminal throughput can lower the actual rate. Surface resolution stays constant so picking and handle coordinates remain stable.

`zig build bench -Doptimize=ReleaseFast -- --quick sixel` compares fresh adaptive, cached adaptive with dithering, and fixed 64-color encoding time, allocations, and terminal bytes for a 300x240 shaded image. This measures encoding; terminal transmission and display costs still depend on the terminal.
