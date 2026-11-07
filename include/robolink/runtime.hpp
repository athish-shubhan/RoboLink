#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "robolink/bounded_queue.hpp"
#include "robolink/packet.hpp"
#include "robolink/stats.hpp"
#include "robolink/watchdog.hpp"

namespace robolink {

struct RuntimeConfig {
  std::size_t command_queue_capacity = 64;
  std::size_t sensor_queue_capacity = 256;
  int64_t control_period_ns = 5'000'000;  // actuator output tick (200 Hz)
  int64_t command_timeout_ns = 50'000'000;  // no command received for this long = one timeout per tick
  WatchdogConfig watchdog;
};

// What the actuator is told to do each control tick.
struct Output {
  int64_t tick_ns = 0;
  State state = State::Hold;
  float linear = 0.f, angular = 0.f;  // zeroed in HOLD and STOP
  int64_t command_age_ns = -1;
};

struct Snapshot {
  State state = State::Hold;
  int64_t command_age_ns = -1;
  std::size_t command_queue_depth = 0, sensor_queue_depth = 0;
  uint64_t commands_accepted = 0, commands_rejected = 0, commands_dropped = 0;
  uint64_t sensors_processed = 0, sensors_dropped = 0;
  uint64_t control_ticks = 0, command_timeouts = 0;
  uint64_t hold_events = 0, stop_events = 0;
  LatencySummary command_latency;  // creation -> control thread
  LatencySummary sensor_latency;   // creation -> sensor thread
};

// Two independent pipelines, each with its own bounded queue and thread:
//   command: submit_command() -> queue -> control thread -> watchdog -> sink
//   sensor : submit_sensor()  -> queue -> sensor thread  -> latency stats
// The control thread waits on the command queue with a timeout of one control
// period; every tick at which nothing has been received for command_timeout_ns
// is counted as a command timeout and the watchdog is still evaluated, so a silent link degrades to HOLD and then STOP.
class Runtime {
 public:
  using Sink = std::function<void(const Output&)>;
  explicit Runtime(RuntimeConfig cfg = {}, Sink sink = nullptr);
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  void start();
  void stop();

  void submit_command(Packet p) { cmd_q_.push_drop_oldest(std::move(p)); }
  void submit_sensor(Packet p) { sensor_q_.push_drop_oldest(std::move(p)); }

  Snapshot snapshot() const;

 private:
  void control_loop();
  void sensor_loop();

  RuntimeConfig cfg_;
  Sink sink_;
  BoundedQueue<Packet> cmd_q_, sensor_q_;
  std::thread control_thread_, sensor_thread_;
  std::atomic<bool> running_{false};

  mutable std::mutex m_;  // guards everything below
  Watchdog wd_;
  Packet latest_;
  bool have_latest_ = false;
  int64_t last_rx_ns_ = 0;
  uint64_t accepted_ = 0, ticks_ = 0, timeouts_ = 0, sensors_ = 0;
  State state_ = State::Hold;
  LatencyWindow cmd_lat_{512}, sensor_lat_{512};
};

}  // namespace robolink
