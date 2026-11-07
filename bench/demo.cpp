// Terminal dashboard demo: a ZeroMQ command stream runs, stalls, arrives late,
// then recovers, while the watchdog goes RUN -> HOLD -> STOP -> RUN.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <zmq.hpp>

#include "robolink/codec.hpp"
#include "robolink/runtime.hpp"

using namespace robolink;
using namespace std::chrono_literals;

namespace {
const char *RST = "\033[0m", *GRN = "\033[32m", *YEL = "\033[33m", *RED = "\033[31m", *DIM = "\033[2m", *BLD = "\033[1m";
bool g_tty = false;

struct Phase { double until_s; const char* name; };
const Phase kPhases[] = {
    {4.0, "streaming commands @100 Hz"},
    {8.0, "LINK STALL: commands stop"},
    {11.0, "link restored: STOP latch releases after 3 fresh commands"},
    {11.6, "BRIEF STALL: short enough for HOLD only"},
    {15.0, "link restored: commands resume"},
};
constexpr double kEnd = 15.0;

const Phase& phase_at(double t) {
  for (auto& p : kPhases) if (t < p.until_s) return p;
  return kPhases[4];
}

const char* color(State s) { return s == State::Run ? GRN : s == State::Hold ? YEL : RED; }

std::string bar(double frac, int width, double hold_frac) {
  if (frac > 1) frac = 1;
  if (frac < 0) frac = 0;
  int fill = static_cast<int>(frac * width), hold_at = static_cast<int>(hold_frac * width);
  std::string s = "[";
  for (int i = 0; i < width; ++i) {
    if (i < fill) s += (i >= hold_at ? (frac >= 1 ? RED : YEL) : GRN), s += "#", s += RST;
    else s += (i == hold_at ? "|" : ".");
  }
  return s + "]";
}
}  // namespace

