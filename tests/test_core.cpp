#include "test_framework.hpp"
#include <limits>
#include <type_traits>
#include "powergovernor/units.hpp"
#include "powergovernor/ids.hpp"
#include "powergovernor/freshness.hpp"
#include "powergovernor/thermal.hpp"
#include "powergovernor/budget.hpp"
#include "powergovernor/reservation.hpp"
#include "powergovernor/burst.hpp"
#include "powergovernor/energy.hpp"
#include "powergovernor/authority.hpp"
#include "powergovernor/policy.hpp"
#include "powergovernor/index.hpp"
#include "powergovernor/change_history.hpp"
using namespace pg;

TEST(strong_ids_are_distinct) {
  GovernorId g = GovernorId::allocate();
  NodeId n = NodeId::allocate();
  CHECK(g.is_valid() && n.is_valid());
  // Distinct strong types: GovernorId and NodeId are different C++ types and cannot be mixed.
  static_assert(!std::is_same_v<GovernorId, NodeId>, "identities must be distinct types");
  static_assert(std::is_same_v<StrongId<GovernorIdTag>, GovernorId>);
  CHECK(StrongId<GovernorIdTag>::from_raw(5).raw() == 5);
  CHECK(!GovernorId::invalid().is_valid());
}

TEST(generations_are_independent) {
  PolicyGeneration p = PolicyGeneration::first();
  BudgetGeneration b = BudgetGeneration::first();
  CHECK(p.raw() == 1 && b.raw() == 1);
  CHECK(p.next().raw() == 2);
  CHECK(DeviceGeneration::from_raw(9) > DeviceGeneration::null());
  CHECK(PolicyGeneration::null().is_null());
}

TEST(uuid_roundtrip) {
  Uuid u = Uuid::from_hex("00112233-4455-6677-8899-aabbccddeeff");
  CHECK(u.to_hex() == "00112233-4455-6677-8899-aabbccddeeff");
  CHECK(Uuid::from_counter(7).is_zero() == false);
  CHECK_THROWS(Uuid::from_hex("zz"));
}

TEST(typed_units_validate) {
  CHECK(Watts(250.0).value() == 250.0);
  // conversion
  CHECK(Watts(1.0).to<MilliwattsTag>().value() == 1000.0);
  CHECK_THROWS(Watts(-1.0));
  CHECK_THROWS(Watts(std::numeric_limits<double>::quiet_NaN()));
  CHECK_THROWS(Watts(std::numeric_limits<double>::infinity()));
  CHECK_THROWS(Utilization(1.5));
  CHECK(Utilization(1.0).value() == 1.0);
  CHECK(Utilization(0.0).is_zero());
}

TEST(duration_overflow_rejected) {
  CHECK(Duration::milliseconds(1000).ns() == 1000000000LL);
  CHECK_THROWS(Duration::nanoseconds(-1));
  CHECK_THROWS(Duration::seconds(5000000000000000000LL));
}

TEST(power_role_hierarchy) {
  BudgetManager bm;
  PowerBudget root; root.id = BudgetId::from_raw(1); root.generation = BudgetGeneration::first();
  root.domain = PowerDomainId::from_raw(1); root.hard_max = Watts(400.0); root.name = "root";
  PowerBudget node; node.id = BudgetId::from_raw(2); node.generation = BudgetGeneration::first();
  node.domain = PowerDomainId::from_raw(2); node.parent = root.id; node.hard_max = Watts(300.0); node.name = "node";
  CHECK(bm.install(root)); CHECK(bm.install(node));
  const auto g1 = BudgetGeneration::first();
  CHECK(bm.reserve(node.id, Watts(100.0), ReservationId::from_raw(1), g1));
  // reissue budget with newer generation == account preserved but generation rollback rejected
  PowerBudget node2 = node; node2.generation = BudgetGeneration::first().next(); node2.hard_max = Watts(300.0);
  CHECK(bm.install(node2));
  CHECK(!bm.reserve(node.id, Watts(10.0), ReservationId::from_raw(2), g1));  // stale budget gen
  CHECK(bm.available(node.id).value() <= 200.0 + 1e-9);
  CHECK(bm.accounting_consistent());
}

