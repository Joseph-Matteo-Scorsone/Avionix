module;

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <stop_token>
#include <utility>
#include <variant>
#include <vector>

export module avionix.object.event_queue;

import avionix.entity.event;

// Multi-producer, single-consumer queue between producer threads (input
// reader, application workers) and the UI thread.
//
//   producers ──push/post──► event_queue ──drain──► UI thread
//
// Producers never touch UI state. They either push a typed event or post a
// callback that the UI thread runs; either way the mutation happens on the
// UI thread. drain() swaps the internal vector with the caller's, so the
// lock is held for O(1) and steady-state operation reuses capacity instead
// of allocating.

export namespace avionix {

// A callback posted from another thread, run on the UI thread.
using ui_callback = std::function<void()>;

using queue_entry = std::variant<event, ui_callback>;

class event_queue {
 public:
  event_queue() { entries_.reserve(64); }

  event_queue(const event_queue&) = delete;
  event_queue& operator=(const event_queue&) = delete;

  // Thread safe. Ignored after close().
  void push(event value) { enqueue(queue_entry{std::move(value)}); }

  // Thread safe. `callback` runs on the thread that calls drain().
  // Ignored after close().
  void post(ui_callback callback) { enqueue(queue_entry{std::move(callback)}); }

  // Waits until entries are available, `deadline` passes, `stop` is
  // requested, or the queue is closed. Replaces the contents of `out`
  // with every pending entry, in push order. Returns the number drained.
  std::size_t drain(std::vector<queue_entry>& out,
                    std::chrono::steady_clock::time_point deadline,
                    const std::stop_token& stop) {
    out.clear();
    std::unique_lock lock{mutex_};
    ready_.wait_until(lock, stop, deadline,
                      [&] { return !entries_.empty() || closed_; });
    entries_.swap(out);
    return out.size();
  }

  // Non-blocking drain.
  std::size_t try_drain(std::vector<queue_entry>& out) {
    out.clear();
    std::scoped_lock lock{mutex_};
    entries_.swap(out);
    return out.size();
  }

  // Wakes every waiter and rejects later pushes. Thread safe.
  void close() {
    {
      std::scoped_lock lock{mutex_};
      closed_ = true;
    }
    ready_.notify_all();
  }

  [[nodiscard]] bool closed() const {
    std::scoped_lock lock{mutex_};
    return closed_;
  }

 private:
  void enqueue(queue_entry entry) {
    {
      std::scoped_lock lock{mutex_};
      if (closed_) return;
      entries_.push_back(std::move(entry));
    }
    ready_.notify_one();
  }

  mutable std::mutex mutex_;
  std::condition_variable_any ready_;
  std::vector<queue_entry> entries_;
  bool closed_{};
};

}  // namespace avionix
