# Avionix

Avionix is a high performance, cross platform terminal user interface library written in C++26.

It owns terminal control, input decoding, layout, rendering, widgets, event delivery, and application infrastructure.

Avionix is an independent library. It must not contain assumptions about any application that consumes it.

Primary goals:

1. Correctness.
2. Low latency.
3. Predictable ownership.
4. Minimal allocations and copies.
5. Minimal terminal output.
6. Modern C++26 APIs.
7. Cross platform behavior.
8. A small public API.

## Communication style

Apply [`skills/SKILL.md`](skills/SKILL.md) to documentation, code comments, CLI help, error messages, and contributor communication. Keep exact technical terms. Cut stock phrasing, decorative emphasis, and vague claims.

Default to not edit files unless specifically asked to.

Favor architecture over syntax.

When discussing a feature, explain:

1. What owns it.
2. What depends on it.
3. What it depends on.
4. How data flows through it.
5. Why it exists.

Express relationships in graph form whenever useful.

```text
interface ──depends──► task ──depends──► object ──depends──► entity
    │                    │                   │
    ├──depends──────────► object             └──depends──► libs
    └──depends──────────► entity
                         │
                         └──depends─────────► entity
```

Never reverse these arrows.

- Interface parses user input and presents results. It never touches OS terminal internals, resource ownership, or library bindings.
- Task coordinates application operations. It never owns terminal resources or implements platform-specific behavior.
- Object owns stateful resources, operating-system interactions, terminal sessions, event queues, renderers, and adapters for external libraries.
- Entity owns pure types, geometry, styles, events, constraints, and rules. It performs no I/O.

## Project structure

Use this top level structure:

```text
Avionix/
├── AGENTS.md
├── README.md
├── LICENSE
├── build.zig
├── build.zig.zon
│
├── core/
│   ├── entity/
│   ├── object/
│   ├── task/
│   └── interface/
│
├── lib/
│   └── avionix.cppm
│
├── libs/
├── tests/
├── benchmarks/
├── examples/
└── skills/
    └── SKILL.md
```

`core/` contains Avionix's implementation architecture.

`lib/` contains the public library façade consumed by external projects.

`libs/` contains vendored third-party dependencies if any become necessary.

## Dependency direction

The architectural dependency graph is:

```text
lib
 │
 ▼
interface ──depends──► task ──depends──► object ──depends──► entity
    │                    │                   │
    ├──depends──────────► object             └──depends──► libs
    └──depends──────────► entity
                         │
                         └──depends─────────► entity
```

Dependencies flow downward only.

Never introduce:

```text
entity ──► object
entity ──► task
entity ──► interface
entity ──► lib

object ──► task
object ──► interface
object ──► lib

task ──► interface
task ──► lib

interface ──► lib
```

`lib/` is the final public composition layer.

Nothing in `core/` depends on `lib/`.

## Public library module

Avionix must expose a simple primary module:

```cpp
import avionix;
```

A consuming project should not need to understand Avionix's internal module graph.

The primary module lives at:

```text
lib/avionix.cppm
```

Conceptually:

```cpp
export module avionix;

export import avionix.interface.application;
export import avionix.interface.widget;
export import avionix.interface.layout;

export import avionix.entity.color;
export import avionix.entity.style;
export import avionix.entity.geometry;
export import avionix.entity.event;
```

Only export modules that form part of the supported public API.

Do not blindly re-export the entire implementation.

Consumers should normally write:

```cpp
import avionix;
import std;

int main() {
    avionix::application app;
    // ...
}
```

rather than:

```cpp
import avionix.object.renderer;
import avionix.object.terminal;
import avionix.task.render;
```

Internal modules may remain importable during development, but they are not part of the stable public contract unless explicitly documented.

The desired boundary is:

```text
consumer
   │
   ▼
import avionix
   │
   ▼
public Avionix API
   │
   ▼
core/interface
   │
   ▼
core implementation
```

## C++26

Avionix targets C++26.

Prefer C++26 and modern standard-library facilities over legacy C++ patterns when compiler support is adequate.

Do not intentionally write C++17-style architecture inside a C++26 codebase without a compatibility reason.

Prefer:

- modules
- concepts
- constraints
- ranges
- views
- `std::span`
- `std::string_view`
- `std::expected`
- `std::optional`
- `std::variant`
- `std::visit`
- `std::print`
- `std::format`
- `std::source_location`
- `std::chrono`
- `std::jthread`
- `std::stop_token`
- `std::atomic`
- `std::bitset` or scoped bit flags where appropriate
- `constexpr`
- `consteval`
- `constinit`
- deducing `this` where it simplifies APIs
- explicit object parameters where useful
- ranges algorithms where they improve expression of intent
- standard-library facilities over equivalent custom abstractions

