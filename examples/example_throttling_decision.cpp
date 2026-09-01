#include "powergovernor/governor.hpp"
#include <cstdio>
using namespace pg;
int main() {
  std::printf("=== Throttling decision under a constrained budget ===\n");
  TestClock clk(Timestamp::from_ns(1000000000LL)); FakeBackend fb;
  GovernorConfig cfg; cfg.id=GovernorId::allocate(); cfg.node=NodeId::from_raw(1);
  cfg.boot=WorkerBootId::from_raw(1); cfg.epoch=CoordinatorEpoch::first();
  Governor gov(clk, fb, cfg);
  PowerPolicy pol = PowerPolicy::defaults(); pol.device_hard_cap = Watts(60.0); pol.generation = PolicyGeneration::first();
  gov.install_policy(pol);
  fb.set_power(Watts(55.0)); fb.set_temperature(Celsius(50.0)); gov.poll(clk.now());
  Decision d = gov.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1), WorkloadPowerClass::THROUGHPUT, Watts(250), Joules(0), 1.0, clk.now());
  std::printf("60 W hard cap vs 250 W request -> %s limit=%s\n", std::string(to_string(d.verdict)).c_str(),
              d.recommended_limit ? d.recommended_limit->to_string().c_str() : "n/a");
  std::printf("why: %s\n", d.explanation.why.c_str());
  return 0;
}
