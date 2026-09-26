// The public Avionix module. Consumers write `import avionix;` and nothing
// else. This file is the stable API: everything it exports is supported,
// and nothing else is.
//
//   consumer ──import avionix──► this facade
//                                   │
//                                   ├─► interface: application, widget, layout,
//                                   controls ├─► entity: geometry, color, style, event,
//                                   constraint, error └─► selected names from unicode
//                                   and the render buffer
//
// Terminal, renderer, input decoding, and the event loop stay internal so
// their implementation can change without breaking consumers.

export module avionix;

export import avionix.entity.geometry;
export import avionix.entity.color;
export import avionix.entity.style;
export import avionix.entity.event;
export import avionix.entity.constraint;
export import avionix.entity.error;

export import avionix.interface.widget;
export import avionix.interface.layout;
export import avionix.interface.controls;
export import avionix.interface.application;

import avionix.entity.unicode;
import avionix.entity.cell;
import avionix.object.buffer;

export namespace avionix {

// Unicode measurement for widget authors.
using avionix::display_width;
using avionix::grapheme;
using avionix::grapheme_view;
using avionix::grapheme_width;
using avionix::graphemes;
using avionix::prefix_fitting_width;

// The back buffer widgets draw into, exposed read-mostly for custom widgets
// (render_context::buffer) and snapshot tests (application::render_to_buffer).
using avionix::cell;
using avionix::render_buffer;

}  // namespace avionix
