module;

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

export module avionix.interface.viewport;

import avionix.entity.viewport;
import avionix.entity.error;
import avionix.object.event_queue;
import avionix.object.scene_store;
import avionix.object.native_window;
import avionix.object.scene_renderer;
import avionix.object.software_renderer;
import avionix.interface.application;

// An interactive 3D viewport in a companion graphical window.
//
// Ownership:
//
//   host (Cloak): exact geometry, modeling, documents, revisions
//      │  scene_update (tessellated meshes, nodes, transforms)    ▲
//      ▼                                                          │ viewport_event
//   viewport ── scene_store (retained copy, survives close/open)  │ (selection,
//   transform
//      │     ── camera, selection, gestures, explode, captures    │  intents, captures)
//      ├──► native_window (GLFW): input, context, swap ───────────┘
//      └──► scene_renderer (OpenGL 3.3 core)
//
// The viewport renders and interprets interaction. It never edits the
// host's document: a drag produces transform_intent events, and the host
// decides what to apply and submits the result as a newer revision.
//
// Threading: every member except post_update() runs on the UI thread, the
// thread that constructed the viewport. On macOS that must be the process
// main thread. Attach the viewport to the application running the terminal
// UI so both share one loop:
//
//   application::run ──drain──► viewport native loop: wait for window
//        ▲                       events or a queue wake-up, then process
//        └──── terminal input, posts, and notify wake it ────┘
//
// Without an application, call pump() regularly.
//
// Event delivery: handlers run on the UI thread from the loop (or pump()),
// never from inside the call that caused the event, so a handler may call
// any viewport member except the destructor. Events are delivered in the
// order produced.

export namespace avionix {

struct viewport_options {
  std::string title{"Avionix viewport"};
  // Logical window size; document coordinates never depend on it.
  std::uint32_t width{1024};
  std::uint32_t height{720};
  bool focus_on_open{true};
  int samples{4};
  length_unit unit{length_unit::millimeter};
  rgba background_top{0.30F, 0.33F, 0.38F, 1.0F};
  rgba background_bottom{0.11F, 0.12F, 0.14F, 1.0F};
  rgba highlight{1.0F, 0.62F, 0.12F, 1.0F};
  // Highlights and reports the node under the pointer. Costs one CPU pick
  // per pointer move.
  bool hover{true};
  bool axis_triad{true};
  std::chrono::milliseconds explode_duration{300};
  // render_view() uses the CPU renderer even when OpenGL is available.
  bool software_only{false};
  // Draw no background in terminal panels and captures, so the terminal's
  // own background (color or background image) shows around the model.
  // The companion window always draws the gradient.
  bool transparent_background{false};
};

class viewport {
 public:
  using event_handler = std::function<void(const viewport_event&)>;
  // Adjusts or rejects (nullopt) a preview proposal. Runs on the UI thread
  // before each preview event; the commit carries the last accepted value.
  using transform_filter =
      std::function<std::optional<transform3d>(const transform_intent&)>;

  explicit viewport(viewport_options options = {})
      : options_{std::move(options)}, owner_{std::this_thread::get_id()} {
    extent_ = {options_.width, options_.height};
    loop_.owner = this;
  }

  viewport(const viewport&) = delete;
  viewport& operator=(const viewport&) = delete;

  // Closes the window without emitting events and detaches from the
  // application. Must run on the UI thread, outside any viewport callback.
  ~viewport() {
    *alive_ = false;
    if (animation_timer_) animation_timer_->cancel();
    detach();
    shutdown_graphics();
    release_offscreen();
  }

  // ── Lifecycle ─────────────────────────────────────────────────────────

  // Opens the window and its context, uploading the retained scene. Opening
  // an open viewport does nothing. Fails with graphics_unavailable when no
  // display or OpenGL 3.3 core context is available; the error context
  // says what to change.
  [[nodiscard]] std::expected<void, error> open() {
    if (auto t = check_thread("open viewport"); !t) return t;
    if (graphics_) return {};
    auto window = native_window::open({options_.title, options_.width, options_.height,
                                       true, options_.focus_on_open, options_.samples});
    if (!window) return std::unexpected(std::move(window.error()));
    window->make_current();
    auto renderer = scene_renderer::create(native_window::proc_lookup());
    if (!renderer) return std::unexpected(std::move(renderer.error()));
    renderer->sync(store_);
    graphics_.emplace(graphics{std::move(*window), std::move(*renderer)});
    extent_ = graphics_->window.framebuffer_size();
    content_scale_ = graphics_->window.content_scale();
    graphics_->window.on_refresh([this] { render_now(); });
    {
      std::scoped_lock lock{post_mutex_};
      window_alive_ = true;
    }
    if (app_ != nullptr) app_->set_native_loop(&loop_);
    emit(viewport_opened{extent_, content_scale_, graphics_->renderer.description()});
    mark_dirty();
    return {};
  }

  // Closes the window and frees GPU resources. The scene, camera,
  // selection, and settings are kept for the next open(). Cancels an active
  // gesture. Emits viewport_closed{close_reason::host}.
  void close() { close_with(close_reason::host); }

  [[nodiscard]] bool is_open() const noexcept { return graphics_.has_value(); }

  // Shares `app`'s UI loop. The application must outlive the attachment;
  // call detach() or destroy the viewport first.
  void attach(application& app) {
    detach();
    {
      std::scoped_lock lock{post_mutex_};
      app_ = &app;
    }
    if (graphics_) app.set_native_loop(&loop_);
  }
  void detach() {
    application* app = nullptr;
    {
      std::scoped_lock lock{post_mutex_};
      app = std::exchange(app_, nullptr);
    }
    if (app != nullptr) app->set_native_loop(nullptr);
  }

  // For hosts without an application: waits up to `max_wait` for window
  // events, then processes input, posted updates, animations, captures,
  // rendering, and event delivery. Returns is_open().
  bool pump(std::chrono::milliseconds max_wait = {}) {
    if (!check_thread("pump viewport")) return is_open();
    if (graphics_) {
      process_frame();
      native_window::wait(animating() ? std::min<std::chrono::steady_clock::duration>(
                                            max_wait, std::chrono::milliseconds{16})
                                      : std::chrono::steady_clock::duration{max_wait});
    }
    process_frame();
    return is_open();
  }

  void focus_window() const {
    if (graphics_) graphics_->window.focus();
  }

  void on_event(event_handler handler) { handler_ = std::move(handler); }

  // ── Scene ─────────────────────────────────────────────────────────────

