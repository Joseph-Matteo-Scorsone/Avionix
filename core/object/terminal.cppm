module;

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <csignal>
#endif

export module avionix.object.terminal;

import avionix.entity.geometry;
import avionix.entity.error;

// terminal_session owns every piece of terminal state Avionix changes:
//
//   open() ──► save modes ──► raw input / VT processing ──► enter_sequence
//   ~terminal_session() ──► leave_sequence ──► restore saved modes
//
// Restoration also runs from fatal-signal handlers (POSIX) and the console
// control handler (Windows), so a terminated process does not leave the
// terminal in raw mode on the alternate screen. That path uses only
// async-signal-safe calls on data prepared in advance.
//
// Only one session may be open per process: terminal modes are process
// global, and the signal handlers need a single restore target.

export namespace avionix {

struct terminal_options {
  bool alternate_screen{true};
  bool hide_cursor{true};
  bool mouse{true};
  // Report pointer motion without a pressed button (DEC 1003). Produces a
  // lot of input; off by default.
  bool mouse_motion{false};
  bool bracketed_paste{true};
  bool focus_events{true};
};

// VT sequence that puts the terminal into the requested state.
[[nodiscard]] std::string enter_sequence(const terminal_options& options) {
  std::string out;
  if (options.alternate_screen) out += "\x1b[?1049h";
  if (options.hide_cursor) out += "\x1b[?25l";
  if (options.bracketed_paste) out += "\x1b[?2004h";
  if (options.mouse) {
    out += "\x1b[?1000h\x1b[?1002h";
    if (options.mouse_motion) out += "\x1b[?1003h";
    out += "\x1b[?1006h";
  }
  if (options.focus_events) out += "\x1b[?1004h";
  return out;
}

// VT sequence that undoes enter_sequence(options), in reverse order, and
// resets SGR state so the shell prompt is not left styled.
[[nodiscard]] std::string leave_sequence(const terminal_options& options) {
  std::string out;
  if (options.focus_events) out += "\x1b[?1004l";
  if (options.mouse) {
    out += "\x1b[?1006l";
    if (options.mouse_motion) out += "\x1b[?1003l";
    out += "\x1b[?1002l\x1b[?1000l";
  }
  if (options.bracketed_paste) out += "\x1b[?2004l";
  out += "\x1b[0m";
  if (options.hide_cursor) out += "\x1b[?25h";
  if (options.alternate_screen) out += "\x1b[?1049l";
  return out;
}

struct read_result {
  std::size_t bytes{};  // bytes written into the caller's buffer
  bool resized{};       // the terminal reported a size change
  bool woken{};         // wake() interrupted the wait
  bool timed_out{};     // the timeout elapsed with no input
};

class terminal_session {
 public:
  // Opens the process's controlling terminal (stdin/stdout). Fails with
  // failure::not_a_terminal when either stream is redirected, and with
  // failure::already_running if another session is open.
  [[nodiscard]] static std::expected<terminal_session, error> open(
      const terminal_options& options = {});

  terminal_session(terminal_session&& other) noexcept = default;
  terminal_session& operator=(terminal_session&& other) noexcept;
  terminal_session(const terminal_session&) = delete;
  terminal_session& operator=(const terminal_session&) = delete;
  ~terminal_session();

  // Writes all bytes, retrying on partial writes and EINTR. Called from the
  // UI thread only.
  [[nodiscard]] std::expected<void, error> write(std::string_view bytes);

  // Waits up to `timeout` for input and reads what is available. Called
  // from a single reader thread. Returns early when wake() is called.
  [[nodiscard]] std::expected<read_result, error> read(
      std::span<char> buffer, std::chrono::milliseconds timeout);

  // Interrupts a blocked read(). Safe to call from any thread.
  void wake() noexcept;

  // Current size of the visible terminal area in cells.
  [[nodiscard]] std::expected<avionix::size, error> query_size() const;

