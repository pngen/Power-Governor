#include "test_framework.hpp"
#include "powergovernor/governor.hpp"
#include <atomic>
#include <thread>
#include <vector>
using namespace pg;

static void init_gov(Governor& gov) {
  PowerPolicy pol = PowerPolicy::defaults();
  pol.device_hard_cap = Watts(402.0); pol.fleet_hard_cap = Watts(5000.0);
  pol.generation = PolicyGeneration::first();
  if (!gov.install_policy(pol)) throw std::runtime_error("policy install failed");
  PowerBudget node; node.id = BudgetId::from_raw(1); node.generation = BudgetGeneration::first();
  node.domain = PowerDomainId::from_raw(1); node.hard_max = Watts(450.0); node.name = "node";
  gov.install_budget(node);
  PowerBudget acc; acc.id = BudgetId::from_raw(2); acc.generation = BudgetGeneration::first();
  acc.domain = PowerDomainId::from_raw(2); acc.parent = node.id; acc.hard_max = Watts(402.0); acc.name = "acc";
  gov.install_budget(acc);
}

TEST(governor_reservation_lifecycle) {
  TestClock clk(Timestamp::from_ns(1000000000LL)); FakeBackend fb;
  GovernorConfig cfg; cfg.id = GovernorId::allocate(); cfg.node = NodeId::from_raw(1);
  cfg.boot = WorkerBootId::from_raw(1); cfg.epoch = CoordinatorEpoch::first();
  Governor gov(clk, fb, cfg); init_gov(gov);
  auto plan = gov.plan_reservation(WorkloadId::from_raw(10), WorkloadPowerClass::THROUGHPUT,
      PowerDomainId::from_raw(2), BudgetId::from_raw(2), Watts(60), Joules(50),
      PolicyGeneration::first(), DeviceGeneration::first(), clk.now(), Duration::seconds(5));
  CHECK(plan.ok);
  CHECK(gov.validate_reservation(plan.id, PolicyGeneration::first(), clk.now()).ok);
  CHECK(gov.reserve_reservation(plan.id, BudgetGeneration::first(), clk.now()).ok);
  CHECK(gov.reservation_state(plan.id) == ReservationState::RESERVED);
  CHECK(gov.admit_reservation(plan.id, clk.now()).ok);
  CHECK(gov.activate_reservation(plan.id, BudgetGeneration::first(), clk.now()).ok);
  CHECK(gov.active_reservations() == 1);
  CHECK(gov.accounting_consistent());
  CHECK(gov.release_reservation(plan.id, BudgetGeneration::first(), clk.now()).ok);
  CHECK(gov.reservation_state(plan.id) == ReservationState::RELEASED);
  CHECK(gov.active_reservations() == 0);
  CHECK(gov.accounting_consistent());
}

TEST(governor_synthetic_constrained_budget_decision_path) {
  TestClock clk(Timestamp::from_ns(1000000000LL)); FakeBackend fb;
  fb.set_power(Watts(120.0)); fb.set_temperature(Celsius(55.0));
  GovernorConfig cfg; cfg.id = GovernorId::allocate(); cfg.node = NodeId::from_raw(1);
  cfg.boot = WorkerBootId::from_raw(1); cfg.epoch = CoordinatorEpoch::first();
  Governor gov(clk, fb, cfg); init_gov(gov);
  gov.poll(clk.now());
  Decision d1 = gov.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1),
      WorkloadPowerClass::THROUGHPUT, Watts(100), Joules(0), 1.0, clk.now());
  CHECK(d1.verdict == DecisionVerdict::ALLOW);
  PowerPolicy pol = PowerPolicy::defaults();
  pol.device_hard_cap = Watts(50.0); pol.fleet_hard_cap = Watts(5000.0);
  pol.generation = PolicyGeneration::from_raw(2);
  CHECK(gov.install_policy(pol));
  Decision d2 = gov.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1),
      WorkloadPowerClass::THROUGHPUT, Watts(100), Joules(0), 1.0, clk.now());
  CHECK(d2.verdict == DecisionVerdict::ALLOW_WITH_LIMIT || d2.verdict == DecisionVerdict::THROTTLE);
  CHECK(d2.recommended_limit && d2.recommended_limit->value() <= 50.0 + 1e-9);
  PowerPolicy pol2 = PowerPolicy::defaults();
  pol2.device_hard_cap = Watts(402.0); pol2.fleet_hard_cap = Watts(5000.0);
  pol2.generation = PolicyGeneration::from_raw(3);
  CHECK(gov.install_policy(pol2));
  clk.advance(Duration::milliseconds(220));  // > min_stable, < fresh: clear hysteresis, keep telemetry CURRENT
  Decision d3 = gov.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1),
      WorkloadPowerClass::THROUGHPUT, Watts(100), Joules(0), 1.0, clk.now());
  CHECK(d3.verdict == DecisionVerdict::ALLOW);
}

