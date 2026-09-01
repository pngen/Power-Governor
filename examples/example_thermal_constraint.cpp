#include "powergovernor/thermal.hpp"
#include "powergovernor/governor.hpp"
#include <cstdio>
using namespace pg;
int main() {
  std::printf("=== Thermal constraint -> governor decision ===\n");
  TestClock clk(Timestamp::from_ns(1000000000LL)); FakeBackend fb;
  GovernorConfig cfg; cfg.id=GovernorId::allocate(); cfg.node=NodeId::from_raw(1);
  cfg.boot=WorkerBootId::from_raw(1); cfg.epoch=CoordinatorEpoch::first();
  Governor gov(clk, fb, cfg);
  PowerPolicy pol = PowerPolicy::defaults(); pol.device_hard_cap = Watts(402.0); pol.generation = PolicyGeneration::first();
  gov.install_policy(pol);
  fb.set_power(Watts(120.0)); fb.set_temperature(Celsius(88.0)); gov.poll(clk.now());
  gov.poll(clk.now());  // second hot sample confirms constrained state (hysteresis)
  Decision d = gov.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1), WorkloadPowerClass::THROUGHPUT,
                              Watts(250), Joules(0), 1.0, clk.now());
  std::printf("device at 88 C (constrained): requested 250 W -> %s\n", std::string(to_string(d.verdict)).c_str());
  return 0;
}
