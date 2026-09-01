#include "powergovernor/governor.hpp"
#include <cstdio>
using namespace pg;
int main() {
  std::printf("=== Workload power classes -> governor decisions ===\n");
  TestClock clk(Timestamp::from_ns(1000000000LL)); FakeBackend fb;
  GovernorConfig cfg; cfg.id=GovernorId::allocate(); cfg.node=NodeId::from_raw(1);
  cfg.boot=WorkerBootId::from_raw(1); cfg.epoch=CoordinatorEpoch::first();
  Governor gov(clk, fb, cfg);
  PowerPolicy pol = PowerPolicy::defaults(); pol.device_hard_cap = Watts(150.0);
  pol.generation = PolicyGeneration::first(); gov.install_policy(pol);
  fb.set_power(Watts(90.0)); fb.set_temperature(Celsius(45.0)); gov.poll(clk.now());
  const char* names[] = {"LATENCY_CRITICAL","THROUGHPUT","BATCH","BACKGROUND","ENERGY_OPTIMIZED"};
  WorkloadPowerClass classes[] = {WorkloadPowerClass::LATENCY_CRITICAL, WorkloadPowerClass::THROUGHPUT,
    WorkloadPowerClass::BATCH, WorkloadPowerClass::BACKGROUND, WorkloadPowerClass::ENERGY_OPTIMIZED};
  for (int i=0;i<5;i++){
    Decision d = gov.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1), classes[i], Watts(200), Joules(0), 1.0, clk.now());
    std::printf("class %s request 200 W -> %s\n", names[i], std::string(to_string(d.verdict)).c_str());
  }
  return 0;
}
