module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

export module avionix.object.native_window;

import avionix.entity.viewport;
import avionix.entity.error;
import avionix.object.gl;

// The graphical window and its OpenGL 3.3 core context, through GLFW.
//
//   GLFW callbacks ──translate──► viewport_input (framebuffer pixels)
//        │                              │
//        └─ run inside poll()/wait() ───┴─► pending input, taken by the viewport
//
// This is the only module that includes GLFW. No GLFW type leaves it.
//
// Threading: GLFW requires window creation, destruction, and event
// processing on the process main thread (macOS enforces this; other
// platforms tolerate one consistent thread). Every member except wake()
// must be called on the UI thread. wake() is safe from any thread while at
// least one window exists.
//
// Library lifetime: the first window initializes GLFW and the last one to
// be destroyed terminates it, so a closed viewport leaves no GLFW state,
// helper windows, or threads behind.

export namespace avionix {

struct window_options {
  std::string title{"Avionix viewport"};
  // Logical size. Scaled by the monitor's content scale on Windows and
  // Linux; on macOS the framebuffer is larger than the window on Retina
  // displays instead.
  std::uint32_t width{1024};
  std::uint32_t height{720};
  bool visible{true};
  bool focus_on_open{true};
  // MSAA samples for the default framebuffer. Falls back to 0 when the
  // driver refuses.
  int samples{4};
};

class native_window {
 public:
  [[nodiscard]] static std::expected<native_window, error> open(
      const window_options& options) {
    if (auto init = library_acquire(); !init)
      return std::unexpected(std::move(init.error()));
    auto s = std::make_unique<state>();
    GLFWwindow* handle = create(options, options.samples);
    if (handle == nullptr && options.samples > 0) handle = create(options, 0);
    if (handle == nullptr) {
      auto e = describe_failure("create OpenGL window");
      library_release();
      return std::unexpected(std::move(e));
    }
    s->handle = handle;
    glfwSetWindowUserPointer(handle, s.get());
    install_callbacks(handle);
    glfwMakeContextCurrent(handle);
    glfwSwapInterval(1);
    native_window window{std::move(s)};
    window.state_->input.push_back(
        framebuffer_resized{window.framebuffer_size(), window.content_scale()});
    return window;
  }

  native_window(native_window&& other) noexcept : state_{std::move(other.state_)} {}
  native_window& operator=(native_window&& other) noexcept {
    if (this != &other) {
      destroy();
      state_ = std::move(other.state_);
    }
    return *this;
  }
  native_window(const native_window&) = delete;
  native_window& operator=(const native_window&) = delete;
  ~native_window() { destroy(); }

  void make_current() const { glfwMakeContextCurrent(state_->handle); }
  void swap() const { glfwSwapBuffers(state_->handle); }

  [[nodiscard]] pixel_extent framebuffer_size() const {
    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(state_->handle, &w, &h);
    return {static_cast<std::uint32_t>(std::max(w, 0)),
            static_cast<std::uint32_t>(std::max(h, 0))};
  }
  // Framebuffer pixels per logical pixel.
  [[nodiscard]] double content_scale() const {
    float x = 1.0F;
    float y = 1.0F;
    glfwGetWindowContentScale(state_->handle, &x, &y);
    return x > 0.0F ? static_cast<double>(x) : 1.0;
  }
  [[nodiscard]] bool focused() const {
    return glfwGetWindowAttrib(state_->handle, GLFW_FOCUSED) == GLFW_TRUE;
  }
  [[nodiscard]] bool minimized() const {
    return glfwGetWindowAttrib(state_->handle, GLFW_ICONIFIED) == GLFW_TRUE;
  }
  void focus() const {
    glfwShowWindow(state_->handle);
    glfwFocusWindow(state_->handle);
  }
  void set_title(const std::string& title) const {
    glfwSetWindowTitle(state_->handle, title.c_str());
  }

