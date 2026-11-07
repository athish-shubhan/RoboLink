#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "robolink/runtime.hpp"

using namespace robolink;
using namespace std::chrono_literals;

namespace {
RuntimeConfig fast_cfg() {
  RuntimeConfig c;
  c.control_period_ns = 2'000'000;
  c.watchdog = {60'000'000, 250'000'000, 2};
  return c;
}

template <typename Pred>
bool wait_for(Pred p, std::chrono::milliseconds limit = 3000ms) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (p()) return true;
    std::this_thread::sleep_for(2ms);
  }
  return p();
}
}  // namespace

TEST(Runtime, SilentLinkTimesOutThenHoldsAndStops) {
  Runtime rt(fast_cfg());
  rt.start();
  for (int i = 0; i < 20; ++i) {
    rt.submit_command(make_packet(i, Kind::Command, 1.f, 0.f));
    std::this_thread::sleep_for(3ms);
  }
  ASSERT_TRUE(wait_for([&] { return rt.snapshot().state == State::Run; }));
  // stop sending: the control thread keeps ticking on queue timeouts
  ASSERT_TRUE(wait_for([&] { return rt.snapshot().state == State::Hold; }));
  ASSERT_TRUE(wait_for([&] { return rt.snapshot().state == State::Stop; }));
  auto s = rt.snapshot();
  EXPECT_GT(s.command_timeouts, 0u);
  EXPECT_EQ(s.hold_events, 1u);
  EXPECT_EQ(s.stop_events, 1u);
  rt.stop();
}

TEST(Runtime, OutputsZeroVelocityOutsideRunAndRecovers) {
  std::mutex m;
  std::vector<Output> outs;
  Runtime rt(fast_cfg(), [&](const Output& o) { std::lock_guard<std::mutex> l(m); outs.push_back(o); });
  rt.start();
  std::atomic<bool> feed{true};
  std::thread feeder([&] {
    uint64_t i = 0;
    while (feed) { rt.submit_command(make_packet(i++, Kind::Command, 0.7f, -0.2f)); std::this_thread::sleep_for(3ms); }
  });
  ASSERT_TRUE(wait_for([&] { return rt.snapshot().state == State::Run; }));
  feed = false;
  feeder.join();
  ASSERT_TRUE(wait_for([&] { return rt.snapshot().state == State::Stop; }));
  feed = true;
  std::thread feeder2([&] {
    uint64_t i = 1000;
    while (feed) { rt.submit_command(make_packet(i++, Kind::Command, 0.7f, -0.2f)); std::this_thread::sleep_for(3ms); }
  });
  ASSERT_TRUE(wait_for([&] { return rt.snapshot().state == State::Run; }));
  feed = false;
  feeder2.join();
  rt.stop();

  std::lock_guard<std::mutex> l(m);
  bool saw_run = false, saw_hold = false, saw_stop = false;
  for (auto& o : outs) {
    if (o.state == State::Run && o.linear == 0.7f) saw_run = true;
    if (o.state == State::Hold) { saw_hold = true; EXPECT_EQ(o.linear, 0.f); EXPECT_EQ(o.angular, 0.f); }
    if (o.state == State::Stop) { saw_stop = true; EXPECT_EQ(o.linear, 0.f); EXPECT_EQ(o.angular, 0.f); }
  }
  EXPECT_TRUE(saw_run && saw_hold && saw_stop);
}

TEST(Runtime, StaleCommandsAreRejectedNotExecuted) {
  Runtime rt(fast_cfg());
  rt.start();
  for (int i = 0; i < 5; ++i)
    rt.submit_command(make_packet(i, Kind::Command, 1.f, 0.f, now_ns() - 1'000'000'000));
  ASSERT_TRUE(wait_for([&] { return rt.snapshot().commands_rejected == 5; }));
  EXPECT_EQ(rt.snapshot().commands_accepted, 0u);
  EXPECT_NE(rt.snapshot().state, State::Run);
  rt.stop();
}

TEST(Runtime, CommandQueueOverflowDropsOldestAndCounts) {
  RuntimeConfig c = fast_cfg();
  c.command_queue_capacity = 4;
  Runtime rt(c);  // not started: nothing consumes
  for (int i = 0; i < 10; ++i) rt.submit_command(make_packet(i, Kind::Command, 0, 0));
  auto s = rt.snapshot();
  EXPECT_EQ(s.command_queue_depth, 4u);
  EXPECT_EQ(s.commands_dropped, 6u);
}

TEST(Runtime, SensorPipelineRunsIndependentlyOfCommands) {
  Runtime rt(fast_cfg());
  rt.start();
  for (int i = 0; i < 50; ++i) {
    rt.submit_sensor(make_packet(i, Kind::Sensor, 0, 0));
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_TRUE(wait_for([&] { return rt.snapshot().sensors_processed == 50; }));
  auto s = rt.snapshot();
  EXPECT_EQ(s.commands_accepted, 0u);
  EXPECT_EQ(s.sensor_latency.count, 50u);
  EXPECT_GT(s.sensor_latency.p50_us, 0.0);
  rt.stop();
}

TEST(Runtime, StopIsIdempotentAndJoinsThreads) {
  Runtime rt(fast_cfg());
  rt.start();
  rt.start();
  rt.stop();
  rt.stop();
  SUCCEED();
}
