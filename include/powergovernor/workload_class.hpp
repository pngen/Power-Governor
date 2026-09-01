// Power Governor - workload power classes and per-class governance rules.
#pragma once
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>
#include "units.hpp"

namespace pg {

enum class WorkloadPowerClass : std::uint8_t {
  LATENCY_CRITICAL, THROUGHPUT, BATCH, BACKGROUND,
  BEST_EFFORT, ENERGY_OPTIMIZED, THERMALLY_SENSITIVE, CUSTOM
};

inline std::string_view to_string(WorkloadPowerClass c) noexcept {
  switch (c) {
    case WorkloadPowerClass::LATENCY_CRITICAL: return "LATENCY_CRITICAL";
    case WorkloadPowerClass::THROUGHPUT:       return "THROUGHPUT";
    case WorkloadPowerClass::BATCH:            return "BATCH";
    case WorkloadPowerClass::BACKGROUND:       return "BACKGROUND";
    case WorkloadPowerClass::BEST_EFFORT:      return "BEST_EFFORT";
    case WorkloadPowerClass::ENERGY_OPTIMIZED: return "ENERGY_OPTIMIZED";
    case WorkloadPowerClass::THERMALLY_SENSITIVE: return "THERMALLY_SENSITIVE";
    case WorkloadPowerClass::CUSTOM:           return "CUSTOM";
  }
  return "UNKNOWN";
}

inline WorkloadPowerClass workload_class_from_string(std::string_view s) {
  if (s == "LATENCY_CRITICAL") return WorkloadPowerClass::LATENCY_CRITICAL;
  if (s == "THROUGHPUT") return WorkloadPowerClass::THROUGHPUT;
  if (s == "BATCH") return WorkloadPowerClass::BATCH;
  if (s == "BACKGROUND") return WorkloadPowerClass::BACKGROUND;
  if (s == "BEST_EFFORT") return WorkloadPowerClass::BEST_EFFORT;
  if (s == "ENERGY_OPTIMIZED") return WorkloadPowerClass::ENERGY_OPTIMIZED;
  if (s == "THERMALLY_SENSITIVE") return WorkloadPowerClass::THERMALLY_SENSITIVE;
  if (s == "CUSTOM") return WorkloadPowerClass::CUSTOM;
  throw std::invalid_argument("pg::workload_class_from_string: unknown class");
}

// Per-class governance rules. These influence governance decisions but never replace the
// scheduler: Power Governor decides how much the workload may consume, not whether it runs.
struct WorkloadClassRule {
  WorkloadPowerClass cls = WorkloadPowerClass::THROUGHPUT;
  int priority = 50;                          // 0 lowest .. 100 highest
  std::optional<Watts> max_power_share;       // cap on share of a domain's power
  std::optional<Joules> energy_target;        // per unit of useful work
  bool burst_eligible = false;
  int latency_sensitivity = 50;               // 0 .. 100
  int throttling_tolerance = 50;              // 0 .. 100 (higher tolerates more throttling)
  double min_service_level = 0.0;             // 0 .. 1

  bool operator==(const WorkloadClassRule& o) const noexcept {
    return cls == o.cls && priority == o.priority &&
           max_power_share == o.max_power_share &&
           energy_target == o.energy_target &&
           burst_eligible == o.burst_eligible &&
           latency_sensitivity == o.latency_sensitivity &&
           throttling_tolerance == o.throttling_tolerance &&
           min_service_level == o.min_service_level;
  }
  bool operator!=(const WorkloadClassRule& o) const noexcept { return !(*this == o); }
};

inline WorkloadClassRule default_rule_for(WorkloadPowerClass c) {
  WorkloadClassRule r;
  r.cls = c;
  switch (c) {
    case WorkloadPowerClass::LATENCY_CRITICAL:
      r.priority = 100; r.burst_eligible = true; r.latency_sensitivity = 100;
      r.throttling_tolerance = 5; r.min_service_level = 0.95; break;
    case WorkloadPowerClass::THROUGHPUT:
      r.priority = 70; r.burst_eligible = false; r.latency_sensitivity = 40;
      r.throttling_tolerance = 40; r.min_service_level = 0.8; break;
    case WorkloadPowerClass::BATCH:
      r.priority = 55; r.burst_eligible = false; r.latency_sensitivity = 20;
      r.throttling_tolerance = 70; r.min_service_level = 0.5; break;
    case WorkloadPowerClass::BACKGROUND:
      r.priority = 25; r.burst_eligible = false; r.latency_sensitivity = 5;
      r.throttling_tolerance = 90; r.min_service_level = 0.2; break;
    case WorkloadPowerClass::BEST_EFFORT:
      r.priority = 10; r.burst_eligible = false; r.latency_sensitivity = 5;
      r.throttling_tolerance = 100; r.min_service_level = 0.0; break;
    case WorkloadPowerClass::ENERGY_OPTIMIZED:
      r.priority = 60; r.burst_eligible = false; r.latency_sensitivity = 30;
      r.throttling_tolerance = 60; r.min_service_level = 0.6; break;
    case WorkloadPowerClass::THERMALLY_SENSITIVE:
      r.priority = 85; r.burst_eligible = false; r.latency_sensitivity = 90;
      r.throttling_tolerance = 10; r.min_service_level = 0.9; break;
    case WorkloadPowerClass::CUSTOM:
      break;
  }
  return r;
}

}  // namespace pg
