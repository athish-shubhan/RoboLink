#pragma once
#include <sys/resource.h>

namespace robolink {
// CPU seconds (user + system, all threads) consumed by this process so far.
inline double cpu_s() {
  rusage r{};
  getrusage(RUSAGE_SELF, &r);
  return r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}
}  // namespace robolink
