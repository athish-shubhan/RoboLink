#include "robolink/stats.hpp"

#include <algorithm>
#include <cmath>

namespace robolink {

double percentile(std::vector<int64_t> s, double p) {
  if (s.empty()) return 0.0;
  std::sort(s.begin(), s.end());
  const double rank = std::ceil(p / 100.0 * static_cast<double>(s.size()));
  const std::size_t idx = rank < 1 ? 0 : static_cast<std::size_t>(rank) - 1;
  return static_cast<double>(s[std::min(idx, s.size() - 1)]);
}

LatencySummary summarize_ns(const std::vector<int64_t>& s) {
  LatencySummary r;
  r.count = s.size();
  if (s.empty()) return r;
  r.p50_us = percentile(s, 50) / 1e3;
  r.p95_us = percentile(s, 95) / 1e3;
  r.p99_us = percentile(s, 99) / 1e3;
  r.max_us = static_cast<double>(*std::max_element(s.begin(), s.end())) / 1e3;
  double acc = 0;
  for (std::size_t i = 1; i < s.size(); ++i) acc += std::llabs(s[i] - s[i - 1]);
  r.jitter_us = s.size() > 1 ? acc / static_cast<double>(s.size() - 1) / 1e3 : 0.0;
  return r;
}

void LatencyWindow::add(int64_t ns) {
  std::lock_guard<std::mutex> l(m_);
  w_.push_back(ns);
  if (w_.size() > cap_) w_.pop_front();
}

std::vector<int64_t> LatencyWindow::samples() const {
  std::lock_guard<std::mutex> l(m_);
  return {w_.begin(), w_.end()};
}

LatencySummary LatencyWindow::summary() const { return summarize_ns(samples()); }

}  // namespace robolink