Use newer C++26 facilities when they make ownership, correctness, performance, or APIs better.

Do not use language features merely for novelty.

## Modules first

C++ modules are the default source organization mechanism.

Prefer:

```cpp
export module avionix.entity.cell;

import std;
```

over:

```cpp
#pragma once
#include <vector>
#include <string>
```

Do not create `.hpp` files by default.

Headers are appropriate primarily for:

- C interoperability
- third-party libraries
- platform APIs that require headers
- compatibility surfaces that cannot use modules

Avionix-owned C++ code should normally be a module interface or module implementation unit.

Module names mirror architectural ownership:

```text
core/entity/cell.cppm
    → avionix.entity.cell

core/entity/style.cppm
    → avionix.entity.style

core/object/renderer.cppm
    → avionix.object.renderer

core/task/render.cppm
    → avionix.task.render

core/interface/application.cppm
    → avionix.interface.application

lib/avionix.cppm
    → avionix
```

Prefer:

```text
file location
      │
      ▼
architectural owner
      │
      ▼
module name
```

Keep these relationships predictable.

## Module boundaries

A module should represent a coherent capability or type family.

Do not create one enormous:

```cpp
export module avionix.core;
```

containing the entire implementation.

Likewise, do not create hundreds of microscopic modules without architectural meaning.

Good:

```text
avionix.entity.geometry
avionix.entity.style
avionix.entity.event

avionix.object.terminal
avionix.object.buffer
avionix.object.renderer
avionix.object.input_decoder

avionix.task.render
avionix.task.dispatch

avionix.interface.application
avionix.interface.widget
avionix.interface.layout
```

The root:

```cpp
import avionix;
```

composes the supported API.

## Third-party and platform headers

Keep headers out of exported module purviews whenever possible.

Use the global module fragment:

```cpp
module;

#include <windows.h>

export module avionix.object.terminal;
```

or:

```cpp
module;

#include <third_party.h>

export module avionix.object.adapter;
```

Do not expose third-party types through Avionix's public API unless there is a compelling reason.

Prefer:

```text
third party
    │
    ▼
Avionix object adapter
    │
    ▼
Avionix types
```

not:

```text
consumer
    │
    ▼
third-party types
```

## Entities

Entities contain pure data and rules.

Examples:

```text
cell
style
color
position
size
rect
constraint
key_event
mouse_event
resize_event
```

Entities perform no I/O.

Prefer value semantics.

Prefer small, contiguous, trivially movable types where appropriate.

Example:

```cpp
export module avionix.entity.geometry;

import std;

export namespace avionix {

struct position {
    std::int32_t x{};
    std::int32_t y{};

    friend constexpr bool operator==(
        const position&,
        const position&
    ) noexcept = default;
};

struct size {
    std::uint32_t width{};
    std::uint32_t height{};

    friend constexpr bool operator==(
        const size&,
        const size&
    ) noexcept = default;
};

struct rect {
    position origin{};
    size extent{};
};

}
```

Make operations `constexpr` when they are naturally usable at compile time.

## Objects

Objects own stateful resources.

Examples:

```text
terminal
screen_buffer
renderer
event_queue
input_decoder
terminal_capabilities
```

Objects may depend on entities.

Objects may interact with:

- operating systems
- terminals
- threads
- synchronization primitives
- external libraries

Resource ownership must be obvious from the type.

Use RAII.

A terminal object must restore any terminal state it changes.

Avoid owning raw pointers.

Do not default to `std::shared_ptr`.

## Tasks

Tasks coordinate operations involving objects and entities.

Examples:

```text
render
dispatch
run
resize
present
```

Tasks do not own long-lived resources.

A rendering task may coordinate:

```text
widget
   │
   ▼
layout
   │
   ▼
back buffer
   │
   ▼
renderer
   │
   ▼
terminal
```

but resource ownership remains with objects.

## Interfaces

Interfaces form the primary internal boundary exposed toward the public library façade.

Examples:

```text
application
widget
layout
component
```

Interfaces may depend on tasks, objects, and entities.

They must not contain platform-specific terminal implementation details.

The public `avionix` module should primarily expose interfaces and the entities needed to use them.

## Rendering architecture

Rendering follows:

```text
application
    │
    ▼
widget tree
    │
    ▼
layout
    │
    ▼
back buffer
    │
    ▼
renderer
    │
    ├──compares──► front buffer
    │
    ▼
changed runs
    │
    ▼
ANSI encoder
    │
    ▼
terminal
```

Maintain a virtual terminal screen.

Use:

```text
front buffer
back buffer
```

Widgets render into the back buffer.

The renderer compares it with the front buffer.

Only changed terminal regions are emitted.

After presentation:

