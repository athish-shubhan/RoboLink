#include <gtest/gtest.h>

#include "robolink/watchdog.hpp"

using namespace robolink;

namespace {
constexpr int64_t ms = 1'000'000;
WatchdogConfig cfg() { return {200 * ms, 1000 * ms, 3}; }
Packet cmd(uint64_t seq, int64_t stamp) { return make_packet(seq, Kind::Command, 1.f, 0.f, stamp); }
}  // namespace

TEST(Watchdog, StartsInHoldUntilFirstCommand) {
  Watchdog w(cfg());
  EXPECT_EQ(w.state(), State::Hold);
  EXPECT_EQ(w.evaluate(10'000 * ms), State::Hold);  // no command, never STOP
  EXPECT_EQ(w.command_age_ns(0), -1);
}

TEST(Watchdog, FreshCommandMovesToRun) {
  Watchdog w(cfg());
  EXPECT_TRUE(w.on_command(cmd(1, 0), 5 * ms));
  EXPECT_EQ(w.state(), State::Run);
}

TEST(Watchdog, RunToHoldToStopAtThresholds) {
  Watchdog w(cfg());
  w.on_command(cmd(1, 0), 0);
  EXPECT_EQ(w.evaluate(199 * ms), State::Run);
  EXPECT_EQ(w.evaluate(200 * ms), State::Hold);  // age == hold threshold
  EXPECT_EQ(w.evaluate(999 * ms), State::Hold);
  EXPECT_EQ(w.evaluate(1000 * ms), State::Stop);  // age == stop threshold
  EXPECT_EQ(w.transitions_to_hold(), 1u);
  EXPECT_EQ(w.transitions_to_stop(), 1u);
}

TEST(Watchdog, HoldRecoversImmediatelyOnFreshCommand) {
  Watchdog w(cfg());
  w.on_command(cmd(1, 0), 0);
  w.evaluate(300 * ms);
  ASSERT_EQ(w.state(), State::Hold);
  w.on_command(cmd(2, 300 * ms), 301 * ms);
  EXPECT_EQ(w.state(), State::Run);
}

TEST(Watchdog, StopIsLatchedUntilConsecutiveFreshCommands) {
  Watchdog w(cfg());
  w.on_command(cmd(1, 0), 0);
  w.evaluate(2000 * ms);
  ASSERT_EQ(w.state(), State::Stop);
  w.on_command(cmd(2, 2000 * ms), 2001 * ms);
  EXPECT_EQ(w.state(), State::Stop);
  w.on_command(cmd(3, 2010 * ms), 2011 * ms);
  EXPECT_EQ(w.state(), State::Stop);
  w.on_command(cmd(4, 2020 * ms), 2021 * ms);
  EXPECT_EQ(w.state(), State::Run);  // third fresh command
}

TEST(Watchdog, StaleCommandResetsRecoveryStreak) {
  Watchdog w(cfg());
  w.on_command(cmd(1, 0), 0);
  w.evaluate(2000 * ms);
  w.on_command(cmd(2, 2000 * ms), 2001 * ms);
  w.on_command(cmd(3, 2010 * ms), 2011 * ms);
  // Delivered late: age 500 ms -> accepted but not "fresh", streak resets.
  w.on_command(cmd(4, 2020 * ms), 2520 * ms);
  w.on_command(cmd(5, 2530 * ms), 2531 * ms);
  EXPECT_EQ(w.state(), State::Stop);
}

TEST(Watchdog, RejectsCommandsOlderThanStopThreshold) {
  Watchdog w(cfg());
  EXPECT_FALSE(w.on_command(cmd(1, 0), 1500 * ms));
  EXPECT_EQ(w.rejected_stale(), 1u);
  EXPECT_NE(w.state(), State::Run);
}

TEST(Watchdog, RejectsOutOfOrderCommands) {
  Watchdog w(cfg());
  EXPECT_TRUE(w.on_command(cmd(2, 100 * ms), 101 * ms));
  EXPECT_FALSE(w.on_command(cmd(1, 50 * ms), 102 * ms));
  EXPECT_FALSE(w.on_command(cmd(3, 100 * ms), 102 * ms));  // equal stamp
  EXPECT_EQ(w.rejected_order(), 2u);
  EXPECT_EQ(w.command_age_ns(150 * ms), 50 * ms);
}

TEST(Watchdog, LateButAcceptedCommandLandsInHold) {
  Watchdog w(cfg());
  EXPECT_TRUE(w.on_command(cmd(1, 0), 500 * ms));  // 500 ms old on arrival
  EXPECT_EQ(w.state(), State::Hold);
}

TEST(Watchdog, StateNames) {
  EXPECT_STREQ(to_string(State::Run), "RUN");
  EXPECT_STREQ(to_string(State::Hold), "HOLD");
  EXPECT_STREQ(to_string(State::Stop), "STOP");
}

TEST(Watchdog, RejectsFutureStampedCommandsInsteadOfLockingOutGenuineOnes) {
  Watchdog w(cfg());
  // A command stamped an hour ahead would otherwise become "newest" and make
  // every real command look out-of-order.
  EXPECT_FALSE(w.on_command(cmd(1, 3'600'000 * ms), 10 * ms));
  EXPECT_EQ(w.rejected_future(), 1u);
  EXPECT_TRUE(w.on_command(cmd(2, 10 * ms), 11 * ms));
  EXPECT_EQ(w.state(), State::Run);
}

TEST(Watchdog, SmallClockSkewIntoTheFutureIsTolerated) {
  Watchdog w(cfg());
  EXPECT_TRUE(w.on_command(cmd(1, 60 * ms), 10 * ms));  // 50 ms ahead < 100 ms allowance
}

TEST(Watchdog, ExtremeTimestampsDoNotOverflow) {
  Watchdog w(cfg());
  EXPECT_FALSE(w.on_command(cmd(1, INT64_MIN), 5 * ms));  // age saturates -> stale
  EXPECT_EQ(w.rejected_stale(), 1u);
  EXPECT_FALSE(w.on_command(cmd(2, INT64_MAX), 5 * ms));  // far future
  EXPECT_EQ(w.rejected_future(), 1u);
  EXPECT_EQ(cmd(3, INT64_MIN).age_ns(5 * ms), INT64_MAX);
  EXPECT_EQ(cmd(3, INT64_MAX).age_ns(-5 * ms), INT64_MIN);
}

TEST(Watchdog, EvaluateIsStableAfterStopWithoutNewCommands) {
  Watchdog w(cfg());
  w.on_command(cmd(1, 0), 0);
  for (int t = 1000; t < 5000; t += 100) EXPECT_EQ(w.evaluate(t * ms), State::Stop);
  EXPECT_EQ(w.transitions_to_stop(), 1u);
}