  // Called when the system asks for a repaint while the UI thread is inside
  // a modal loop (Windows window move and resize), so the picture follows
  // the border during the drag.
  void on_refresh(std::function<void()> callback) const {
    state_->refresh = std::move(callback);
  }

  // Moves pending input into `out` (replacing its contents).
  void take_input(std::vector<viewport_input>& out) const {
    out.clear();
    out.swap(state_->input);
  }

  [[nodiscard]] static gl::proc_lookup proc_lookup() noexcept {
    return [](const char* name) -> gl::proc { return glfwGetProcAddress(name); };
  }

  // Processes pending native events without waiting. UI thread.
  static void poll() {
    if (library_count() > 0) glfwPollEvents();
  }
  // Processes native events, waiting up to `timeout` for the first. Returns
  // early on any native event or wake(). UI thread.
  static void wait(std::chrono::steady_clock::duration timeout) {
    if (library_count() == 0) return;
    const double seconds = std::chrono::duration<double>(timeout).count();
    if (seconds <= 0.0) {
      glfwPollEvents();
    } else {
      glfwWaitEventsTimeout(std::min(seconds, 3600.0));
    }
  }
  // Interrupts wait(). Any thread.
  static void wake() noexcept {
    if (library_count() > 0) glfwPostEmptyEvent();
  }

 private:
  struct state {
    GLFWwindow* handle{};
    std::vector<viewport_input> input;
    std::function<void()> refresh;
    modifiers mods{};
  };

  explicit native_window(std::unique_ptr<state> s) : state_{std::move(s)} {}

  void destroy() {
    if (!state_) return;
    if (state_->handle != nullptr) {
      if (glfwGetCurrentContext() == state_->handle) glfwMakeContextCurrent(nullptr);
      glfwDestroyWindow(state_->handle);
      state_->handle = nullptr;
      library_release();
    }
    state_.reset();
  }

  static GLFWwindow* create(const window_options& options, int samples) {
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, samples);
    glfwWindowHint(GLFW_VISIBLE, options.visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, options.focus_on_open ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, options.focus_on_open ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);
    return glfwCreateWindow(
        static_cast<int>(std::clamp<std::uint32_t>(options.width, 64, 16384)),
        static_cast<int>(std::clamp<std::uint32_t>(options.height, 64, 16384)),
        options.title.c_str(), nullptr, nullptr);
  }

  [[nodiscard]] static error describe_failure(std::string_view operation_text) {
    const char* description = nullptr;
    const int code = glfwGetError(&description);
    std::string detail = description != nullptr ? description : "no detail from GLFW";
    std::string advice;
    switch (code) {
      case GLFW_PLATFORM_UNAVAILABLE:
      case GLFW_PLATFORM_ERROR:
        advice =
            "No graphical display is reachable. On Linux, run inside an X11 or Wayland "
            "desktop session (XWayland is used on Wayland) or set DISPLAY; over SSH "
            "use X "
            "forwarding. The terminal UI keeps working without the viewport.";
        break;
      case GLFW_VERSION_UNAVAILABLE:
      case GLFW_API_UNAVAILABLE:
      case GLFW_FORMAT_UNAVAILABLE:
        advice =
            "OpenGL 3.3 core is required. Update the GPU driver. Remote Desktop "
            "sessions "
            "and some virtual machines expose only OpenGL 1.1; on Linux, Mesa's "
            "software "
            "renderer works with LIBGL_ALWAYS_SOFTWARE=1.";
        break;
      default:
        break;
    }
    if (!advice.empty()) detail += ". " + advice;
    return error{operation_text == "initialize windowing" ? "initialize windowing"
                                                          : "create OpenGL window",
                 failure::graphics_unavailable, std::move(detail), code};
  }

  // UI-thread reference count of open windows. Atomic only so wake() can
  // read it from other threads.
  static std::atomic<int>& library_count() {
    static std::atomic<int> count{0};
    return count;
  }

