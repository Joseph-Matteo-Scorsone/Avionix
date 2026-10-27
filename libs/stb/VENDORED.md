# stb_image (vendored)

- Upstream: https://github.com/nothings/stb
- Version: stb_image 2.30, commit 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5
- License: public domain or MIT, at the user's choice (end of `stb_image.h`)
- Contents: `stb_image.h` only. `stb_image_impl.c` is Avionix's, not upstream.
- Modifications: none.

Compiled into `libavionix` with PNG and JPEG only, no stdio, and a
16384-pixel dimension limit. `core/object/image_file.cppm` is the only
module that includes the header; no stb type appears in the public API.
