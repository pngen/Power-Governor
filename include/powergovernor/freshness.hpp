// Power Governor - observation freshness evaluation.
#pragma once
#include <string_view>
#include "clock.hpp"

namespace pg {

enum class FreshnessState : std::uint8_t { CURRENT, AGING, STALE, EXPIRED };

inline std::string_view to_string(FreshnessState f) noexcept {
  switch (f) {
    case FreshnessState::CURRENT: return "CURRENT";
    case FreshnessState::AGING: return "AGING";
    case FreshnessState::STALE: return "STALE";
    case FreshnessState::EXPIRED: return "EXPIRED";
  }
  return "UNKNOWN";
}

// Deterministic freshness classifier. A source observation is CURRENT while its age is below the
// fresh threshold, AGING below the stale threshold, STALE below the expired threshold, and
// EXPIRED at or beyond it. Stale and expired observations must not drive authoritative decisions
// unless a policy explicitly tolerates them.
struct FreshnessThresholds {
  Duration fresh = Duration::milliseconds(250);
  Duration stale = Duration::seconds(1);
  Duration expired = Duration::seconds(10);
};

class FreshnessClassifier {
 public:
  explicit FreshnessClassifier(FreshnessThresholds t = {}) : t_(t) {}
  FreshnessThresholds thresholds() const noexcept { return t_; }

  FreshnessState classify(Timestamp now, Timestamp observed) const noexcept {
    const Duration age = age_of(now, observed);
    if (age.is_zero() && observed.is_zero()) return FreshnessState::EXPIRED;  // never observed
    return age_from_duration(age);
  }
  FreshnessState age_from_duration(Duration age) const noexcept {
    if (age < t_.fresh) return FreshnessState::CURRENT;
    if (age < t_.stale) return FreshnessState::AGING;
    if (age < t_.expired) return FreshnessState::STALE;
    return FreshnessState::EXPIRED;
  }

  static Duration age_of(Timestamp now, Timestamp observed) noexcept {
    if (observed.is_zero()) return Duration::zero();
    return now.elapsed_since(observed);
  }

 private:
  FreshnessThresholds t_;
};

}  // namespace pg