  [[nodiscard]] static std::expected<void, error> library_acquire() {
    if (library_count().load() == 0) {
#if defined(__linux__)
      // Only the X11 backend is compiled in; Wayland sessions use XWayland.
      glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
      if (glfwInit() != GLFW_TRUE)
        return std::unexpected(describe_failure("initialize windowing"));
    }
    library_count().fetch_add(1);
    return {};
  }
  static void library_release() {
    if (library_count().fetch_sub(1) == 1) glfwTerminate();
  }

  static state& of(GLFWwindow* w) {
    return *static_cast<state*>(glfwGetWindowUserPointer(w));
  }

  static modifiers translate_mods(int mods) {
    modifiers m = modifiers::none;
    if ((mods & GLFW_MOD_SHIFT) != 0) m |= modifiers::shift;
    if ((mods & GLFW_MOD_CONTROL) != 0) m |= modifiers::ctrl;
    if ((mods & GLFW_MOD_ALT) != 0) m |= modifiers::alt;
    if ((mods & GLFW_MOD_SUPER) != 0) m |= modifiers::super;
    return m;
  }

  // Window points to framebuffer pixels.
  static vec2 to_pixels(GLFWwindow* w, double x, double y) {
    int ww = 0;
    int wh = 0;
    int fw = 0;
    int fh = 0;
    glfwGetWindowSize(w, &ww, &wh);
    glfwGetFramebufferSize(w, &fw, &fh);
    const double sx = ww > 0 ? static_cast<double>(fw) / ww : 1.0;
    const double sy = wh > 0 ? static_cast<double>(fh) / wh : 1.0;
    return {x * sx, y * sy};
  }
  static vec2 cursor(GLFWwindow* w) {
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(w, &x, &y);
    return to_pixels(w, x, y);
  }

  static bool named_key(int k, key& out) {
    switch (k) {
      case GLFW_KEY_ESCAPE:
        out = key::escape;
        return true;
      case GLFW_KEY_ENTER:
      case GLFW_KEY_KP_ENTER:
        out = key::enter;
        return true;
      case GLFW_KEY_TAB:
        out = key::tab;
        return true;
      case GLFW_KEY_BACKSPACE:
        out = key::backspace;
        return true;
      case GLFW_KEY_DELETE:
        out = key::del;
        return true;
      case GLFW_KEY_INSERT:
        out = key::insert;
        return true;
      case GLFW_KEY_UP:
        out = key::up;
        return true;
      case GLFW_KEY_DOWN:
        out = key::down;
        return true;
      case GLFW_KEY_LEFT:
        out = key::left;
        return true;
      case GLFW_KEY_RIGHT:
        out = key::right;
        return true;
      case GLFW_KEY_HOME:
        out = key::home;
        return true;
      case GLFW_KEY_END:
        out = key::end;
        return true;
      case GLFW_KEY_PAGE_UP:
        out = key::page_up;
        return true;
      case GLFW_KEY_PAGE_DOWN:
        out = key::page_down;
        return true;
      default:
        break;
    }
    if (k >= GLFW_KEY_F1 && k <= GLFW_KEY_F12) {
      out = static_cast<key>(static_cast<int>(key::f1) + (k - GLFW_KEY_F1));
      return true;
    }
    return false;
  }

