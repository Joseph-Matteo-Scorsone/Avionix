module;

#include <cstdlib>
#include <string>
#include <string_view>

export module avionix.object.terminal_capabilities;

export import avionix.entity.color;
export import avionix.entity.terminal_graphics;

// Capability detection is split in two so the rules are testable:
//
//   process environment ──read_environment──► terminal_environment
//   terminal_environment ──detect_capabilities──► terminal_capabilities
//
// Text capabilities are environment-based. Image support starts from the
// environment too; when it cannot decide, `query_graphics` asks the event
// loop to send primary device attributes (DA1) and a cell-size query
// (XTWINOPS 16). The input decoder recognizes both replies wherever they
// arrive in the stream, and the loop upgrades to Sixel when DA1 lists
// feature 4.

export namespace avionix {

struct terminal_environment {
  std::string term{};          // $TERM
  std::string colorterm{};     // $COLORTERM
  std::string term_program{};  // $TERM_PROGRAM
  bool no_color{};             // $NO_COLOR present (https://no-color.org)
  bool windows_terminal{};     // $WT_SESSION present
  bool windows_console{};      // running on the Windows console host
  bool kitty_window{};         // $KITTY_WINDOW_ID present
  bool multiplexer{};          // $TMUX or $STY present
  std::string graphics{};      // $AVIONIX_GRAPHICS: kitty, sixel, iterm2, or blocks
};

struct terminal_capabilities {
  color_depth colors{color_depth::ansi16};
  bool alternate_screen{true};
  bool mouse{true};
  bool bracketed_paste{true};
  bool focus_events{true};
  // DEC mode 2026. Terminals that do not recognize it ignore it, so it is
  // enabled everywhere except dumb terminals.
  bool synchronized_output{true};
  // Pixel images. `none` draws them as half-block characters.
  graphics_protocol graphics{graphics_protocol::none};
  // Ask the terminal (DA1) whether it supports Sixel.
  bool query_graphics{};

  friend bool operator==(const terminal_capabilities&,
                         const terminal_capabilities&) = default;
};

[[nodiscard]] terminal_environment read_environment() {
  const auto get = [](const char* name) -> std::string {
    const char* value =
        std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read once at startup
    return value != nullptr ? std::string{value} : std::string{};
  };
  terminal_environment env;
  env.term = get("TERM");
  env.colorterm = get("COLORTERM");
  env.term_program = get("TERM_PROGRAM");
  env.no_color = std::getenv("NO_COLOR") != nullptr;  // NOLINT(concurrency-mt-unsafe)
  env.windows_terminal =
      std::getenv("WT_SESSION") != nullptr;  // NOLINT(concurrency-mt-unsafe)
  env.kitty_window =
      std::getenv("KITTY_WINDOW_ID") != nullptr;       // NOLINT(concurrency-mt-unsafe)
  env.multiplexer = std::getenv("TMUX") != nullptr ||  // NOLINT(concurrency-mt-unsafe)
                    std::getenv("STY") != nullptr;     // NOLINT(concurrency-mt-unsafe)
  env.graphics = get("AVIONIX_GRAPHICS");
#if defined(_WIN32)
  env.windows_console = true;
#endif
  return env;
}

[[nodiscard]] terminal_capabilities detect_capabilities(
    const terminal_environment& env) noexcept {
  terminal_capabilities caps;
  const std::string_view term = env.term;
  const auto term_has = [&](std::string_view needle) {
    return term.find(needle) != std::string_view::npos;
  };

  if (term == "dumb") {
    return {.colors = color_depth::monochrome,
            .alternate_screen = false,
            .mouse = false,
            .bracketed_paste = false,
            .focus_events = false,
            .synchronized_output = false};
  }

  if (env.colorterm == "truecolor" || env.colorterm == "24bit" ||
      env.windows_terminal || env.term_program == "iTerm.app" ||
      env.term_program == "WezTerm" || env.term_program == "vscode" ||
      term_has("direct") || term_has("kitty") || term_has("alacritty") ||
      term_has("foot") || term_has("ghostty")) {
    caps.colors = color_depth::truecolor;
  } else if (term_has("256color")) {
    caps.colors = color_depth::indexed256;
  } else if (env.windows_console && term.empty()) {
    // The Windows 10+ console host renders 24-bit SGR once VT
    // processing is enabled, which terminal_session requires.
    caps.colors = color_depth::truecolor;
  } else {
    caps.colors = color_depth::ansi16;
  }

  if (env.no_color) {
    caps.colors = color_depth::monochrome;
  }
  if (term == "linux") {
    // The Linux virtual console has no mouse reporting without gpm and
    // does not implement focus events.
    caps.mouse = false;
    caps.focus_events = false;
  }
  caps.graphics = graphics_protocol::none;
  caps.query_graphics = false;
  if (env.graphics == "kitty") {
    caps.graphics = graphics_protocol::kitty;
  } else if (env.graphics == "sixel") {
    caps.graphics = graphics_protocol::sixel;
  } else if (env.graphics == "iterm2") {
    caps.graphics = graphics_protocol::iterm2;
  } else if (env.graphics == "blocks" || env.graphics == "none" || term == "linux" ||
             env.multiplexer || term.starts_with("screen") ||
             term.starts_with("tmux")) {
    // Multiplexers need per-protocol passthrough that Avionix does not
    // implement; blocks work everywhere.
  } else if (term_has("kitty") || env.kitty_window || term_has("ghostty") ||
             env.term_program == "ghostty" || env.term_program == "WezTerm") {
    caps.graphics = graphics_protocol::kitty;
  } else if (env.term_program == "iTerm.app") {
    caps.graphics = graphics_protocol::iterm2;
  } else {
    // Windows Terminal 1.22+, xterm, foot, mlterm, Konsole, and VTE may
    // support Sixel; DA1 decides. Terminal.app and the classic console
    // answer without feature 4 and keep blocks.
    caps.query_graphics = true;
  }
  return caps;
}

}  // namespace avionix
