#include "powergovernor/governor.hpp"
#include <cstdio>
using namespace pg;
int main() {
  std::printf("=== Hierarchical budget (node -> accelerator) ===\n");
  TestClock clk(Timestamp::from_ns(1000000000LL)); FakeBackend fb;
  GovernorConfig cfg; cfg.id=GovernorId::allocate(); cfg.node=NodeId::from_raw(1);
  cfg.boot=WorkerBootId::from_raw(1); cfg.epoch=CoordinatorEpoch::first();
  Governor gov(clk, fb, cfg);
  PowerPolicy pol = PowerPolicy::defaults(); pol.device_hard_cap = Watts(402.0);
  pol.fleet_hard_cap = Watts(5000.0); pol.node_hard_cap = Watts(450.0);
  pol.generation = PolicyGeneration::first(); gov.install_policy(pol);
  PowerBudget node; node.id=BudgetId::from_raw(1); node.generation=BudgetGeneration::first();
  node.domain=PowerDomainId::from_raw(1); node.hard_max=Watts(450.0); node.name="node"; gov.install_budget(node);
  PowerBudget acc; acc.id=BudgetId::from_raw(2); acc.generation=BudgetGeneration::first();
  acc.domain=PowerDomainId::from_raw(2); acc.parent=node.id; acc.hard_max=Watts(402.0); acc.name="acc"; gov.install_budget(acc);
  auto plan = gov.plan_reservation(WorkloadId::from_raw(1), WorkloadPowerClass::THROUGHPUT, PowerDomainId::from_raw(2),
      BudgetId::from_raw(2), Watts(250), Joules(0), PolicyGeneration::first(), DeviceGeneration::first(), clk.now(), Duration::seconds(5));
  if (!plan.ok) { std::printf("plan failed: %s\n", plan.reason.c_str()); return 1; }
  gov.reserve_reservation(plan.id, BudgetGeneration::first(), clk.now());
  std::printf("acc reserved 250 W of 402; node available %s\n", gov.budget_available(BudgetId::from_raw(1)).to_string().c_str());
  auto plan2 = gov.plan_reservation(WorkloadId::from_raw(2), WorkloadPowerClass::BACKGROUND, PowerDomainId::from_raw(2),
      BudgetId::from_raw(2), Watts(300), Joules(0), PolicyGeneration::first(), DeviceGeneration::first(), clk.now(), Duration::seconds(5));
  auto r2 = gov.reserve_reservation(plan2.id, BudgetGeneration::first(), clk.now());
  std::printf("second 300 W reservation -> %s (parent budget prevents overflow)\n", r2.ok ? "OK" : "REJECTED");
  return 0;
}
