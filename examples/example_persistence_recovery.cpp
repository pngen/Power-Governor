#include "powergovernor/governor.hpp"
#include <cstdio>
#include <fstream>
using namespace pg;
int main() {
  std::printf("=== Persistence / recovery ===\n");
  TestClock clk(Timestamp::from_ns(1000000000LL)); FakeBackend fb;
  GovernorConfig cfg; cfg.id=GovernorId::allocate(); cfg.node=NodeId::from_raw(1);
  cfg.boot=WorkerBootId::from_raw(1); cfg.epoch=CoordinatorEpoch::first();
  {
    Governor gov(clk, fb, cfg);
    PowerPolicy pol = PowerPolicy::defaults(); pol.device_hard_cap = Watts(402.0); pol.generation = PolicyGeneration::first();
    gov.install_policy(pol);
    PowerBudget acc; acc.id=BudgetId::from_raw(2); acc.generation=BudgetGeneration::first();
    acc.domain=PowerDomainId::from_raw(2); acc.hard_max=Watts(402.0); acc.name="acc"; gov.install_budget(acc);
    fb.set_power(Watts(120.0)); fb.set_temperature(Celsius(45.0)); gov.poll(clk.now());
    std::printf("saved digest: %s\n", gov.state_digest().c_str());
    gov.save("example_state.bin");
  }
  {
    Governor gov2(clk, fb, cfg);
    gov2.load("example_state.bin");
    std::printf("loaded digest: %s\n", gov2.state_digest().c_str());
    Decision d = gov2.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1), WorkloadPowerClass::THROUGHPUT, Watts(250), Joules(0), 1.0, clk.now());
    std::printf("recovered telemetry is not CURRENT -> %s\n", std::string(to_string(d.verdict)).c_str());
  }
  return 0;
}
