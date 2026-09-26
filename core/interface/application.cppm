module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module avionix.interface.application;

import avionix.entity.geometry;
import avionix.entity.color;
import avionix.entity.style;
import avionix.entity.cell;
import avionix.entity.event;
import avionix.entity.error;
import avionix.object.buffer;
import avionix.object.event_queue;
import avionix.task.run;
import avionix.interface.widget;

// The entry point for consumers. An application owns the event queue and
// focus state, and drives task.run with a widget tree:
//
//   application::run(root)
//        │
//        ▼
//   task.run ──events──► application::handle ──► focused path / hit path
//        │                                          (bubbles to the root)
//        └──draw──► root.render_in(render_context over the back buffer)
//
// Threading: construct, run, and mutate widgets on one thread (the UI
// thread). post() and notify() may be called from any thread; posted
// callbacks run on the UI thread between frames.

export namespace avionix {

struct application_options {
  bool alternate_screen{true};
  bool mouse{true};
  bool bracketed_paste{true};
  bool focus_events{true};
  // Ctrl+C quits unless a widget handles it first. Raw mode delivers
  // Ctrl+C as a key, not as SIGINT.
  bool quit_on_ctrl_c{true};
  // Tab / Shift+Tab move focus unless the focused widget handles them.
  bool tab_moves_focus{true};
  // Dragging the primary mouse button selects rendered cells. Ctrl+C
  // copies an active selection through OSC 52; otherwise it keeps its
  // normal quit behavior.
  bool text_selection{true};
  std::chrono::milliseconds frame_interval{16};
  // Overrides the detected color depth.
  std::optional<color_depth> colors{};
};

class application {
 public:
  // Returns true to stop further handling of the event.
  using event_hook = std::function<bool(const event&)>;

  explicit application(application_options options = {}) : options_{options} {}

  application(const application&) = delete;
  application& operator=(const application&) = delete;

  // Runs the event loop until quit() or an unrecoverable error. The
  // terminal is restored before this returns, including on error. `root`
  // must outlive the call.
  [[nodiscard]] std::expected<void, error> run(component& root) {
    if (running_.exchange(true)) {
      return std::unexpected(error{"run application", failure::already_running,
                                   "application::run is already active", 0});
    }
    root_ = &root;
    quit_requested_ = false;
    dirty_ = true;
    focus_first();

    run_options opts;
    opts.terminal.alternate_screen = options_.alternate_screen;
    opts.terminal.mouse = options_.mouse;
    opts.terminal.bracketed_paste = options_.bracketed_paste;
    opts.terminal.focus_events = options_.focus_events;
    opts.frame_interval = options_.frame_interval;
    opts.colors = options_.colors;

    driver d{*this};
    auto result = avionix::run(queue_, d, opts);
    root_ = nullptr;
    focused_ = nullptr;
    running_.store(false);
    return result;
  }

  // Runs with a widget that is not a component. `root` must outlive the
  // call.
  template <widget W>
    requires(!std::derived_from<W, component>)
  [[nodiscard]] std::expected<void, error> run(W& root) {
    borrowed<W> adapter{root};
    return run(static_cast<component&>(adapter));
  }

  // Stops the loop after the current event batch. UI thread only; from
  // other threads use post([&] { app.quit(); }).
  void quit() noexcept { quit_requested_ = true; }

  // Runs `callback` on the UI thread and redraws afterwards. Thread safe.
  void post(ui_callback callback) {
    queue_.post([this, callback = std::move(callback)] {
      if (callback) callback();
      dirty_ = true;
    });
  }

  // Delivers a user_event through the normal event path. Thread safe.
  void notify(user_event value) { queue_.push(event{value}); }

  // Called for every event after widgets had their chance, or before them
  // when `before_widgets` is true. UI thread only.
  void on_event(event_hook hook, bool before_widgets = false) {
    (before_widgets ? early_hook_ : late_hook_) = std::move(hook);
  }

  // Marks the screen for redraw. Every handled event already does this;
  // callbacks that change state outside event handling should call it.
  void request_redraw() noexcept { dirty_ = true; }

  [[nodiscard]] size screen_size() const noexcept { return screen_size_; }

  // Moves focus to `target`, which must be focusable and part of the
  // running tree.
  void focus(component& target) {
    if (focused_ == &target) return;
    if (focused_ != nullptr) focused_->set_focused(false);
    focused_ = &target;
    focused_->set_focused(true);
    dirty_ = true;
  }

  [[nodiscard]] component* focused() const noexcept { return focused_; }

