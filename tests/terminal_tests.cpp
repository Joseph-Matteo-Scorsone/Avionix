// Terminal lifecycle and capability detection.
//
// The session tests run in whatever environment the test binary gets. Under
// `zig build test` stdout is a pipe, so open() must fail cleanly with
// not_a_terminal. When run by hand in a terminal, open() succeeds and the
// test checks that restore() reports success and a second session is
// rejected while the first is open.

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <variant>

import avionix.entity.color;
import avionix.entity.error;
import avionix.object.terminal;
import avionix.object.input_decoder;
import avionix.entity.event;
import avionix.object.terminal_capabilities;
import avionix_tests.check;

using namespace avionix;
using avionix_tests::check;
using avionix_tests::check_equal;

namespace {

// Splits "\x1b[?Nh\x1b[?Ml..." into ("N", 'h'), ("M", 'l'), ...
std::vector<std::pair<std::string, char>> private_modes(std::string_view bytes) {
  std::vector<std::pair<std::string, char>> out;
  std::size_t at = 0;
  while ((at = bytes.find("\x1b[?", at)) != std::string_view::npos) {
    const std::size_t end = bytes.find_first_of("hl", at);
    out.emplace_back(std::string{bytes.substr(at + 3, end - at - 3)}, bytes[end]);
    at = end + 1;
  }
  return out;
}

// Every private mode enter_sequence changes must be inverted by
// leave_sequence, in reverse order, so nested state unwinds correctly.
bool leave_reverses_enter(const terminal_options& options) {
  const auto entered = private_modes(enter_sequence(options));
  const auto left = private_modes(leave_sequence(options));
  if (entered.size() != left.size()) return false;
  for (std::size_t i = 0; i < entered.size(); ++i) {
    const auto& [mode, final_byte] = entered[entered.size() - 1 - i];
    const char inverse = final_byte == 'h' ? 'l' : 'h';
    if (left[i].first != mode || left[i].second != inverse) return false;
  }
  return true;
}

const avionix_tests::suite lifecycle{
    "terminal",
    {
        {"leave_undoes_enter_in_reverse",
         [] {
           check(leave_reverses_enter({}));
           check(leave_reverses_enter({.mouse_motion = true}));
           check(leave_reverses_enter({.alternate_screen = false, .mouse = false}));
         }},
        {"leave_resets_style_and_shows_cursor",
         [] {
           const std::string leave = leave_sequence({});
           check(leave.find("\x1b[0m") != std::string::npos);
           check(leave.find("\x1b[?25h") != std::string::npos);
           check(leave.find("\x1b[?1049l") != std::string::npos);
         }},
        {"disabled_features_emit_nothing",
         [] {
           const terminal_options none{.alternate_screen = false,
                                       .hide_cursor = false,
                                       .mouse = false,
                                       .mouse_motion = false,
                                       .bracketed_paste = false,
                                       .focus_events = false};
#ifdef _WIN32
           check(enter_sequence(none) == "\x1b[?9001h");
           check(leave_sequence(none) == "\x1b[0m\x1b[?9001l");
#else
           check(enter_sequence(none).empty());
           check(leave_sequence(none) == "\x1b[0m");
#endif
         }},
#ifdef _WIN32
        {"conpty_multiline_paste", [] {
           if (!std::getenv("AVIONIX_CONPTY_PASTE_TEST")) return;
           struct ConsoleHandles {
             HANDLE saved_input = GetStdHandle(STD_INPUT_HANDLE);
             HANDLE saved_output = GetStdHandle(STD_OUTPUT_HANDLE);
             HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
             HANDLE output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
             ConsoleHandles() { SetStdHandle(STD_INPUT_HANDLE, input); SetStdHandle(STD_OUTPUT_HANDLE, output); }
             ~ConsoleHandles() {
               SetStdHandle(STD_INPUT_HANDLE, saved_input); SetStdHandle(STD_OUTPUT_HANDLE, saved_output);
               CloseHandle(input); CloseHandle(output);
             }
           } handles;

           auto session = terminal_session::open({});
           check(session.has_value());
           if (!session) return;
           check(session->write("AVIONIX_PASTE_READY").has_value());
           input_decoder decoder{input_decoder::default_max_paste_bytes, true};
           std::vector<event> events;
           std::array<char, 4096> bytes{};
           for (unsigned n = 0; n < 100 && events.size() < 3; ++n) {
             auto read = session->read(bytes, std::chrono::milliseconds{100});
             check(read.has_value());
             decoder.feed({bytes.data(), read->bytes}, events);
             std::erase_if(events, [](const auto& item) {
               return !std::holds_alternative<key_event>(item) && !std::holds_alternative<paste_event>(item);
             });
           }
           check(events.size() == 3);
           auto text = std::get<paste_event>(events.at(0)).text;
           std::string normalized;
           for (std::size_t i = 0; i < text.size(); ++i) {
             if (text[i] == '\r') {
               normalized += '\n';
               if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
             } else normalized += text[i];
           }
           check(normalized == "Aurora\n\nRifts\nCosmos");
           check(std::get<key_event>(events.at(1)).is(key::enter, modifiers::shift));
           check(std::get<key_event>(events.at(2)).is(key::enter));
         }},
        {"native_modified_enter", [] {
           if (!std::getenv("AVIONIX_NATIVE_INPUT_TEST")) return;
           auto session = terminal_session::open({});
           check(session.has_value());
           if (!session) return;
           const auto input = GetStdHandle(STD_INPUT_HANDLE);
           DWORD mode{};
           check(GetConsoleMode(input, &mode) != 0);
           check((mode & ENABLE_VIRTUAL_TERMINAL_INPUT) != 0);
           check(FlushConsoleInputBuffer(input) != 0);
           INPUT_RECORD records[2]{};
           for (auto& record : records) {
             record.EventType = KEY_EVENT;
             record.Event.KeyEvent.bKeyDown = TRUE;
             record.Event.KeyEvent.wRepeatCount = 1;
             record.Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
             record.Event.KeyEvent.uChar.UnicodeChar = L'\r';
           }
           records[0].Event.KeyEvent.dwControlKeyState = SHIFT_PRESSED;
           DWORD written{};
           check(WriteConsoleInputW(input, records, 2, &written) && written == 2);
           std::array<char, 128> bytes{};
           input_decoder decoder{input_decoder::default_max_paste_bytes, true};
           std::vector<event> events;
           for (unsigned n = 0; n < 32 && events.size() < 2; ++n) {
             auto read = session->read(bytes, std::chrono::milliseconds{100});
             check(read.has_value());
             decoder.feed({bytes.data(), read->bytes}, events);
           }
           check(events.size() == 2);
           check(std::get<key_event>(events.at(0)).is(key::enter, modifiers::shift));
           check(std::get<key_event>(events.at(1)).is(key::enter));
         }},
#endif
        {"session_open_is_clean",
         [] {
           auto session = terminal_session::open({});
           if (!session) {
             const auto kind = session.error().kind;
             check(kind == failure::not_a_terminal ||
                   kind == failure::unsupported_capability);
             check(!session.error().message().empty());
             // A failed open must not leave the process marked as having
             // a session: a second attempt fails the same way, not with
             // already_running.
             auto again = terminal_session::open({});
             check(!again && again.error().kind == kind);
             return;
           }
           auto second = terminal_session::open({});
           check(!second && second.error().kind == failure::already_running);
           check(session->restore().has_value());
           check(session->restore().has_value());  // idempotent
         }},
    }};

const avionix_tests::suite capabilities{
    "capabilities",
    {
        {"dumb_terminal",
         [] {
           const auto caps = detect_capabilities({.term = "dumb"});
           check(caps.colors == color_depth::monochrome);
           check(!caps.mouse && !caps.alternate_screen && !caps.synchronized_output);
         }},
        {"colorterm_truecolor",
         [] {
           const auto caps =
               detect_capabilities({.term = "xterm", .colorterm = "truecolor"});
           check(caps.colors == color_depth::truecolor);
         }},
        {"term_256color",
         [] {
           check(detect_capabilities({.term = "xterm-256color"}).colors ==
                 color_depth::indexed256);
         }},
        {"plain_xterm_is_16",
         [] {
           check(detect_capabilities({.term = "xterm"}).colors == color_depth::ansi16);
         }},
        {"windows_terminal",
         [] {
           check(detect_capabilities({.windows_terminal = true}).colors ==
                 color_depth::truecolor);
         }},
        {"windows_console_without_term",
         [] {
           check(detect_capabilities({.windows_console = true}).colors ==
                 color_depth::truecolor);
         }},
        {"no_color_wins",
         [] {
           const auto caps =
               detect_capabilities({.colorterm = "truecolor", .no_color = true});
           check(caps.colors == color_depth::monochrome);
           check(caps.mouse);
         }},
        {"linux_console",
         [] {
           const auto caps = detect_capabilities({.term = "linux"});
           check(!caps.mouse);
           check(!caps.focus_events);
         }},
    }};

}  // namespace