  // Applies an update now. Fails with stale_revision when update.revision
  // is not newer than revision(), or invalid_argument for a bad command;
  // a failed update changes nothing. Success clears committed transform
  // proposals still on screen and cancels a gesture whose node (or an
  // ancestor) changed.
  [[nodiscard]] std::expected<void, error> submit(scene_update update) {
    if (auto t = check_thread("submit scene update"); !t) return t;
    auto snapshot = gesture_snapshot();
    if (auto ok = store_.apply(std::move(update)); !ok) return ok;
    after_update(snapshot);
    return {};
  }

  // Thread safe. Queues an update for the UI thread, which applies it in
  // posting order and emits update_applied or update_rejected. A worker
  // that computed an update against an old revision is rejected as stale
  // instead of overwriting newer state.
  void post_update(scene_update update) {
    std::scoped_lock lock{post_mutex_};
    posted_.push_back(std::move(update));
    wake_locked();
  }

  [[nodiscard]] const scene_store& scene() const noexcept { return store_; }
  [[nodiscard]] std::uint64_t revision() const noexcept { return store_.revision(); }
  [[nodiscard]] length_unit unit() const noexcept { return options_.unit; }

  // ── Presentation ──────────────────────────────────────────────────────

  // Shows (true) or hides the exploded presentation. Physical transforms,
  // picking results, and intents are unaffected except that they operate
  // on what is drawn. Cancels an active gesture.
  void set_exploded(bool exploded, bool animate = true) {
    if (exploded == exploded_ && !animating()) return;
    cancel_gesture();
    exploded_ = exploded;
    const double target = exploded ? 1.0 : 0.0;
    if (animate && (graphics_ || app_ != nullptr) &&
        options_.explode_duration.count() > 0) {
      explode_from_ = explode_factor_;
      explode_start_ = std::chrono::steady_clock::now();
      explode_animating_ = true;
    } else {
      explode_factor_ = target;
      explode_animating_ = false;
    }
    mark_dirty();
    schedule();
  }
  [[nodiscard]] bool exploded() const noexcept { return exploded_; }
  // Current presentation blend in [0, 1], including animation.
  [[nodiscard]] double explode_factor() const noexcept { return explode_factor_; }

  // See viewport_options::transparent_background.
  void set_transparent_background(bool transparent) {
    if (options_.transparent_background == transparent) return;
    options_.transparent_background = transparent;
    mark_dirty();
    schedule();
  }
  [[nodiscard]] bool transparent_background() const noexcept {
    return options_.transparent_background;
  }

  void set_display_mode(display_mode mode) {
    mode_ = mode;
    mark_dirty();
    schedule();
  }
  [[nodiscard]] display_mode mode() const noexcept { return mode_; }

  // Up to max_section_planes planes. Each keeps the side where
  // dot(normal, p) <= offset. Cut solids are not capped: the inside of the
  // cut shows the solid's interior faces, darkened.
  [[nodiscard]] std::expected<void, error> set_section_planes(
      std::vector<section_plane> planes) {
    if (planes.size() > max_section_planes) {
      return std::unexpected(error{"set section planes", failure::invalid_argument,
                                   std::format("{} planes; at most {} are supported",
                                               planes.size(), max_section_planes),
                                   0});
    }
    for (auto& p : planes) {
      if (length(p.value.normal) < 1e-12)
        return std::unexpected(error{"set section planes", failure::invalid_argument,
                                     "section plane normal is zero", 0});
      const double len = length(p.value.normal);
      p.value = {p.value.normal / len, p.value.offset / len};
    }
    sections_ = std::move(planes);
    mark_dirty();
    schedule();
    return {};
  }
  [[nodiscard]] const std::vector<section_plane>& section_planes() const noexcept {
    return sections_;
  }

  // Replaces one overlay layer. Layers draw in ascending order.
  void set_overlay(std::uint32_t layer, overlay_set overlay) {
    overlays_[layer] = std::move(overlay);
    mark_dirty();
    schedule();
  }
  void clear_overlay(std::uint32_t layer) {
    overlays_.erase(layer);
    mark_dirty();
    schedule();
  }

  // Saved views.
  [[nodiscard]] view_state view() const {
    return {camera_, mode_, exploded_, sections_};
  }
  void restore(const view_state& state) {
    set_camera(state.view);
    set_display_mode(state.mode);
    set_exploded(state.exploded, false);
    (void)set_section_planes(state.sections);
  }

  // ── Camera ────────────────────────────────────────────────────────────

  [[nodiscard]] const camera& camera_value() const noexcept { return camera_; }
  void set_camera(const camera& value) {
    camera_ = value;
    camera_.distance = std::max(camera_.distance, 1e-6);
    camera_.ortho_height = std::max(camera_.ortho_height, 1e-9);
    camera_moved();
  }
  void set_projection(projection_kind kind) {
    camera_ = with_projection(camera_, kind);
    camera_moved();
  }
  void set_standard_view(standard_view v) {
    camera_ = oriented(camera_, v);
    camera_moved();
  }
  // Frames every visible node. No change for an empty scene.
  void fit_scene() {
    camera_ = fit(camera_, presented_bounds({}), extent_.aspect());
    camera_moved();
  }
  // Frames the selection, or the scene when nothing is selected.
  void fit_selection() {
    if (selection_.empty()) return fit_scene();
    const std::unordered_set<node_id, id_hash> roots{selection_.begin(),
                                                     selection_.end()};
    camera_ = fit(camera_, presented_bounds(roots), extent_.aspect());
    camera_moved();
  }
  // Output size in framebuffer pixels. While closed, the last window size
  // (or the configured size) is kept so picks and projections stay defined.
  [[nodiscard]] pixel_extent extent() const noexcept { return extent_; }
  [[nodiscard]] double content_scale() const noexcept { return content_scale_; }

  // ── Selection and picking ─────────────────────────────────────────────

  [[nodiscard]] const std::vector<node_id>& selection() const noexcept {
    return selection_;
  }
  // Host-driven selection, for example from an assembly tree in the
  // terminal. Unknown ids are dropped. Emits no event, so a host that
  // mirrors selection_changed into its tree does not loop.
  void select(std::vector<node_id> nodes) {
    std::erase_if(nodes, [&](node_id id) { return store_.node(id) == nullptr; });
    std::vector<node_id> unique;
    for (const node_id id : nodes)
      if (std::ranges::find(unique, id) == unique.end()) unique.push_back(id);
    if (unique == selection_) return;
    cancel_gesture();
    selection_ = std::move(unique);
    mark_dirty();
    schedule();
  }

  // What is drawn under `pixel` (framebuffer pixels) right now, including
  // exploded placement and drag previews.
  [[nodiscard]] std::optional<pick_hit> pick(vec2 pixel) const {
    resolve_current();
    std::vector<pick_target> targets;
    build_targets(targets);
    const auto planes = enabled_planes();
    pick_options opts;
    opts.section_planes = planes;
    opts.edge_tolerance = 6.0 * content_scale_;
    opts.edges = mode_ != display_mode::shaded;
    return avionix::pick(targets, current_frame(), pixel, opts);
  }

