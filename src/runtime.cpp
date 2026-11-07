#include "robolink/runtime.hpp"

#include <chrono>

namespace robolink {

Runtime::Runtime(RuntimeConfig cfg, Sink sink)
    : cfg_(cfg),
      sink_(std::move(sink)),
      cmd_q_(cfg.command_queue_capacity),
      sensor_q_(cfg.sensor_queue_capacity),
      wd_(cfg.watchdog) {}

Runtime::~Runtime() { stop(); }

void Runtime::start() {
  if (running_.exchange(true)) return;
  control_thread_ = std::thread([this] { control_loop(); });
  sensor_thread_ = std::thread([this] { sensor_loop(); });
}

void Runtime::stop() {
  if (!running_.exchange(false)) return;
  cmd_q_.close();
  sensor_q_.close();
  control_thread_.join();
  sensor_thread_.join();
}

void Runtime::control_loop() {
  const auto period = std::chrono::nanoseconds(cfg_.control_period_ns);
  int64_t next_tick = now_ns() + cfg_.control_period_ns;
  while (running_) {
    const int64_t wait = next_tick - now_ns();
    auto cmd = cmd_q_.pop_for(std::chrono::nanoseconds(wait > 0 ? wait : 0));
    const int64_t now = now_ns();
    Output out;
    bool emit = false;
    {
      std::lock_guard<std::mutex> l(m_);
      if (cmd) {
        last_rx_ns_ = now;
        cmd_lat_.add(cmd->age_ns(now));
        if (wd_.on_command(*cmd, now)) {
          ++accepted_;
          latest_ = *cmd;
          have_latest_ = true;
        }
      } else if (now < next_tick) {
        continue;  // woken by close(), not a real timeout
      }
      if (now >= next_tick) {
        if (now - last_rx_ns_ >= cfg_.command_timeout_ns) ++timeouts_;
        state_ = wd_.evaluate(now);
        out.tick_ns = now;
        out.state = state_;
        out.command_age_ns = wd_.command_age_ns(now);
        if (state_ == State::Run && have_latest_) {
          out.linear = latest_.linear;
          out.angular = latest_.angular;
        }
        ++ticks_;
        emit = true;
        do { next_tick += period.count(); } while (next_tick <= now);
      } else {
        state_ = wd_.state();
      }
    }
    if (emit && sink_) sink_(out);
  }
}

void Runtime::sensor_loop() {
  while (running_) {
    auto s = sensor_q_.pop_for(std::chrono::milliseconds(50));
    if (!s) continue;
    const int64_t lat = s->age_ns(now_ns());
    std::lock_guard<std::mutex> l(m_);
    sensor_lat_.add(lat);
    ++sensors_;
  }
}

Snapshot Runtime::snapshot() const {
  Snapshot s;
  std::lock_guard<std::mutex> l(m_);
  s.state = state_;
  s.command_age_ns = wd_.command_age_ns(now_ns());
  s.command_queue_depth = cmd_q_.size();
  s.sensor_queue_depth = sensor_q_.size();
  s.commands_accepted = accepted_;
  s.commands_rejected = wd_.rejected_stale() + wd_.rejected_order();
  s.commands_dropped = cmd_q_.dropped();
  s.sensors_processed = sensors_;
  s.sensors_dropped = sensor_q_.dropped();
  s.control_ticks = ticks_;
  s.command_timeouts = timeouts_;
  s.hold_events = wd_.transitions_to_hold();
  s.stop_events = wd_.transitions_to_stop();
  s.command_latency = cmd_lat_.summary();
  s.sensor_latency = sensor_lat_.summary();
  return s;
}

}  // namespace robolink