  // Moves focus to the next (or previous) focusable component in tree
  // order, wrapping around.
  void focus_next(bool backwards = false) {
    std::vector<component*> chain;
    collect_focusable(root_, chain);
    if (chain.empty()) return;
    auto it = std::ranges::find(chain, focused_);
    std::size_t index = 0;
    if (it != chain.end()) {
      const auto current = static_cast<std::size_t>(it - chain.begin());
      index = backwards ? (current + chain.size() - 1) % chain.size()
                        : (current + 1) % chain.size();
    }
    focus(*chain[index]);
  }

  // Draws `root` into a fresh buffer without a terminal. Deterministic;
  // intended for tests and snapshots.
  [[nodiscard]] static render_buffer render_to_buffer(component& root, size extent) {
    render_buffer buffer{extent};
    frame_state frame;
    render_context context{buffer, bounds(extent), frame};
    root.render_in(context);
    return buffer;
  }

  // Delivers one event to `root` exactly as run() would, without a
  // terminal. For tests. UI thread only.
  void simulate(component& root, const event& value) {
    root_ = &root;
    if (focused_ == nullptr) focus_first();
    handle(value);
  }

 private:
  struct driver {
    application& app;

    void handle(const event& value) { app.handle(value); }
    void draw(render_buffer& back) { app.draw(back); }
    [[nodiscard]] bool running() const noexcept { return !app.quit_requested_; }
    [[nodiscard]] bool needs_redraw() const noexcept { return app.dirty_; }
    [[nodiscard]] std::optional<position> cursor() const noexcept {
      return app.frame_.cursor;
    }
    [[nodiscard]] std::string take_output() {
      return std::exchange(app.pending_output_, {});
    }
  };

  void draw(render_buffer& back) {
    frame_ = {};
    if (root_ != nullptr) {
      render_context context{back, back.area(), frame_};
      root_->render_in(context);
    }
    rendered_ = back;
    if (selection_visible_) apply_selection(back);
    dirty_ = false;
  }

  void handle(const event& value) {
    dirty_ = true;
    if (const auto* r = std::get_if<resize_event>(&value)) {
      screen_size_ = r->extent;
    }
    if (early_hook_ && early_hook_(value)) return;

    if (options_.text_selection) {
      if (const auto* m = std::get_if<mouse_event>(&value);
          m != nullptr && handle_selection_mouse(*m)) {
        return;
      }
    }

    std::vector<component*> path;
    if (const auto* m = std::get_if<mouse_event>(&value)) {
      hit_path(root_, m->where, path);
    } else if (std::holds_alternative<key_event>(value) ||
               std::holds_alternative<paste_event>(value)) {
      focus_path(root_, path);
    } else if (root_ != nullptr) {
      path.push_back(root_);
    }

    bool handled = false;
    event_context context;
    for (auto it = path.rbegin(); it != path.rend() && !handled; ++it) {
      handled = (*it)->on_event(value, context) == event_result::handled;
    }
    if (context.focus_request() != nullptr) focus(*context.focus_request());
    if (context.quit_requested()) quit();
    if (handled) return;

    if (const auto* k = std::get_if<key_event>(&value)) {
      if (k->is(U'c', modifiers::ctrl)) {
        if (selection_visible_) {
          pending_output_ = osc52(selected_text());
          selection_visible_ = false;
          selecting_ = false;
          dirty_ = true;
          return;
        }
        if (options_.quit_on_ctrl_c) {
          quit();
          return;
        }
      }
      if (options_.tab_moves_focus && k->is(key::tab)) {
        focus_next(false);
        return;
      }
      if (options_.tab_moves_focus && k->is(key::tab, modifiers::shift)) {
        focus_next(true);
        return;
      }
    }
    if (const auto* m = std::get_if<mouse_event>(&value)) {
      // Clicking a focusable component focuses it even if it ignores
      // the click.
      if (m->action == mouse_action::press && m->button == mouse_button::left) {
        for (auto it = path.rbegin(); it != path.rend(); ++it) {
          if ((*it)->focusable()) {
            focus(**it);
            break;
          }
        }
      }
    }
    if (late_hook_) late_hook_(value);
  }

  [[nodiscard]] bool handle_selection_mouse(const mouse_event& mouse) {
    if (mouse.button != mouse_button::left) return false;
    if (mouse.action == mouse_action::press) {
      selection_anchor_ = mouse.where;
      selection_active_ = mouse.where;
      selecting_ = true;
      selection_visible_ = false;
      return false;
    }
    if (!selecting_) return false;
    if (mouse.action == mouse_action::drag) {
      selection_active_ = mouse.where;
      selection_visible_ = selection_active_ != selection_anchor_;
      return selection_visible_;
    }
    if (mouse.action == mouse_action::release) {
      selection_active_ = mouse.where;
      selection_visible_ = selection_visible_ || selection_active_ != selection_anchor_;
      selecting_ = false;
      return selection_visible_;
    }
    return false;
  }

