module;

#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

export module avionix.object.input_reader;

import avionix.entity.geometry;
import avionix.entity.event;
import avionix.entity.error;
import avionix.object.terminal;
import avionix.object.input_decoder;
import avionix.object.event_queue;

// Owns the input thread:
//
//   terminal_session::read ──bytes──► input_decoder ──events──► event_queue
//
// The thread reads through a reference to the session and pushes into a
// reference to the queue; both must outlive the reader. Destruction
// requests stop, wakes the blocked read, and joins (std::jthread).
//
// Only this thread calls terminal_session::read; only the UI thread writes.

export namespace avionix {

struct input_reader_options {
  // How long a lone ESC waits for the rest of a sequence before it is
  // reported as the Escape key.
  std::chrono::milliseconds escape_timeout{25};
  // Poll interval for size changes on platforms or terminals that do not
  // signal them. Also bounds how stale a missed resize can get.
  std::chrono::milliseconds size_poll_interval{250};
};

class input_reader {
 public:
  input_reader(terminal_session& session, event_queue& queue,
               avionix::size initial_size, input_reader_options options = {})
      : session_{session},
        queue_{queue},
        options_{options},
        last_size_{initial_size},
        thread_{[this](std::stop_token stop) { run(std::move(stop)); }} {}

  input_reader(const input_reader&) = delete;
  input_reader& operator=(const input_reader&) = delete;

  ~input_reader() {
    thread_.request_stop();
    session_.wake();
    // jthread joins on destruction.
  }

  // True when the thread stopped because reading the terminal failed. The
  // reader closes the queue in that case so the UI thread wakes up.
  [[nodiscard]] bool failed() const noexcept { return failed_.load(); }

 private:
  void run(std::stop_token stop) {
    std::stop_callback wake_on_stop{stop, [this] { session_.wake(); }};
    std::array<char, 4096> buffer{};
    std::vector<event> events;
    events.reserve(64);

    while (!stop.stop_requested()) {
      const auto timeout = decoder_.has_pending() ? options_.escape_timeout
                                                  : options_.size_poll_interval;
      auto result = session_.read(buffer, timeout);
      if (!result) {
        // Reading failed (for example the terminal hung up). Report it
        // as a user-visible condition: the application sees the queue
        // close and shuts down cleanly.
        failed_.store(true);
        queue_.close();
        return;
      }
      events.clear();
      if (result->bytes > 0) {
        decoder_.feed({buffer.data(), result->bytes}, events);
      } else if (result->timed_out && decoder_.has_pending()) {
        decoder_.flush(events);
      }
      if (result->resized || result->timed_out) {
        check_size(events);
      }
      for (event& e : events) {
        queue_.push(std::move(e));
      }
    }
  }

  void check_size(std::vector<event>& events) {
    const auto current = session_.query_size();
    if (current && *current != last_size_) {
      last_size_ = *current;
      events.push_back(event{resize_event{*current}});
    }
  }

  terminal_session& session_;
  event_queue& queue_;
  input_reader_options options_;
  input_decoder decoder_{};
  avionix::size last_size_;
  std::atomic<bool> failed_{false};
  // Declared last: the thread starts in the constructor and must see every
  // other member initialized, and it must be joined before they are
  // destroyed.
  std::jthread thread_;
};

}  // namespace avionix