  // Framebuffer pixel of a world point, or nullopt behind the eye. Hosts
  // use it to place labels.
  [[nodiscard]] std::optional<vec2> project(vec3 world) const {
    resolve_current();
    const auto p = avionix::project(current_frame(), world);
    if (!p) return std::nullopt;
    return vec2{p->x, p->y};
  }

  // ── Manipulation ──────────────────────────────────────────────────────

  // Which handles appear on a single selected node; nullopt hides them.
  void set_manipulator(std::optional<transform_kind> kind) {
    if (gesture_ && kind != gesture_->kind) cancel_gesture();
    manipulator_ = kind;
    mark_dirty();
    schedule();
  }
  [[nodiscard]] std::optional<transform_kind> manipulator() const noexcept {
    return manipulator_;
  }
  void set_transform_space(transform_space space) {
    cancel_gesture();
    space_ = space;
    mark_dirty();
    schedule();
  }
  [[nodiscard]] transform_space space() const noexcept { return space_; }
  void set_snap(snap_settings snaps) { snap_ = snaps; }
  [[nodiscard]] const snap_settings& snap() const noexcept { return snap_; }
  void set_transform_filter(transform_filter filter) { filter_ = std::move(filter); }

  // Host-driven numeric edit of the single selected node: one preview and
  // one commit (or one cancel when the filter rejects it). `value` is in
  // document units (translate), degrees (rotate), or a factor (scale).
  // Returns false without events when no single node is selected or the
  // constraint is not permitted.
  bool propose_transform(transform_kind kind, axis_constraint constraint,
                         double value) {
    cancel_gesture();
    if (!begin_gesture(kind, constraint, gesture_source::host)) return false;
    const auto proposal =
        numeric_transform(gesture_->frame, kind, constraint, value, current_frame());
    if (!proposal) {
      abandon_gesture();
      return false;
    }
    update_gesture(*proposal);
    return commit_gesture();
  }

  // Ends an active gesture with a cancel event.
  void cancel_gesture() {
    if (!gesture_) return;
    emit_intent(gesture_phase::cancel, gesture_->original);
    abandon_gesture();
  }
  [[nodiscard]] bool gesture_active() const noexcept { return gesture_.has_value(); }

  // Drops committed proposals that are still drawn while the host decides,
  // for a host that rejected a commit without submitting an update.
  void discard_pending() {
    for (const node_id id : pending_) {
      if (!gesture_ || gesture_->node != id) overrides_.erase(id);
    }
    pending_.clear();
    mark_dirty();
    schedule();
  }

  // ── Capture ───────────────────────────────────────────────────────────

  // Queues an offscreen render with an explicit camera and size. Returns
  // the request number carried by its capture_completed event, which
  // arrives from a later loop iteration, never from inside this call.
  // Captures show the host's committed scene: no drag previews, handles,
  // or hover. They work while the window is closed by creating a hidden
  // context for the duration of the capture.
  std::uint64_t request_capture(capture_request request) {
    const std::uint64_t id = ++capture_counter_;
    captures_.push_back({id, std::move(request)});
    schedule();
    return id;
  }

  // ── Terminal panels ───────────────────────────────────────────────────

  // Renders the interactive picture (handles, selection, hover, axis
  // triad) at `size`, for display in a terminal (viewport_panel). With the
  // window open the picture uses the window's camera and aspect ratio;
  // callers pass a size with that aspect. Uses the window's OpenGL
  // context, else a hidden context kept until destruction, else the CPU
  // renderer when no OpenGL 3.3 context can be created. UI thread.
  [[nodiscard]] std::expected<image_rgba8, error> render_view(pixel_extent size) {
    if (size.empty() || size.width > 8192 || size.height > 8192) {
      return std::unexpected(error{
          "render viewport image", failure::invalid_argument,
          std::format("size {}x{} is outside 1..8192", size.width, size.height), 0});
    }
    resolve_current();
    const double scale = extent_.empty() ? content_scale_
                                         : content_scale_ * size.height /
                                               static_cast<double>(extent_.height);
    frame_inputs f;
    build_frame(f, camera_, size, true, true, scale);
    const frame_description d = describe(f);
    if (graphics_) {
      graphics_->window.make_current();
      if (auto image = graphics_->renderer.capture(d, options_.samples)) return image;
    } else if (!options_.software_only && ensure_offscreen()) {
      offscreen_->window.make_current();
      offscreen_->renderer.sync(store_);
      if (auto image = offscreen_->renderer.capture(d, options_.samples)) return image;
      release_offscreen();
      offscreen_failed_ = true;
    }
    return cpu_.render(d, store_, size.width * size.height > 1'000'000 ? 1 : 2);
  }

  // Which renderer render_view() used last: "opengl" or "cpu".
  [[nodiscard]] std::string_view view_renderer() const noexcept {
    return graphics_ || offscreen_ ? "opengl" : "cpu";
  }

  // Sets the drawing surface size while the window is closed, so picks,
  // handles, and fits match a terminal panel's picture. Ignored while the
  // window is open (the window is the surface then).
  void set_surface(pixel_extent size, double scale) {
    if (graphics_ || size.empty()) return;
    if (size == extent_ && scale == content_scale_) return;
    extent_ = size;
    content_scale_ = scale > 0.0 ? scale : 1.0;
    mark_dirty();
  }

  // Increments whenever the picture may have changed. A panel re-renders
  // when it differs from the value it last drew.
  [[nodiscard]] std::uint64_t changes() const noexcept { return changes_; }

  // Whether the viewport acts on `k` (camera, handle, and gesture keys), so
  // a terminal panel can leave other keys to the application.
  [[nodiscard]] bool wants_key(const key_event& k) const noexcept {
    if (gesture_) return true;
    if (k.is(key::home)) return true;
    if (k.is(key::escape)) return !selection_.empty();
    if (k.code != key::character || k.mods != modifiers::none) return false;
    switch (k.character) {
      case U'f':
      case U'h':
      case U'p':
      case U'5':
      case U'1':
      case U'3':
      case U'7':
      case U'0':
      case U'w':
      case U't':
      case U'r':
      case U's':
        return true;
      case U'x':
      case U'y':
      case U'z':
      case U'X':
      case U'Y':
      case U'Z':
        return manipulator_.has_value() && selection_.size() == 1;
      default:
        return false;
    }
  }

  // ── Testing ───────────────────────────────────────────────────────────

