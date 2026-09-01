// Power Governor - control backend abstraction.
//
// The runtime reads telemetry and applies control through a backend. Every capability is reported
// explicitly as SUPPORTED, UNAVAILABLE, or PERMISSION_DENIED; the governor never claims a hardware
// control was applied unless the backend confirms it. Real telemetry and synthetic control paths
// are always distinguishable by name and capability, so real vs. synthetic evidence never blurs.
#pragma once
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "clock.hpp"
#include "energy.hpp"
#include "ids.hpp"
#include "units.hpp"

namespace pg {

enum class CapabilityStatus : std::uint8_t { SUPPORTED, UNAVAILABLE, PERMISSION_DENIED };

inline std::string_view to_string(CapabilityStatus s) noexcept {
  switch (s) {
    case CapabilityStatus::SUPPORTED: return "SUPPORTED";
    case CapabilityStatus::UNAVAILABLE: return "UNAVAILABLE";
    case CapabilityStatus::PERMISSION_DENIED: return "PERMISSION_DENIED";
  }
  return "UNKNOWN";
}

enum class ControlResult : std::uint8_t {
  APPLIED, NOT_APPLIED, UNSUPPORTED, PERMISSION_DENIED, FAILED
};

inline std::string_view to_string(ControlResult r) noexcept {
  switch (r) {
    case ControlResult::APPLIED: return "APPLIED";
    case ControlResult::NOT_APPLIED: return "NOT_APPLIED";
    case ControlResult::UNSUPPORTED: return "UNSUPPORTED";
    case ControlResult::PERMISSION_DENIED: return "PERMISSION_DENIED";
    case ControlResult::FAILED: return "FAILED";
  }
  return "UNKNOWN";
}

struct BackendCapabilities {
  CapabilityStatus power_observe = CapabilityStatus::UNAVAILABLE;
  CapabilityStatus power_control = CapabilityStatus::UNAVAILABLE;   // set/restore power limit
  CapabilityStatus temperature = CapabilityStatus::UNAVAILABLE;
  CapabilityStatus clocks = CapabilityStatus::UNAVAILABLE;
  CapabilityStatus utilization = CapabilityStatus::UNAVAILABLE;
  CapabilityStatus energy = CapabilityStatus::UNAVAILABLE;          // energy counters
  CapabilityStatus throttle_reasons = CapabilityStatus::UNAVAILABLE;
};

// One passive sample of the device. Every field is optional when the backend cannot provide it,
// so a caller can always see exactly what is real vs. missing.
struct BackendSample {
  std::optional<Watts> power;
  std::optional<Celsius> temperature;
  std::optional<FrequencyMhz> sm_clock;
  std::optional<FrequencyMhz> mem_clock;
  std::optional<Utilization> utilization;
  std::optional<Joules> energy_total;      // cumulative energy since boot (may be 0)
  std::optional<Watts> power_limit;        // the currently applied hardware limit
  bool throttled = false;
  std::vector<std::string> throttle_reasons;
  EnergyProvenance provenance = EnergyProvenance::UNKNOWN;
  std::string source;                      // "fake", "nvml", "cuda"
  bool any_present() const noexcept {
    return power || temperature || sm_clock || mem_clock || utilization || energy_total || power_limit;
  }
};

class ControlBackend {
 public:
  virtual ~ControlBackend() = default;
  virtual BackendCapabilities capabilities() const = 0;
  virtual std::string name() const = 0;
  virtual BackendSample sample() = 0;
  // Apply a vendor-supported power limit; nullopt means "restore/unset". Returns the concrete
  // result; a synthetic backend confirms application, a real backend only reports what it did.
  virtual ControlResult set_power_limit(std::optional<Watts> limit) = 0;
  virtual ControlResult restore_power_limit() = 0;
  virtual std::optional<Watts> applied_limit() const noexcept { return std::nullopt; }
};

// -------------------------------------------------------------------------
// Deterministic fake/test backend. Observe support and synthetic power control.
// -------------------------------------------------------------------------
class FakeBackend final : public ControlBackend {
 public:
  FakeBackend() {
    caps_.power_observe = CapabilityStatus::SUPPORTED;
    caps_.temperature = CapabilityStatus::SUPPORTED;
    caps_.clocks = CapabilityStatus::SUPPORTED;
    caps_.utilization = CapabilityStatus::SUPPORTED;
    caps_.energy = CapabilityStatus::SUPPORTED;
    caps_.power_control = CapabilityStatus::SUPPORTED;  // synthetic control path
    caps_.throttle_reasons = CapabilityStatus::UNAVAILABLE;
  }

  BackendCapabilities capabilities() const override { return caps_; }
  std::string name() const override { return "fake"; }

  void set_power(Watts w) { power_ = w; }
  void set_temperature(Celsius t) { temp_ = t; }
  void set_sm_clock(FrequencyMhz c) { sm_ = c; }
  void set_mem_clock(FrequencyMhz c) { mem_ = c; }
  void set_utilization(Utilization u) { util_ = u; }
  void set_energy_total(Joules j) { energy_ = j; }
  void set_throttled(bool b, std::vector<std::string> reasons = {}) { throttled_ = b; reasons_ = std::move(reasons); }

  BackendSample sample() override {
    BackendSample s;
    s.power = power_;
    s.temperature = temp_;
    s.sm_clock = sm_;
    s.mem_clock = mem_;
    s.utilization = util_;
    s.energy_total = energy_;
    s.power_limit = applied_;
    s.throttled = throttled_;
    s.throttle_reasons = reasons_;
    s.provenance = synthetic_ ? EnergyProvenance::DERIVED : EnergyProvenance::MEASURED;
    s.source = "fake";
    return s;
  }

  ControlResult set_power_limit(std::optional<Watts> limit) override {
    if (limit) {
      if (!std::isfinite(limit->value()) || limit->value() <= 0.0) {
        return ControlResult::FAILED;
      }
      applied_ = limit;  // synthetic backend confirms application
      return ControlResult::APPLIED;
    }
    applied_ = std::nullopt;
    return ControlResult::APPLIED;
  }

  ControlResult restore_power_limit() override {
    applied_ = std::nullopt;
    return ControlResult::APPLIED;
  }

  std::optional<Watts> applied_limit() const noexcept override { return applied_; }

  // Mark whether next samples are synthetic (DERIVED) or judged real.
  void set_synthetic_provenance(bool b) noexcept { synthetic_ = b; }

 private:
  BackendCapabilities caps_;
  std::optional<Watts> power_;
  std::optional<Celsius> temp_;
  std::optional<FrequencyMhz> sm_;
  std::optional<FrequencyMhz> mem_;
  std::optional<Utilization> util_;
  std::optional<Joules> energy_;
  std::optional<Watts> applied_;
  bool throttled_ = false;
  std::vector<std::string> reasons_;
  bool synthetic_ = true;
};

}  // namespace pg
