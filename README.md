# Avionix

Avionix is a terminal user interface library written in C++26 with C++ modules. It owns terminal setup and restoration, input decoding, layout, a double-buffered diff renderer, widgets, and the event loop. It has no runtime dependencies beyond the C++ standard library and the platform APIs.

```cpp
import avionix;

int main() {
    avionix::application app;
    avionix::block frame{"Avionix", avionix::border_kind::rounded};
    frame.set_child(avionix::text{"Hello. Ctrl+C quits."});
    return app.run(frame) ? 0 : 1;
}
```

## Requirements

- Zig 0.17.0-dev.813 or a compatible build (`minimum_zig_version` in `build.zig.zon`). Zig's bundled Clang 22 and libc++ compile everything.
- Windows 10 1809 or later (virtual terminal processing and ConPTY), Linux, or macOS.

Nothing else is installed or vendored. `libs/` exists for future vendored dependencies and is empty.

## Build commands

| Command | What it does |
| --- | --- |
| `zig build` | Builds `libavionix` and installs it with the module BMIs into `zig-out/` |
| `zig build test` | Runs the unit tests (193 tests, no terminal needed) |
| `zig build smoke` | Runs each example under a pseudo terminal, resizes it, sends its quit key, and checks for a clean exit |
| `zig build bench -Doptimize=ReleaseFast` | Runs the hot-path benchmarks |
| `zig build examples` | Builds `hello`, `counter`, and `dashboard` into `zig-out/bin` |
| `zig build run-dashboard` | Runs one example in the current terminal |

Useful options: `-Dtarget=x86_64-linux-gnu` or `-Dtarget=aarch64-macos` to cross-compile, `-Doptimize=Debug|ReleaseSafe|ReleaseFast|ReleaseSmall`, and `-Dwerror=false` to keep warnings from failing the build. Arguments after `--` go to the program, for example `zig build test -- unicode` runs the tests whose name contains `unicode`.

### CMake

CMake 3.28 or newer can build the same C++ module sources with Clang, GCC, or MSVC versions that support C++26 modules:

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Run an example from `build` after compiling it. With a single-configuration Ninja build, the executables are `build/hello`, `build/counter`, and `build/dashboard` (`.exe` on Windows).

Examples and tests are enabled by default. Benchmarks are opt-in:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DAVIONIX_BUILD_BENCHMARKS=ON
cmake --build build-release --target avionix-benchmarks
```

Use `-DAVIONIX_BUILD_EXAMPLES=OFF`, `-DBUILD_TESTING=OFF`, or `-DAVIONIX_WARNINGS_AS_ERRORS=OFF` when needed. Zig remains the primary build and handles cross-compilation and installable Clang BMIs.

The installed tree:

```text
zig-out/
├── lib/libavionix.a        (avionix.lib on Windows)
└── modules/
    ├── avionix.pcm         the public module
    └── avionix.*.pcm       internal modules it depends on
```

## Using Avionix from another project

Consumers write `import avionix;` and nothing else from Avionix. The internal module graph is an implementation detail.

Clang BMIs (`.pcm` files) only load in the same compiler version with the same language and codegen options. Consumer sources that import Avionix must therefore be compiled by the same `zig c++`, with the same target and optimization mode used to build Avionix.

### From a Zig build

Add Avionix to `build.zig.zon`, then use the `consumerObject` helper that Avionix's `build.zig` exports. It compiles a source with the matching flags and the BMI directory on the module search path.

```zig
const std = @import("std");
const avionix = @import("avionix");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const dep = b.dependency("avionix", .{ .target = target, .optimize = optimize });

    const exe = b.addExecutable(.{
        .name = "app",
        .root_module = b.createModule(.{ .target = target, .optimize = optimize, .link_libcpp = true }),
    });
    exe.root_module.addObjectFile(avionix.consumerObject(b, dep, b.path("src/main.cpp"), target, optimize, &.{}));
    exe.root_module.linkLibrary(dep.artifact("avionix"));
    b.installArtifact(exe);
}
```

Call `consumerObject` once per source file. The last argument takes extra flags such as warnings or include paths.

### From the command line

After `zig build` (Debug, native target):

```sh
zig c++ -std=c++2c -fno-sized-deallocation -O0 -g -fsanitize-trap=undefined \
    -fprebuilt-module-path=zig-out/modules -x c++ -c main.cpp -o main.o
