// Test runner. Usage: avionix-tests [filter]
// Runs every registered test whose "suite.name" contains the filter.

import avionix_tests.check;

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : "";
  return avionix_tests::run_all(filter) == 0 ? 0 : 1;
}
