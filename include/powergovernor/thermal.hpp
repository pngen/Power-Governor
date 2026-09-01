// Power Governor - thermal governance model.
//
// Thermal state is tracked separately from power. The runtime classifies thermal state from real
// temperature samples and derives a thermal power ceiling, but never fabricates hardware thermal
// facts when telemetry is unavailable: an unknown temperature yields UNKNOWN, and the caller must
// decide how to treat that (the decision engine treats "unavailable" distinctly from "fresh").
#pragma once
#include <optional>
#include <string_view>
#include "clock.hpp"
#include "ids.hpp"
#include "units.hpp"

namespace pg {

enum class ThermalState : std::uint8_t {
  UNKNOWN, COOL, NORMAL, WARM, HOT, THERMALLY_CONSTRAINED, RECOVERING
};

inline std::string_view to_string(ThermalState s) noexcept {
  switch (s) {
    case ThermalState::UNKNOWN: return "UNKNOWN";
    case ThermalState::COOL: return "COOL";
    case ThermalState::NORMAL: return "NORMAL";
    case ThermalState::WARM: return "WARM";
    case ThermalState::HOT: return "HOT";
    case ThermalState::THERMALLY_CONSTRAINED: return "THERMALLY_CONSTRAINED";
    case ThermalState::RECOVERING: return "RECOVERING";
  }
  return "UNKNOWN";
}

enum class ThermalTrend : std::uint8_t { UNKNOWN, RISING, FALLING, STABLE };

inline std::string_view to_string(ThermalTrend t) noexcept {
  switch (t) {
    case ThermalTrend::UNKNOWN: return "UNKNOWN";
    case ThermalTrend::RISING: return "RISING";
    case ThermalTrend::FALLING: return "FALLING";
    case ThermalTrend::STABLE: return "STABLE";
  }
  return "UNKNOWN";
}

// Hardware thermal thresholds and hysteresis margins (all in degrees Celsius).
struct ThermalThresholds {
  Celsius cool_max{40.0};     // below -> COOL
  Celsius normal_max{60.0};   // below -> NORMAL
  Celsius warm_max{75.0};     // below -> WARM
  Celsius hot_max{85.0};      // below -> HOT, at/above -> THERMALLY_CONSTRAINED
  Celsius recovery_margin{5.0};  // must drop this far below hot_max to begin recovery
  double constrained_reduction = 0.60;  // fraction of budget permitted while constrained
  int constrained_confirm_count = 2;    // consecutive hot samples before declaring constrained
};

// A thermal observation snapshot (what a backend reports), independent of classification.
struct ThermalObservation {
  ObservationId id;             // set by the ingestion layer
  std::optional<Celsius> temperature;
  ObservationGeneration generation;  // filled by ingestion
  ThermalDomainId domain;       // may be invalid for a device-scoped source
  Timestamp timestamp;          // source observation timestamp
  WorkerBootId boot;            // originating process incarnation
  DeviceGeneration device_gen;
};

// Stateful thermal classifier with bounded, deterministic hysteresis.
class ThermalGovernor {
 public:
  explicit ThermalGovernor(ThermalThresholds t = {}, ThermalDomainId domain = ThermalDomainId())
      : thresholds_(t), domain_(domain) {}

  const ThermalThresholds& thresholds() const noexcept { return thresholds_; }
  ThermalState state() const noexcept { return state_; }
  ThermalGeneration generation() const noexcept { return generation_; }
  ThermalDomainId domain() const noexcept { return domain_; }

  // Feed a temperature sample; returns the resolved thermal state (with hysteresis).
  ThermalState observe(std::optional<Celsius> temp, Timestamp now) noexcept {
    last_now_ = now;
    if (!temp) { state_ = ThermalState::UNKNOWN; return state_; }
    last_temp_ = temp;
    const double t = temp->value();
    const ThermalState raw = classify_raw(t);
    generation_ = generation_.next();

    if (raw == ThermalState::THERMALLY_CONSTRAINED) {
      if (state_ != ThermalState::THERMALLY_CONSTRAINED) {
        if (++need_confirm_ >= thresholds_.constrained_confirm_count) {
          state_ = ThermalState::THERMALLY_CONSTRAINED;
          need_confirm_ = 0;
        }
      } else {
        state_ = ThermalState::THERMALLY_CONSTRAINED;
        need_confirm_ = 0;
      }
      return state_;
    }
    need_confirm_ = 0;

    if (state_ == ThermalState::THERMALLY_CONSTRAINED) {
      if (t < thresholds_.hot_max.value() - thresholds_.recovery_margin.value()) {
        state_ = ThermalState::RECOVERING;
      } else {
        return state_;  // still constrained
      }
      return state_;
    }
    if (state_ == ThermalState::RECOVERING) {
      if (t <= thresholds_.warm_max.value()) {
        state_ = ThermalState::NORMAL;      // fully recovered
      } else {
        return state_;
      }
      return state_;
    }
    state_ = raw;
    return state_;
  }

  // Fraction (0..1) of thermal headroom remaining for a given temperature.
  double headroom_fraction(std::optional<Celsius> temp) const noexcept {
    if (!temp) return 1.0;  // no hardware fact: treat as no constraint (see observe for UNKNOWN)
    const double t = temp->value();
    const double nm = thresholds_.normal_max.value();
    const double hm = thresholds_.hot_max.value();
    const double red = thresholds_.constrained_reduction;
    if (t <= nm) return 1.0;
    if (t < hm) {
      const double frac = (t - nm) / (hm - nm);
      return 1.0 - frac * (1.0 - red);
    }
    return red;
  }

  // Power ceiling permitted by thermal state. nullopt when temperature is unknown (unavailable).
  std::optional<Watts> permitted_power(Watts nominal_cap, std::optional<Celsius> temp) const noexcept {
    if (!temp) return std::nullopt;
    return Watts(nominal_cap.value() * headroom_fraction(temp));
  }

  std::optional<Celsius> last_temperature() const noexcept { return last_temp_; }
  Timestamp last_observation() const noexcept { return last_now_; }

  // Reset without changing thresholds (used to fence a stale thermal domain).
  void reset() noexcept {
    state_ = ThermalState::UNKNOWN;
    need_confirm_ = 0;
    generation_ = ThermalGeneration();
    last_temp_.reset();
    last_now_ = Timestamp();
  }

 private:
  ThermalState classify_raw(double t) const noexcept {
    if (t < thresholds_.cool_max.value()) return ThermalState::COOL;
    if (t < thresholds_.normal_max.value()) return ThermalState::NORMAL;
    if (t < thresholds_.warm_max.value()) return ThermalState::WARM;
    if (t < thresholds_.hot_max.value()) return ThermalState::HOT;
    return ThermalState::THERMALLY_CONSTRAINED;
  }

  ThermalThresholds thresholds_;
  ThermalDomainId domain_;
  ThermalState state_ = ThermalState::UNKNOWN;
  int need_confirm_ = 0;
  ThermalGeneration generation_;
  std::optional<Celsius> last_temp_;
  Timestamp last_now_;
};

}  // namespace pg
