module;

#include <cstddef>
#include <exception>
#include <format>
#include <functional>
#include <initializer_list>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

export module avionix_tests.check;

// A small test harness. Tests register through `suite` objects at static
// initialization and run from tests/main.cpp. A failed check records the
// failure and the test keeps running, so one run reports every failure.

export namespace avionix_tests {

struct test_case {
  std::string_view name;
  std::function<void()> body;
};

struct registered_test {
  std::string suite;
  std::string_view name;
  std::function<void()> body;
};

// Function-local static so registration order across translation units
// does not matter.
std::vector<registered_test>& registry() {
  static std::vector<registered_test> tests;
  return tests;
}

struct run_state {
  std::size_t failures_in_test{};
  std::size_t total_checks{};
};

run_state& state() {
  static run_state s;
  return s;
}

struct suite {
  suite(std::string_view name, std::initializer_list<test_case> cases) {
    for (const auto& c : cases) {
      registry().push_back({std::string{name}, c.name, c.body});
    }
  }
};

void check(bool condition, std::string_view what = "check",
           std::source_location where = std::source_location::current()) {
  ++state().total_checks;
  if (!condition) {
    ++state().failures_in_test;
    std::println("    FAIL {}:{}: {}", where.file_name(), where.line(), what);
  }
}

template <typename A, typename B>
void check_equal(const A& actual, const B& expected,
                 std::source_location where = std::source_location::current()) {
  ++state().total_checks;
  if (!(actual == expected)) {
    ++state().failures_in_test;
    if constexpr (std::formattable<A, char> && std::formattable<B, char>) {
      std::println("    FAIL {}:{}: expected {} but got {}", where.file_name(),
                   where.line(), expected, actual);
    } else {
      std::println("    FAIL {}:{}: values differ", where.file_name(), where.line());
    }
  }
}

// Renders bytes with escapes visible, for readable failure output.
std::string visible(std::string_view bytes) {
  std::string out;
  for (const char c : bytes) {
    const auto u = static_cast<unsigned char>(c);
    if (u == 0x1b) {
      out += "\\e";
    } else if (u < 0x20 || u == 0x7f) {
      out += std::format("\\x{:02x}", u);
    } else {
      out += c;
    }
  }
  return out;
}

void check_bytes(std::string_view actual, std::string_view expected,
                 std::source_location where = std::source_location::current()) {
  ++state().total_checks;
  if (actual != expected) {
    ++state().failures_in_test;
    std::println("    FAIL {}:{}:\n      expected \"{}\"\n      got      \"{}\"",
                 where.file_name(), where.line(), visible(expected), visible(actual));
  }
}

// Runs every test whose "suite.name" contains `filter`. Returns the number
// of failed tests.
int run_all(std::string_view filter) {
  std::size_t passed = 0;
  std::size_t failed = 0;
  for (const auto& test : registry()) {
    const std::string full = std::format("{}.{}", test.suite, test.name);
    if (!filter.empty() && full.find(filter) == std::string::npos) continue;
    state().failures_in_test = 0;
    try {
      test.body();
    } catch (const std::exception& e) {
      ++state().failures_in_test;
      std::println("    FAIL uncaught exception: {}", e.what());
    }
    if (state().failures_in_test == 0) {
      ++passed;
    } else {
      ++failed;
      std::println("  FAILED {}", full);
    }
  }
  std::println("{} passed, {} failed, {} checks", passed, failed, state().total_checks);
  return static_cast<int>(failed);
}

}  // namespace avionix_tests
