module;

#include <algorithm>
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
//
// A native_loop lets a UI-thread event source that has its own blocking
// wait, such as a graphical window, share the UI thread:
//
//   drain() ──no entries──► native_loop::wait_until(deadline)
//   producer push/post ──► native_loop::wake() ──► wait_until returns
//
// The loop pointer is read and changed under the queue mutex, and wake()
// runs under it too, so once set_native_loop(nullptr) returns no producer
// is inside or will enter wake().

export namespace avionix {

class native_loop {
 public:
  native_loop() = default;
  native_loop(const native_loop&) = delete;
  native_loop& operator=(const native_loop&) = delete;
  virtual ~native_loop() = default;

  // UI thread. Processes native events until one arrives, `deadline`
  // passes, or wake() is called. May push or post to the queue.
  virtual void wait_until(std::chrono::steady_clock::time_point deadline) = 0;
  // Any thread, under the queue mutex. Must not block or call back into
  // the queue.
  virtual void wake() noexcept = 0;
};

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

  // Thread safe. Waits until fewer than `max_pending` entries are queued,
  // the queue closes, or `stop` is requested. Used by bounded input
  // producers so an unread terminal stream cannot grow the queue without
  // limit. A zero limit behaves as one.
  [[nodiscard]] bool push_bounded(event value, std::size_t max_pending,
                                  const std::stop_token& stop) {
    std::unique_lock lock{mutex_};
    const std::size_t limit = std::max<std::size_t>(max_pending, 1);
    if (!space_.wait(lock, stop, [&] { return entries_.size() < limit || closed_; }) ||
        closed_) {
      return false;
    }
    entries_.push_back(queue_entry{std::move(value)});
    if (native_ != nullptr) native_->wake();
    lock.unlock();
    ready_.notify_one();
    return true;
  }

  // Thread safe. `callback` runs on the thread that calls drain().
  // Ignored after close().
  void post(ui_callback callback) { enqueue(queue_entry{std::move(callback)}); }

  // Waits until entries are available, `deadline` passes, `stop` is
  // requested, or the queue is closed. Replaces the contents of `out`
  // with every pending entry, in push order. Returns the number drained.
  //
  // With a native loop installed, the wait happens inside the loop instead
  // and `stop` is not observed; the loop returns at `deadline` at the
  // latest.
  std::size_t drain(std::vector<queue_entry>& out,
                    std::chrono::steady_clock::time_point deadline,
                    const std::stop_token& stop) {
    out.clear();
    std::unique_lock lock{mutex_};
    if (native_loop* loop = native_; loop != nullptr && entries_.empty() && !closed_) {
      lock.unlock();
      loop->wait_until(deadline);
      lock.lock();
    } else {
      ready_.wait_until(lock, stop, deadline,
                        [&] { return !entries_.empty() || closed_; });
    }
    entries_.swap(out);
    lock.unlock();
    space_.notify_all();
    return out.size();
  }

  // Non-blocking drain.
  std::size_t try_drain(std::vector<queue_entry>& out) {
    out.clear();
    {
      std::scoped_lock lock{mutex_};
      entries_.swap(out);
    }
    space_.notify_all();
    return out.size();
  }

  // Installs or removes (nullptr) the native loop that drain() waits in.
  // Thread safe. `loop` must stay alive until it is removed.
  void set_native_loop(native_loop* loop) {
    std::scoped_lock lock{mutex_};
    native_ = loop;
    if (native_ != nullptr && !entries_.empty()) native_->wake();
  }

  // Wakes every waiter and rejects later pushes. Thread safe.
  void close() {
    {
      std::scoped_lock lock{mutex_};
      closed_ = true;
      if (native_ != nullptr) native_->wake();
    }
    ready_.notify_all();
    space_.notify_all();
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
      if (native_ != nullptr) native_->wake();
    }
    ready_.notify_one();
  }

  mutable std::mutex mutex_;
  std::condition_variable_any ready_;
  std::condition_variable_any space_;
  std::vector<queue_entry> entries_;
  native_loop* native_{};
  bool closed_{};
};

}  // namespace avionix