  // Processes one input exactly as if the window had produced it, then
  // delivers resulting events. Works with the window closed, so
  // interaction can be tested without a display.
  void simulate(const viewport_input& input) {
    handle_input(input);
    deliver();
  }
  // Processes posted updates, animations, captures, and pending events
  // without waiting. With the window open this is pump(0).
  void process() { process_frame(); }

 private:
  struct graphics {
    native_window window;
    scene_renderer renderer;
  };

  struct loop_adapter final : native_loop {
    viewport* owner{};
    void wait_until(std::chrono::steady_clock::time_point deadline) override {
      owner->native_wait(deadline);
    }
    void wake() noexcept override { native_window::wake(); }
  };

  struct gesture_state {
    std::uint64_t id{};
    node_id node{};
    std::uint64_t revision{};
    transform_kind kind{};
    axis_constraint constraint{};
    gesture_source source{};
    manipulator_frame frame{};
    std::optional<drag_state> drag;
    vec2 last_pixel{};
    std::string numeric;
    transform3d original{};
    transform3d proposed{};
    std::optional<transform3d> prior_override;
  };

  struct pending_capture {
    std::uint64_t id{};
    capture_request request;
  };

  // Versions of the dragged node and its ancestors.
  using version_path = std::vector<std::pair<node_id, std::uint64_t>>;

  // ── Thread and scheduling ─────────────────────────────────────────────

  [[nodiscard]] std::expected<void, error> check_thread(
      std::string_view operation) const {
    if (std::this_thread::get_id() == owner_) return {};
    return std::unexpected(
        error{operation == "open viewport"   ? "open viewport"
              : operation == "pump viewport" ? "pump viewport"
                                             : "submit scene update",
              failure::wrong_thread,
              "viewport members run on the thread that created it; use "
              "post_update() or application::post() from other threads",
              0});
  }

  // Requires post_mutex_.
  void wake_locked() {
    if (window_alive_) {
      native_window::wake();
    } else if (app_ != nullptr) {
      app_->post([this, alive = std::weak_ptr<bool>{alive_}] {
        if (const auto a = alive.lock(); a && *a) process_frame();
      });
    }
  }

  // UI thread: makes sure process_frame() runs soon.
  void schedule() {
    std::scoped_lock lock{post_mutex_};
    if (scheduled_) return;
    if (window_alive_ || app_ != nullptr) scheduled_ = true;
    wake_locked();
  }

  [[nodiscard]] bool animating() const noexcept { return explode_animating_; }

  void native_wait(std::chrono::steady_clock::time_point deadline) {
    process_frame();
    if (!graphics_) return;
    auto timeout = deadline - std::chrono::steady_clock::now();
    if (animating())
      timeout = std::min<std::chrono::steady_clock::duration>(
          timeout, std::chrono::milliseconds{16});
    native_window::wait(timeout);
    process_frame();
  }

  void process_frame() {
    {
      std::scoped_lock lock{post_mutex_};
      scheduled_ = false;
    }
    if (graphics_) {
      graphics_->window.take_input(inputs_);
      for (const auto& input : inputs_) {
        handle_input(input);
        if (!graphics_) break;
      }
      inputs_.clear();
    }
    apply_posted();
    advance_animation();
    process_captures();
    if (close_requested_) {
      close_requested_ = false;
      close_with(close_reason::graphics_error);
    }
    if (camera_event_pending_ && !pointer_.camera_drag) {
      camera_event_pending_ = false;
      emit(camera_changed{camera_});
    }
    if (dirty_ && graphics_ && !graphics_->window.minimized()) render_now();
    deliver();
  }

  void deliver() {
    if (delivering_) return;
    delivering_ = true;
    for (int round = 0; round < 64 && !events_.empty(); ++round) {
      std::vector<viewport_event> batch;
      batch.swap(events_);
      for (const auto& e : batch)
        if (handler_) handler_(e);
    }
    delivering_ = false;
    if (app_ != nullptr) app_->request_redraw();
  }

  void emit(viewport_event e) { events_.push_back(std::move(e)); }

  void close_with(close_reason reason) {
    if (!graphics_) return;
    cancel_gesture();
    shutdown_graphics();
    pointer_ = {};
    hover_.reset();
    hovered_handle_.reset();
    emit(viewport_closed{reason});
    schedule();
  }

  void mark_dirty() {
    dirty_ = true;
    ++changes_;
  }

  [[nodiscard]] bool ensure_offscreen() {
    if (offscreen_) return true;
    if (offscreen_failed_) return false;
    auto window = native_window::open({options_.title, 64, 64, false, false, 0});
    if (!window) {
      offscreen_failed_ = true;
      return false;
    }
    window->make_current();
    auto renderer = scene_renderer::create(native_window::proc_lookup());
    if (!renderer) {
      offscreen_failed_ = true;
      return false;
    }
    offscreen_.emplace(graphics{std::move(*window), std::move(*renderer)});
    return true;
  }

  void release_offscreen() {
    if (!offscreen_) return;
    offscreen_->window.make_current();
    offscreen_->renderer.release();
    offscreen_.reset();
  }

  void shutdown_graphics() {
    if (!graphics_) return;
    {
      std::scoped_lock lock{post_mutex_};
      window_alive_ = false;
    }
    if (app_ != nullptr) app_->set_native_loop(nullptr);
    graphics_->window.on_refresh({});
    graphics_->window.make_current();
    graphics_->renderer.release();
    graphics_.reset();
  }

  // ── Updates ───────────────────────────────────────────────────────────

  [[nodiscard]] version_path gesture_snapshot() const {
    version_path path;
    if (!gesture_) return path;
    for (node_id id = gesture_->node; id;) {
      const auto* n = store_.node(id);
      if (n == nullptr) break;
      path.emplace_back(id, n->version);
      id = n->desc.parent;
    }
    return path;
  }

  void after_update(const version_path& before) {
    // Committed proposals were waiting for exactly this.
    for (const node_id id : pending_)
      if (!gesture_ || gesture_->node != id) overrides_.erase(id);
    pending_.clear();
    if (gesture_ && gesture_snapshot() != before) cancel_gesture();
    std::erase_if(overrides_, [&](const auto& entry) {
      return store_.node(entry.first) == nullptr;
    });
    const auto old = selection_;
    std::erase_if(selection_, [&](node_id id) { return store_.node(id) == nullptr; });
    if (selection_ != old)
      emit(selection_changed{store_.revision(), selection_, std::nullopt});
    if (hover_ && store_.node(hover_->node) == nullptr) hover_.reset();
    if (graphics_) {
      graphics_->window.make_current();
      graphics_->renderer.sync(store_);
    }
    mark_dirty();
    schedule();
  }

