# Active camouflage

`camouflage<T>` wraps one widget and stops drawing it. The widget shows up only because it bends what earlier siblings drew beneath it. Inside its shape each cell shows a background cell from up to two cells away. Near the outline the offset follows the outline's normal, so grid lines and text behind it break and curve at the edge. Outline cells take a pale blue tint that drifts over time, and the interior offset wobbles slowly.

`set_reveal(1)` makes the widget solid again. It doesn't fade in. Rows flip between camouflaged and solid at random while they shift sideways, and electric-blue veins run through the shape until the change finishes. With `pointer_reveals`, the field also fails in a ragged disc around the mouse.

Detail behind the widget makes the bend visible, but it isn't required. Over a blank background there is nothing to bend, and the widget shows as its outline alone. Every cell of the shape with three or more of its 9 neighborhood cells outside the shape is tinted, so a one-row word shows as a pale bar and a panel as a frame. This needs truecolor; see the limits below.

```cpp
import avionix;

avionix::layer root;
root.add(avionix::canvas{draw_grid});  // optional; texture makes the bend visible
auto& cloak = root.add(avionix::camouflage{
    avionix::block{"Cloaked", avionix::border_kind::rounded, panel_contents},
    avionix::camouflage_options{.pointer_reveals = true}});

avionix::application app{{.mouse_motion = true}};
auto last = std::chrono::steady_clock::now();
app.set_interval(std::chrono::milliseconds{50}, [&] {
  const auto now = std::chrono::steady_clock::now();
  cloak.advance(now - last);
  last = now;
});
// Later, on the UI thread:
cloak.set_reveal(1.0);
```

`examples/camouflage.cpp` is a complete program. Run it with `zig build run-camouflage`. A panel sits over a grid, and below it the word SIGNAL ACQUIRED sits over a blank band. The word runs a 6 s cycle in the example's interval callback, solid for 3 s and camouflaged for 3 s, so it shows the outline-only look and then comes all the way back. The panel also toggles every 3 s. Space toggles it immediately and restarts its timer, and the cursor still reveals it nearby. g turns the grid off to show the panel without it, and q quits.

The large five-row SIGNAL stays camouflaged throughout. Its canvas draws only the letter strokes, leaving the gaps untouched. Over its blank band, the animated rim traces the letters so the word stays readable without revealing it. Like other outline-only shapes, this needs truecolor.

## Ownership and data flow

```text
lib/avionix.cppm ──exports──► avionix.interface.camouflage
                                      │
                                      ├─► avionix.interface.widget (component, render_context)
                                      ├─► avionix.object.buffer (render_buffer, the scratch copy)
                                      └─► avionix.entity.camouflage (pure field math)
                                                  │
                                                  └─► avionix.entity.geometry, color
```

`camouflage<T>` owns the child through a `std::unique_ptr<component>`. It also owns a scratch `render_buffer`, the shape mask, a copy of the source cells, and the per-cell plan, and reuses all four across frames. The application owns the clock. The wrapper has no timer, and nothing moves until `advance` is called.

One frame:

```text
child.render_in ──► scratch render_buffer ──► mask (which cells the child drew)
                                                    │
back buffer cells already under the wrapper ──┐     ▼
clock, reveal, pointer ───────────────────────┼─► field (entity, pure)
                                              │     │ per cell: solid? offset, rim, accent
                                              ▼     ▼
                             composite into render_context (the back buffer)
```

- The scratch buffer uses back-buffer coordinates. The child's `last_area()`, hit testing, and cursor requests are the same as if it had drawn into the back buffer. Only the wrapper's clip is cleared and read. The buffer covers the rectangle from the screen origin to the clip's bottom-right corner, so a wrapper in the bottom-right corner of a 200 by 60 screen holds 12,000 cells, about 480 KB.
- A cell is in the mask when the child wrote it, blanks included. The scratch clip starts filled with a NUL cell that text drawing never produces.
- Source cells are copied before anything is written, so no offset reads a cell this frame already displaced.
- A solid cell is the child's cell, with its hyperlink re-registered in the back buffer. A displaced cell is a copy of the source cell at `position + offset`, clamped to the clip, with its hyperlink dropped. Cells outside the mask are left alone.
- A width-2 glyph moves only as a whole. If an offset lands on half of one, or would copy only one half, the cell becomes a blank with the source's background.
- Below truecolor the rim and accent tints are skipped. The displacement stays.

Events are not forwarded by the wrapper. `children()` reports the child, so the application delivers keys, mouse events, and focus to it as it does for any container, and events the child ignores bubble up to the wrapper. Forwarding as well would deliver every event twice. The child stays focusable and clickable while camouflaged.

The wrapper records the pointer from mouse events that reach it and forgets it when neither the wrapper nor any descendant is hovered. A child that handles mouse moves itself keeps them from reaching the wrapper, so the disrupted region does not follow the pointer over such a child.

## Public contract