  [[nodiscard]] const terminal_options& options() const noexcept;

  // Restores the terminal now instead of at destruction. Idempotent. The
  // session is unusable afterwards.
  [[nodiscard]] std::expected<void, error> restore();

 private:
  struct state;
  explicit terminal_session(std::unique_ptr<state> s) noexcept;
  std::unique_ptr<state> state_;
};

}  // namespace avionix

namespace avionix {

namespace {

// Storage shared with signal/console handlers. Written before handlers are
// installed and cleared after they are removed; handlers only read it.
struct emergency_restore {
  std::atomic<bool> armed{false};
  std::array<char, 256> leave_bytes{};
  std::size_t leave_length{};
#if defined(_WIN32)
  HANDLE output{};
  HANDLE input{};
  DWORD original_input_mode{};
  DWORD original_output_mode{};
  UINT original_input_cp{};
  UINT original_output_cp{};
#else
  termios original{};
  int wake_write_fd{-1};
#endif
};

constinit emergency_restore g_restore{};
constinit std::atomic<bool> g_session_open{false};

#if !defined(_WIN32)
constinit std::atomic<bool> g_resized{false};
#endif

error platform_error(std::string_view operation, std::string context) {
#if defined(_WIN32)
  const auto code = static_cast<std::int64_t>(GetLastError());
#else
  const auto code = static_cast<std::int64_t>(errno);
#endif
  return error{operation, failure::platform_call_failed, std::move(context), code};
}

}  // namespace

#if defined(_WIN32)

struct terminal_session::state {
  terminal_options options;
  HANDLE input{};
  HANDLE output{};
  HANDLE wake_event{};
  DWORD original_input_mode{};
  DWORD original_output_mode{};
  UINT original_input_cp{};
  UINT original_output_cp{};
  wchar_t pending_high_surrogate{};
  DWORD mouse_buttons{};
  bool active{};
};

namespace {

void write_all_raw(HANDLE output, const char* data, std::size_t length) noexcept {
  while (length > 0) {
    DWORD written = 0;
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(length, 1U << 20U));
    if (WriteFile(output, data, chunk, &written, nullptr) == 0 || written == 0) {
      return;
    }
    data += written;
    length -= written;
  }
}

BOOL WINAPI console_control_handler(DWORD type) {
  switch (type) {
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
    case CTRL_BREAK_EVENT:
      if (g_restore.armed.exchange(false)) {
        write_all_raw(g_restore.output, g_restore.leave_bytes.data(),
                      g_restore.leave_length);
        SetConsoleMode(g_restore.input, g_restore.original_input_mode);
        SetConsoleMode(g_restore.output, g_restore.original_output_mode);
        SetConsoleCP(g_restore.original_input_cp);
        SetConsoleOutputCP(g_restore.original_output_cp);
      }
      return FALSE;  // continue with default handling (process exit)
    default:
      return FALSE;
  }
}

void append_utf8(char*& out, std::uint32_t cp) noexcept {
  if (cp < 0x80) {
    *out++ = static_cast<char>(cp);
  } else if (cp < 0x800) {
    *out++ = static_cast<char>(0xC0U | (cp >> 6U));
    *out++ = static_cast<char>(0x80U | (cp & 0x3FU));
  } else if (cp < 0x10000) {
    *out++ = static_cast<char>(0xE0U | (cp >> 12U));
    *out++ = static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
    *out++ = static_cast<char>(0x80U | (cp & 0x3FU));
  } else {
    *out++ = static_cast<char>(0xF0U | (cp >> 18U));
    *out++ = static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
    *out++ = static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
    *out++ = static_cast<char>(0x80U | (cp & 0x3FU));
  }
}

bool append_bytes(char*& out, const char* limit, std::string_view bytes) noexcept {
  if (static_cast<std::size_t>(limit - out) < bytes.size()) return false;
  out = std::ranges::copy(bytes, out).out;
  return true;
}

bool append_decimal(char*& out, const char* limit, std::uint32_t value) noexcept {
  std::array<char, 10> digits{};
  const auto [end, error] =
      std::to_chars(digits.data(), digits.data() + digits.size(), value);
  return error == std::errc{} && append_bytes(out, limit, {digits.data(), end});
}

std::uint32_t vt_modifier(DWORD state) noexcept {
  std::uint32_t value = 1;
  if ((state & SHIFT_PRESSED) != 0) value += 1;
  if ((state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0) value += 2;
  if ((state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0) value += 4;
  return value;
}

bool append_csi_key(char*& out, const char* limit, std::string_view code, char final,
                    DWORD state) noexcept {
  if (!append_bytes(out, limit, "\x1b[")) return false;
  if (!append_bytes(out, limit, code)) return false;
  const std::uint32_t modifier = vt_modifier(state);
  if (modifier != 1 &&
      (!append_bytes(out, limit, ";") || !append_decimal(out, limit, modifier))) {
    return false;
  }
  return append_bytes(out, limit, {&final, 1});
}

bool append_special_key(char*& out, const char* limit,
                        const KEY_EVENT_RECORD& record) noexcept {
  std::string_view code;
  char final = '~';
  switch (record.wVirtualKeyCode) {
    case VK_UP:
      code = "1";
      final = 'A';
      break;
    case VK_DOWN:
      code = "1";
      final = 'B';
      break;
    case VK_RIGHT:
      code = "1";
      final = 'C';
      break;
    case VK_LEFT:
      code = "1";
      final = 'D';
      break;
    case VK_HOME:
      code = "1";
      final = 'H';
      break;
    case VK_END:
      code = "1";
      final = 'F';
      break;
    case VK_INSERT:
      code = "2";
      break;
    case VK_DELETE:
      code = "3";
      break;
    case VK_PRIOR:
      code = "5";
      break;
    case VK_NEXT:
      code = "6";
      break;
    case VK_F1:
      code = "11";
      break;
    case VK_F2:
      code = "12";
      break;
    case VK_F3:
      code = "13";
      break;
    case VK_F4:
      code = "14";
      break;
    case VK_F5:
      code = "15";
      break;
    case VK_F6:
      code = "17";
      break;
    case VK_F7:
      code = "18";
      break;
    case VK_F8:
      code = "19";
      break;
    case VK_F9:
      code = "20";
      break;
    case VK_F10:
      code = "21";
      break;
    case VK_F11:
      code = "23";
      break;
    case VK_F12:
      code = "24";
      break;
    default:
      return false;
  }
  return append_csi_key(out, limit, code, final, record.dwControlKeyState);
}

bool append_mouse(char*& out, const char* limit, const MOUSE_EVENT_RECORD& mouse,
                  DWORD& previous_buttons, position window_origin) noexcept {
  std::uint32_t button = 3;
  char final = 'M';
  if (mouse.dwEventFlags == MOUSE_WHEELED || mouse.dwEventFlags == MOUSE_HWHEELED) {
    const auto delta = static_cast<short>(HIWORD(mouse.dwButtonState));
    button = mouse.dwEventFlags == MOUSE_WHEELED ? (delta > 0 ? 64U : 65U)
                                                 : (delta < 0 ? 66U : 67U);
  } else if (mouse.dwEventFlags == MOUSE_MOVED) {
    button = (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0   ? 0U
             : (mouse.dwButtonState & FROM_LEFT_2ND_BUTTON_PRESSED) != 0 ? 1U
             : (mouse.dwButtonState & RIGHTMOST_BUTTON_PRESSED) != 0     ? 2U
                                                                         : 3U;
    button += 32U;
  } else if (mouse.dwEventFlags == 0) {
    const DWORD changed = mouse.dwButtonState ^ previous_buttons;
    const DWORD active = mouse.dwButtonState != 0 ? mouse.dwButtonState : changed;
    button = (active & FROM_LEFT_1ST_BUTTON_PRESSED) != 0   ? 0U
             : (active & FROM_LEFT_2ND_BUTTON_PRESSED) != 0 ? 1U
             : (active & RIGHTMOST_BUTTON_PRESSED) != 0     ? 2U
                                                            : 3U;
    if (mouse.dwButtonState == 0 || (changed & previous_buttons) != 0) final = 'm';
  } else {
    previous_buttons = mouse.dwButtonState;
    return false;
  }
  previous_buttons = mouse.dwButtonState;
  if ((mouse.dwControlKeyState & SHIFT_PRESSED) != 0) button += 4U;
  if ((mouse.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0)
    button += 8U;
  if ((mouse.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0)
    button += 16U;

  const auto x = static_cast<std::uint32_t>(
      std::max<LONG>(0, mouse.dwMousePosition.X - window_origin.x) + 1);
  const auto y = static_cast<std::uint32_t>(
      std::max<LONG>(0, mouse.dwMousePosition.Y - window_origin.y) + 1);
  return append_bytes(out, limit, "\x1b[<") && append_decimal(out, limit, button) &&
         append_bytes(out, limit, ";") && append_decimal(out, limit, x) &&
         append_bytes(out, limit, ";") && append_decimal(out, limit, y) &&
         append_bytes(out, limit, {&final, 1});
}

}  // namespace

std::expected<terminal_session, error> terminal_session::open(
    const terminal_options& options) {
  constexpr std::string_view operation = "open terminal session";
  if (g_session_open.exchange(true)) {
    return std::unexpected(error{operation, failure::already_running,
                                 "another terminal_session is open", 0});
  }
  auto s = std::make_unique<state>();
  s->options = options;
  s->input = GetStdHandle(STD_INPUT_HANDLE);
  s->output = GetStdHandle(STD_OUTPUT_HANDLE);

  // From here on, any failure must undo what was already changed. The
  // session object performs that undo in its destructor, so construct it
  // early and let it clean up on the error path.
  terminal_session session{std::move(s)};
  state& st = *session.state_;

  if (GetConsoleMode(st.input, &st.original_input_mode) == 0 ||
      GetConsoleMode(st.output, &st.original_output_mode) == 0) {
    auto e =
        platform_error(operation, "GetConsoleMode: stdin or stdout is not a console");
    e.kind = failure::not_a_terminal;
    return std::unexpected(std::move(e));
  }
  st.original_input_cp = GetConsoleCP();
  st.original_output_cp = GetConsoleOutputCP();
  st.active = true;

  const DWORD output_mode = ENABLE_PROCESSED_OUTPUT |
                            ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                            DISABLE_NEWLINE_AUTO_RETURN;
  if (SetConsoleMode(st.output, output_mode) == 0) {
    auto e =
        platform_error(operation, "SetConsoleMode: ENABLE_VIRTUAL_TERMINAL_PROCESSING");
    e.kind = failure::unsupported_capability;
    return std::unexpected(std::move(e));
  }
  // ENABLE_EXTENDED_FLAGS without ENABLE_QUICK_EDIT_MODE turns off console
  // text selection so mouse input reaches the application.
  DWORD input_mode = ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT;
  if (options.mouse) {
    input_mode |= ENABLE_EXTENDED_FLAGS;
  }
  if (SetConsoleMode(st.input, input_mode) == 0) {
    auto e = platform_error(operation, "SetConsoleMode: ENABLE_VIRTUAL_TERMINAL_INPUT");
    e.kind = failure::unsupported_capability;
    return std::unexpected(std::move(e));
  }
  if (SetConsoleOutputCP(CP_UTF8) == 0 || SetConsoleCP(CP_UTF8) == 0) {
    return std::unexpected(platform_error(operation, "SetConsoleCP: UTF-8"));
  }

  st.wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (st.wake_event == nullptr) {
    return std::unexpected(platform_error(operation, "CreateEventW"));
  }

  const std::string leave = leave_sequence(options);
  std::ranges::copy(leave, g_restore.leave_bytes.begin());
  g_restore.leave_length = leave.size();
  g_restore.input = st.input;
  g_restore.output = st.output;
  g_restore.original_input_mode = st.original_input_mode;
  g_restore.original_output_mode = st.original_output_mode;
  g_restore.original_input_cp = st.original_input_cp;
  g_restore.original_output_cp = st.original_output_cp;
  g_restore.armed.store(true);
  SetConsoleCtrlHandler(console_control_handler, TRUE);

  if (auto written = session.write(enter_sequence(options)); !written) {
    return std::unexpected(std::move(written.error()));
  }
  return session;
}

std::expected<void, error> terminal_session::restore() {
  if (!state_ || !state_->active) {
    return {};
  }
  state& st = *state_;
  st.active = false;
  std::expected<void, error> result{};

  SetConsoleCtrlHandler(console_control_handler, FALSE);
  if (g_restore.armed.exchange(false)) {
    const std::string leave = leave_sequence(st.options);
    write_all_raw(st.output, leave.data(), leave.size());
  }
  if (SetConsoleMode(st.input, st.original_input_mode) == 0 ||
      SetConsoleMode(st.output, st.original_output_mode) == 0) {
    result = std::unexpected(platform_error("restore terminal", "SetConsoleMode"));
  }
  SetConsoleCP(st.original_input_cp);
  SetConsoleOutputCP(st.original_output_cp);
  if (st.wake_event != nullptr) {
    CloseHandle(st.wake_event);
    st.wake_event = nullptr;
  }
  return result;
}

std::expected<void, error> terminal_session::write(std::string_view bytes) {
  const char* data = bytes.data();
  std::size_t length = bytes.size();
  while (length > 0) {
    DWORD written = 0;
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(length, 1U << 20U));
    if (WriteFile(state_->output, data, chunk, &written, nullptr) == 0) {
      auto e = platform_error("write terminal output", "WriteFile");
      e.kind = failure::io_failed;
      return std::unexpected(std::move(e));
    }
    data += written;
    length -= written;
  }
  return {};
}

std::expected<read_result, error> terminal_session::read(
    std::span<char> buffer, std::chrono::milliseconds timeout) {
  state& st = *state_;
  read_result result;
  const std::array<HANDLE, 2> handles{st.input, st.wake_event};
  const auto wait_ms =
      static_cast<DWORD>(std::clamp<std::int64_t>(timeout.count(), 0, INFINITE - 1));
  const DWORD signaled = WaitForMultipleObjects(2, handles.data(), FALSE, wait_ms);
  if (signaled == WAIT_TIMEOUT) {
    result.timed_out = true;
    return result;
  }
  if (signaled == WAIT_OBJECT_0 + 1) {
    result.woken = true;
    return result;
  }
  if (signaled != WAIT_OBJECT_0) {
    return std::unexpected(
        platform_error("read terminal input", "WaitForMultipleObjects"));
  }

  DWORD available = 0;
  if (GetNumberOfConsoleInputEvents(st.input, &available) == 0) {
    return std::unexpected(
        platform_error("read terminal input", "GetNumberOfConsoleInputEvents"));
  }
  // Reserve enough output room for the longest key or mouse sequence.
  std::array<INPUT_RECORD, 128> records{};
  const DWORD capacity_records = static_cast<DWORD>(std::min<std::size_t>(
      {records.size(), buffer.size() / 32, std::size_t{available}}));
  if (capacity_records == 0) {
    return result;
  }
  DWORD count = 0;
  if (ReadConsoleInputW(st.input, records.data(), capacity_records, &count) == 0) {
    return std::unexpected(platform_error("read terminal input", "ReadConsoleInputW"));
  }

  char* out = buffer.data();
  char* const limit = buffer.data() + buffer.size();
  CONSOLE_SCREEN_BUFFER_INFO screen{};
  const bool have_screen = GetConsoleScreenBufferInfo(st.output, &screen) != 0;
  const position window_origin =
      have_screen ? position{screen.srWindow.Left, screen.srWindow.Top} : position{};
  for (DWORD i = 0; i < count; ++i) {
    const INPUT_RECORD& record = records[i];
    if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
      result.resized = true;
      continue;
    }
    if (record.EventType == MOUSE_EVENT && st.options.mouse) {
      (void)append_mouse(out, limit, record.Event.MouseEvent, st.mouse_buttons,
                         window_origin);
      continue;
    }
    if (record.EventType != KEY_EVENT) {
      continue;
    }
    const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
    const wchar_t unit = key.uChar.UnicodeChar;
    if (key.bKeyDown == 0) {
      continue;
    }
    const WORD repeat = std::max<WORD>(key.wRepeatCount, 1);
    for (WORD r = 0; r < repeat && limit - out >= 32; ++r) {
      if (unit == 0) {
        (void)append_special_key(out, limit, key);
        continue;
      }
      if (key.wVirtualKeyCode == VK_TAB &&
          (key.dwControlKeyState & SHIFT_PRESSED) != 0) {
        (void)append_bytes(out, limit, "\x1b[Z");
        continue;
      }
      if ((key.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0 &&
          unit >= 0x20) {
        (void)append_bytes(out, limit, "\x1b");
      }
      if (unit >= 0xD800 && unit <= 0xDBFF) {
        st.pending_high_surrogate = unit;
        continue;
      }
      std::uint32_t cp = unit;
      if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (st.pending_high_surrogate == 0) {
          cp = 0xFFFD;
        } else {
          cp = 0x10000U +
               ((static_cast<std::uint32_t>(st.pending_high_surrogate) - 0xD800U)
                << 10U) +
               (static_cast<std::uint32_t>(unit) - 0xDC00U);
        }
      }
      st.pending_high_surrogate = 0;
      append_utf8(out, cp);
    }
  }
  result.bytes = static_cast<std::size_t>(out - buffer.data());
  return result;
}

void terminal_session::wake() noexcept {
  if (state_ && state_->wake_event != nullptr) {
    SetEvent(state_->wake_event);
  }
}

std::expected<avionix::size, error> terminal_session::query_size() const {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (GetConsoleScreenBufferInfo(state_->output, &info) == 0) {
    return std::unexpected(
        platform_error("query terminal size", "GetConsoleScreenBufferInfo"));
  }
  return avionix::size{
      static_cast<std::uint32_t>(
          std::max(0, info.srWindow.Right - info.srWindow.Left + 1)),
      static_cast<std::uint32_t>(
          std::max(0, info.srWindow.Bottom - info.srWindow.Top + 1)),
  };
}

#else  // POSIX

struct terminal_session::state {
  terminal_options options;
  int input{STDIN_FILENO};
  int output{STDOUT_FILENO};
  std::array<int, 2> wake_pipe{-1, -1};
  termios original{};
  struct sigaction previous_winch{};
  std::array<struct sigaction, 4> previous_fatal{};
  bool active{};
  bool handlers_installed{};
};

namespace {

constexpr std::array<int, 4> fatal_signals{SIGTERM, SIGHUP, SIGINT, SIGQUIT};

void write_all_raw(int fd, const char* data, std::size_t length) noexcept {
  while (length > 0) {
    const ssize_t n = ::write(fd, data, length);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    data += n;
    length -= static_cast<std::size_t>(n);
  }
}

extern "C" void avionix_on_winch(int) {
  const int saved = errno;
  g_resized.store(true);
  if (g_restore.wake_write_fd >= 0) {
    const char byte = 'r';
    (void)::write(g_restore.wake_write_fd, &byte, 1);
  }
  errno = saved;
}

// Restores the terminal, then re-raises with the default disposition so the
// process terminates with the original signal status.
extern "C" void avionix_on_fatal(int signal_number) {
  if (g_restore.armed.exchange(false)) {
    write_all_raw(STDOUT_FILENO, g_restore.leave_bytes.data(), g_restore.leave_length);
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_restore.original);
  }
  struct sigaction default_action{};
  default_action.sa_handler = SIG_DFL;
  sigemptyset(&default_action.sa_mask);
  sigaction(signal_number, &default_action, nullptr);
  raise(signal_number);
}

void set_nonblocking(int fd) noexcept {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  const int fd_flags = fcntl(fd, F_GETFD, 0);
  if (fd_flags >= 0) {
    fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
  }
}

}  // namespace

std::expected<terminal_session, error> terminal_session::open(
    const terminal_options& options) {
  constexpr std::string_view operation = "open terminal session";
  if (g_session_open.exchange(true)) {
    return std::unexpected(error{operation, failure::already_running,
                                 "another terminal_session is open", 0});
  }
  auto s = std::make_unique<state>();
  s->options = options;
  terminal_session session{std::move(s)};
  state& st = *session.state_;

  if (isatty(st.input) == 0 || isatty(st.output) == 0) {
    return std::unexpected(error{operation, failure::not_a_terminal,
                                 "stdin and stdout must both be terminals", 0});
  }
  if (tcgetattr(st.input, &st.original) != 0) {
    return std::unexpected(platform_error(operation, "tcgetattr"));
  }
  if (pipe(st.wake_pipe.data()) != 0) {
    return std::unexpected(platform_error(operation, "pipe"));
  }
  set_nonblocking(st.wake_pipe[0]);
  set_nonblocking(st.wake_pipe[1]);

  // Raw mode: no line buffering, echo, signal keys, flow control, or CR/NL
  // translation. OPOST stays on so a stray "\n" in output still returns
  // the carriage; the renderer positions the cursor explicitly anyway.
  termios raw = st.original;
  raw.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_cflag |= CS8;
  raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;

  const std::string leave = leave_sequence(options);
  std::ranges::copy(leave, g_restore.leave_bytes.begin());
  g_restore.leave_length = leave.size();
  g_restore.original = st.original;
  g_restore.wake_write_fd = st.wake_pipe[1];

  if (tcsetattr(st.input, TCSAFLUSH, &raw) != 0) {
    return std::unexpected(platform_error(operation, "tcsetattr: raw mode"));
  }
  st.active = true;
  g_restore.armed.store(true);

  struct sigaction winch{};
  winch.sa_handler = avionix_on_winch;
  sigemptyset(&winch.sa_mask);
  winch.sa_flags = SA_RESTART;
  sigaction(SIGWINCH, &winch, &st.previous_winch);

  struct sigaction fatal{};
  fatal.sa_handler = avionix_on_fatal;
  sigemptyset(&fatal.sa_mask);
  for (std::size_t i = 0; i < fatal_signals.size(); ++i) {
    sigaction(fatal_signals[i], &fatal, &st.previous_fatal[i]);
  }
  st.handlers_installed = true;

  if (auto written = session.write(enter_sequence(options)); !written) {
    return std::unexpected(std::move(written.error()));
  }
  return session;
}

std::expected<void, error> terminal_session::restore() {
  if (!state_) {
    return {};
  }
  state& st = *state_;
  std::expected<void, error> result{};
  if (st.active) {
    st.active = false;
    if (g_restore.armed.exchange(false)) {
      const std::string leave = leave_sequence(st.options);
      write_all_raw(st.output, leave.data(), leave.size());
    }
    if (tcsetattr(st.input, TCSAFLUSH, &st.original) != 0) {
      result = std::unexpected(platform_error("restore terminal", "tcsetattr"));
    }
  }
  if (st.handlers_installed) {
    st.handlers_installed = false;
    sigaction(SIGWINCH, &st.previous_winch, nullptr);
    for (std::size_t i = 0; i < fatal_signals.size(); ++i) {
      sigaction(fatal_signals[i], &st.previous_fatal[i], nullptr);
    }
  }
  g_restore.wake_write_fd = -1;
  for (int& fd : st.wake_pipe) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
  return result;
}

std::expected<void, error> terminal_session::write(std::string_view bytes) {
  const char* data = bytes.data();
  std::size_t length = bytes.size();
  while (length > 0) {
    const ssize_t n = ::write(state_->output, data, length);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN) {
        pollfd pfd{state_->output, POLLOUT, 0};
        (void)poll(&pfd, 1, 100);
        continue;
      }
      auto e = platform_error("write terminal output", "write");
      e.kind = failure::io_failed;
      return std::unexpected(std::move(e));
    }
    data += n;
    length -= static_cast<std::size_t>(n);
  }
  return {};
}

