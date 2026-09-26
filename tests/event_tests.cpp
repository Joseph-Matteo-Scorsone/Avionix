// Event queue and dispatch: ordering, cross-thread posting, coalescing.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <thread>
#include <variant>
#include <vector>

import avionix.entity.geometry;
import avionix.entity.event;
import avionix.object.event_queue;
import avionix.task.dispatch;
import avionix_tests.check;

using namespace avionix;
using avionix_tests::check;
using avionix_tests::check_equal;

namespace {

using namespace std::chrono_literals;

const avionix_tests::suite queue{
    "event_queue",
    {
        {"preserves_order",
         [] {
           event_queue q;
           q.push(event{user_event{1, 0}});
           q.push(event{user_event{2, 0}});
           std::vector<queue_entry> out;
           check_equal(q.try_drain(out), std::size_t{2});
           check_equal(std::get<user_event>(std::get<event>(out[0])).tag,
                       std::uint64_t{1});
           check_equal(std::get<user_event>(std::get<event>(out[1])).tag,
                       std::uint64_t{2});
         }},
        {"drain_times_out",
         [] {
           event_queue q;
           std::vector<queue_entry> out;
           const auto start = std::chrono::steady_clock::now();
           check_equal(q.drain(out, start + 10ms, {}), std::size_t{0});
           check(std::chrono::steady_clock::now() - start >= 9ms);
         }},
        {"producer_threads_wake_consumer",
         [] {
           event_queue q;
           constexpr int per_thread = 500;
           {
             std::vector<std::jthread> producers;
             for (int t = 0; t < 4; ++t) {
               producers.emplace_back([&q, t] {
                 for (int i = 0; i < per_thread; ++i) {
                   q.push(event{user_event{static_cast<std::uint64_t>(t),
                                           static_cast<std::uint64_t>(i)}});
                 }
               });
             }
           }
           std::vector<queue_entry> out;
           std::size_t total = 0;
           while (q.try_drain(out) > 0) total += out.size();
           check_equal(total, std::size_t{4 * per_thread});
         }},
        {"per_producer_order_is_kept",
         [] {
           event_queue q;
           std::jthread producer{[&q] {
             for (std::uint64_t i = 0; i < 1000; ++i) q.push(event{user_event{0, i}});
           }};
           producer.join();
           std::vector<queue_entry> out;
           q.try_drain(out);
           bool ordered = true;
           for (std::size_t i = 0; i < out.size(); ++i) {
             ordered =
                 ordered && std::get<user_event>(std::get<event>(out[i])).value == i;
           }
           check(ordered);
         }},
        {"close_wakes_waiter",
         [] {
           event_queue q;
           std::jthread closer{[&q] {
             std::this_thread::sleep_for(5ms);
             q.close();
           }};
           std::vector<queue_entry> out;
           const auto start = std::chrono::steady_clock::now();
           q.drain(out, start + 5s, {});
           check(std::chrono::steady_clock::now() - start < 4s);
           check(q.closed());
           q.push(event{user_event{}});
           check_equal(q.try_drain(out), std::size_t{0});
         }},
        {"stop_token_wakes_waiter",
         [] {
           event_queue q;
           std::stop_source source;
           std::jthread stopper{[&source] {
             std::this_thread::sleep_for(5ms);
             source.request_stop();
           }};
           std::vector<queue_entry> out;
           const auto start = std::chrono::steady_clock::now();
           q.drain(out, start + 5s, source.get_token());
           check(std::chrono::steady_clock::now() - start < 4s);
         }},
    }};

const avionix_tests::suite dispatching{
    "dispatch",
    {
        {"events_and_callbacks_in_order",
         [] {
           event_queue q;
           std::vector<int> order;
           q.push(event{user_event{1, 0}});
           q.post([&order] { order.push_back(2); });
           q.push(event{user_event{3, 0}});
           std::vector<queue_entry> entries;
           q.try_drain(entries);
           auto handler = [&order](const event& e) {
             order.push_back(static_cast<int>(std::get<user_event>(e).tag));
           };
           const auto result = dispatch(entries, handler);
           check(order == std::vector<int>{1, 2, 3});
           check_equal(result.events, std::size_t{2});
           check_equal(result.callbacks, std::size_t{1});
         }},
        {"resize_events_coalesce_to_latest",
         [] {
           std::vector<queue_entry> entries;
           entries.emplace_back(event{resize_event{{10, 10}}});
           entries.emplace_back(
               event{key_event{key::character, U'a', modifiers::none}});
           entries.emplace_back(event{resize_event{{20, 20}}});
           std::vector<size> sizes;
           std::size_t keys = 0;
           auto handler = [&](const event& e) {
             std::visit(overloaded{
                            [&](const resize_event& r) { sizes.push_back(r.extent); },
                            [&](const key_event&) { ++keys; },
                            [](const auto&) {},
                        },
                        e);
           };
           const auto result = dispatch(entries, handler);
           check_equal(sizes.size(), std::size_t{1});
           check(sizes[0] == size{20, 20});
           check_equal(keys, std::size_t{1});
           check_equal(result.coalesced, std::size_t{1});
           check(entries.empty());
         }},
        {"callback_from_worker_runs_on_consumer",
         [] {
           event_queue q;
           std::thread::id ran_on{};
           {
             std::jthread worker{
                 [&] { q.post([&] { ran_on = std::this_thread::get_id(); }); }};
           }
           std::vector<queue_entry> entries;
           q.try_drain(entries);
           auto handler = [](const event&) {};
           dispatch(entries, handler);
           check(ran_on == std::this_thread::get_id());
         }},
    }};

}  // namespace