```text
back becomes current front
```

Do not redraw the physical terminal blindly.

## Rendering performance

Speed is a default architectural constraint.

Optimize for:

```text
less work
less allocation
less copying
less synchronization
less terminal output
fewer syscalls
better locality
```

Prefer contiguous render storage.

Use:

```cpp
std::vector<cell>
```

with:

```text
index = y * width + x
```

rather than nested vectors.

Avoid heap allocation in per-cell operations.

Batch ANSI output into a contiguous output buffer.

Prefer:

```text
changed cells
    ↓
changed runs
    ↓
ANSI output buffer
    ↓
few terminal writes
```

over one terminal write per cell.

## Optimization policy

Do not confuse complexity with performance.

The optimization progression should normally be:

```text
correct implementation
        ↓
measurement
        ↓
double-buffer diff
        ↓
changed runs
        ↓
allocation reduction
        ↓
dirty regions
        ↓
specialized optimization
```

Profile before adding complex caching or synchronization.

Maintain benchmarks for hot paths.

## Memory

Prefer:

```cpp
std::span
std::string_view
```

for non-owning access.

Use owning containers when ownership is required.

Do not retain a view beyond the lifetime of its backing object.

Prefer stack storage for small fixed-size temporary state.

Use `std::pmr` when a measured allocation pattern benefits from an arena or custom memory resource.

Do not introduce custom allocators without evidence.

## Concurrency

The UI thread owns UI state.

External producers communicate through events.

```text
worker
   │
   ▼
event queue
   │
   ▼
UI thread
   │
   ▼
state
   │
   ▼
render
```

Do not allow worker threads to mutate widgets or render buffers directly.

Prefer message passing to shared mutable state.

Use modern cancellation mechanisms such as:

```cpp
std::jthread
std::stop_token
```

where appropriate.

Avoid blocking the UI thread on:

- filesystem operations
- network operations
- child processes
- long computations
- locks controlled by worker threads

## Events

Events are typed values.

Prefer:

```cpp
using event = std::variant<
    key_event,
    mouse_event,
    paste_event,
    resize_event,
    focus_event
>;
```

over magic strings or loosely typed maps.

Use `std::visit` where it provides clear exhaustive dispatch.

Input data flows:

```text
terminal
    │
    ▼
raw bytes
    │
    ▼
input decoder
    │
    ▼
event
    │
    ▼
event queue
    │
    ▼
application
```

## Unicode

Never assume:

```text
1 byte = 1 character
1 code point = 1 visible character
1 visible character = 1 terminal cell
```

Treat:

```text
UTF-8
  │
  ▼
code points
  │
  ▼
grapheme clusters
  │
  ▼
display width
  │
  ▼
terminal cells
```

as distinct concepts.

Centralize Unicode processing.

Do not use `std::string::size()` to determine terminal display width.

Wide graphemes and continuation cells must be represented consistently in the render buffer.

## Layout

Layout operates on geometry and constraints.

It does not perform terminal I/O.

Start with:

```text
horizontal
vertical
fixed
fill
percentage
minimum
maximum
```

Do not implement browser-style layout without a demonstrated need.

Layout must be deterministic.

## Widgets

Widgets describe rendering and interaction.

Widgets do not:

- perform networking
- access files
- spawn processes
- manipulate OS terminal state
- own global application state

Prefer composition.

Use concepts and static polymorphism when they produce simpler APIs or remove meaningful overhead.

Use runtime polymorphism when runtime composition requires it.

Do not replace straightforward virtual dispatch with complex templates unless there is a measurable or architectural benefit.

## Concepts

Use concepts to express compile-time contracts.

For example:

```cpp
template<typename T>
concept widget =
    requires(
        T value,
        render_context& context
    ) {
        value.render(context);
    };
```

Prefer meaningful constraints over template substitution failures.

Public generic APIs should clearly state their requirements through concepts.

## Error handling

Expected failures should generally use:

```cpp
std::expected<T, error>
```

Examples:

- terminal initialization failure
- unsupported terminal capability
- malformed escape sequence
- platform API failure

Use exceptions when they provide a clearly better boundary.

Never silently ignore failures that can leave terminal state corrupted.

Errors should communicate:

```text
operation
failure
context
```

## Terminal lifecycle

Terminal state is owned by a terminal/session object.

Initialization may:

```text
detect capabilities
enable VT processing
enter alternate screen
enable raw input
enable bracketed paste
hide cursor
```

Destruction restores anything Avionix changed.

Cleanup must happen through RAII, including error paths.

## Platform support

Target:

```text
Windows
Linux
macOS
```

Platform-specific code stays behind objects or internal implementation modules.

Consumers should never need:

```cpp
#ifdef _WIN32
```

