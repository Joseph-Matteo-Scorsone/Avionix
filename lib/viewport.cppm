// The public 3D viewport module. Consumers write
//
//   import avionix;            // terminal UI
//   import avionix.viewport;   // graphical 3D viewport
//
// and link `avionix_viewport` in addition to `avionix`. The viewport is a
// separate module and library because it brings a windowing library
// (GLFW) and OpenGL into the link; terminal-only consumers do not pay for
// either.
//
//   consumer ──import avionix.viewport──► this facade
//                                            │
//                                            ├─► interface: viewport
//                                            ├─► object: scene_store (read-only queries)
//                                            └─► entity: spatial, scene, camera, picking,
//                                                manipulator, image, viewport events
//
// The window adapter, GL loader, and renderer stay internal.

export module avionix.viewport;

export import avionix.entity.spatial;
export import avionix.entity.scene;
export import avionix.entity.camera;
export import avionix.entity.picking;
export import avionix.entity.manipulator;
export import avionix.entity.image;
export import avionix.entity.viewport;
export import avionix.object.scene_store;
export import avionix.interface.viewport;
export import avionix.interface.viewport_panel;