zig c++ main.o zig-out/lib/libavionix.a -o app    # zig-out/lib/avionix.lib on Windows
```

For other modes, use the flags from `cxxFlags` in `build.zig`: `-O2 -g -fsanitize=undefined -fsanitize-trap=undefined` for ReleaseSafe, `-O3` for ReleaseFast, and `-Os` for ReleaseSmall.

## Architecture

The code is layered, and dependencies only point down:

```text
lib/avionix.cppm            public facade: `import avionix;`
        │
        ▼
core/interface              application, widget, layout, controls
        │
        ├─► core/task       run, dispatch, render, resize
        │        │
        ├────────┼─► core/object    terminal, input_decoder, input_reader,
        │        │                  event_queue, buffer, renderer, ansi_encoder,
        │        │                  terminal_capabilities
        │        │         │
        └────────┴─────────┴─► core/entity    geometry, color, style, unicode,
                                              cell, event, constraint, error
```

- Entities are values and pure rules: no I/O, mostly `constexpr`.
- Objects own resources: the terminal session, buffers, the input thread, and the queue.
- Tasks coordinate objects for one operation and own nothing long lived.
- Interfaces are what applications program against.

`build.zig` declares the module graph once. A `comptime` check rejects any import that points up a layer, any module whose file path does not match its name (`avionix.object.renderer` must be `core/object/renderer.cppm`), and any import of a module declared later in the table. Each module is compiled twice from the same `.cppm`: once to a BMI for its importers and once to an object for the library.

### Data flow

Input:

```text
terminal ──bytes──► input_reader thread ──► input_decoder ──events──► event_queue
                                                                        │
worker threads ──application::post / notify─────────────────────────────┤
                                                                        ▼
                                                   UI thread: dispatch ──► widgets
