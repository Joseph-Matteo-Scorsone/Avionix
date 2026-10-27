# GLFW (vendored)

- Upstream: https://github.com/glfw/glfw
- Version: 3.4, commit 7b6aead9fb88b3623e3b3725ebb42670cbe4c579
- License: zlib/libpng (`LICENSE.md`)
- Contents: `include/` and the C and Objective-C sources from `src/`.
  Upstream CMake files, docs, examples, tests, and `deps/` are not copied.
- Modifications: none.

Avionix compiles GLFW only into the optional `avionix_viewport` library.
`core/object/native_window.cppm` is the only Avionix module that includes
`GLFW/glfw3.h`. No GLFW type appears in the public API.
