module;

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module avionix.interface.widget;

import avionix.entity.geometry;
import avionix.entity.style;
import avionix.entity.unicode;
import avionix.entity.cell;
import avionix.entity.event;
import avionix.object.buffer;

// Widgets describe drawing and interaction. They draw through a
// render_context, which is a clipped, translated view of the back buffer,
// and receive events through on_event. They never touch the terminal.
//
//   application ──render_context──► component::render ──► render_buffer
//   application ──event + event_context──► component::on_event
//
// Two forms are supported:
// - `component`: an abstract base for runtime composition (containers hold
//   std::unique_ptr<component>).
// - the `widget` concept: any type with render(render_context&). Such types
//   are wrapped into a component by make_component() without the user
//   writing a virtual class.

export namespace avionix {

// Per-frame state shared by every render_context of one frame.
struct frame_state {
  std::optional<position> cursor{};
};

class render_context {
 public:
  // `area` is in buffer coordinates. The context draws only inside
  // area ∩ clip. The buffer and frame state must outlive the context.
  render_context(render_buffer& buffer, rect area, frame_state& frame,
                 style base = {}) noexcept
      : buffer_{&buffer},
        frame_{&frame},
        area_{area},
        clip_{intersect(area, buffer.area())},
        base_{base} {}

  [[nodiscard]] rect area() const noexcept { return area_; }
  [[nodiscard]] rect clip() const noexcept { return clip_; }
  [[nodiscard]] avionix::size extent() const noexcept { return area_.extent; }
  [[nodiscard]] std::uint32_t width() const noexcept { return area_.width(); }
  [[nodiscard]] std::uint32_t height() const noexcept { return area_.height(); }
  [[nodiscard]] const style& base_style() const noexcept { return base_; }

  // A context for a sub-rectangle given in local coordinates. The child is
  // clipped to this context, so a child can never draw outside its parent.
  [[nodiscard]] render_context child(rect local) const noexcept {
    render_context sub = *this;
    sub.area_ = local.translated(area_.origin);
    sub.clip_ = intersect(sub.area_, clip_);
    return sub;
  }

  [[nodiscard]] render_context with_style(const style_patch& patch) const noexcept {
    render_context sub = *this;
    sub.base_ = apply(base_, patch);
    return sub;
  }

  // Draws UTF-8 text on one row at a local position. Returns the columns
  // advanced (the text's display width, even when clipped). `hyperlink`
  // is a URL attached to every cell of the text. The renderer emits it as
  // an OSC 8 hyperlink. An empty URL draws ordinary text.
  std::uint32_t draw_text(position local, std::string_view text,
                          const style_patch& patch = {},
                          std::string_view hyperlink = {}) const {
    const std::uint16_t link =
        hyperlink.empty() ? std::uint16_t{0} : buffer_->intern_link(hyperlink);
    return buffer_->put_text(local + area_.origin, text, apply(base_, patch), clip_,
                             link);
  }

  // Draws text using an absolute style instead of patching the base.
  std::uint32_t draw_text_styled(position local, std::string_view text,
                                 const style& appearance,
                                 std::string_view hyperlink = {}) const {
    const std::uint16_t link =
        hyperlink.empty() ? std::uint16_t{0} : buffer_->intern_link(hyperlink);
    return buffer_->put_text(local + area_.origin, text, appearance, clip_, link);
  }

  // Fills a local rect with a single narrow grapheme (default: space).
  void fill(rect local, std::string_view glyph = " ",
            const style_patch& patch = {}) const {
    const rect target = intersect(local.translated(area_.origin), clip_);
    if (target.empty()) return;
    const style appearance = apply(base_, patch);
    const std::uint8_t w = grapheme_width(glyph);
    if (glyph == " " || w != 1) {
      buffer_->fill(target, cell::blank(appearance));
      return;
    }
    buffer_->fill(target, cell::from_grapheme(glyph, 1, appearance));
  }