TEST(parent_child_overflow_never_silent) {
  BudgetManager bm;
  PowerBudget node; node.id = BudgetId::from_raw(1); node.generation = BudgetGeneration::first();
  node.domain = PowerDomainId::from_raw(1); node.hard_max = Watts(300.0); node.name = "node";
  PowerBudget gpu; gpu.id = BudgetId::from_raw(2); gpu.generation = BudgetGeneration::first();
  gpu.domain = PowerDomainId::from_raw(2); gpu.parent = node.id; gpu.hard_max = Watts(200.0); gpu.name = "gpu";
  CHECK(bm.install(node)); CHECK(bm.install(gpu));
  const auto g1 = BudgetGeneration::first();
  CHECK(bm.reserve(gpu.id, Watts(150.0), ReservationId::from_raw(1), g1));
  CHECK(!bm.reserve(gpu.id, Watts(60.0), ReservationId::from_raw(2), g1));  // exceeds own cap
  // parent bound: node at 300, gpu 150 reserved; reserving 160 more on gpu would push parent over
  CHECK(!bm.reserve(gpu.id, Watts(200.0), ReservationId::from_raw(3), g1));
  CHECK(bm.available(gpu.id).value() <= 50.0 + 1e-9);
  CHECK(bm.accounting_consistent());
}

TEST(reservation_lifecycle_full) {
  TestClock clk(Timestamp::from_ns(1000000000LL));
  BudgetManager bm;
  PowerBudget b; b.id = BudgetId::from_raw(1); b.generation = BudgetGeneration::first();
  b.domain = PowerDomainId::from_raw(1); b.hard_max = Watts(200.0); b.name = "b";
  bm.install(b);
  ReservationManager rm;
  AuthorityEnvelope env; env.epoch = CoordinatorEpoch::first(); env.boot = WorkerBootId::from_raw(1);
  auto plan = rm.plan(WorkloadId::from_raw(10), WorkloadPowerClass::THROUGHPUT, PowerDomainId::from_raw(1),
                      b.id, Watts(60), Joules(50), PolicyGeneration::first(), DeviceGeneration::first(),
                      env, clk.now(), Duration::seconds(3));
  CHECK(plan.ok);
  const ReservationId id = plan.id;
  CHECK(rm.validate(id, PolicyGeneration::first()).ok);
  CHECK(rm.reserve(id, bm, BudgetGeneration::first(), clk.now()).ok);
  CHECK(rm.state(id) == ReservationState::RESERVED);
  CHECK(rm.admit(id, clk.now()).ok);
  CHECK(rm.activate(id, bm, BudgetGeneration::first(), clk.now()).ok);
  CHECK(rm.state(id) == ReservationState::ACTIVE);
  CHECK(bm.used(b.id).value() >= 59.0);
  CHECK(rm.audit_no_leaks(clk.now()));
  // double release rejected
  CHECK(rm.release(id, bm, BudgetGeneration::first(), clk.now()).ok);
  CHECK(!rm.release(id, bm, BudgetGeneration::first(), clk.now()).ok);
  CHECK(bm.accounting_consistent());
}

TEST(reservation_prevents_double_reserve) {
  TestClock clk;
  BudgetManager bm;
  PowerBudget b; b.id = BudgetId::from_raw(1); b.generation = BudgetGeneration::first();
  b.domain = PowerDomainId::from_raw(1); b.hard_max = Watts(100.0); b.name = "b";
  bm.install(b);
  ReservationManager rm;
  AuthorityEnvelope env; env.epoch = CoordinatorEpoch::first(); env.boot = WorkerBootId::from_raw(1);
  auto plan = rm.plan(WorkloadId::from_raw(1), WorkloadPowerClass::THROUGHPUT, PowerDomainId::from_raw(1),
                      b.id, Watts(50), Joules(0), PolicyGeneration::first(), DeviceGeneration::first(),
                      env, clk.now(), Duration::seconds(2));
  CHECK(plan.ok);
  CHECK(rm.reserve(plan.id, bm, BudgetGeneration::first(), clk.now()).ok);
  CHECK(!rm.reserve(plan.id, bm, BudgetGeneration::first(), clk.now()).ok);  // double reserve
}