  void apply_posted() {
    std::vector<scene_update> updates;
    {
      std::scoped_lock lock{post_mutex_};
      updates.swap(posted_);
    }
    for (auto& update : updates) {
      const std::uint64_t rev = update.revision;
      auto snapshot = gesture_snapshot();
      if (auto ok = store_.apply(std::move(update)); !ok) {
        emit(update_rejected{rev, std::move(ok.error())});
        continue;
      }
      after_update(snapshot);
      emit(update_applied{rev});
    }
  }

  void advance_animation() {
    if (!explode_animating_) {
      if (animation_timer_) {
        animation_timer_->cancel();
        animation_timer_.reset();
      }
      return;
    }
    // Without a window nothing wakes the loop at frame rate; an interval on
    // the attached application does.
    if (!graphics_ && app_ != nullptr && !animation_timer_) {
      animation_timer_ = app_->set_interval(
          std::chrono::milliseconds{16}, [this, alive = std::weak_ptr<bool>{alive_}] {
            if (const auto a = alive.lock(); a && *a) process_frame();
          });
    }
    const double target = exploded_ ? 1.0 : 0.0;
    const double t =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - explode_start_)
            .count() /
        std::chrono::duration<double>(options_.explode_duration).count();
    if (t >= 1.0) {
      explode_factor_ = target;
      explode_animating_ = false;
    } else {
      const double s = t * t * (3.0 - 2.0 * t);
      explode_factor_ = explode_from_ + (target - explode_from_) * s;
    }
    mark_dirty();
  }

  // ── Scene resolution ──────────────────────────────────────────────────

  void resolve_current() const {
    store_.resolve(resolved_, explode_factor_, &overrides_);
  }

  [[nodiscard]] aabb presented_bounds(
      const std::unordered_set<node_id, id_hash>& roots) const {
    resolve_current();
    aabb box;
    for (const auto& r : resolved_) {
      if (!r.visible || !r.record->desc.mesh) continue;
      if (!roots.empty() && !roots.contains(r.id) &&
          std::ranges::none_of(
              roots, [&](node_id root) { return store_.is_ancestor(root, r.id); }))
        continue;
      if (const auto* m = store_.mesh(r.record->desc.mesh))
        box.extend(transformed(m->bounds, r.world));
    }
    return box;
  }

  [[nodiscard]] view_frame current_frame() const {
    aabb box;
    for (const auto& r : resolved_) {
      if (!r.visible || !r.record->desc.mesh) continue;
      if (const auto* m = store_.mesh(r.record->desc.mesh))
        box.extend(transformed(m->bounds, r.world));
    }
    return frame_for(camera_, extent_, box);
  }

  void build_targets(std::vector<pick_target>& out) const {
    for (const auto& r : resolved_) {
      if (!r.visible || !r.record->desc.pickable || !r.record->desc.mesh) continue;
      const auto* m = store_.mesh(r.record->desc.mesh);
      if (m == nullptr) continue;
      out.push_back({r.id, r.record->desc.mesh, &m->data, r.world, inverse(r.world),
                     transformed(m->bounds, r.world)});
    }
  }

  [[nodiscard]] std::vector<plane> enabled_planes() const {
    std::vector<plane> out;
    for (const auto& s : sections_)
      if (s.enabled) out.push_back(s.value);
    return out;
  }

  [[nodiscard]] bool selected_or_descendant(node_id id) const {
    for (const node_id s : selection_)
      if (s == id || store_.is_ancestor(s, id)) return true;
    return false;
  }

  // ── Handles ───────────────────────────────────────────────────────────

  [[nodiscard]] std::optional<node_id> manipulated_node() const {
    if (!manipulator_ || selection_.size() != 1) return std::nullopt;
    if (store_.node(selection_.front()) == nullptr) return std::nullopt;
    return selection_.front();
  }

  [[nodiscard]] manipulator_frame frame_of(node_id id, transform_kind kind) const {
    const auto* n = store_.node(id);
    const auto it = overrides_.find(id);
    const transform3d& local = it != overrides_.end() ? it->second : n->desc.local;
    return make_frame(store_.parent_frame(id, explode_factor_, &overrides_), local,
                      n->desc.permissions, space_, kind);
  }

  [[nodiscard]] gizmo_style handle_style() const { return {90.0, 7.0, content_scale_}; }

  [[nodiscard]] std::vector<gizmo_shape> current_shapes() const {
    const auto id = manipulated_node();
    if (!id) return {};
    const transform_kind kind = gesture_ ? gesture_->kind : *manipulator_;
    resolve_current();
    return gizmo_shapes(frame_of(*id, kind), current_frame(), kind, handle_style());
  }

  // ── Gestures ──────────────────────────────────────────────────────────

  bool begin_gesture(transform_kind kind, axis_constraint constraint,
                     gesture_source source,
                     std::optional<drag_state> drag = std::nullopt) {
    if (gesture_ || selection_.size() != 1) return false;
    const node_id id = selection_.front();
    const auto* n = store_.node(id);
    if (n == nullptr) return false;
    resolve_current();
    gesture_state g;
    g.id = ++gesture_counter_;
    g.node = id;
    g.revision = store_.revision();
    g.kind = kind;
    g.constraint = constraint;
    g.source = source;
    g.frame = drag ? drag->frame : frame_of(id, kind);
    if (!constraint_permitted(g.frame, kind, constraint)) return false;
    g.drag = std::move(drag);
    g.original = g.frame.local;
    g.proposed = g.original;
    if (const auto it = overrides_.find(id); it != overrides_.end())
      g.prior_override = it->second;
    gesture_ = std::move(g);
    mark_dirty();
    return true;
  }

  void emit_intent(gesture_phase phase, const transform3d& proposed) {
    emit(transform_intent{gesture_->id, gesture_->revision, phase, gesture_->source,
                          gesture_->kind, gesture_->constraint, gesture_->node,
                          gesture_->original, proposed});
  }

  void update_gesture(transform3d proposal) {
    if (!gesture_) return;
    if (filter_) {
      const transform_intent probe{
          gesture_->id,     gesture_->revision, gesture_phase::preview,
          gesture_->source, gesture_->kind,     gesture_->constraint,
          gesture_->node,   gesture_->original, proposal};
      const auto filtered = filter_(probe);
      if (!filtered) return;
      proposal = *filtered;
    }
    if (proposal == gesture_->proposed) return;
    gesture_->proposed = proposal;
    overrides_[gesture_->node] = proposal;
    emit_intent(gesture_phase::preview, proposal);
    mark_dirty();
  }