std::expected<read_result, error> terminal_session::read(
    std::span<char> buffer, std::chrono::milliseconds timeout) {
  state& st = *state_;
  read_result result;
  std::array<pollfd, 2> fds{{{st.input, POLLIN, 0}, {st.wake_pipe[0], POLLIN, 0}}};
  const int wait_ms =
      static_cast<int>(std::clamp<std::int64_t>(timeout.count(), 0, 1 << 30));
  int ready = 0;
  do {
    ready = poll(fds.data(), fds.size(), wait_ms);
  } while (ready < 0 && errno == EINTR && !g_resized.load());

  if (g_resized.exchange(false)) {
    result.resized = true;
  }
  if (ready < 0 && errno != EINTR) {
    return std::unexpected(platform_error("read terminal input", "poll"));
  }
  if (ready <= 0) {
    result.timed_out = !result.resized;
    return result;
  }
  if ((fds[1].revents & POLLIN) != 0) {
    std::array<char, 64> drain{};
    while (::read(st.wake_pipe[0], drain.data(), drain.size()) > 0) {
    }
    result.woken = !result.resized;
  }
  if ((fds[0].revents & (POLLIN | POLLHUP)) != 0) {
    const ssize_t n = ::read(st.input, buffer.data(), buffer.size());
    if (n < 0 && errno != EINTR && errno != EAGAIN) {
      auto e = platform_error("read terminal input", "read");
      e.kind = failure::io_failed;
      return std::unexpected(std::move(e));
    }
    if (n == 0 && (fds[0].revents & POLLHUP) != 0) {
      return std::unexpected(
          error{"read terminal input", failure::io_failed, "terminal hung up", 0});
    }
    result.bytes = n > 0 ? static_cast<std::size_t>(n) : 0;
  }
  return result;
}

void terminal_session::wake() noexcept {
  if (state_ && state_->wake_pipe[1] >= 0) {
    const char byte = 'w';
    (void)::write(state_->wake_pipe[1], &byte, 1);
  }
}

std::expected<avionix::size, error> terminal_session::query_size() const {
  winsize ws{};
  if (ioctl(state_->output, TIOCGWINSZ, &ws) != 0) {
    return std::unexpected(platform_error("query terminal size", "ioctl(TIOCGWINSZ)"));
  }
  return avionix::size{ws.ws_col, ws.ws_row};
}

#endif

terminal_session::terminal_session(std::unique_ptr<state> s) noexcept
    : state_{std::move(s)} {}

terminal_session& terminal_session::operator=(terminal_session&& other) noexcept {
  if (this != &other) {
    if (state_) {
      (void)restore();
      g_session_open.store(false);
    }
    state_ = std::move(other.state_);
  }
  return *this;
}

terminal_session::~terminal_session() {
  if (state_) {
    // Destruction cannot report failure; restore() has already done
    // everything that can be done, and there is no better place to
    // surface the error.
    (void)restore();
    g_session_open.store(false);
  }
}

const terminal_options& terminal_session::options() const noexcept {
  return state_->options;
}

}  // namespace avionix
