// Simulated robot "plant" for the control-loop comparison. Replies to every
// command with a sensor packet; applies the same watchdog as the runtime to
// the command age observed on arrival. Prints a JSON summary on exit.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <zmq.hpp>

#include "bench_util.hpp"
#include "robolink/codec.hpp"
#include "robolink/stats.hpp"
#include "robolink/watchdog.hpp"

using namespace robolink;

int main(int argc, char** argv) {
  int port = 5611;
  WatchdogConfig wc{8'000'000, 50'000'000, 3};  // control-loop scale thresholds
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    if (k == "--port") port = atoi(argv[i + 1]);
    else if (k == "--hold-ms") wc.hold_ns = static_cast<int64_t>(atof(argv[i + 1]) * 1e6);
    else if (k == "--stop-ms") wc.stop_ns = static_cast<int64_t>(atof(argv[i + 1]) * 1e6);
  }
  zmq::context_t ctx(1);
  zmq::socket_t s(ctx, zmq::socket_type::rep);
  s.bind("tcp://127.0.0.1:" + std::to_string(port));
  Watchdog wd(wc);
  double x = 0, cpu0 = -1;
  uint64_t n = 0, executed = 0, not_run = 0;
  std::vector<int64_t> ages;
  for (;;) {
    zmq::message_t m;
    if (!s.recv(m, zmq::recv_flags::none)) break;
    const int64_t now = now_ns();
    auto p = decode(m.data(), m.size());
    if (!p) { s.send(zmq::buffer("", 0), zmq::send_flags::none); continue; }
    if (p->kind == Kind::End) { s.send(m, zmq::send_flags::none); break; }
    if (cpu0 < 0) cpu0 = cpu_s();
    ++n;
    ages.push_back(p->age_ns(now));
    wd.on_command(*p, now);
    if (wd.state() == State::Run) { x += p->linear * 0.01; ++executed; } else ++not_run;
    Packet r = make_packet(p->seq, Kind::Sensor, static_cast<float>(x), 0.f, p->stamp_ns);
    auto b = encode(r);
    s.send(zmq::buffer(b), zmq::send_flags::none);
  }
  auto sum = summarize_ns(ages);
  std::printf("{\"role\":\"plant\",\"commands\":%llu,\"executed\":%llu,\"held_or_stopped\":%llu,"
              "\"hold_events\":%llu,\"stop_events\":%llu,\"rejected_stale\":%llu,"
              "\"cmd_age_p50_us\":%.1f,\"cmd_age_p99_us\":%.1f,\"cmd_age_max_us\":%.1f,\"cpu_s\":%.6f}\n",
              (unsigned long long)n, (unsigned long long)executed, (unsigned long long)not_run,
              (unsigned long long)wd.transitions_to_hold(), (unsigned long long)wd.transitions_to_stop(),
              (unsigned long long)wd.rejected_stale(), sum.p50_us, sum.p99_us, sum.max_us, cpu_s() - cpu0);
  return 0;
}