  // Emits commit, or cancel when nothing changed. Returns true on commit.
  bool commit_gesture() {
    if (!gesture_) return false;
    if (gesture_->proposed == gesture_->original) {
      cancel_gesture();
      return false;
    }
    emit_intent(gesture_phase::commit, gesture_->proposed);
    overrides_[gesture_->node] = gesture_->proposed;
    pending_.insert(gesture_->node);
    gesture_.reset();
    mark_dirty();
    return true;
  }

  // Restores what was drawn before the gesture, without an event.
  void abandon_gesture() {
    if (!gesture_) return;
    if (gesture_->prior_override) {
      overrides_[gesture_->node] = *gesture_->prior_override;
    } else {
      overrides_.erase(gesture_->node);
    }
    gesture_.reset();
    mark_dirty();
  }

  void refresh_gesture() {
    if (!gesture_) return;
    resolve_current();
    const view_frame view = current_frame();
    if (!gesture_->numeric.empty()) {
      const std::string text = gesture_->numeric;
      char* end = nullptr;
      const double value = std::strtod(text.c_str(), &end);
      if (end != text.c_str() && *end == '\0') {
        if (auto p = numeric_transform(gesture_->frame, gesture_->kind,
                                       gesture_->constraint, value, view))
          update_gesture(*p);
      }
      return;
    }
    if (gesture_->drag) {
      update_gesture(
          drag_to(*gesture_->drag, view, gesture_->last_pixel, snap_, false));
    } else {
      update_gesture(gesture_->original);
    }
  }

  // ── Input ─────────────────────────────────────────────────────────────

  struct pointer_state {
    std::optional<pointer_button> down;
    vec2 press{};
    vec2 last{};
    modifiers press_mods{};
    bool camera_drag{};
  };

  void handle_input(const viewport_input& input) {
    std::visit([this](const auto& e) { on_input(e); }, input);
  }

  void on_input(const framebuffer_resized& e) {
    extent_ = e.extent;
    content_scale_ = e.content_scale > 0.0 ? e.content_scale : 1.0;
    mark_dirty();
  }
  void on_input(const window_focus& e) {
    if (!e.focused) {
      if (gesture_ && gesture_->drag) cancel_gesture();
      if (pointer_.camera_drag) camera_event_pending_ = true;
      pointer_ = {};
    }
    emit(viewport_focus_changed{e.focused});
  }
  void on_input(const window_close_requested&) { close_with(close_reason::user); }
  void on_input(const pointer_left&) {
    if (hover_) {
      hover_.reset();
      emit(hover_changed{store_.revision(), std::nullopt});
    }
    hovered_handle_.reset();
    mark_dirty();
  }

  void on_input(const pointer_pressed& e) {
    if (gesture_) {
      if (e.button == pointer_button::left) {
        commit_gesture();
      } else if (e.button == pointer_button::right) {
        cancel_gesture();
      }
      return;
    }
    if (pointer_.down) return;
    pointer_.down = e.button;
    pointer_.press = e.where;
    pointer_.last = e.where;
    pointer_.press_mods = e.mods;
    pointer_.camera_drag = false;
    if (e.button != pointer_button::left) return;
    const auto shapes = current_shapes();
    const auto handle = hit_handle(shapes, e.where, handle_style());
    if (!handle) return;
    const transform_kind kind = *manipulator_;
    const node_id id = selection_.front();
    resolve_current();
    auto drag = begin_drag(frame_of(id, kind), kind, *handle, current_frame(), e.where);
    if (drag &&
        begin_gesture(kind, *handle, gesture_source::pointer, std::move(drag))) {
      gesture_->last_pixel = e.where;
      pointer_.down.reset();
    }
  }

  void on_input(const pointer_moved& e) {
    const vec2 delta = e.where - pointer_.last;
    if (gesture_) {
      gesture_->last_pixel = e.where;
      if (gesture_->drag && gesture_->numeric.empty()) {
        resolve_current();
        update_gesture(drag_to(*gesture_->drag, current_frame(), e.where, snap_,
                               has(e.mods, modifiers::ctrl)));
      }
      pointer_.last = e.where;
      return;
    }
    if (pointer_.down) {
      const double threshold = 3.0 * content_scale_;
      if (!pointer_.camera_drag && length(e.where - pointer_.press) >= threshold)
        pointer_.camera_drag = true;
      if (pointer_.camera_drag) {
        resolve_current();
        const bool pan_drag = *pointer_.down != pointer_button::left ||
                              has(pointer_.press_mods, modifiers::shift);
        if (pan_drag) {
          camera_ = avionix::pan(camera_, current_frame(), delta);
        } else {
          const double rate = 0.0075 / content_scale_;
          camera_ = orbit(camera_, -delta.x * rate, delta.y * rate);
        }
        camera_event_pending_ = true;
        mark_dirty();
      }
      pointer_.last = e.where;
      return;
    }
    pointer_.last = e.where;
    const auto handle = hit_handle(current_shapes(), e.where, handle_style());
    if (handle != hovered_handle_) {
      hovered_handle_ = handle;
      mark_dirty();
    }
    if (!options_.hover) return;
    const auto hit = handle ? std::nullopt : pick(e.where);
    const bool changed =
        hit.has_value() != hover_.has_value() ||
        (hit && (hit->node != hover_->node || hit->face != hover_->face ||
                 hit->edge != hover_->edge));
    if (changed) {
      hover_ = hit;
      emit(hover_changed{store_.revision(), hit});
      mark_dirty();
    }
  }

  void on_input(const pointer_released& e) {
    if (gesture_ && gesture_->source == gesture_source::pointer &&
        e.button == pointer_button::left) {
      commit_gesture();
      return;
    }
    if (!pointer_.down || *pointer_.down != e.button) return;
    const bool was_drag = pointer_.camera_drag;
    pointer_.down.reset();
    pointer_.camera_drag = false;
    if (was_drag) {
      camera_event_pending_ = true;
      return;
    }
    if (e.button != pointer_button::left) return;
    const auto hit = pick(e.where);
    const node_id target = hit ? store_.selection_target(hit->node) : node_id{};
    std::vector<node_id> next = selection_;
    const bool additive = has(e.mods, modifiers::shift) || has(e.mods, modifiers::ctrl);
    if (additive) {
      if (target) {
        if (const auto it = std::ranges::find(next, target); it != next.end()) {
          next.erase(it);
        } else {
          next.push_back(target);
        }
      }
    } else {
      next.clear();
      if (target) next.push_back(target);
    }
    if (next == selection_) return;
    selection_ = std::move(next);
    emit(selection_changed{store_.revision(), selection_, hit});
    mark_dirty();
  }