```

Output:

```text
widget tree ──layout──► render_context ──► back buffer
back buffer vs front buffer ──changed runs──► ansi_encoder ──one write──► terminal
back buffer becomes the front buffer
```

The renderer keeps both buffers as one contiguous `std::vector<cell>` each (index `y * width + x`). Unchanged rows are skipped with one word-wise comparison. Changed cells become runs, and runs on the same row that are within 4 cells of each other merge, because rewriting a short gap costs fewer bytes than a cursor move. The encoder tracks the terminal's cursor and SGR state, picks the shortest cursor movement, and emits the smaller of an incremental SGR change and a reset plus full style. Each frame is wrapped in DEC mode 2026 (synchronized output) and written with a single call.

## Public API contracts

The stable API is exactly what `import avionix;` exports:

- Entities: `position`, `size`, `rect`, `color`, `colors::*`, `color_depth`, `style`, `style_patch`, `attribute`, the event types and `event`, `constraint`, `direction`, `error`, and `failure`.
- Interfaces: `application`, `component`, the `widget` concept, `render_context`, `event_context`, `row`, `column`, `stack`, `padding`, `text`, `block`, `list_view`, `text_input`, `progress_bar`, `spacer`, and `canvas`.
- Selected helpers: `display_width`, `graphemes`, `grapheme_width`, `prefix_fitting_width`, `render_buffer`, and `cell`.

Internal modules (`avionix.object.*`, `avionix.task.*`) can be imported, and the tests do, but they may change without notice.

### Ownership and lifetime

- `application::run(root)` borrows `root` for the duration of the call.
- Containers own children added with `add()`. `add()` returns a reference to the stored widget, which stays valid while the container lives. `add_borrowed()` stores a pointer, so the caller keeps ownership and must keep the widget alive.
- Do not move a component after adding it to a tree. The application and containers hold pointers to it.
- `render_context` is a view into the back buffer that is valid only during `render()`. Do not store it.
- Views returned by `wrap_text`, `graphemes`, and `render_buffer::row` refer into their source and must not outlive it. `render_buffer::resize` invalidates row spans.

### Thread safety

- One thread, the UI thread, constructs the application, calls `run`, and touches widgets.
- `application::post(callback)` and `application::notify(user_event)` are safe from any thread. Posted callbacks run on the UI thread between frames, and the screen redraws afterwards.
- Workers must not touch widgets directly. They post a callback that does the change on the UI thread, as `examples/counter.cpp` and `examples/dashboard.cpp` do.

### Errors

Expected failures return `std::expected<T, avionix::error>`. An `error` carries the operation that failed, a `failure` kind, context text, and the OS error code (`errno` or `GetLastError()`); `message()` formats all four. `run()` returns an error for:

- `not_a_terminal`: stdin or stdout is redirected.
- `unsupported_capability`: for example, the Windows console refused VT mode.
- `already_running`: another session is open.
- `platform_call_failed` or `io_failed`: an OS call failed, or the terminal hung up.

The terminal is restored before `run()` returns, on success and on error.

Malformed input sequences are not errors for the application. The decoder counts and drops them, and decoding continues with the next byte.

## Terminal lifecycle

`terminal_session` saves the terminal modes, then enables raw input (termios on POSIX, console modes plus UTF-8 code pages on Windows). It enters the alternate screen, hides the cursor, and enables bracketed paste, SGR mouse reporting, and focus events. Destruction undoes each step in reverse order. A test checks that every mode the enter sequence sets is reset by the leave sequence.

If the process is killed instead of exiting, POSIX handlers for SIGTERM, SIGHUP, SIGINT, and SIGQUIT restore the terminal with async-signal-safe calls and re-raise the signal. On Windows, a console control handler does the same for close, logoff, and shutdown events. SIGKILL cannot be handled on any platform.

Capabilities come from the environment (`TERM`, `COLORTERM`, `TERM_PROGRAM`, `WT_SESSION`, and `NO_COLOR`). Avionix does not send terminal queries, because the replies would arrive on the input stream and race the decoder. Colors are reduced to what the terminal supports: truecolor, 256 colors, 16 colors, or none.

## Unicode

All text measurement goes through `avionix.entity.unicode`: UTF-8 decoding, then grapheme clusters, then display width in cells. Wide characters occupy two cells: the second holds a continuation marker that the renderer never emits. Control characters never reach the terminal as cell content.

Known limits:

- Grapheme segmentation implements UAX #29 rules GB3 to GB13, except GB9a (SpacingMark) and GB9b (Prepend). Some Indic conjuncts therefore split into more clusters than Unicode specifies.
- The property tables are a compact subset of Unicode 15.1 covering the common scripts and emoji. Rare scripts may measure one cell off.
- Terminals disagree on emoji and ambiguous-width characters. Avionix treats VS16 sequences as two cells wide and ambiguous-width characters as one.

## Benchmarks

Measured with `zig build bench -Doptimize=ReleaseFast` on Windows 11, x86-64, with a 200x60 screen (12,000 cells):

| Benchmark | Time per op | Terminal bytes | Allocations |
| --- | --- | --- | --- |
| Diff of two identical frames | 26 µs | | 0 |
| Present an animated frame (3 cells move) | 88 µs | 38 | 0 |
| Present an unchanged frame | 90 µs | 0 | 0 |
| Present a full repaint | 244 µs | 5,012 | 0 |
| Decode mixed input (keys, mouse, paste) | 72 MB/s | | 0 |
| Display width, ASCII | 620 MB/s | | 0 |
| Display width, mixed scripts and emoji | 86 MB/s | | 0 |
| Event queue push, drain, and dispatch | 12.7 M events/s | | 0 |

The present timings include clearing the back buffer and drawing 60 rows of text into it. The benchmark binary replaces `operator new` to count allocations. Steady-state frames allocate nothing.

## Project layout

```text
core/entity/     pure types and rules
core/object/     resource owners and platform code
core/task/       coordination
core/interface/  application-facing API
lib/             the `avionix` facade
tests/           unit tests, the test harness module, and the PTY smoke harness
benchmarks/      hot-path benchmarks
examples/        programs that use only `import avionix;`
libs/            vendored dependencies (none)
```

## Editor support

`.clangd` is not wired to the build. The BMIs Zig produces come from Zig's Clang 22, and a clangd from another LLVM version cannot load them. Editor support needs a clangd that matches Zig's Clang, or a `compile_commands.json` that lets clangd build its own BMIs.
