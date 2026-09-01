#include "powergovernor/burst.hpp"
#include "powergovernor/policy.hpp"
#include <cstdio>
using namespace pg;
int main() {
  std::printf("=== Bounded burst allowance ===\n");
  BurstLimits bl; bl.enabled = true; bl.allowance = Watts(60.0); bl.window = Duration::seconds(2);
  BurstBudget bb(bl);
  TestClock t; t.advance(Duration::milliseconds(10));
  std::printf("capacity=%.1f J\n", bb.capacity().value());
  std::printf("consume 80 J -> %s\n", bb.consume(Joules(80), t.now()) ? "ok" : "denied");
  std::printf("consume 40 J -> %s (would overrun, denied)\n", bb.consume(Joules(40), t.now()) ? "ok" : "denied");
  std::printf("available=%.1f J utilization=%.2f\n", bb.available(t.now()).value(), bb.utilization(t.now()));
  t.advance(Duration::seconds(3));
  std::printf("after window refill: available=%.1f J\n", bb.available(t.now()).value());
  return 0;
}