  void on_input(const wheel_scrolled& e) {
    if (e.delta.y == 0.0) return;
    resolve_current();
    camera_ =
        avionix::zoom(camera_, current_frame(), std::pow(0.88, e.delta.y), e.where);
    camera_event_pending_ = true;
    mark_dirty();
  }

  void on_input(const key_event& k) {
    if (gesture_) {
      gesture_key(k);
      return;
    }
    if (k.code == key::character && k.mods == modifiers::none) {
      switch (k.character) {
        case U'f':
          fit_selection();
          return;
        case U'h':
          fit_scene();
          return;
        case U'p':
        case U'5':
          set_projection(camera_.projection == projection_kind::perspective
                             ? projection_kind::orthographic
                             : projection_kind::perspective);
          return;
        case U'1':
          set_standard_view(standard_view::front);
          return;
        case U'3':
          set_standard_view(standard_view::right);
          return;
        case U'7':
          set_standard_view(standard_view::top);
          return;
        case U'0':
          set_standard_view(standard_view::isometric);
          return;
        case U'w':
          set_display_mode(
              static_cast<display_mode>((static_cast<int>(mode_) + 1) % 4));
          return;
        case U't':
          set_manipulator(transform_kind::translate);
          return;
        case U'r':
          set_manipulator(transform_kind::rotate);
          return;
        case U's':
          set_manipulator(transform_kind::scale);
          return;
        case U'x':
        case U'y':
        case U'z':
        case U'X':
        case U'Y':
        case U'Z':
          if (start_keyboard_gesture(axis_for(k.character))) {
            emit(numeric_entry_changed{
                gesture_->id, gesture_->kind, gesture_->constraint, {}});
            return;
          }
          break;
        default:
          break;
      }
    }
    if (k.is(key::home)) {
      fit_scene();
      return;
    }
    if (k.is(key::escape) && !selection_.empty()) {
      selection_.clear();
      emit(selection_changed{store_.revision(), selection_, std::nullopt});
      mark_dirty();
      return;
    }
    emit(viewport_key{k});
  }

  // X, Y, or Z with a single selection: the pointer moves the node along
  // the axis (or in the plane for Shift) and typed digits set an exact
  // value. Enter or a left click commits; Escape or a right click cancels.
  bool start_keyboard_gesture(axis_constraint constraint) {
    if (!manipulator_ || selection_.size() != 1 ||
        store_.node(selection_.front()) == nullptr)
      return false;
    const transform_kind kind = *manipulator_;
    resolve_current();
    auto drag = begin_drag(frame_of(selection_.front(), kind), kind, constraint,
                           current_frame(), pointer_.last);
    if (!begin_gesture(kind, constraint, gesture_source::keyboard, std::move(drag)))
      return false;
    gesture_->last_pixel = pointer_.last;
    return true;
  }

  [[nodiscard]] static axis_constraint axis_for(char32_t c) noexcept {
    switch (c) {
      case U'x':
        return axis_constraint::x;
      case U'y':
        return axis_constraint::y;
      case U'z':
        return axis_constraint::z;
      case U'X':
        return axis_constraint::yz;
      case U'Y':
        return axis_constraint::zx;
      default:
        return axis_constraint::xy;
    }
  }

  void gesture_key(const key_event& k) {
    if (k.is(key::escape)) {
      cancel_gesture();
      return;
    }
    if (k.is(key::enter)) {
      commit_gesture();
      return;
    }
    bool changed = false;
    if (k.is(key::backspace) && !gesture_->numeric.empty()) {
      gesture_->numeric.pop_back();
      changed = true;
    } else if (k.code == key::character && k.mods == modifiers::none) {
      const char32_t c = k.character;
      if ((c >= U'0' && c <= U'9') || c == U'.' ||
          (c == U'-' && gesture_->numeric.empty())) {
        gesture_->numeric.push_back(static_cast<char>(c));
        changed = true;
      } else if (c == U'x' || c == U'y' || c == U'z' || c == U'X' || c == U'Y' ||
                 c == U'Z') {
        const axis_constraint next = axis_for(c);
        if (constraint_permitted(gesture_->frame, gesture_->kind, next)) {
          gesture_->constraint = next;
          if (gesture_->drag) {
            resolve_current();
            if (auto d = begin_drag(gesture_->frame, gesture_->kind, next,
                                    current_frame(), gesture_->drag->start_pixel))
              gesture_->drag = std::move(d);
          }
          changed = true;
        }
      }
    }
    if (!changed) return;
    refresh_gesture();
    emit(numeric_entry_changed{gesture_->id, gesture_->kind, gesture_->constraint,
                               gesture_->numeric});
  }

  void camera_moved() {
    camera_event_pending_ = true;
    mark_dirty();
    schedule();
  }

  // ── Rendering ─────────────────────────────────────────────────────────

  struct frame_inputs {
    std::vector<draw_item> items;
    std::vector<plane> planes;
    std::vector<world_line> lines_depth;
    std::vector<world_line> lines_top;
    screen_batch screen;
    view_frame view;
  };

  // `scale` is framebuffer pixels per logical pixel for handle sizes and
  // line widths in this output.
  void build_frame(frame_inputs& f, const camera& cam, pixel_extent extent,
                   bool interactive, bool show_selection, double scale) const {
    std::unordered_map<node_id, mat4, id_hash> worlds;
    aabb box;
    for (const auto& r : resolved_) {
      worlds.emplace(r.id, r.world);
      if (!r.visible || !r.record->desc.mesh) continue;
      const auto* m = store_.mesh(r.record->desc.mesh);
      if (m == nullptr) continue;
      box.extend(transformed(m->bounds, r.world));
      const material* look = store_.find_material(r.record->desc.material);
      float highlight = 0.0F;
      if (show_selection && selected_or_descendant(r.id)) highlight = 1.0F;
      if (interactive && highlight == 0.0F && hover_ &&
          (hover_->node == r.id || store_.selection_target(hover_->node) == r.id ||
           store_.is_ancestor(store_.selection_target(hover_->node), r.id)))
        highlight = 0.35F;
      f.items.push_back({r.record->desc.mesh, r.world,
                         look != nullptr ? *look : material{}, highlight});
    }
    f.view = frame_for(cam, extent, box);
    f.planes = enabled_planes();

    for (const auto& [layer, overlay] : overlays_) {
      auto& target = overlay.depth_tested ? f.lines_depth : f.lines_top;
      const auto place = [&](node_id attached, vec3 p) {
        if (!attached) return p;
        const auto it = worlds.find(attached);
        return it == worlds.end() ? p : transform_point(it->second, p);
      };
      for (const auto& l : overlay.lines)
        target.push_back({place(l.attached, l.a), place(l.attached, l.b), l.color});
      for (const auto& m : overlay.markers) {
        if (const auto p = avionix::project(f.view, place(m.attached, m.at))) {
          const double s = m.size * scale * 0.5;
          const vec2 c{p->x, p->y};
          const std::array<vec2, 4> diamond{c + vec2{0, -s}, c + vec2{s, 0},
                                            c + vec2{0, s}, c + vec2{-s, 0}};
          f.screen.polygon(diamond, m.color);
        }
      }
    }

    if (!interactive) return;
    if (const auto id = manipulated_node()) {
      const transform_kind kind = gesture_ ? gesture_->kind : *manipulator_;
      const auto shapes = gizmo_shapes(frame_of(*id, kind), f.view, kind,
                                       gizmo_style{90.0, 7.0, scale});
      const std::optional<axis_constraint> active =
          gesture_ ? std::optional{gesture_->constraint} : hovered_handle_;
      for (const auto& s : shapes) {
        const rgba color =
            active && *active == s.constraint ? active_handle_color : s.color;
        if (s.filled) {
          f.screen.polygon(s.points, color);
        } else {
          f.screen.polyline(s.points, s.loop, 2.5 * scale, color);
        }
      }
    }
    if (options_.axis_triad) {
      const double s = scale;
      const vec2 origin{48.0 * s, static_cast<double>(extent.height) - 48.0 * s};
      for (std::size_t i = 0; i < 3; ++i) {
        const vec3 a = std::array{axis_x, axis_y, axis_z}[i];
        const vec2 tip =
            origin + vec2{dot(a, f.view.right), -dot(a, f.view.up)} * (32.0 * s);
        f.screen.line(origin, tip, 2.0 * s, axis_color(i));
      }
    }
  }