TEST(governor_persistence_recovery_not_current) {
  TestClock clk(Timestamp::from_ns(1000000000LL)); FakeBackend fb;
  GovernorConfig cfg; cfg.id = GovernorId::allocate(); cfg.node = NodeId::from_raw(1);
  cfg.boot = WorkerBootId::from_raw(1); cfg.epoch = CoordinatorEpoch::first();
  Governor gov(clk, fb, cfg); init_gov(gov);
  auto plan = gov.plan_reservation(WorkloadId::from_raw(10), WorkloadPowerClass::THROUGHPUT,
      PowerDomainId::from_raw(2), BudgetId::from_raw(2), Watts(60), Joules(0),
      PolicyGeneration::first(), DeviceGeneration::first(), clk.now(), Duration::seconds(2));
  CHECK(plan.ok);
  CHECK(gov.reserve_reservation(plan.id, BudgetGeneration::first(), clk.now()).ok);
  fb.set_power(Watts(120.0)); fb.set_temperature(Celsius(55.0));
  gov.poll(clk.now());
  const std::string dg = gov.state_digest();
  const std::string file = "test_state.bin";
  CHECK(gov.save(file));
  GovernorConfig cfg2; cfg2.id = GovernorId::allocate(); cfg2.node = NodeId::from_raw(1);
  cfg2.boot = WorkerBootId::from_raw(1); cfg2.epoch = CoordinatorEpoch::first();
  Governor gov2(clk, fb, cfg2);   // no init_gov: load() brings policy/budgets back
  CHECK(gov2.load(file));
  CHECK(gov2.state_digest() == dg);
  Decision d = gov2.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1),
      WorkloadPowerClass::THROUGHPUT, Watts(100), Joules(0), 1.0, clk.now());
  CHECK(d.verdict == DecisionVerdict::REVALIDATION_REQUIRED);
  CHECK(gov2.accounting_consistent());
}

TEST(governor_concurrency_exact_accounting) {
  TestClock clk(Timestamp::from_ns(1000000000LL)); FakeBackend fb;
  GovernorConfig cfg; cfg.id = GovernorId::allocate(); cfg.node = NodeId::from_raw(1);
  cfg.boot = WorkerBootId::from_raw(1); cfg.epoch = CoordinatorEpoch::first();
  Governor gov(clk, fb, cfg); init_gov(gov);
  constexpr int kThreads = 8;
  constexpr int kIterations = 200;
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int wIdx = 0; wIdx < kThreads; ++wIdx) {
    threads.emplace_back([&, wIdx]() {
      for (int i = 0; i < kIterations; ++i) {
        auto plan = gov.plan_reservation(WorkloadId::from_raw(100 + wIdx),
            WorkloadPowerClass::THROUGHPUT, PowerDomainId::from_raw(2), BudgetId::from_raw(2),
            Watts(20), Joules(0), PolicyGeneration::first(), DeviceGeneration::first(),
            clk.now(), Duration::seconds(3));
        if (!plan.ok) { ++errors; continue; }
        if (!gov.validate_reservation(plan.id, PolicyGeneration::first(), clk.now()).ok) { ++errors; continue; }
        if (!gov.reserve_reservation(plan.id, BudgetGeneration::first(), clk.now()).ok) { ++errors; continue; }
        if (!gov.activate_reservation(plan.id, BudgetGeneration::first(), clk.now()).ok) { ++errors; continue; }
        if (!gov.release_reservation(plan.id, BudgetGeneration::first(), clk.now()).ok) { ++errors; continue; }
      }
    });
  }
  for (auto& th : threads) th.join();
  CHECK(errors.load() == 0);
  CHECK(gov.active_reservations() == 0);
  CHECK(gov.accounting_consistent());
  CHECK(gov.budget_manager().reserved(BudgetId::from_raw(2)).value() == 0.0);
}

TEST(governor_rejects_budget_generation_rollback) {
  TestClock clk; FakeBackend fb;
  GovernorConfig cfg; cfg.id = GovernorId::allocate(); cfg.node = NodeId::from_raw(1);
  cfg.boot = WorkerBootId::from_raw(1); cfg.epoch = CoordinatorEpoch::first();
  Governor gov(clk, fb, cfg); init_gov(gov);
  PowerBudget b; b.id = BudgetId::from_raw(3); b.generation = BudgetGeneration::first();
  b.domain = PowerDomainId::from_raw(3); b.hard_max = Watts(100.0); b.name = "b3";
  CHECK(gov.install_budget(b));
  PowerBudget b_old = b; b_old.generation = BudgetGeneration();
  CHECK(!gov.install_budget(b_old));
}

