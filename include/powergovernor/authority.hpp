// Power Governor - authority envelope and deterministic fencing.
//
// Distributed mutations and observations are fenced with a coordinator epoch, a worker boot id, and
// the high-water generation of every authority domain. A message is accepted only if its epoch and
// boot match the current authority and its generation-tagged fields are not older than the current
// high-water marks. A worker restarted with a fresh WorkerBootId fences every report and reservation
// from the prior process incarnation.
#pragma once
#include <cstdint>
#include <string_view>
#include "ids.hpp"

namespace pg {

struct AuthorityEnvelope {
  CoordinatorEpoch epoch;
  WorkerBootId boot;
  PolicyGeneration policy;
  BudgetGeneration budget;
  ReservationGeneration reservation;
  ObservationGeneration observation;
  DecisionGeneration decision;
  DeviceGeneration device;

  bool has_epoch_boot() const noexcept { return epoch.is_null() == false && boot.is_valid(); }
  bool operator==(const AuthorityEnvelope& o) const noexcept {
    return epoch == o.epoch && boot == o.boot && policy == o.policy && budget == o.budget &&
           reservation == o.reservation && observation == o.observation &&
           decision == o.decision && device == o.device;
  }
};

class AuthorityGate {
 public:
  enum class Verdict : std::uint8_t {
    ACCEPT, STALE_EPOCH, STALE_BOOT, STALE_POLICY, STALE_BUDGET, STALE_RESERVATION,
    STALE_OBSERVATION, STALE_DECISION, STALE_DEVICE, INVALID
  };

  explicit AuthorityGate(CoordinatorEpoch epoch = CoordinatorEpoch::first(),
                         WorkerBootId boot = WorkerBootId::from_raw(1)) noexcept
      : epoch_(epoch), boot_(boot) {}

  CoordinatorEpoch epoch() const noexcept { return epoch_; }
  WorkerBootId boot() const noexcept { return boot_; }
  PolicyGeneration policy() const noexcept { return policy_; }
  BudgetGeneration budget() const noexcept { return budget_; }
  ReservationGeneration reservation() const noexcept { return reservation_; }
  ObservationGeneration observation() const noexcept { return observation_; }
  DecisionGeneration decision() const noexcept { return decision_; }
  DeviceGeneration device() const noexcept { return device_; }

  void roll_epoch() noexcept { epoch_ = epoch_.next(); }
  void reset_epoch(CoordinatorEpoch e) noexcept { epoch_ = e; }
  void set_boot(WorkerBootId b) noexcept { boot_ = b; }
  void set_policy(PolicyGeneration g) noexcept { policy_ = g; }
  void set_budget(BudgetGeneration g) noexcept { budget_ = g; }
  void set_reservation(ReservationGeneration g) noexcept { reservation_ = g; }
  void set_observation(ObservationGeneration g) noexcept { observation_ = g; }
  void set_decision(DecisionGeneration g) noexcept { decision_ = g; }
  void set_device(DeviceGeneration g) noexcept { device_ = g; }

  // Advance a high-water mark only monotonically (never regress).
  void advance_observation(ObservationGeneration g) noexcept { if (g > observation_) observation_ = g; }
  void advance_decision(DecisionGeneration g) noexcept { if (g > decision_) decision_ = g; }

  // Fence a message. A stale epoch or boot, or any generation older than the current high-water
  // mark, is rejected. Future generations are accepted and advance the high-water mark.
  Verdict check(const AuthorityEnvelope& e) const {
    if (!e.has_epoch_boot()) return Verdict::INVALID;
    if (e.epoch != epoch_) return Verdict::STALE_EPOCH;
    if (e.boot != boot_) return Verdict::STALE_BOOT;
    if (gen_stale(e.policy, policy_)) return Verdict::STALE_POLICY;
    if (gen_stale(e.budget, budget_)) return Verdict::STALE_BUDGET;
    if (gen_stale(e.reservation, reservation_)) return Verdict::STALE_RESERVATION;
    if (gen_stale(e.observation, observation_)) return Verdict::STALE_OBSERVATION;
    if (gen_stale(e.decision, decision_)) return Verdict::STALE_DECISION;
    if (gen_stale(e.device, device_)) return Verdict::STALE_DEVICE;
    return Verdict::ACCEPT;
  }

  // Update high-water marks to the max of current and message (monotonic only).
  void adopt(const AuthorityEnvelope& e) noexcept {
    if (e.policy.is_null() == false && e.policy > policy_) policy_ = e.policy;
    if (e.budget.is_null() == false && e.budget > budget_) budget_ = e.budget;
    if (e.reservation.is_null() == false && e.reservation > reservation_) reservation_ = e.reservation;
    if (e.observation.is_null() == false && e.observation > observation_) observation_ = e.observation;
    if (e.decision.is_null() == false && e.decision > decision_) decision_ = e.decision;
    if (e.device.is_null() == false && e.device > device_) device_ = e.device;
  }

 private:
  template <class G>
  static bool gen_stale(G msg, G current) noexcept {
    if (msg.is_null()) return false;      // the field is simply not carried by this message
    return msg < current;
  }

  CoordinatorEpoch epoch_;
  WorkerBootId boot_;
  PolicyGeneration policy_;
  BudgetGeneration budget_;
  ReservationGeneration reservation_;
  ObservationGeneration observation_;
  DecisionGeneration decision_;
  DeviceGeneration device_;
};

inline std::string_view to_string(AuthorityGate::Verdict v) noexcept {
  using V = AuthorityGate::Verdict;
  switch (v) {
    case V::ACCEPT: return "ACCEPT";
    case V::STALE_EPOCH: return "STALE_EPOCH";
    case V::STALE_BOOT: return "STALE_BOOT";
    case V::STALE_POLICY: return "STALE_POLICY";
    case V::STALE_BUDGET: return "STALE_BUDGET";
    case V::STALE_RESERVATION: return "STALE_RESERVATION";
    case V::STALE_OBSERVATION: return "STALE_OBSERVATION";
    case V::STALE_DECISION: return "STALE_DECISION";
    case V::STALE_DEVICE: return "STALE_DEVICE";
    case V::INVALID: return "INVALID";
  }
  return "UNKNOWN";
}

}  // namespace pg