TEST(stale_reservation_activation_rejected) {
  TestClock clk;
  BudgetManager bm;
  PowerBudget b; b.id = BudgetId::from_raw(1); b.generation = BudgetGeneration::first();
  b.domain = PowerDomainId::from_raw(1); b.hard_max = Watts(200.0); b.name = "b";
  bm.install(b);
  ReservationManager rm;
  AuthorityEnvelope env; env.epoch = CoordinatorEpoch::first(); env.boot = WorkerBootId::from_raw(1);
  auto plan = rm.plan(WorkloadId::from_raw(1), WorkloadPowerClass::THROUGHPUT, PowerDomainId::from_raw(1),
                      b.id, Watts(50), Joules(0), PolicyGeneration::first(), DeviceGeneration::first(),
                      env, clk.now(), Duration::seconds(2));
  CHECK(rm.reserve(plan.id, bm, BudgetGeneration::first(), clk.now()).ok);
  CHECK(rm.admit(plan.id, clk.now()).ok);
  // reissue the budget with a newer generation; old reservation is now stale on activate
  PowerBudget b2 = b; b2.generation = BudgetGeneration::first().next();
  CHECK(bm.install(b2));
  CHECK(!rm.activate(plan.id, bm, BudgetGeneration::first(), clk.now()).ok);
}

TEST(thermal_hysteresis_and_recovery) {
  ThermalGovernor tg;
  CHECK(tg.observe(Celsius(50.0), Timestamp::from_ns(1)) == ThermalState::NORMAL);
  CHECK(tg.observe(Celsius(80.0), Timestamp::from_ns(2)) == ThermalState::HOT);
  CHECK(tg.observe(Celsius(88.0), Timestamp::from_ns(3)) == ThermalState::HOT);
  CHECK(tg.observe(Celsius(88.0), Timestamp::from_ns(4)) == ThermalState::THERMALLY_CONSTRAINED);
  // recovery only after dropping below hot_max - margin (80 -> recover, then back to normal)
  CHECK(tg.observe(Celsius(79.0), Timestamp::from_ns(5)) == ThermalState::RECOVERING);
  CHECK(tg.observe(Celsius(55.0), Timestamp::from_ns(6)) == ThermalState::NORMAL);
  CHECK(tg.observe(Celsius(70.0), Timestamp::from_ns(7)) == ThermalState::WARM);
}

TEST(energy_provenance_never_blurred) {
  EnergyMeter m;
  m.add(Joules(5.0), EnergyProvenance::MEASURED, Timestamp::from_ns(1));
  m.add(Joules(3.0), EnergyProvenance::ESTIMATED, Timestamp::from_ns(2));
  CHECK(m.total().value() == 8.0);
  CHECK(m.by(EnergyProvenance::MEASURED).value() == 5.0);
  CHECK(m.by(EnergyProvenance::ESTIMATED).value() == 3.0);
  CHECK_THROWS(m.add(Joules(-1.0), EnergyProvenance::MEASURED, Timestamp::from_ns(3)));
}

TEST(instantaneous_avg_peak_distinct) {
  PowerWindow pw(Duration::seconds(1));
  pw.record(Timestamp::from_ns(0), Watts(100.0));
  pw.record(Timestamp::from_ns(500000000LL), Watts(300.0));
  pw.record(Timestamp::from_ns(1000000000LL), Watts(200.0));
  CHECK(pw.peak().value() == 300.0);
  const double avg = pw.average_power();
  CHECK(avg >= 220.0 && avg <= 230.0);
  CHECK(pw.integrated_energy_us().value() >= 220.0 && pw.integrated_energy_us().value() <= 230.0);
}