- `camouflage{child, options}` deduces `camouflage<T>` from the child, which is a `component` or a `widget`. `child()` returns `T&`. Write the type out to nest wrappers, because `camouflage{camouflage{x}}` deduces a copy: `camouflage<camouflage<canvas>> outer{camouflage{canvas{...}}};`.
- `camouflage_options` sets `pointer_reveals` (off by default), `pointer_radius` in rows (6; columns count half, so the disc looks round), and the `rim` and `accent` colors. `options()` returns them.
- `set_reveal(target)` clamps to [0, 1] and treats NaN as 0. `advance` moves `reveal()` toward the target at a full swing per 0.75 s. `reveal_target()` returns the target.
- `advance(elapsed)` ignores zero and negative durations. It returns true when the next render will write different cells, assuming the child and the content behind it have not changed. The application redraws after every interval callback either way. The return value is for callers that track dirtiness themselves.
- `animating()` is false only at rest fully solid. There `render` passes the context straight to the child and touches no scratch state.
- Zero-sized and fully clipped areas draw and read nothing. Resizing rebuilds the scratch buffer and mask on the next frame. Wrappers share no static state, so nested wrappers each run their own field.
- Rendering does not throw, except `std::bad_alloc` while the buffers first grow or when the child's hyperlinks are new to the back buffer.
- UI thread only, like every component.

## Cost per frame

Measured with `zig build bench -Doptimize=ReleaseFast -- camouflage` on the Windows 11 machine from the README. The block covers the whole 80 by 24 screen over a grid. Rows include drawing the grid, the effect, the diff, and the ANSI encoding. During this run `present.idle_frame` took 138 µs, against 88.2 µs in the README, so the machine was running slower than when those numbers were taken.

| State                 | Time per frame | Cells changed per frame | Terminal bytes per frame | Allocations |
| --------------------- | -------------: | ----------------------: | -----------------------: | ----------: |
| At rest, camouflaged  |         221 µs |                    41.6 |                      406 |           0 |
| During a reveal       |         427 µs |                   763.7 |                    5,987 |           0 |
| At rest, solid        |         105 µs |                       0 |                        0 |           0 |
| Effect alone, at rest |         111 µs |                       - |                        - |           0 |

At rest the field reads the clock in 0.2 s steps, so with a 50 ms interval three frames in four write the same cells as the frame before. The 41.6 cells are an average. The changes arrive about every fourth frame, around 166 cells, or 9% of the block, at a time. The plan is rebuilt only on those frames. Mid-reveal, rows flip at 24 Hz and about 40% of the block changes each frame. Use reveals sparingly over slow links.

Only cells in the mask, plus one cell beside a width-2 glyph that a write had to split, change between frames.

## Determinism

The same clock, reveal, pointer, and mask give bit-identical cells on Windows, Linux, and macOS. The pixel-shader reference hashes with `fract(sin(x) * 43758.5453)`, which magnifies the last bits of the C library's `sin` and so differs between platforms. The field uses an integer hash, `lowbias32`, and its own polynomial `sin`. The module also turns floating-point contraction off, so no compiler fuses a multiply and add into an FMA on one target and not another.

## Changes from the reference math

| Term                     | Reference                     | Here                                                                           | Why                                                                                                        |
| ------------------------ | ----------------------------- | ------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------- |
| hash                     | `fract(sin(dot) * 43758.5)`   | integer `lowbias32`                                                            | Bit-stable across C libraries                                                                              |
| interior drift           | `(2, -1)`                     | `(1.5, -0.75)`                                                                 | The reference reaches 3 columns. A full row of vertical shift also read as a jump, not a bend              |
| wobble amplitude         | `1` on each axis              | `0.75` on x, `0.6` on y                                                        | Same reason. The offset is also clamped to ±2 on each axis                                                 |
| wobble and shimmer clock | continuous                    | 0.2 s steps                                                                    | Three frames in four write nothing new at rest                                                             |
| rim and accent           | continuous                    | quarters                                                                       | Fewer distinct colors, so fewer cells change from frame to frame                                           |
| rim strength             | `min(length(grad) * 1.4, 1)`  | the larger of that and outside neighbors / 3, capped at 1                      | Along a one-row word the gradient cancels to zero, so a hidden word showed no outline                      |
| shimmer range            | `0.65 + 0.35 * sin`           | `0.8 + 0.2 * sin`                                                              | Over a blank background the rim is all that shows; a floor of 0.3 let it fade out                          |
| tint strength            | both colors by the full value | glyph 0.7 and background 0.4 for rim, glyph 1.0 and background 0.45 for accent | A full-strength tint on both turned outline cells into solid pale blocks that hid the glyphs               |
| tear hash input          | `(p.y * 1.7, tick + 9)`       | `(p.y, tick + 9)` with its own seed                                            | The integer hash takes lattice points; the seed keeps it independent of the dissolve hash                  |
| shimmer                  | `sin(t*2.2 + x*0.1 - y*0.2)`  | the same, expanded as `sin(a + b)`                                             | One `sin` per row and column instead of per cell. Results differ from a direct `sin` in the last bits only |

New constants with no counterpart in the shader: a 0.75 s reveal swing, default foreground and background colors treated as RGB (229, 229, 229) and black when tinted, and a skip of the pointer noise beyond 1.25 radii, where it cannot change the result.

I tuned the constants on text dumps of the example's frames, not in a live terminal. They may want another look in a real terminal with the mouse moving.

## Limits

- Displacement is whole cells. A cell is about 8 by 16 pixels, so the bend is coarse next to a pixel shader and reads best over regular detail such as grids, rules, and text.
- Over blank or uniform backgrounds only the outline shows. The outline is a tint, and tints need truecolor, so below truecolor a widget over a blank background is invisible.
- Default colors have no RGB value. For tinting they count as a dark theme, light gray on black.
- Images the child places with Kitty, Sixel, or iTerm2 graphics are dropped while any part of it is camouflaged. At rest fully solid they pass through.
- The child's cursor shows only over solid cells.
- Pointer disruption depends on mouse events reaching the wrapper. See the note on events above.
