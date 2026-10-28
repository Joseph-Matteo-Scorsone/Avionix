#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
import avionix;
import avionix_tests.check;

namespace {
using namespace avionix;
using avionix_tests::check;
using namespace std::chrono;
const sys_seconds start = sys_days{2026y / October / 1};
render_buffer draw(equity_chart& chart, size extent = {50, 12}) {
  render_buffer buffer{extent};
  frame_state frame;
  render_context context{buffer, buffer.area(), frame};
  chart.render_in(context);
  check(frame.placements.empty(), "chart needs no pixel graphics");
  return buffer;
}
bool braille(const cell& c) {
  const auto text = c.text();
  return text.size() == 3 && static_cast<unsigned char>(text[0]) == 0xE2 &&
         (static_cast<unsigned char>(text[1]) & 0xFCU) == 0xA0;
}
const avionix_tests::suite charts{
    "equity_chart",
    {
        {"signed_values_and_utc_time_axis",
         [] {
           equity_chart chart{{{start, -10}, {start + hours{1}, 10}}};
           const auto b = draw(chart);
           check(b.find("10.00").has_value() && b.find("-10.00").has_value());
           check(b.find("00:00:00").has_value() && b.find("01:00:00").has_value());
           check(b.find("0").has_value());
           bool baseline = false, line = false;
           for (const auto& c : b.row(5)) baseline = baseline || c.text() == "-";
           for (const auto& c : b.cells()) {
             line = line || braille(c);
             check(c.appearance().foreground == colors::white &&
                   c.appearance().background == colors::black);
           }
           check(baseline && line, "zero baseline and Braille line are drawn");
         }},
        {"sorting_and_elapsed_time_spacing",
         [] {
           equity_chart chart{
               {{start + seconds{100}, 0}, {start, 0}, {start + seconds{10}, 10}}};
           check(chart.points().front().time == start);
           const auto b = draw(chart, {50, 12});
           // Y label width 5 + one gap + axis: plot begins at column 6.
           // The peak occurs 10% through the plot, not at the sample midpoint.
           bool near_peak = false, middle_peak = false;
           for (std::uint32_t x = 6; x < 16; ++x)
             near_peak = near_peak || braille(b.at({static_cast<int>(x), 0}));
           for (std::uint32_t x = 24; x < 30; ++x)
             middle_peak = middle_peak || braille(b.at({static_cast<int>(x), 0}));
           check(near_peak && !middle_peak, "timestamps determine horizontal spacing");
         }},
        {"empty_constant_and_nonfinite_data",
         [] {
           equity_chart chart;
           check(draw(chart).find("No data").has_value());
           chart.set_points({{start, 0}, {start, 0}});
           const auto flat = draw(chart);
           check(flat.find("1.00").has_value() && flat.find("-1.00").has_value());
           chart.set_points({{start, std::numeric_limits<double>::quiet_NaN()}});
           check(draw(chart).find("No data").has_value());
           chart.set_points(
               {{start, -1},
                {start + seconds{50}, std::numeric_limits<double>::infinity()},
                {start + seconds{100}, 1}});
           const auto gap = draw(chart);
           bool middle = false;
           for (std::uint32_t y = 0; y < 10; ++y)
             middle = middle || braille(gap.at({25, static_cast<int>(y)}));
           check(!middle,
                 "nonfinite data breaks the line rather than bridging the gap");
         }},
        {"positive_negative_and_extreme_ranges",
         [] {
           equity_chart chart{{{start, 5}, {start + days{2}, 20}}};
           check(draw(chart).find("0.00").has_value());
           check(draw(chart).find("2026-10-03").has_value());
           chart.set_points({{start, -5}, {start + days{2}, -20}});
           check(draw(chart).find("0.00").has_value());
           chart.set_points({{start, -std::numeric_limits<double>::max()},
                             {start + seconds{1}, std::numeric_limits<double>::max()}});
           check(draw(chart).find("1.8e+308").has_value());
         }},
        {"clips_and_handles_tiny_sizes",
         [] {
           equity_chart chart{{{start, -10}, {start + hours{1}, 10}}};
           render_buffer buffer{{60, 20}};
           buffer.clear(cell::from_ascii('#'));
           frame_state frame;
           render_context context{buffer, {{4, 3}, {40, 12}}, frame};
           chart.render_in(context);
           check(buffer.at({3, 3}).text() == "#" && buffer.at({44, 3}).text() == "#");
           check(buffer.at({4, 2}).text() == "#" && buffer.at({4, 15}).text() == "#");
           for (const auto extent : std::vector<size>{{0, 0}, {1, 1}, {8, 4}, {12, 4}})
             (void)draw(chart, extent);
         }},
        {"composes_between_text_blocks",
         [] {
           column body;
           body.add(constraint::fixed(1), text{"Before chart"});
           body.add(constraint::fixed(12),
                    equity_chart{{{start, -10}, {start + hours{1}, 10}}});
           body.add(constraint::fixed(1), text{"After chart"});
           render_buffer buffer{{50, 14}};
           frame_state frame;
           render_context context{buffer, buffer.area(), frame};
           body.render_in(context);
           check(buffer.find("Before chart")->y == 0 &&
                 buffer.find("After chart")->y == 13);
           check(buffer.find("-10.00").has_value());
         }},
    }};
}  // namespace
