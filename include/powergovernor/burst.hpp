// Power Governor - bounded transient burst accounting.
//
// A burst budget is a token bucket expressed in joules: capacity = allowance_watts * window_seconds.
// It permits a workload to draw above its soft target for a bounded window, but a burst can never
// bypass a hard hardware/policy limit (the decision engine checks that independently). Burst
// capacity refills at the window boundary; oversubscription is explicit and bounded by policy.
#pragma once
#include <stdexcept>
#include "clock.hpp"
#include "policy.hpp"
#include "units.hpp"

namespace pg {

class BurstBudget {
 public:
  explicit BurstBudget(BurstLimits limits = {}) : limits_(limits) {}

  void set_limits(BurstLimits l) { limits_ = l; reset(); }
  const BurstLimits& limits() const noexcept { return limits_; }

  Joules capacity() const noexcept {
    return Joules(limits_.allowance.value() * limits_.window.seconds_f());
  }

  Joules available(Timestamp now) const {
    if (!limits_.enabled) return Joules(0.0);
    refill_if_needed(now);
    const double a = capacity().value() - consumed_.value();
    return Joules(a < 0.0 ? 0.0 : a);
  }

  bool can_burst(Joules j, Timestamp now) const {
    if (!limits_.enabled) return false;
    return available(now).value() + 1e-9 >= j.value() && j.value() >= 0.0;
  }

  // Consume burst energy when it is within the budget. Never returns true if it would overrun.
  bool consume(Joules j, Timestamp now) {
    if (!limits_.enabled) return false;
    refill_if_needed(now);
    if (!can_burst(j, now)) return false;
    consumed_ = Joules::clamped(consumed_.value() + j.value());
    return true;
  }

  Joules consumed(Timestamp now) const {
    if (!limits_.enabled) return Joules(0.0);
    refill_if_needed(now);
    return consumed_;
  }

  bool exhausted(Timestamp now) const { return !limits_.enabled || available(now).value() <= 0.0; }
  double utilization(Timestamp now) const {
    if (!limits_.enabled) return 0.0;
    const double cap = capacity().value();
    if (cap <= 0.0) return 0.0;
    return consumed(now).value() / cap;
  }

  void reset() noexcept { consumed_ = Joules(0.0); window_start_ = Timestamp(); started_ = false; }

 private:
  void refill_if_needed(Timestamp now) const {
    if (!started_) { window_start_ = now; started_ = true; }
    else if (now.elapsed_since(window_start_) >= limits_.window) {
      window_start_ = now;
      consumed_ = Joules(0.0);
    }
  }

  BurstLimits limits_;
  mutable Joules consumed_{0.0};
  mutable Timestamp window_start_;
  mutable bool started_ = false;
};

}  // namespace pg