to use Avionix.

Prefer common ANSI/VT behavior where modern platforms support it.

Isolate unavoidable OS differences.

## Public API stability

The stable API is what `import avionix;` exports.

Internal modules are not automatically stable APIs.

This distinction allows Avionix to change:

```text
renderer internals
buffer representation
platform implementation
event queue implementation
ANSI encoder
optimization strategies
```

without breaking consumers.

Keep the public surface intentionally small.

## Dependency policy

Prefer zero external runtime dependencies where practical.

Before adding a dependency, determine:

1. What owns the dependency.
2. What capability it provides.
3. Why Avionix should not implement the capability itself.
4. Its runtime cost.
5. Its binary-size cost.
6. Its build-time cost.
7. Its supported platforms.
8. Its maintenance status.
9. Its license.
10. Whether its types would leak into the public API.

Vendored dependencies belong under:

```text
libs/
```

Pin dependencies to known versions or commits.

Do not modify vendored source without documenting why.

## Build system

Use Zig as the primary build system.

The build must understand the C++ module dependency graph.

The build must produce an Avionix library consumable by another C++ project.

The intended consumer experience is:

```cpp
import avionix;
```

not manual inclusion of Avionix implementation files.

Keep compiler options centralized.

Do not weaken warnings globally because a dependency emits warnings.

Apply dependency-specific flags to dependencies.

## Tests

Tests should target architectural boundaries.

Prioritize:

```text
geometry
styles
clipping
layout
UTF-8 decoding
display width
buffer operations
buffer diffing
changed-run generation
ANSI encoding
input decoding
event dispatch
resize behavior
terminal cleanup
```

Rendering tests should normally operate on virtual buffers rather than requiring a real interactive terminal.

Keep tests deterministic.

## Benchmarks

Benchmark the hot path.

At minimum measure:

```text
buffer clear
buffer resize
glyph insertion
text rendering
front/back comparison
changed-run generation
ANSI encoding
layout
Unicode width
input decoding
event dispatch
```

Track more than elapsed time.

Where useful, measure:

```text
latency
throughput
allocations
bytes copied
cells inspected
terminal bytes generated
```

A renderer that executes faster but emits substantially more terminal output may be worse overall.

## Naming

Use `snake_case` for:

- files
- variables
- functions
- namespaces

Use names that communicate ownership and responsibility.

Prefer:

```text
terminal_session
render_buffer
input_decoder
event_queue
display_width
```

Avoid vague buckets:

```text
manager
helper
utils
common
misc
```

If functionality has a coherent responsibility, give it an owner.

## Comments

Comments explain:

- invariants
- ownership
- lifetime
- synchronization
- terminal protocol behavior
- platform differences
- unusual performance decisions
- why an implementation is necessary

Do not narrate syntax.

## Documentation

Document public contracts.

For public APIs describe:

- ownership
- lifetime
- thread safety
- error behavior
- complexity where relevant
- invalidation behavior where relevant

Examples should use:

```cpp
import avionix;
```

rather than relying on internal modules.

## Application independence

Avionix must not depend on assumptions about its consumers.

It must not require:

- networking
- an AI model
- an agent architecture
- a database
- a storage engine
- a specific application event model
- a particular logger

The dependency direction is:

```text
application
    │
    └──depends──► Avionix

Avionix
    ──X──► application
```

## Initial implementation sequence

Build bottom-up.

```text
entity.geometry
entity.color
entity.style
entity.cell
        │
        ▼
object.terminal
        │
        ▼
object.ansi_encoder
        │
        ▼
object.buffer
        │
        ▼
object.renderer
        │
        ▼
entity.event
        │
        ▼
object.input_decoder
        │
        ▼
object.event_queue
        │
        ▼
task.dispatch
        │
        ▼
interface.layout
        │
        ▼
interface.widget
        │
        ▼
interface.application
        │
        ▼
lib/avionix.cppm
```

Do not begin with sophisticated widgets.

First establish:

```text
terminal
   +
input
   +
virtual screen
   +
diff renderer
   +
event loop
```

Then build higher-level UI.

## Definition of the library boundary

The final architecture should remain understandable as:

```text
                       Consumer
                          │
                          ▼
                  ┌───────────────┐
                  │ import avionix│
                  └───────┬───────┘
                          │
                          ▼
                     lib/avionix
                          │
                          ▼
                    core/interface
                     │    │    │
              ┌──────┘    │    └──────┐
              ▼           ▼           ▼
            task         object      entity
              │           │           ▲
              └───────────┼───────────┘
                          │
                          ▼
                       platform
```

The public façade stays small.

The core stays layered.

The terminal hot path stays fast.

The implementation stays replaceable.

The consuming application sees Avionix rather than its machinery.