  // Fills the whole area with blanks in the base style patched by `patch`.
  void clear(const style_patch& patch = {}) const {
    fill(bounds(area_.extent), " ", patch);
  }

  // Patches the style of already drawn cells (highlighting, selection).
  void restyle(rect local, const style_patch& patch) const {
    buffer_->apply_style(intersect(local.translated(area_.origin), clip_), patch);
  }

  // Requests the visible terminal cursor at a local position for this
  // frame. The last request in a frame wins. Ignored when outside the clip.
  void set_cursor(position local) const noexcept {
    const position absolute = local + area_.origin;
    if (clip_.contains(absolute)) {
      frame_->cursor = absolute;
    }
  }

  [[nodiscard]] render_buffer& buffer() const noexcept { return *buffer_; }

 private:
  render_buffer* buffer_;
  frame_state* frame_;
  rect area_;
  rect clip_;
  style base_;
};

enum class event_result : std::uint8_t { ignored, handled };

class component;

// Requests a widget can make while handling an event. The application
// applies them after the handler returns.
class event_context {
 public:
  void request_quit() noexcept { quit_ = true; }
  void request_focus(component& target) noexcept { focus_ = &target; }

  // Mouse events through release go to this widget, including drags that
  // leave its area. The application drops the capture on release. Used by
  // text selection that scrolls while the pointer is held.
  void capture_pointer() noexcept { capture_pointer_ = true; }

  // Asks the application to copy `text` to the terminal clipboard with
  // OSC 52. The application owns the bytes it writes. Empty text is ignored.
  void copy_text(std::string text) { copy_ = std::move(text); }

  [[nodiscard]] bool quit_requested() const noexcept { return quit_; }
  [[nodiscard]] component* focus_request() const noexcept { return focus_; }
  [[nodiscard]] bool pointer_captured() const noexcept { return capture_pointer_; }
  [[nodiscard]] std::string take_copy() { return std::exchange(copy_, {}); }

 private:
  component* focus_{};
  std::string copy_{};
  bool quit_{};
  bool capture_pointer_{};
};

// Base for runtime-composed widgets. Instances are owned by their parent
// container (or by the caller for the root) and used only on the UI thread.
//
// Components are movable so a freshly built value can be moved into a
// container. Do not move a component after it has been added to a tree or
// focused: the application and containers hold pointers to it.
class component {
 public:
  component() = default;
  component(const component&) = delete;
  component& operator=(const component&) = delete;
  component(component&&) noexcept = default;
  component& operator=(component&&) noexcept = default;
  virtual ~component() = default;

  // Draws into the context. Called every frame with the component's
  // current area; implementations must not cache the context.
  virtual void render(render_context& context) = 0;

  virtual event_result on_event(const event& /*value*/, event_context& /*context*/) {
    return event_result::ignored;
  }

  [[nodiscard]] virtual bool focusable() const noexcept { return false; }

  // Appends direct children in focus/traversal order.
  virtual void children(std::vector<component*>& /*out*/) {}

  [[nodiscard]] bool focused() const noexcept { return focused_; }
  virtual void on_focus_changed(bool) {}
  virtual void on_hover_changed(bool) {}
  virtual void on_mouse_enter() {}
  virtual void on_mouse_leave() {}
  void set_focused(bool value) {
    if (focused_ == value) return;
    focused_ = value;
    on_focus_changed(value);
  }
  [[nodiscard]] bool hovered() const noexcept { return hovered_; }
  void set_hovered(bool value) {
    if (hovered_ == value) return;
    hovered_ = value;
    on_hover_changed(value);
    if (value)
      on_mouse_enter();
    else
      on_mouse_leave();
  }
  void clear_area() {
    last_area_ = {};
    set_focused(false);
    set_hovered(false);
    std::vector<component*> kids;
    children(kids);
    for (auto* child : kids) child->clear_area();
  }

  // Area assigned by the most recent render, in screen coordinates. Used
  // for mouse hit testing.
  [[nodiscard]] rect last_area() const noexcept { return last_area_; }

