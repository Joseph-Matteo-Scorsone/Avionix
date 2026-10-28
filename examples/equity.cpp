// A chart between ordinary text blocks. Press q to quit.
#include <chrono>
#include <cstddef>
#include <print>
#include <utility>
#include <vector>
import avionix;

int main() {
  using namespace avionix;
  using namespace std::chrono;
  const auto start = sys_days{2026y / October / 1};
  std::vector<equity_point> points;
  const std::vector<double> values{0, 40, 15, -30, -65, -10, 25, 80, 55, 120};
  for (std::size_t i = 0; i < values.size(); ++i)
    points.push_back({start + days{static_cast<int>(i)}, values[i]});
  column body;
  body.add(constraint::fixed(3),
           text{"Equity performance\nCumulative change over time (UTC)."});
  auto& chart = body.add(constraint::fill(), block{"Equity"});
  chart.set_child(equity_chart{std::move(points)});
  body.add(constraint::fixed(3), text{"Notes: values can cross zero.\nThis chart uses "
                                      "terminal characters only. Press q to quit."});
  application app;
  app.on_event([&](const event& e) {
    if (const auto* k = as<key_event>(e); k && k->is(U'q')) {
      app.quit();
      return true;
    }
    return false;
  });
  if (auto result = app.run(body); !result) {
    std::println(stderr, "equity: {}", result.error().message());
    return 1;
  }
  return 0;
}
