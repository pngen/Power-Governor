// Power Governor - typed power policy.
//
// A PowerPolicy controls hard caps, soft targets, energy windows, burst behavior, thermal
// thresholds, priorities, reserves, oversubscription, degraded-mode behavior, hardware-control
// permissions, and freshness/hysteresis timing. Every update advances the PolicyGeneration; old
// decisions and reservations under a stale policy are never silently authoritative unless a caller
// explicitly grand-fathers them through the authority envelope.
#pragma once
#include <optional>
#include <string_view>
#include <unordered_map>
#include "ids.hpp"
#include "thermal.hpp"
#include "units.hpp"
#include "workload_class.hpp"

namespace pg {

struct EnergyWindow {
  Joules budget{0.0};
  Duration window{Duration::seconds(1)};
  bool enforce = false;

  bool operator==(const EnergyWindow& o) const noexcept {
    return budget == o.budget && window == o.window && enforce == o.enforce;
  }
};

struct BurstLimits {
  Watts allowance{0.0};
  Duration window{Duration::seconds(1)};
  Duration refill{Duration::seconds(1)};
  bool enabled = false;

  bool operator==(const BurstLimits& o) const noexcept {
    return allowance == o.allowance && window == o.window && refill == o.refill && enabled == o.enabled;
  }
};

struct OversubscriptionPolicy {
  bool allowed = false;
  double max_overcommit_factor = 1.0;  // 1.0 = no overcommit; 1.2 = up to 20%
  bool require_authority = true;

  bool operator==(const OversubscriptionPolicy& o) const noexcept {
    return allowed == o.allowed && max_overcommit_factor == o.max_overcommit_factor &&
           require_authority == o.require_authority;
  }
};

enum class DegradedMode : std::uint8_t {
  REJECT_NEW, THROTTLE_ALL, DRAIN, UNCHANGED
};

inline std::string_view to_string(DegradedMode m) noexcept {
  switch (m) {
    case DegradedMode::REJECT_NEW: return "REJECT_NEW";
    case DegradedMode::THROTTLE_ALL: return "THROTTLE_ALL";
    case DegradedMode::DRAIN: return "DRAIN";
    case DegradedMode::UNCHANGED: return "UNCHANGED";
  }
  return "UNKNOWN";
}

struct HardwareControlPermissions {
  bool set_power_limit = false;   // may apply a vendor-supported power limit
  bool set_clocks = false;        // may apply a vendor-supported clock control

  bool operator==(const HardwareControlPermissions& o) const noexcept {
    return set_power_limit == o.set_power_limit && set_clocks == o.set_clocks;
  }
};

// A complete, validated governance policy.
struct PowerPolicy {
  PolicyId id;
  PolicyGeneration generation;
  std::string name;

  // Hard caps (the absolute bound). nullopt => unconstrained by this level.
  std::optional<Watts> fleet_hard_cap;
  std::optional<Watts> node_hard_cap;
  std::optional<Watts> device_hard_cap;
  std::optional<Watts> soft_target;      // preference, not a hard bound

  EnergyWindow energy_window;
  BurstLimits burst_limits;
  ThermalThresholds thermal_thresholds;

  int default_priority = 50;             // 0 lowest .. 100 highest
  double min_reserve_fraction = 0.10;    // fraction of each budget reserved against exhaustion

  OversubscriptionPolicy oversubscription;
  DegradedMode degraded_mode = DegradedMode::UNCHANGED;
  HardwareControlPermissions hardware_controls;

  // Freshness / hysteresis timing.
  Duration observation_freshness{Duration::milliseconds(250)};
  Duration observation_stale{Duration::seconds(1)};
  Duration observation_expired{Duration::seconds(10)};
  Duration min_stable_duration{Duration::milliseconds(200)};   // anti-oscillation floor
  Duration decision_hysteresis{Duration::milliseconds(150)};
  Duration recovery_threshold{Duration::seconds(2)};           // time to observe recovery

  // Per-class rules. Callers may override any class; unknown classes fall back to defaults.
  std::unordered_map<WorkloadPowerClass, WorkloadClassRule> class_rules;

  bool operator==(const PowerPolicy& o) const noexcept {
    return id == o.id && generation == o.generation && name == o.name &&
           fleet_hard_cap == o.fleet_hard_cap && node_hard_cap == o.node_hard_cap &&
           device_hard_cap == o.device_hard_cap && soft_target == o.soft_target &&
           energy_window == o.energy_window && burst_limits == o.burst_limits &&
           default_priority == o.default_priority &&
           min_reserve_fraction == o.min_reserve_fraction &&
           oversubscription == o.oversubscription && degraded_mode == o.degraded_mode &&
           hardware_controls == o.hardware_controls &&
           observation_freshness == o.observation_freshness &&
           observation_stale == o.observation_stale &&
           observation_expired == o.observation_expired &&
           min_stable_duration == o.min_stable_duration &&
           decision_hysteresis == o.decision_hysteresis &&
           recovery_threshold == o.recovery_threshold;
  }
  bool operator!=(const PowerPolicy& o) const noexcept { return !(*this == o); }

  // Build a fresh policy with default values and a new generation.
  static PowerPolicy defaults(PolicyId id = PolicyId::allocate(), std::string name = "default") {
    PowerPolicy p;
    p.id = id;
    p.generation = PolicyGeneration::first();
    p.name = std::move(name);
    // Populate per-class default rules.
    for (auto c : {WorkloadPowerClass::LATENCY_CRITICAL, WorkloadPowerClass::THROUGHPUT,
                   WorkloadPowerClass::BATCH, WorkloadPowerClass::BACKGROUND,
                   WorkloadPowerClass::BEST_EFFORT, WorkloadPowerClass::ENERGY_OPTIMIZED,
                   WorkloadPowerClass::THERMALLY_SENSITIVE, WorkloadPowerClass::CUSTOM}) {
      p.class_rules[c] = default_rule_for(c);
    }
    return p;
  }

  // Advance the generation and return a reference to *this (so bump() chains).
  PowerPolicy& bump_generation() noexcept {
    generation = generation.next();
    return *this;
  }

  const WorkloadClassRule& rule_for(WorkloadPowerClass cls) const {
    auto it = class_rules.find(cls);
    if (it != class_rules.end()) return it->second;
    static const WorkloadClassRule default_rule = default_rule_for(WorkloadPowerClass::CUSTOM);
    return default_rule;
  }

  // Validate invariants and ranges; throws on violation.
  void validate() const;
};

inline void PowerPolicy::validate() const {
  if (min_reserve_fraction < 0.0 || min_reserve_fraction > 1.0) {
    throw std::invalid_argument("pg::PowerPolicy::validate: min_reserve_fraction outside [0,1]");
  }
  if (oversubscription.max_overcommit_factor < 1.0) {
    throw std::invalid_argument("pg::PowerPolicy::validate: max_overcommit_factor < 1");
  }
  if (energy_window.enforce && energy_window.budget.is_zero()) {
    throw std::invalid_argument("pg::PowerPolicy::validate: enforced energy window has zero budget");
  }
}

}  // namespace pg