  // Records the area for hit testing, then renders. Containers and the
  // application call this rather than render() directly.
  void render_in(render_context& context) {
    last_area_ = context.clip();
    render(context);
  }

 private:
  rect last_area_{};
  bool focused_{};
  bool hovered_{};
};

// Compile-time contract for widgets that are not components.
template <typename T>
concept widget = requires(T& value, render_context& context) { value.render(context); };

template <typename T>
concept interactive_widget =
    widget<T> && requires(T& value, const event& e, event_context& context) {
      { value.on_event(e, context) } -> std::same_as<event_result>;
    };

template <typename T>
concept focusable_widget = widget<T> && requires(const T& value) {
  { value.focusable() } -> std::convertible_to<bool>;
};

// Adapts a `widget` value into a component. Owns the value.
template <widget T>
class widget_adapter final : public component {
 public:
  template <typename... Args>
  explicit widget_adapter(std::in_place_t /*tag*/, Args&&... args)
      : value_(std::forward<Args>(args)...) {}

  void render(render_context& context) override { value_.render(context); }

  event_result on_event(const event& value, event_context& context) override {
    if constexpr (interactive_widget<T>) {
      return value_.on_event(value, context);
    } else {
      return event_result::ignored;
    }
  }

  [[nodiscard]] bool focusable() const noexcept override {
    if constexpr (focusable_widget<T>) {
      return value_.focusable();
    } else {
      return false;
    }
  }

  [[nodiscard]] T& get() noexcept { return value_; }

 private:
  T value_;
};

// Borrows a component or widget owned elsewhere. The referenced object must
// outlive the borrowing container.
//
// A borrowed component is exposed as this node's only child, so focus,
// hit testing, and event bubbling reach it directly; the wrapper itself
// then ignores events to avoid delivering them twice. A borrowed widget
// (non-component) is forwarded everything.
template <typename T>
class borrowed final : public component {
 public:
  explicit borrowed(T& target) noexcept : target_{&target} {}

  void render(render_context& context) override {
    if constexpr (std::derived_from<T, component>) {
      target_->render_in(context);
    } else {
      target_->render(context);
    }
  }

  event_result on_event(const event& value, event_context& context) override {
    if constexpr (!std::derived_from<T, component> && interactive_widget<T>) {
      return target_->on_event(value, context);
    } else {
      (void)value;
      (void)context;
      return event_result::ignored;
    }
  }

  [[nodiscard]] bool focusable() const noexcept override {
    if constexpr (!std::derived_from<T, component> && focusable_widget<T>) {
      return target_->focusable();
    } else {
      return false;
    }
  }

  void children(std::vector<component*>& out) override {
    if constexpr (std::derived_from<T, component>) {
      out.push_back(target_);
    }
  }

 private:
  T* target_;
};

// Result of make_component: the owning pointer plus a reference to the
// stored value so callers can keep configuring it.
template <typename T>
struct made_component {
  std::unique_ptr<component> owner;
  T& value;
};

template <typename T>
  requires std::derived_from<std::remove_cvref_t<T>, component> ||
           widget<std::remove_cvref_t<T>>
[[nodiscard]] auto make_component(T&& value) {
  using stored = std::remove_cvref_t<T>;
  if constexpr (std::derived_from<stored, component>) {
    auto owner = std::make_unique<stored>(std::forward<T>(value));
    stored& ref = *owner;
    return made_component<stored>{std::move(owner), ref};
  } else {
    auto owner =
        std::make_unique<widget_adapter<stored>>(std::in_place, std::forward<T>(value));
    stored& ref = owner->get();
    return made_component<stored>{std::move(owner), ref};
  }
}

// A component that draws with a callable, for one-off custom drawing.
class canvas final : public component {
 public:
  using draw_function = std::function<void(render_context&)>;

  explicit canvas(draw_function draw) : draw_{std::move(draw)} {}

  void render(render_context& context) override {
    if (draw_) draw_(context);
  }

 private:
  draw_function draw_;
};

}  // namespace avionix
