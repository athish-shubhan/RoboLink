#pragma once
#include <cstdint>
#include <ctime>
#include <string>

namespace robolink {

// Monotonic nanoseconds. CLOCK_MONOTONIC is used explicitly (not
// std::chrono::steady_clock) so C++ and Python processes on one host share an
// identical time base.
inline int64_t now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

enum class Kind : uint32_t { Command = 0, Sensor = 1, End = 2 };

struct Packet {
  uint64_t seq = 0;
  int64_t stamp_ns = 0;  // time the sender created the packet
  Kind kind = Kind::Command;
  float linear = 0.f;
  float angular = 0.f;
  std::string payload;

  // Age of the packet relative to `now` (negative if stamped in the future).
  int64_t age_ns(int64_t now) const { return now - stamp_ns; }
};

inline Packet make_packet(uint64_t seq, Kind kind, float linear, float angular,
                          int64_t stamp = now_ns()) {
  Packet p;
  p.seq = seq;
  p.stamp_ns = stamp;
  p.kind = kind;
  p.linear = linear;
  p.angular = angular;
  return p;
}

}  // namespace robolink
