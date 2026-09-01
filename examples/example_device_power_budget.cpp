#include "powergovernor/governor.hpp"
#include <cstdio>
using namespace pg;
int main() {
  TestClock clk(Timestamp::from_ns(1000000000LL)); FakeBackend fb;
  std::printf("=== Device power budget ===\n");
  GovernorConfig cfg; cfg.id=GovernorId::allocate(); cfg.node=NodeId::from_raw(1);
  cfg.boot=WorkerBootId::from_raw(1); cfg.epoch=CoordinatorEpoch::first();
  Governor gov(clk, fb, cfg);
  PowerPolicy pol = PowerPolicy::defaults(); pol.device_hard_cap = Watts(300.0);
  pol.fleet_hard_cap = Watts(5000.0); pol.generation = PolicyGeneration::first();
  gov.install_policy(pol);
  PowerBudget dev; dev.id=BudgetId::from_raw(1); dev.generation=BudgetGeneration::first();
  dev.domain=PowerDomainId::from_raw(1); dev.hard_max=Watts(300.0); dev.name="device";
  gov.install_budget(dev);
  fb.set_power(Watts(130.0)); fb.set_temperature(Celsius(42.0));
  gov.poll(clk.now());
  Decision d = gov.decide_for(BudgetId::from_raw(1), WorkloadId::from_raw(1), WorkloadPowerClass::THROUGHPUT,
                              Watts(250.0), Joules(0), 1.0, clk.now());
  std::printf("requested 250 W under a 300 W device budget -> %s limit=%s\n",
              std::string(to_string(d.verdict)).c_str(),
              d.recommended_limit ? d.recommended_limit->to_string().c_str() : "n/a");
  return 0;
}