  [[nodiscard]] static bool before(position a, position b) noexcept {
    return a.y < b.y || (a.y == b.y && a.x < b.x);
  }

  void apply_selection(render_buffer& buffer) const {
    position first = selection_anchor_;
    position last = selection_active_;
    if (before(last, first)) std::swap(first, last);
    const auto width = static_cast<std::int32_t>(buffer.width());
    for (std::int32_t y = std::max(0, first.y);
         y <= last.y && y < static_cast<std::int32_t>(buffer.height()); ++y) {
      const std::int32_t left = y == first.y ? std::clamp(first.x, 0, width) : 0;
      const std::int32_t right = y == last.y ? std::clamp(last.x + 1, 0, width) : width;
      if (left < right)
        buffer.apply_style({{left, y}, {static_cast<std::uint32_t>(right - left), 1}},
                           {.add = attribute::reverse});
    }
  }

  [[nodiscard]] std::string selected_text() const {
    if (!selection_visible_ || rendered_.width() == 0) return {};
    position first = selection_anchor_;
    position last = selection_active_;
    if (before(last, first)) std::swap(first, last);
    const auto width = static_cast<std::int32_t>(rendered_.width());
    std::string result;
    for (std::int32_t y = std::max(0, first.y);
         y <= last.y && y < static_cast<std::int32_t>(rendered_.height()); ++y) {
      const std::int32_t left = y == first.y ? std::clamp(first.x, 0, width) : 0;
      const std::int32_t right = y == last.y ? std::clamp(last.x + 1, 0, width) : width;
      std::string line;
      for (std::int32_t x = left; x < right; ++x) {
        const cell& value = rendered_.at({x, y});
        if (!value.is_continuation()) line.append(value.text());
      }
      while (!line.empty() && line.back() == ' ') line.pop_back();
      result += line;
      if (y != last.y) result.push_back('\n');
    }
    return result;
  }

  [[nodiscard]] static std::string osc52(std::string_view text) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((text.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < text.size(); i += 3) {
      const auto a = static_cast<unsigned char>(text[i]);
      const auto b = i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0U;
      const auto c = i + 2 < text.size() ? static_cast<unsigned char>(text[i + 2]) : 0U;
      const std::uint32_t bits =
          (std::uint32_t{a} << 16U) | (std::uint32_t{b} << 8U) | c;
      encoded.push_back(alphabet[(bits >> 18U) & 63U]);
      encoded.push_back(alphabet[(bits >> 12U) & 63U]);
      encoded.push_back(i + 1 < text.size() ? alphabet[(bits >> 6U) & 63U] : '=');
      encoded.push_back(i + 2 < text.size() ? alphabet[bits & 63U] : '=');
    }
    return "\x1b]52;c;" + encoded + "\x1b\\";
  }

  void focus_first() {
    std::vector<component*> chain;
    collect_focusable(root_, chain);
    if (focused_ != nullptr) focused_->set_focused(false);
    focused_ = nullptr;
    if (!chain.empty()) focus(*chain.front());
  }

  static void collect_focusable(component* node, std::vector<component*>& out) {
    if (node == nullptr) return;
    if (node->focusable()) out.push_back(node);
    std::vector<component*> kids;
    node->children(kids);
    for (component* child : kids) collect_focusable(child, out);
  }

  // Path from the root to the focused component, or just the root when
  // nothing is focused.
  void focus_path(component* node, std::vector<component*>& path) const {
    if (node == nullptr) return;
    if (focused_ == nullptr || !find_path(node, focused_, path)) {
      path.assign({node});
    }
  }

  static bool find_path(component* node, const component* target,
                        std::vector<component*>& path) {
    path.push_back(node);
    if (node == target) return true;
    std::vector<component*> kids;
    node->children(kids);
    for (component* child : kids) {
      if (find_path(child, target, path)) return true;
    }
    path.pop_back();
    return false;
  }

  // Deepest chain of components whose last drawn area contains `where`.
  // Later siblings win because they draw on top.
  static void hit_path(component* node, position where, std::vector<component*>& path) {
    if (node == nullptr) return;
    path.push_back(node);
    std::vector<component*> kids;
    node->children(kids);
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
      if ((*it)->last_area().contains(where)) {
        hit_path(*it, where, path);
        return;
      }
    }
  }

  application_options options_;
  event_queue queue_{};
  component* root_{};
  component* focused_{};
  frame_state frame_{};
  event_hook early_hook_{};
  event_hook late_hook_{};
  size screen_size_{};
  render_buffer rendered_{};
  position selection_anchor_{};
  position selection_active_{};
  std::string pending_output_{};
  std::atomic<bool> running_{false};
  bool selecting_{};
  bool selection_visible_{};
  bool quit_requested_{};
  bool dirty_{true};
};

}  // namespace avionix
