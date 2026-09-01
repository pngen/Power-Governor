#include "powergovernor/governor.hpp"
#include <cstdio>
using namespace pg;
int main() {
  std::printf("=== Transactional workload reservation ===\n");
  TestClock clk(Timestamp::from_ns(1000000000LL)); FakeBackend fb;
  GovernorConfig cfg; cfg.id=GovernorId::allocate(); cfg.node=NodeId::from_raw(1);
  cfg.boot=WorkerBootId::from_raw(1); cfg.epoch=CoordinatorEpoch::first();
  Governor gov(clk, fb, cfg);
  PowerPolicy pol = PowerPolicy::defaults(); pol.device_hard_cap = Watts(402.0);
  pol.generation = PolicyGeneration::first(); gov.install_policy(pol);
  PowerBudget acc; acc.id=BudgetId::from_raw(2); acc.generation=BudgetGeneration::first();
  acc.domain=PowerDomainId::from_raw(2); acc.hard_max=Watts(402.0); acc.name="acc"; gov.install_budget(acc);
  auto plan = gov.plan_reservation(WorkloadId::from_raw(7), WorkloadPowerClass::BATCH, PowerDomainId::from_raw(2),
      BudgetId::from_raw(2), Watts(80), Joules(2000), PolicyGeneration::first(), DeviceGeneration::first(), clk.now(), Duration::seconds(4));
  std::printf("plan -> %s (id=%s)\n", plan.ok ? "ok" : plan.reason.c_str(), plan.id.to_string().c_str());
  gov.validate_reservation(plan.id, PolicyGeneration::first(), clk.now());
  gov.reserve_reservation(plan.id, BudgetGeneration::first(), clk.now());
  gov.admit_reservation(plan.id, clk.now());
  gov.activate_reservation(plan.id, BudgetGeneration::first(), clk.now());
  std::printf("reservation %s active; active count=%zu\n", plan.id.to_string().c_str(), gov.active_reservations());
  gov.release_reservation(plan.id, BudgetGeneration::first(), clk.now());
  std::printf("released; active count=%zu\n", gov.active_reservations());
  return 0;
}
