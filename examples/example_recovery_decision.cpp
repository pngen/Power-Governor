#include "powergovernor/governor.hpp"
#include <cstdio>
using namespace pg;
int main() {
  std::printf("=== Recovery from thermally constrained state ===\n");
  TestClock clk(Timestamp::from_ns(1000000000LL)); FakeBackend fb;
  GovernorConfig cfg; cfg.id=GovernorId::allocate(); cfg.node=NodeId::from_raw(1);
  cfg.boot=WorkerBootId::from_raw(1); cfg.epoch=CoordinatorEpoch::first();
  Governor gov(clk, fb, cfg);
  PowerPolicy pol = PowerPolicy::defaults(); pol.device_hard_cap = Watts(402.0); pol.generation = PolicyGeneration::first();
  gov.install_policy(pol);
  fb.set_temperature(Celsius(90.0)); gov.poll(clk.now()); gov.poll(clk.now());
  Decision d1 = gov.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1), WorkloadPowerClass::THROUGHPUT, Watts(250), Joules(0), 1.0, clk.now());
  std::printf("at 90 C: %s (thermal recovery gates execution)\n", std::string(to_string(d1.verdict)).c_str());
  fb.set_temperature(Celsius(50.0)); gov.poll(clk.now());
  Decision d2 = gov.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1), WorkloadPowerClass::THROUGHPUT, Watts(250), Joules(0), 1.0, clk.now());
  std::printf("after cooling to 50 C: %s (normal eligibility returns)\n", std::string(to_string(d2.verdict)).c_str());
  return 0;
}
