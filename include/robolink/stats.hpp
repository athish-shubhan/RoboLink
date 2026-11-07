#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace robolink {

struct LatencySummary {
  std::size_t count = 0;
  double p50_us = 0, p95_us = 0, p99_us = 0, max_us = 0;
  double jitter_us = 0;  // mean absolute difference of successive samples
};

// Nearest-rank percentile (p in [0,100]) of an unsorted sample set.
double percentile(std::vector<int64_t> samples, double p);
LatencySummary summarize_ns(const std::vector<int64_t>& samples_ns);

// Thread-safe sliding window of the most recent latency samples.
class LatencyWindow {
 public:
  explicit LatencyWindow(std::size_t capacity = 512) : cap_(capacity) {}
  void add(int64_t ns);
  LatencySummary summary() const;
  std::vector<int64_t> samples() const;

 private:
  std::size_t cap_;
  mutable std::mutex m_;
  std::deque<int64_t> w_;
};

}  // namespace robolink
