#include "robolink/watchdog.hpp"

namespace robolink {

const char* to_string(State s) {
  switch (s) {
    case State::Run: return "RUN";
    case State::Hold: return "HOLD";
    case State::Stop: return "STOP";
  }
  return "?";
}

void Watchdog::set(State s) {
  if (s == state_) return;
  if (s == State::Hold && state_ == State::Run) ++to_hold_;
  if (s == State::Stop) ++to_stop_;
  state_ = s;
}

bool Watchdog::on_command(const Packet& p, int64_t now) {
  const int64_t age = p.age_ns(now);
  if (age >= cfg_.stop_ns) { ++rejected_stale_; fresh_streak_ = 0; return false; }
  if (age < -cfg_.max_future_ns) { ++rejected_future_; return false; }
  if (have_cmd_ && p.stamp_ns <= last_stamp_) { ++rejected_order_; return false; }
  have_cmd_ = true;
  last_stamp_ = p.stamp_ns;
  fresh_streak_ = (age < cfg_.hold_ns) ? fresh_streak_ + 1 : 0;
  evaluate(now);
  return true;
}

State Watchdog::evaluate(int64_t now) {
  if (!have_cmd_) { return state_; }  // stays HOLD until the first command
  const int64_t age = age_between(now, last_stamp_);
  if (age >= cfg_.stop_ns) {
    set(State::Stop);
    fresh_streak_ = 0;
  } else if (state_ == State::Stop) {
    if (fresh_streak_ >= cfg_.recover_commands && age < cfg_.hold_ns) set(State::Run);
  } else if (age >= cfg_.hold_ns) {
    set(State::Hold);
  } else {
    set(State::Run);
  }
  return state_;
}

}  // namespace robolink