TEST(burst_is_bounded) {
  BurstLimits bl; bl.enabled = true; bl.allowance = Watts(50.0); bl.window = Duration::seconds(2);
  BurstBudget bb(bl);
  TestClock t; t.advance(Duration::milliseconds(10));
  CHECK(bb.capacity().value() == 100.0);
  CHECK(bb.can_burst(Joules(60.0), t.now()));
  CHECK(bb.consume(Joules(60.0), t.now()));
  CHECK(!bb.can_burst(Joules(60.0), t.now()));  // only 40 left
  CHECK(!bb.exhausted(t.now()));
  CHECK(bb.consume(Joules(40.0), t.now()));
  CHECK(bb.exhausted(t.now()));
  // window refill
  t.advance(Duration::seconds(3));
  CHECK(bb.available(t.now()).value() == 100.0);
}

TEST(freshness_classification) {
  FreshnessClassifier fc;
  TestClock t(Timestamp::from_ns(10000000000LL));  // 10s
  CHECK(fc.classify(t.now(), Timestamp::from_ns(9900000000LL)) == FreshnessState::CURRENT);  // 100ms
  CHECK(fc.classify(t.now(), Timestamp::from_ns(9500000000LL)) == FreshnessState::AGING);    // 500ms
  CHECK(fc.classify(t.now(), Timestamp::from_ns(5000000000LL)) == FreshnessState::STALE);    // 5s
  CHECK(fc.classify(t.now(), Timestamp::from_ns(0)) == FreshnessState::EXPIRED);             // never observed
}

TEST(authority_fencing) {
  AuthorityGate gate(CoordinatorEpoch::first(), WorkerBootId::from_raw(1));
  AuthorityEnvelope e; e.epoch = CoordinatorEpoch::first(); e.boot = WorkerBootId::from_raw(1);
  CHECK(gate.check(e) == AuthorityGate::Verdict::ACCEPT);
  e.epoch = CoordinatorEpoch::first().next();
  CHECK(gate.check(e) == AuthorityGate::Verdict::STALE_EPOCH);
  e.epoch = CoordinatorEpoch::first(); e.boot = WorkerBootId::from_raw(2);
  CHECK(gate.check(e) == AuthorityGate::Verdict::STALE_BOOT);
  e.boot = WorkerBootId::from_raw(1);
  CHECK(gate.check(e) == AuthorityGate::Verdict::ACCEPT);
  gate.set_boot(WorkerBootId::from_raw(9));
  CHECK(gate.check(e) == AuthorityGate::Verdict::STALE_BOOT);
}

TEST(index_lookup_is_consistent) {
  ReservationIndex ri;
  ReservationId r1 = ReservationId::from_raw(1), r2 = ReservationId::from_raw(2);
  BudgetId b = BudgetId::from_raw(10); WorkloadId w = WorkloadId::from_raw(100);
  ri.add(r1, b, w, ReservationState::RESERVED);
  ri.add(r2, b, w, ReservationState::ACTIVE);
  CHECK(ri.by_budget(b).size() == 2);
  CHECK(ri.by_state(ReservationState::ACTIVE).size() == 1);
  ri.set_state(r1, b, w, ReservationState::RELEASED);
  CHECK(ri.by_state(ReservationState::RESERVED).size() == 0);
  ri.remove(r2);
  CHECK(ri.size() == 1);
}

TEST(change_history_append_only) {
  ChangeHistory ch;
  ch.append(ChangeKind::POLICY_CHANGED, "a", Timestamp::from_ns(1));
  ch.append(ChangeKind::BUDGET_CHANGED, "b", Timestamp::from_ns(2));
  CHECK(ch.size() == 2);
  CHECK(ch.entries()[0].seq == 1 && ch.entries()[1].seq == 2);
}

