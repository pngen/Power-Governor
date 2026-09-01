// Power Governor - deterministic decision engine and explainability.
//
// The engine is a pure, deterministic function of its inputs plus a bounded hysteresis state. It is
// not a black-box ML model: every decision lists exact, ordered reasons, the binding constraint,
// the evidence provenance, and what would have to change for a different decision. Decisions
// produce a deterministic text explanation, a deterministic JSON document, and a stable digest.
#pragma once
#include <optional>
#include <string>
#include <vector>
#include <limits>
#include "clock.hpp"
#include "energy.hpp"
#include "freshness.hpp"
#include "ids.hpp"
#include "perf_state.hpp"
#include "policy.hpp"
#include "thermal.hpp"
#include "units.hpp"
#include "util.hpp"
#include "workload_class.hpp"

namespace pg {

enum class DecisionVerdict : std::uint8_t {
  ALLOW, ALLOW_WITH_LIMIT, THROTTLE, DEFER, DRAIN_RECOMMENDED,
  REVALIDATION_REQUIRED, REJECT, UNKNOWN
};

inline std::string_view to_string(DecisionVerdict v) noexcept {
  switch (v) {
    case DecisionVerdict::ALLOW: return "ALLOW";
    case DecisionVerdict::ALLOW_WITH_LIMIT: return "ALLOW_WITH_LIMIT";
    case DecisionVerdict::THROTTLE: return "THROTTLE";
    case DecisionVerdict::DEFER: return "DEFER";
    case DecisionVerdict::DRAIN_RECOMMENDED: return "DRAIN_RECOMMENDED";
    case DecisionVerdict::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
    case DecisionVerdict::REJECT: return "REJECT";
    case DecisionVerdict::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}

inline DecisionVerdict verdict_from_string(std::string_view s) {
  if (s == "ALLOW") return DecisionVerdict::ALLOW;
  if (s == "ALLOW_WITH_LIMIT") return DecisionVerdict::ALLOW_WITH_LIMIT;
  if (s == "THROTTLE") return DecisionVerdict::THROTTLE;
  if (s == "DEFER") return DecisionVerdict::DEFER;
  if (s == "DRAIN_RECOMMENDED") return DecisionVerdict::DRAIN_RECOMMENDED;
  if (s == "REVALIDATION_REQUIRED") return DecisionVerdict::REVALIDATION_REQUIRED;
  if (s == "REJECT") return DecisionVerdict::REJECT;
  if (s == "UNKNOWN") return DecisionVerdict::UNKNOWN;
  throw std::invalid_argument("pg::verdict_from_string: unknown verdict");
}

// Everything the engine needs, with explicit "_known" flags so unavailable telemetry is never
// silently treated as a real zero.
struct DecisionInput {
  Watts current_power{0.0};    bool power_known = false;
  Watts average_power{0.0};    bool average_known = false;
  Joules energy_window_used{0.0};  bool energy_known = false;

  Watts requested_power{0.0};
  Joules requested_energy{0.0};
  double requested_duration_s = 0.0;

  std::optional<Watts> hardware_cap;      // device hardware power limit
  std::optional<Watts> budget_available;  // after parent/enforced constraints
  std::optional<Watts> soft_target;
  std::optional<Watts> thermal_ceiling;

  ThermalState thermal_state = ThermalState::UNKNOWN;
  bool thermal_known = false;
  Utilization utilization{0.0};  bool utilization_known = false;

  WorkloadPowerClass power_class = WorkloadPowerClass::THROUGHPUT;
  WorkloadClassRule rule;                 // set by caller (default_rule_for handles fill-in)
  PowerPolicy policy;

  bool policy_current = false;
  FreshnessState power_freshness = FreshnessState::EXPIRED;
  EnergyProvenance evidence = EnergyProvenance::UNKNOWN;

  bool hardware_throttled = false;
  bool in_thermal_recovery = false;

  bool burst_available = false;
  double burst_utilization = 0.0;

  PerformanceState requested_perf = PerformanceState::BALANCED;

  std::optional<BudgetId> budget_applied;
  std::vector<BudgetId> parent_budgets;
  std::size_t active_reservations = 0;
};

struct DecisionExplanation {
  std::optional<BudgetId> budget_applied;
  std::vector<BudgetId> parent_budgets;
  std::size_t active_reservations = 0;

  ThermalState thermal_state = ThermalState::UNKNOWN;
  WorkloadPowerClass power_class = WorkloadPowerClass::THROUGHPUT;
  bool burst_available = false;
  double burst_utilization = 0.0;

  std::optional<Watts> binding_limit;
  std::string binding_kind;
  FreshnessState telemetry_freshness = FreshnessState::EXPIRED;
  bool telemetry_current = false;
  EnergyProvenance evidence = EnergyProvenance::UNKNOWN;
  bool hardware_throttled = false;

  std::string why;
  std::string what_would_change;
};

struct Decision {
  DecisionId id;
  DecisionGeneration generation;
  Timestamp now;
  DecisionVerdict verdict = DecisionVerdict::UNKNOWN;
  std::vector<std::string> reasons;
  PerformanceState effective_state = PerformanceState::BALANCED;
  std::optional<Watts> recommended_limit;
  DecisionExplanation explanation;
  std::string json;
  std::string digest;

  bool operator==(const Decision& o) const noexcept {
    return verdict == o.verdict && recommended_limit == o.recommended_limit &&
           generation == o.generation && digest == o.digest;
  }
};

// Bounded, deterministic hysteresis: after a restrictive verdict, the engine refuses to flip to a
// less restrictive verdict within min_stable_duration unless a hard constraint was removed.
class DecisionEngine {
 public:
  explicit DecisionEngine(DecisionGeneration gen = DecisionGeneration::first()) : gen_(gen) {}

  Decision evaluate(const DecisionInput& in, Timestamp now);

  DecisionGeneration generation() const noexcept { return gen_; }
  std::optional<DecisionVerdict> last_verdict() const noexcept { return last_ ? std::optional<DecisionVerdict>(last_->verdict) : std::nullopt; }
  void reset_hysteresis() noexcept { last_.reset(); }

 private:
  Decision make_verdict(const DecisionInput& in, Timestamp now, DecisionVerdict verdict,
                        std::vector<std::string> reasons,
                        std::optional<Watts> limit, const std::string& binding_kind);
  void fill_explanation(const DecisionInput& in, DecisionExplanation& ex,
                        const std::vector<std::string>& reasons) const;
  std::string build_json(const Decision& d) const;
  void persist_hysteresis(DecisionVerdict v, Timestamp now) noexcept { last_ = Last{v, now}; }

  struct Last { DecisionVerdict verdict; Timestamp at; };
  std::optional<Last> last_;
  DecisionGeneration gen_;
};

inline Decision DecisionEngine::evaluate(const DecisionInput& in, Timestamp now) {
  std::vector<std::string> reasons;
  DecisionVerdict verdict = DecisionVerdict::ALLOW;
  std::optional<Watts> limit;
  std::string binding_kind;

  // --- 1. Freshness / revalidation gate -----------------------------------
  const bool stale = (in.power_freshness == FreshnessState::STALE ||
                      in.power_freshness == FreshnessState::EXPIRED);
  if (!in.policy_current || stale) {
    reasons.push_back("telemetry is not current (freshness=" + std::string(to_string(in.power_freshness)) + ")");
    if (!in.policy_current) reasons.push_back("policy generation is not current");
    verdict = DecisionVerdict::REVALIDATION_REQUIRED;
    return make_verdict(in, now, verdict, reasons, std::nullopt, "freshness");
  }

  const bool thermal_known = in.thermal_known;
  const bool constrained = thermal_known &&
      (in.thermal_state == ThermalState::THERMALLY_CONSTRAINED);
  const bool recovering = thermal_known &&
      (in.thermal_state == ThermalState::RECOVERING);
  const bool hot = thermal_known && in.thermal_state == ThermalState::HOT;

  // --- 2. Compute the hard effective ceiling ------------------------------
  auto consider = [&](const std::optional<Watts>& cap, const char* kind) {
    if (cap && cap->is_non_negative()) {
      if (!limit || cap->value() < limit->value()) {
        limit = cap;
        binding_kind = kind;
      }
    }
  };
  consider(in.hardware_cap, "device_hard_cap");
  consider(in.budget_available, "budget");
  consider(in.thermal_ceiling, "thermal");

  const double requested = in.requested_power.value();
  const double ceiling = limit ? limit->value() : std::numeric_limits<double>::infinity();

  // --- 3. No capacity ------------------------------------------------------
  if (ceiling < requested && ceiling <= 1e-9) {
    reasons.push_back("no available power capacity");
    if (constrained) { reasons.push_back("device is thermally constrained"); verdict = DecisionVerdict::DEFER; }
    else { verdict = DecisionVerdict::REJECT; }
    return make_verdict(in, now, verdict, reasons, limit, binding_kind);
  }

  // --- 4. Requested exceeds hard ceiling ----------------------------------
  if (requested > ceiling + 1e-9) {
    reasons.push_back("requested power " + in.requested_power.to_string() +
                      " exceeds available ceiling " + std::to_string(ceiling) + "W");
    if (in.rule.priority < 40 && constrained) {
      verdict = DecisionVerdict::THROTTLE;
      reasons.push_back("low-priority class under thermal constraint");
    } else if (constrained) {
      verdict = DecisionVerdict::THROTTLE;
      reasons.push_back("device thermally constrained");
    } else if (recovering) {
      verdict = DecisionVerdict::ALLOW_WITH_LIMIT;
      reasons.push_back("thermal recovery in progress; limited to thermal ceiling");
    } else {
      verdict = DecisionVerdict::ALLOW_WITH_LIMIT;
      reasons.push_back("hard ceiling constrains execution");
    }
    return make_verdict(in, now, verdict, reasons, limit, binding_kind);
  }

  // --- 5. Under hard ceiling: thermal nuance ------------------------------
  if (constrained) {
    verdict = DecisionVerdict::THROTTLE;
    reasons.push_back("device thermally constrained");
    return make_verdict(in, now, verdict, reasons, limit, binding_kind);
  }
  if (recovering) {
    verdict = DecisionVerdict::ALLOW_WITH_LIMIT;
    reasons.push_back("thermal recovery in progress; limited to ceiling");
    return make_verdict(in, now, verdict, reasons, limit, binding_kind);
  }

  // --- 6. Soft target / burst ---------------------------------------------
  if (in.soft_target && requested > in.soft_target->value()) {
    if (in.burst_available && requested <= ceiling) {
      verdict = DecisionVerdict::ALLOW;
      reasons.push_back("burst budget covers temporary over-soft-target demand");
    } else {
      verdict = DecisionVerdict::ALLOW_WITH_LIMIT;
      limit = in.soft_target;
      binding_kind = "soft_target";
      reasons.push_back("requested power exceeds soft target; limited (no burst capacity)");
    }
    return make_verdict(in, now, verdict, reasons, limit, binding_kind);
  }

  // --- 7. Energy window ----------------------------------------------------
  if (in.policy.energy_window.enforce && in.policy.energy_window.budget.is_positive()) {
    const double remaining = in.policy.energy_window.budget.value() - in.energy_window_used.value();
    if (in.requested_energy.value() > remaining + 1e-9) {
      verdict = DecisionVerdict::THROTTLE;
      reasons.push_back("energy window would be exceeded");
      return make_verdict(in, now, verdict, reasons, limit, binding_kind);
    }
  }

  // --- 8. Default allow ----------------------------------------------------
  verdict = DecisionVerdict::ALLOW;
  if (hot) { reasons.push_back("hot but within hard ceiling; monitoring closely"); }
  if (in.hardware_throttled) { reasons.push_back("hardware is already throttled"); }

  // --- 9. Hysteresis stability --------------------------------------------
  if (last_ && last_->verdict != DecisionVerdict::ALLOW &&
      now.elapsed_since(last_->at) < in.policy.min_stable_duration) {
    // Do not oscillate back to ALLOW too quickly after a throttle.
    const Duration dt = now.elapsed_since(last_->at);
    reasons.push_back("hysteresis holding previous restrictive verdict for " + dt.to_string());
    verdict = last_->verdict;
    if (verdict == DecisionVerdict::ALLOW_WITH_LIMIT && limit == std::nullopt) {
      limit = in.soft_target;
      binding_kind = "soft_target";
    }
  }

  return make_verdict(in, now, verdict, reasons, limit, binding_kind);
}

inline void DecisionEngine::fill_explanation(const DecisionInput& in, DecisionExplanation& ex,
                                             const std::vector<std::string>& reasons) const {
  ex.budget_applied = in.budget_applied;
  ex.parent_budgets = in.parent_budgets;
  ex.active_reservations = in.active_reservations;
  ex.thermal_state = in.thermal_state;
  ex.power_class = in.power_class;
  ex.burst_available = in.burst_available;
  ex.burst_utilization = in.burst_utilization;
  ex.telemetry_freshness = in.power_freshness;
  ex.telemetry_current = in.policy_current && in.power_freshness == FreshnessState::CURRENT;
  ex.evidence = in.evidence;
  ex.hardware_throttled = in.hardware_throttled;

  std::string why;
  for (const auto& r : reasons) { if (!why.empty()) why += "; "; why += r; }
  ex.why = why.empty() ? "no binding constraint" : why;

  std::string change;
  if (!ex.telemetry_current) {
    change = "refresh telemetry to CURRENT and re-evaluate";
  } else if (ex.binding_kind.empty()) {
    change = "no binding constraint; a more restrictive policy/budget would throttle";
  } else if (ex.binding_kind == "device_hard_cap") {
    change = "raise the hardware power limit (within vendor support) to allow more";
  } else if (ex.binding_kind == "budget") {
    change = "increase budget capacity or release reservations to allow more";
  } else if (ex.binding_kind == "thermal") {
    change = "reduce temperature and allow thermal recovery to raise the ceiling";
  } else if (ex.binding_kind == "soft_target") {
    change = "raise the soft target or grant burst capacity to allow more";
  } else if (ex.binding_kind == "freshness") {
    change = "ingest fresh telemetry and re-evaluate";
  } else {
    change = "relieve the binding constraint to allow more";
  }
  ex.what_would_change = change;
}

inline Decision DecisionEngine::make_verdict(const DecisionInput& in, Timestamp now,
                                             DecisionVerdict verdict,
                                             std::vector<std::string> reasons,
                                             std::optional<Watts> limit,
                                             const std::string& binding_kind) {
  Decision d;
  d.id = DecisionId::allocate();
  d.generation = gen_.next();
  d.now = now;
  d.verdict = verdict;
  d.reasons = reasons;
  d.recommended_limit = limit;

  switch (verdict) {
    case DecisionVerdict::ALLOW: d.effective_state = in.requested_perf; break;
    case DecisionVerdict::ALLOW_WITH_LIMIT:
      d.effective_state = PerformanceState::BALANCED; break;
    case DecisionVerdict::THROTTLE:
    case DecisionVerdict::DEFER:
    case DecisionVerdict::DRAIN_RECOMMENDED:
    case DecisionVerdict::REVALIDATION_REQUIRED:
    case DecisionVerdict::REJECT:
      d.effective_state = PerformanceState::THROTTLED; break;
    case DecisionVerdict::UNKNOWN: d.effective_state = PerformanceState::BALANCED; break;
  }

  // Set binding kind BEFORE filling the explanation so what_would_change reflects the real
  // binding constraint.
  d.explanation.binding_limit = limit;
  d.explanation.binding_kind = binding_kind;
  fill_explanation(in, d.explanation, reasons);
  d.json = build_json(d);
  d.digest = digest_hex(d.json);
  persist_hysteresis(verdict, now);
  return d;
}

inline std::string DecisionEngine::build_json(const Decision& d) const {
  JsonWriter w;
  w.begin_object();
  w.raw_key("id"); w.raw_value("\"" + d.id.to_string() + "\"");
  w.raw_key("generation"); w.raw_value(d.generation.to_string());
  w.raw_key("verdict"); w.raw_value("\"" + std::string(to_string(d.verdict)) + "\"");
  w.raw_key("effective_state"); w.raw_value("\"" + std::string(to_string(d.effective_state)) + "\"");
  if (d.recommended_limit) {
    w.raw_key("recommended_limit_w"); w.raw_value(std::to_string(d.recommended_limit->value()));
  } else { w.raw_key("recommended_limit_w"); w.raw_value("null"); }
  w.raw_key("binding_kind"); w.raw_value("\"" + d.explanation.binding_kind + "\"");
  w.raw_key("telemetry_current"); w.raw_value(d.explanation.telemetry_current ? "true" : "false");
  w.raw_key("telemetry_freshness"); w.raw_value("\"" + std::string(to_string(d.explanation.telemetry_freshness)) + "\"");
  w.raw_key("thermal_state"); w.raw_value("\"" + std::string(to_string(d.explanation.thermal_state)) + "\"");
  w.raw_key("power_class"); w.raw_value("\"" + std::string(to_string(d.explanation.power_class)) + "\"");
  w.raw_key("burst_available"); w.raw_value(d.explanation.burst_available ? "true" : "false");
  w.raw_key("evidence"); w.raw_value("\"" + std::string(to_string(d.explanation.evidence)) + "\"");
  w.raw_key("why"); w.raw_value("\"" + json_escape(d.explanation.why) + "\"");
  w.raw_key("what_would_change"); w.raw_value("\"" + json_escape(d.explanation.what_would_change) + "\"");
  w.raw_key("reasons");
  w.begin_array();
  for (const auto& r : d.reasons) w.value(r);
  w.end_array();
  w.end_object();
  return w.str();
}

}  // namespace pg
