module;

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

export module avionix.entity.error;

export namespace avionix {

enum class failure : std::uint8_t {
  not_a_terminal,
  unsupported_capability,
  platform_call_failed,
  malformed_sequence,
  io_failed,
  invalid_argument,
  already_running,
};

[[nodiscard]] constexpr std::string_view describe(failure value) noexcept {
  switch (value) {
    case failure::not_a_terminal:
      return "not a terminal";
    case failure::unsupported_capability:
      return "unsupported terminal capability";
    case failure::platform_call_failed:
      return "platform call failed";
    case failure::malformed_sequence:
      return "malformed escape sequence";
    case failure::io_failed:
      return "terminal I/O failed";
    case failure::invalid_argument:
      return "invalid argument";
    case failure::already_running:
      return "already running";
  }
  return "unknown failure";
}

// An expected failure, reported through std::expected<T, error>.
//
// `operation` names what Avionix was doing ("enable raw mode") and must be
// a string with static storage duration. `context` carries details such as
// the platform function that failed. `system_code` is errno on POSIX or
// GetLastError() on Windows, 0 when not applicable.
struct error {
  std::string_view operation;
  avionix::failure kind{};
  std::string context{};
  std::int64_t system_code{};

  [[nodiscard]] std::string message() const {
    std::string text = std::format("{}: {}", operation, describe(kind));
    if (!context.empty()) {
      text += std::format(" ({})", context);
    }
    if (system_code != 0) {
      text += std::format(" [system code {}]", system_code);
    }
    return text;
  }
};

}  // namespace avionix