int main() {
  const double speed = 1.0;
  g_tty = isatty(STDOUT_FILENO);
  RuntimeConfig cfg;
  cfg.control_period_ns = 10'000'000;
  cfg.watchdog = {200'000'000, 1'000'000'000, 3};
  std::mutex om;
  Output last;
  Runtime rt(cfg, [&](const Output& o) { std::lock_guard<std::mutex> l(om); last = o; });
  rt.start();

  zmq::context_t ctx(1);
  std::atomic<bool> run{true};
  std::atomic<int> send_mode{0};  // 0 normal, 1 stalled, 2 delayed
  // Receivers: ZeroMQ PULL -> runtime queues.
  auto receiver = [&](int port, bool command) {
    zmq::socket_t s(ctx, zmq::socket_type::pull);
    s.set(zmq::sockopt::rcvtimeo, 50);
    s.bind("tcp://127.0.0.1:" + std::to_string(port));
    while (run) {
      zmq::message_t m;
      if (!s.recv(m, zmq::recv_flags::none)) continue;
      if (auto p = decode(m.data(), m.size())) command ? rt.submit_command(*p) : rt.submit_sensor(*p);
    }
  };
  std::thread rx_cmd(receiver, 5701, true), rx_sens(receiver, 5702, false);
  std::this_thread::sleep_for(100ms);

  auto sender = [&](int port, bool command) {
    zmq::socket_t s(ctx, zmq::socket_type::push);
    s.set(zmq::sockopt::linger, 0);
    s.connect("tcp://127.0.0.1:" + std::to_string(port));
    uint64_t seq = 0;
    const auto period = std::chrono::microseconds(static_cast<int64_t>((command ? 10000 : 5000) / speed));
    auto next = std::chrono::steady_clock::now();
    while (run) {
      next += period;
      std::this_thread::sleep_until(next);
      int64_t stamp = now_ns();
      if (command) {
        if (send_mode == 1) continue;
        if (send_mode == 2) stamp -= 300'000'000;
      }
      Packet p = make_packet(seq++, command ? Kind::Command : Kind::Sensor, command ? 0.5f : 0.f,
                             command ? 0.1f : 0.f, stamp);
      auto b = encode(p);
      s.send(zmq::buffer(b), zmq::send_flags::dontwait);
    }
  };
  std::thread tx_cmd(sender, 5701, true), tx_sens(sender, 5702, false);

  std::string timeline;
  std::vector<double> spark;
  const auto t0 = std::chrono::steady_clock::now();
  if (g_tty) std::printf("\033[2J\033[?25l");
  double last_line = -1;
  uint64_t state_hist[3] = {0, 0, 0};
  for (;;) {
    std::this_thread::sleep_for(100ms / speed);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * speed;
    if (t >= kEnd) break;
    const Phase& ph = phase_at(t);
    send_mode = ((t >= 4 && t < 8) || (t >= 11 && t < 11.6)) ? 1 : 0;
    Snapshot s = rt.snapshot();
    ++state_hist[static_cast<int>(s.state)];
    timeline += std::string(color(s.state)) + (s.state == State::Run ? "=" : s.state == State::Hold ? "~" : "X") + RST;
    spark.push_back(s.command_latency.p50_us);
    Output o;
    { std::lock_guard<std::mutex> l(om); o = last; }
    const double age_ms = s.command_age_ns < 0 ? 0 : s.command_age_ns / 1e6;
    if (!g_tty) {
      if (t - last_line < 0.5) continue;
      last_line = t;
      std::printf("t=%5.1fs state=%-4s cmd_age=%7.1fms cmd_lat_p50=%6.0fus p99=%6.0fus timeouts=%llu hold=%llu stop=%llu | %s\n",
                  t, to_string(s.state), age_ms, s.command_latency.p50_us, s.command_latency.p99_us,
                  (unsigned long long)s.command_timeouts, (unsigned long long)s.hold_events,
                  (unsigned long long)s.stop_events, ph.name);
      continue;
    }
    std::printf("\033[H");
    std::printf("%sRoboLink demo%s  safe-stop watchdog  (hold >= %.0f ms, stop >= %.0f ms)\033[K\n\033[K\n",
                BLD, RST, cfg.watchdog.hold_ns / 1e6, cfg.watchdog.stop_ns / 1e6);
    std::printf("t = %5.1f s / %.0f s    phase: %s%s%s\033[K\n\033[K\n", t, kEnd, BLD, ph.name, RST);
    std::printf("  state        %s%s %-4s %s\033[K\n", color(s.state), BLD, to_string(s.state), RST);
    std::printf("  command age  %s %7.1f ms\033[K\n", bar(age_ms * 1e6 / cfg.watchdog.stop_ns, 40,
                                                       double(cfg.watchdog.hold_ns) / cfg.watchdog.stop_ns).c_str(), age_ms);
    std::printf("  actuator     linear=%+.2f angular=%+.2f   %s\033[K\n", o.linear, o.angular,
                s.state == State::Run ? "(executing commands)" : "(output forced to zero)");
    std::printf("\033[K\n  %scommand latency (ZeroMQ -> control thread, last 512)%s\033[K\n", DIM, RST);
    std::printf("    p50 %7.0f us   p95 %7.0f us   p99 %7.0f us   jitter %6.0f us\033[K\n", s.command_latency.p50_us,
                s.command_latency.p95_us, s.command_latency.p99_us, s.command_latency.jitter_us);
    std::printf("  %ssensor latency (independent pipeline)%s\033[K\n", DIM, RST);
    std::printf("    p50 %7.0f us   p95 %7.0f us   p99 %7.0f us   processed %llu\033[K\n", s.sensor_latency.p50_us,
                s.sensor_latency.p95_us, s.sensor_latency.p99_us, (unsigned long long)s.sensors_processed);
    std::printf("\033[K\n  queues       cmd %zu/%zu  sensor %zu/%zu    drops cmd %llu sensor %llu\033[K\n",
                s.command_queue_depth, cfg.command_queue_capacity, s.sensor_queue_depth, cfg.sensor_queue_capacity,
                (unsigned long long)s.commands_dropped, (unsigned long long)s.sensors_dropped);
    std::printf("  commands     accepted %llu  rejected %llu  queue timeouts %llu\033[K\n",
                (unsigned long long)s.commands_accepted, (unsigned long long)s.commands_rejected,
                (unsigned long long)s.command_timeouts);
    std::printf("  watchdog     HOLD events %llu   STOP events %llu\033[K\n", (unsigned long long)s.hold_events,
                (unsigned long long)s.stop_events);
    std::printf("\033[K\n  timeline (100 ms/char)  %s= run  %s~ hold  %sX stop%s\033[K\n  %s\033[K\n", GRN, YEL, RED, RST,
                timeline.c_str());
    std::fflush(stdout);
  }
  if (g_tty) std::printf("\033[?25h");
  std::printf("\nsummary: samples RUN=%llu HOLD=%llu STOP=%llu (x100 ms); HOLD events=%llu STOP events=%llu\n",
              (unsigned long long)state_hist[0], (unsigned long long)state_hist[1],
              (unsigned long long)state_hist[2], (unsigned long long)rt.snapshot().hold_events,
              (unsigned long long)rt.snapshot().stop_events);
  run = false;
  tx_cmd.join(); tx_sens.join(); rx_cmd.join(); rx_sens.join();
  rt.stop();
  return 0;
}