  frame_description describe(const frame_inputs& f) const {
    frame_description d;
    d.view = f.view;
    d.mode = mode_;
    d.background_top = options_.background_top;
    d.background_bottom = options_.background_bottom;
    d.highlight = options_.highlight;
    d.transparent_background = options_.transparent_background;
    d.items = f.items;
    d.sections = f.planes;
    d.lines_depth = f.lines_depth;
    d.lines_on_top = f.lines_top;
    d.screen = f.screen.vertices();
    return d;
  }

  void render_now() {
    if (!graphics_ || extent_.empty()) return;
    graphics_->window.make_current();
    resolve_current();
    frame_inputs f;
    build_frame(f, camera_, extent_, true, true, content_scale_);
    frame_description d = describe(f);
    // A desktop window has nothing of the terminal behind it.
    d.transparent_background = false;
    if (auto ok = graphics_->renderer.draw(d); !ok) {
      emit(graphics_error{std::move(ok.error())});
      close_requested_ = true;
      return;
    }
    graphics_->window.swap();
    dirty_ = false;
  }

  void process_captures() {
    if (captures_.empty()) return;
    std::vector<pending_capture> work;
    work.swap(captures_);
    for (auto& c : work) emit(run_capture(c));
  }

  [[nodiscard]] capture_completed run_capture(const pending_capture& c) {
    capture_completed out{
        c.id, capture_status::failed, store_.revision(), c.request.view, {}, {}};
    const auto& req = c.request;
    if (req.revision && *req.revision != store_.revision()) {
      out.status = capture_status::stale;
      out.detail = std::format("requested revision {}, scene is at revision {}",
                               *req.revision, store_.revision());
      return out;
    }
    if (req.extent.width == 0 || req.extent.height == 0 || req.extent.width > 8192 ||
        req.extent.height > 8192) {
      out.detail = std::format("output size {}x{} is outside 1..8192", req.extent.width,
                               req.extent.height);
      return out;
    }
    // The committed scene: no previews or pending proposals.
    store_.resolve(resolved_, req.exploded ? 1.0 : 0.0, nullptr);
    frame_inputs f;
    build_frame(f, req.view, req.extent, false, req.show_selection, content_scale_);
    frame_description d = describe(f);
    if (req.mode) d.mode = *req.mode;

    std::expected<image_rgba8, error> image;
    if (graphics_) {
      graphics_->window.make_current();
      image = graphics_->renderer.capture(d, options_.samples);
    } else {
      // Hidden context for the duration of this capture.
      auto window = native_window::open({options_.title, 64, 64, false, false, 0});
      if (!window) {
        out.detail = window.error().message();
        return out;
      }
      window->make_current();
      auto renderer = scene_renderer::create(native_window::proc_lookup());
      if (!renderer) {
        out.detail = renderer.error().message();
        return out;
      }
      renderer->sync(store_);
      image = renderer->capture(d, options_.samples);
      renderer->release();
    }
    if (!image) {
      out.detail = image.error().message();
      return out;
    }
    out.status = capture_status::complete;
    out.image = std::move(*image);
    return out;
  }

  // ── State ─────────────────────────────────────────────────────────────

  viewport_options options_;
  std::thread::id owner_;
  scene_store store_;
  camera camera_{};
  pixel_extent extent_{};
  double content_scale_{1.0};
  std::optional<graphics> graphics_;
  std::optional<graphics> offscreen_;  // hidden context for render_view
  bool offscreen_failed_{};
  software_renderer cpu_;
  std::uint64_t changes_{1};
  std::optional<interval_handle> animation_timer_;
  loop_adapter loop_{};
  std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};

  std::mutex post_mutex_;  // guards posted_, app_, window_alive_, scheduled_
  std::vector<scene_update> posted_;
  application* app_{};
  bool window_alive_{};
  bool scheduled_{};

  event_handler handler_;
  std::vector<viewport_event> events_;
  bool delivering_{};

  std::vector<viewport_input> inputs_;
  pointer_state pointer_{};
  std::optional<pick_hit> hover_;
  std::optional<axis_constraint> hovered_handle_;
  std::vector<node_id> selection_;

  display_mode mode_{display_mode::shaded_edges};
  std::vector<section_plane> sections_;
  std::map<std::uint32_t, overlay_set> overlays_;

  bool exploded_{};
  double explode_factor_{};
  double explode_from_{};
  bool explode_animating_{};
  std::chrono::steady_clock::time_point explode_start_{};

  std::optional<transform_kind> manipulator_{transform_kind::translate};
  transform_space space_{transform_space::parent};
  snap_settings snap_{};
  transform_filter filter_;
  std::optional<gesture_state> gesture_;
  std::uint64_t gesture_counter_{};
  transform_overrides overrides_;
  std::unordered_set<node_id, id_hash> pending_;

  std::vector<pending_capture> captures_;
  std::uint64_t capture_counter_{};

  bool dirty_{true};
  bool camera_event_pending_{};
  bool close_requested_{};
  mutable std::vector<scene_store::resolved_node> resolved_;
};

}  // namespace avionix
