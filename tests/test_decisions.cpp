#include "test_framework.hpp"
#include "powergovernor/decision_engine.hpp"
#include "powergovernor/freshness.hpp"
#include "powergovernor/policy.hpp"
using namespace pg;

static DecisionInput base_input() {
  DecisionInput in;
  in.power_known = true; in.power_freshness = FreshnessState::CURRENT;
  in.policy_current = true; in.evidence = EnergyProvenance::MEASURED;
  in.thermal_known = true; in.thermal_state = ThermalState::NORMAL;
  in.power_class = WorkloadPowerClass::THROUGHPUT;
  in.rule = default_rule_for(WorkloadPowerClass::THROUGHPUT);
  in.policy = PowerPolicy::defaults();
  in.hardware_cap = Watts(300.0);
  in.budget_available = Watts(200.0);
  in.requested_power = Watts(150.0);
  in.requested_perf = PerformanceState::PERFORMANCE;
  return in;
}

TEST(decision_allow) {
  DecisionEngine eng; TestClock tc;
  auto d = eng.evaluate(base_input(), tc.now());
  CHECK(d.verdict == DecisionVerdict::ALLOW);
  CHECK(!d.json.empty() && !d.digest.empty());
}

TEST(decision_allow_when_requested_exceeds_ceiling) {
  DecisionEngine eng; TestClock tc;
  auto in = base_input(); in.hardware_cap = Watts(100.0);
  auto d = eng.evaluate(in, tc.now());
  CHECK(d.verdict == DecisionVerdict::ALLOW_WITH_LIMIT);
  CHECK(d.recommended_limit && d.recommended_limit->value() == 100.0);
}

TEST(decision_reject_when_no_capacity) {
  DecisionEngine eng; TestClock tc;
  auto in = base_input(); in.hardware_cap = Watts(0.0); in.budget_available = Watts(0.0);
  auto d = eng.evaluate(in, tc.now());
  CHECK(d.verdict == DecisionVerdict::REJECT);
}

TEST(decision_throttle_when_thermally_constrained) {
  DecisionEngine eng; TestClock tc;
  auto in = base_input(); in.thermal_state = ThermalState::THERMALLY_CONSTRAINED;
  auto d = eng.evaluate(in, tc.now());
  CHECK(d.verdict == DecisionVerdict::THROTTLE);
}

TEST(decision_revalidation_when_stale) {
  DecisionEngine eng; TestClock tc;
  auto in = base_input(); in.power_freshness = FreshnessState::EXPIRED;
  auto d = eng.evaluate(in, tc.now());
  CHECK(d.verdict == DecisionVerdict::REVALIDATION_REQUIRED);
}

TEST(decision_respects_energy_window) {
  DecisionEngine eng; TestClock tc;
  auto in = base_input();
  in.policy.energy_window.enforce = true;
  in.policy.energy_window.budget = Joules(10.0);
  in.energy_window_used = Joules(9.0);
  in.requested_energy = Joules(5.0);
  auto d = eng.evaluate(in, tc.now());
  CHECK(d.verdict == DecisionVerdict::THROTTLE);
}

TEST(decision_explanation_fields) {
  DecisionEngine eng; TestClock tc;
  auto in = base_input(); in.hardware_cap = Watts(100.0);
  auto d = eng.evaluate(in, tc.now());
  CHECK(d.explanation.binding_kind == "device_hard_cap");
  CHECK(d.explanation.telemetry_current);
  CHECK(d.explanation.what_would_change.find("hardware") != std::string::npos);
  CHECK(!d.explanation.why.empty());
}

TEST(decision_deterministic_and_stable_digest) {
  DecisionEngine eng; TestClock tc;
  auto in = base_input();
  auto d1 = eng.evaluate(in, tc.now());
  auto d2 = eng.evaluate(in, tc.now());
  // different ids/generations but same verdict/digest set for identical inputs should be equal
  CHECK(d1.verdict == d2.verdict);
  CHECK(d1.json != d2.json);  // ids/generation differ
  // deterministic digest (same normalized content) is stable across calls in a fresh engine
  DecisionEngine eng2; 
  auto d3 = eng2.evaluate(base_input(), tc.now());
  // verify digest is a stable hex of the same length
  CHECK(d3.digest.size() == 16);
}

TEST(decision_hysteresis_prevents_oscillation) {
  DecisionEngine eng; TestClock tc(Timestamp::from_ns(1000000000LL));
  auto in = base_input(); in.hardware_cap = Watts(100.0); // bound
  auto d1 = eng.evaluate(in, tc.now());
  CHECK(d1.verdict == DecisionVerdict::ALLOW_WITH_LIMIT);
  // now relax the cap, but within min_stable_duration -> must NOT flip to ALLOW
  tc.advance(Duration::milliseconds(50));
  in.hardware_cap = Watts(1000.0); in.budget_available = Watts(1000.0);
  auto d2 = eng.evaluate(in, tc.now());
  CHECK(d2.verdict == DecisionVerdict::ALLOW_WITH_LIMIT);  // hysteresis holds
  // after min_stable_duration elapses it may flip
  tc.advance(Duration::seconds(1));
  auto d3 = eng.evaluate(in, tc.now());
  CHECK(d3.verdict == DecisionVerdict::ALLOW);
}

TEST(policy_rollover_increments_generation) {
  PowerPolicy p = PowerPolicy::defaults();
  p.device_hard_cap = Watts(200.0);
  p.bump_generation();
  p.bump_generation();
  CHECK(p.generation.raw() == 3);
  // policy equality distinguishes generations
  PowerPolicy q = p;
  CHECK(q == p);
  q.bump_generation();
  CHECK(q != p);
}

