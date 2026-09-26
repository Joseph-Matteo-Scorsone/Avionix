// Terminal lifecycle and capability detection.
//
// The session tests run in whatever environment the test binary gets. Under
// `zig build test` stdout is a pipe, so open() must fail cleanly with
// not_a_terminal. When run by hand in a terminal, open() succeeds and the
// test checks that restore() reports success and a second session is
// rejected while the first is open.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import avionix.entity.color;
import avionix.entity.error;
import avionix.object.terminal;
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
           check(leave.ends_with("\x1b[?1049l"));
         }},
        {"disabled_features_emit_nothing",
         [] {
           const terminal_options none{.alternate_screen = false,
                                       .hide_cursor = false,
                                       .mouse = false,
                                       .mouse_motion = false,
                                       .bracketed_paste = false,
                                       .focus_events = false};
           check(enter_sequence(none).empty());
           check(leave_sequence(none) == "\x1b[0m");
         }},
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
