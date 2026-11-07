// ROS 2 C++ (rclcpp) endpoint with the same CLI/JSON contract as bench/bench_main.cpp.
#include <sys/resource.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "robolink_msgs/msg/packet.hpp"

using Packet = robolink_msgs::msg::Packet;
using namespace std::chrono_literals;

static int64_t now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}
static double cpu_s() {
  rusage r{};
  getrusage(RUSAGE_SELF, &r);
  return r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}

struct Args {
  std::string role = "client", mode = "latency";
  int count = 5000, warmup = 200, payload = 64;
  double rate = 1000, duration = 3;
};

static rclcpp::QoS qos_for(const Args& a) {
  rclcpp::QoS q(rclcpp::KeepLast(10));
  q.reliable();
  if (a.mode == "throughput") q = rclcpp::QoS(rclcpp::KeepAll()).reliable();
  return q;
}

static Packet make(uint64_t seq, uint32_t kind, int payload) {
  Packet p;
  p.seq = seq;
  p.stamp_ns = now_ns();
  p.kind = kind;
  p.linear = 1.f;
  p.payload.assign(static_cast<size_t>(payload), 'x');
  return p;
}

static int server(const Args& a) {
  auto node = rclcpp::Node::make_shared("rl_server");
  auto pub = node->create_publisher<Packet>("/rl/echo", qos_for(a));
  uint64_t n = 0;
  int64_t first = 0, last = 0;
  double cpu0 = 0;
  bool done = false;
  auto sub = node->create_subscription<Packet>("/rl/cmd", qos_for(a), [&](const Packet& m) {
    last = now_ns();
    if (m.kind == 2) {
      pub->publish(m);  // end marker is echoed in both modes so the client knows the stream drained
      done = true;
      return;
    }
    if (n++ == 0) { first = last; cpu0 = cpu_s(); }
    if (a.mode == "latency") pub->publish(m);
  });
  rclcpp::executors::SingleThreadedExecutor ex;
  ex.add_node(node);
  while (!done && rclcpp::ok()) ex.spin_once(100ms);
  std::printf("{\"role\":\"server\",\"received\":%llu,\"first_ns\":%lld,\"last_ns\":%lld,\"cpu_s\":%.6f}\n",
              (unsigned long long)n, (long long)first, (long long)last, cpu_s() - cpu0);
  return 0;
}

static int client(const Args& a) {
  auto node = rclcpp::Node::make_shared("rl_client");
  auto pub = node->create_publisher<Packet>("/rl/cmd", qos_for(a));
  int64_t got_seq = -1;
  auto sub = node->create_subscription<Packet>("/rl/echo", qos_for(a), [&](const Packet& m) { got_seq = (int64_t)m.seq; });
  rclcpp::executors::SingleThreadedExecutor ex;
  ex.add_node(node);
  // wait for discovery in both directions
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  while (std::chrono::steady_clock::now() < deadline &&
         (pub->get_subscription_count() == 0 || (a.mode == "latency" && sub->get_publisher_count() == 0)))
    ex.spin_once(50ms);
  std::this_thread::sleep_for(300ms);

  std::vector<int64_t> rtt;
  uint64_t sent = 0, recv = 0;
  const double cpu0 = cpu_s();
  const int64_t t0 = now_ns() + 50'000'000;
  if (a.mode == "latency") {
    const int total = a.warmup + a.count;
    const int64_t period = static_cast<int64_t>(1e9 / a.rate);
    for (int i = 0; i < total; ++i) {
      const int64_t sched = t0 + i * period;
      while (now_ns() < sched) ex.spin_once(std::chrono::nanoseconds(std::max<int64_t>(sched - now_ns(), 0)));
      Packet p = make(i, 0, a.payload);
      const int64_t t = now_ns();
      pub->publish(p);
      ++sent;
      const int64_t limit = t + 2'000'000'000LL;
      while (got_seq != i && now_ns() < limit) ex.spin_once(10ms);
      if (got_seq == i) { ++recv; if (i >= a.warmup) rtt.push_back(now_ns() - t); }
    }
    pub->publish(make(total, 2, 0));
    ex.spin_once(100ms);
  } else {
    const int64_t end = now_ns() + static_cast<int64_t>(a.duration * 1e9);
    while (now_ns() < end) { pub->publish(make(sent, 0, a.payload)); ++sent; }
    pub->publish(make(sent, 2, 0));
    // keep the publisher alive until the subscriber has drained and echoed the end marker
    const int64_t limit = now_ns() + 300'000'000'000LL;
    while (got_seq != static_cast<int64_t>(sent) && now_ns() < limit) ex.spin_once(50ms);
  }
  std::printf("{\"role\":\"client\",\"transport\":\"ros2\",\"lang\":\"cpp\",\"mode\":\"%s\",\"sent\":%llu,"
              "\"received\":%llu,\"duration_s\":%.6f,\"cpu_s\":%.6f,\"rtt_ns\":[",
              a.mode.c_str(), (unsigned long long)sent, (unsigned long long)recv, (now_ns() - t0) / 1e9,
              cpu_s() - cpu0);
  for (size_t i = 0; i < rtt.size(); ++i) std::printf(i ? ",%lld" : "%lld", (long long)rtt[i]);
  std::printf("]}\n");
  return 0;
}

int main(int argc, char** argv) {
  rclcpp::init(argc, argv);
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto v = [&] { return i + 1 < argc ? argv[++i] : (char*)"0"; };
    if (k == "--role") a.role = v();
    else if (k == "--mode") a.mode = v();
    else if (k == "--count") a.count = atoi(v());
    else if (k == "--warmup") a.warmup = atoi(v());
    else if (k == "--payload") a.payload = atoi(v());
    else if (k == "--rate") a.rate = atof(v());
    else if (k == "--duration") a.duration = atof(v());
    else if (k == "--port") (void)v();  // ROS 2 uses topics; accepted for CLI parity
  }
  int rc = a.role == "server" ? server(a) : client(a);
  rclcpp::shutdown();
  return rc;
}
