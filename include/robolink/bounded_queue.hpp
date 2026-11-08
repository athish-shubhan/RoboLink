#pragma once
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace robolink {

// Fixed-capacity, thread-safe MPMC queue. Producers never block: they either
// get `false` back (try_push) or evict the oldest entry (push_drop_oldest).
// Consumers block with a timeout so callers can run timeout handling.
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  bool try_push(T v) {
    {
      std::lock_guard<std::mutex> l(m_);
      if (closed_ || q_.size() >= capacity_) { ++rejected_; return false; }
      q_.push_back(std::move(v));
    }
    cv_.notify_one();
    return true;
  }

  // Always accepts (unless closed). Returns true if an old entry was evicted.
  bool push_drop_oldest(T v) {
    bool dropped = false;
    {
      std::lock_guard<std::mutex> l(m_);
      if (closed_) return false;
      if (q_.size() >= capacity_) { q_.pop_front(); ++dropped_; dropped = true; }
      q_.push_back(std::move(v));
    }
    cv_.notify_one();
    return dropped;
  }

  std::optional<T> try_pop() {
    std::lock_guard<std::mutex> l(m_);
    return pop_locked();
  }

  // Waits up to `timeout`; returns nullopt on timeout or when closed+drained.
  template <typename Rep, typename Period>
  std::optional<T> pop_for(std::chrono::duration<Rep, Period> timeout) {
    std::unique_lock<std::mutex> l(m_);
    cv_.wait_for(l, timeout, [&] { return !q_.empty() || closed_; });
    return pop_locked();
  }

  void close() {
    { std::lock_guard<std::mutex> l(m_); closed_ = true; }
    cv_.notify_all();
  }

  // Undo close(). Only call while no consumer is waiting on a closed queue.
  void reopen() { std::lock_guard<std::mutex> l(m_); closed_ = false; }

  std::size_t size() const { std::lock_guard<std::mutex> l(m_); return q_.size(); }
  std::size_t capacity() const { return capacity_; }
  std::size_t dropped() const { std::lock_guard<std::mutex> l(m_); return dropped_; }
  std::size_t rejected() const { std::lock_guard<std::mutex> l(m_); return rejected_; }
  bool closed() const { std::lock_guard<std::mutex> l(m_); return closed_; }

 private:
  std::optional<T> pop_locked() {
    if (q_.empty()) return std::nullopt;
    T v = std::move(q_.front());
    q_.pop_front();
    return v;
  }

  const std::size_t capacity_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<T> q_;
  bool closed_ = false;
  std::size_t dropped_ = 0;
  std::size_t rejected_ = 0;
};

}  // namespace robolink
