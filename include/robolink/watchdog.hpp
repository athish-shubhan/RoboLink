#pragma once
#include <cstdint>

#include "robolink/packet.hpp"

namespace robolink {

enum class State { Run, Hold, Stop };
const char* to_string(State s);

struct WatchdogConfig {
  int64_t hold_ns = 200'000'000;   // command age >= this  -> HOLD
  int64_t stop_ns = 1'000'000'000; // command age >= this  -> STOP
  int recover_commands = 3;        // consecutive fresh commands needed to leave STOP
};

// Pure state machine; the caller supplies `now`, so it is deterministic to test.
//
//  - The "command age" is now - stamp of the newest accepted command.
//  - Before any command has arrived the state is HOLD.
//  - HOLD  <-> RUN  follow the age directly.
//  - STOP is latched: it is left only after `recover_commands` consecutive
//    commands that are fresh (age < hold_ns) on arrival.
//  - A command that is already older than stop_ns on arrival, or not newer than
//    the newest one seen, is rejected.
class Watchdog {
 public:
  explicit Watchdog(WatchdogConfig cfg = {}) : cfg_(cfg) {}

  // Returns true if the command was accepted.
  bool on_command(const Packet& p, int64_t now);
  // Re-evaluates the state at time `now` (call periodically).
  State evaluate(int64_t now);

  State state() const { return state_; }
  int64_t command_age_ns(int64_t now) const { return have_cmd_ ? now - last_stamp_ : -1; }
  uint64_t rejected_stale() const { return rejected_stale_; }
  uint64_t rejected_order() const { return rejected_order_; }
  uint64_t transitions_to_hold() const { return to_hold_; }
  uint64_t transitions_to_stop() const { return to_stop_; }

 private:
  void set(State s);
  WatchdogConfig cfg_;
  State state_ = State::Hold;
  bool have_cmd_ = false;
  int64_t last_stamp_ = 0;
  int fresh_streak_ = 0;
  uint64_t rejected_stale_ = 0, rejected_order_ = 0, to_hold_ = 0, to_stop_ = 0;
};

}  // namespace robolink
