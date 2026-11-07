// C++ fixed-rate controller for the Python-vs-C++ control-loop comparison.
// Each cycle: wake at an absolute deadline, compute a command from the last
// plant state, send it over ZeroMQ, wait for the plant's reply.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <zmq.hpp>

#include "bench_util.hpp"
#include "robolink/codec.hpp"

using namespace robolink;

int main(int argc, char** argv) {
  int port = 5611, cycles = 1000;
  double rate = 100;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i];
    if (k == "--port") port = atoi(argv[i + 1]);
    else if (k == "--cycles") cycles = atoi(argv[i + 1]);
    else if (k == "--rate") rate = atof(argv[i + 1]);
  }
  const int64_t period = static_cast<int64_t>(1e9 / rate), reply_timeout_ms = 100;
  zmq::context_t ctx(1);
  auto connect = [&] {
    auto s = std::make_unique<zmq::socket_t>(ctx, zmq::socket_type::req);
    s->set(zmq::sockopt::linger, 0);
    s->set(zmq::sockopt::rcvtimeo, (int)reply_timeout_ms);
    s->connect("tcp://127.0.0.1:" + std::to_string(port));
    return s;
  };
  auto sock = connect();
  std::vector<int64_t> rtt, late;
  uint64_t overruns = 0, timeouts = 0;
  double x = 0, target = 1.0;
  const int64_t t0 = now_ns() + 100'000'000;
  const double cpu0 = cpu_s();
  for (int i = 0; i < cycles; ++i) {
    const int64_t sched = t0 + static_cast<int64_t>(i) * period;
    int64_t d = sched - now_ns();
    if (d > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(d));
    const int64_t start = now_ns();
    const float u = static_cast<float>(2.0 * (target - x));  // P controller
    auto bytes = encode(make_packet(i, Kind::Command, u, 0.f));
    sock->send(zmq::buffer(bytes), zmq::send_flags::none);
    zmq::message_t m;
    if (sock->recv(m, zmq::recv_flags::none)) {
      if (auto r = decode(m.data(), m.size())) x = r->linear;
    } else {
      ++timeouts;
      sock = connect();
    }
    const int64_t end = now_ns();
    rtt.push_back(end - start);
    late.push_back(start - sched);
    if (end > sched + period) ++overruns;
  }
  auto eb = encode(make_packet(cycles, Kind::End, 0, 0));
  sock->send(zmq::buffer(eb), zmq::send_flags::none);
  zmq::message_t m;
  (void)sock->recv(m, zmq::recv_flags::none);
  std::printf("{\"role\":\"controller\",\"lang\":\"cpp\",\"rate_hz\":%.1f,\"cycles\":%d,\"overruns\":%llu,"
              "\"timeouts\":%llu,\"cpu_s\":%.6f,\"rtt_ns\":[", rate, cycles, (unsigned long long)overruns,
              (unsigned long long)timeouts, cpu_s() - cpu0);
  for (size_t i = 0; i < rtt.size(); ++i) std::printf(i ? ",%lld" : "%lld", (long long)rtt[i]);
  std::printf("],\"wake_late_ns\":[");
  for (size_t i = 0; i < late.size(); ++i) std::printf(i ? ",%lld" : "%lld", (long long)late[i]);
  std::printf("]}\n");
  return 0;
}