  static void install_callbacks(GLFWwindow* handle) {
    glfwSetCursorPosCallback(handle, [](GLFWwindow* w, double x, double y) {
      of(w).input.push_back(pointer_moved{to_pixels(w, x, y), of(w).mods});
    });
    glfwSetCursorEnterCallback(handle, [](GLFWwindow* w, int entered) {
      if (entered == GLFW_FALSE) of(w).input.push_back(pointer_left{});
    });
    glfwSetMouseButtonCallback(
        handle, [](GLFWwindow* w, int button, int action, int mods) {
          pointer_button b = pointer_button::left;
          if (button == GLFW_MOUSE_BUTTON_RIGHT)
            b = pointer_button::right;
          else if (button == GLFW_MOUSE_BUTTON_MIDDLE)
            b = pointer_button::middle;
          else if (button != GLFW_MOUSE_BUTTON_LEFT)
            return;
          of(w).mods = translate_mods(mods);
          if (action == GLFW_PRESS) {
            of(w).input.push_back(pointer_pressed{cursor(w), b, of(w).mods});
          } else if (action == GLFW_RELEASE) {
            of(w).input.push_back(pointer_released{cursor(w), b, of(w).mods});
          }
        });
    glfwSetScrollCallback(handle, [](GLFWwindow* w, double dx, double dy) {
      of(w).input.push_back(wheel_scrolled{cursor(w), {dx, dy}, of(w).mods});
    });
    glfwSetKeyCallback(handle, [](GLFWwindow* w, int k, int, int action, int mods) {
      of(w).mods = translate_mods(mods);
      if (action == GLFW_RELEASE) return;
      key named{};
      if (named_key(k, named)) {
        of(w).input.push_back(key_event{named, 0, of(w).mods});
        return;
      }
      // Text arrives through the character callback, which carries the
      // keyboard layout. Shortcuts with Ctrl, Alt, or Super produce no
      // character there, so they are reported from the key code.
      const bool shortcut = has(of(w).mods, modifiers::ctrl) ||
                            has(of(w).mods, modifiers::alt) ||
                            has(of(w).mods, modifiers::super);
      if (!shortcut) return;
      char32_t c = 0;
      if (k >= GLFW_KEY_A && k <= GLFW_KEY_Z)
        c = U'a' + static_cast<char32_t>(k - GLFW_KEY_A);
      if (k >= GLFW_KEY_0 && k <= GLFW_KEY_9)
        c = U'0' + static_cast<char32_t>(k - GLFW_KEY_0);
      if (c != 0) of(w).input.push_back(key_event{key::character, c, of(w).mods});
    });
    glfwSetCharCallback(handle, [](GLFWwindow* w, unsigned int codepoint) {
      // Shift is already reflected in the character.
      modifiers m = of(w).mods;
      m = static_cast<modifiers>(static_cast<std::uint8_t>(m) &
                                 ~static_cast<std::uint8_t>(modifiers::shift));
      of(w).input.push_back(
          key_event{key::character, static_cast<char32_t>(codepoint), m});
    });
    glfwSetFramebufferSizeCallback(handle, [](GLFWwindow* w, int width, int height) {
      float sx = 1.0F;
      float sy = 1.0F;
      glfwGetWindowContentScale(w, &sx, &sy);
      of(w).input.push_back(
          framebuffer_resized{{static_cast<std::uint32_t>(std::max(width, 0)),
                               static_cast<std::uint32_t>(std::max(height, 0))},
                              sx > 0.0F ? static_cast<double>(sx) : 1.0});
    });
    glfwSetWindowContentScaleCallback(handle, [](GLFWwindow* w, float sx, float) {
      int width = 0;
      int height = 0;
      glfwGetFramebufferSize(w, &width, &height);
      of(w).input.push_back(
          framebuffer_resized{{static_cast<std::uint32_t>(std::max(width, 0)),
                               static_cast<std::uint32_t>(std::max(height, 0))},
                              sx > 0.0F ? static_cast<double>(sx) : 1.0});
    });
    glfwSetWindowFocusCallback(handle, [](GLFWwindow* w, int focused) {
      of(w).input.push_back(window_focus{focused == GLFW_TRUE});
    });
    glfwSetWindowCloseCallback(handle, [](GLFWwindow* w) {
      // The viewport decides; GLFW's own flag is not used.
      glfwSetWindowShouldClose(w, GLFW_FALSE);
      of(w).input.push_back(window_close_requested{});
    });
    glfwSetWindowRefreshCallback(handle, [](GLFWwindow* w) {
      if (of(w).refresh) of(w).refresh();
    });
  }

  std::unique_ptr<state> state_;
};

}  // namespace avionix
