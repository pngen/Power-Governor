// Power Governor - requested, effective, observed, and inferred performance states.
#pragma once
#include <stdexcept>
#include <string_view>
#include "units.hpp"

namespace pg {

// Abstract performance states. The runtime never assumes it can directly control hardware
// P-states: it may request/recommend through a backend, and must report when control is absent.
enum class PerformanceState : std::uint8_t {
  ECO, EFFICIENT, BALANCED, PERFORMANCE, MAX_PERFORMANCE, THROTTLED, RECOVERY
};

inline std::string_view to_string(PerformanceState s) noexcept {
  switch (s) {
    case PerformanceState::ECO: return "ECO";
    case PerformanceState::EFFICIENT: return "EFFICIENT";
    case PerformanceState::BALANCED: return "BALANCED";
    case PerformanceState::PERFORMANCE: return "PERFORMANCE";
    case PerformanceState::MAX_PERFORMANCE: return "MAX_PERFORMANCE";
    case PerformanceState::THROTTLED: return "THROTTLED";
    case PerformanceState::RECOVERY: return "RECOVERY";
  }
  return "UNKNOWN";
}

inline PerformanceState perf_state_from_string(std::string_view s) {
  if (s == "ECO") return PerformanceState::ECO;
  if (s == "EFFICIENT") return PerformanceState::EFFICIENT;
  if (s == "BALANCED") return PerformanceState::BALANCED;
  if (s == "PERFORMANCE") return PerformanceState::PERFORMANCE;
  if (s == "MAX_PERFORMANCE") return PerformanceState::MAX_PERFORMANCE;
  if (s == "THROTTLED") return PerformanceState::THROTTLED;
  if (s == "RECOVERY") return PerformanceState::RECOVERY;
  throw std::invalid_argument("pg::perf_state_from_string: unknown state");
}

// A performance-state snapshot separates the four distinct concepts that are often conflated:
// requested (what we want), effective (what the policy computes), observed (what a backend
// reports), and inferred throttling (a derived truth about hardware limit engagement).
struct PerformanceStateSet {
  PerformanceState requested = PerformanceState::BALANCED;
  PerformanceState effective = PerformanceState::BALANCED;
  PerformanceState observed = PerformanceState::BALANCED;
  bool throttling_inferred = false;

  bool operator==(const PerformanceStateSet& o) const noexcept {
    return requested == o.requested && effective == o.effective &&
           observed == o.observed && throttling_inferred == o.throttling_inferred;
  }
};

}  // namespace pg
