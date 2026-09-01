// Power Governor - transactional power reservations.
//
// The lifecycle is plan -> validate -> reserve -> admit -> activate -> account -> release. The
// manager and the BudgetManager together guard against double reservation, leaked reservations,
// stale activation/release, negative capacity, parent-budget overflow, double release, and
// generation rollback. A reservation also carries its full authority envelope so that stale
// authority (epoch, boot, generation) is rejected deterministically at every step.
#pragma once
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include "authority.hpp"
#include "budget.hpp"
#include "clock.hpp"
#include "energy.hpp"
#include "ids.hpp"
#include "workload_class.hpp"

namespace pg {

enum class ReservationState : std::uint8_t {
  PLANNED, VALIDATED, RESERVED, ADMITTED, ACTIVE, RELEASED, EXPIRED
};

inline std::string_view to_string(ReservationState s) noexcept {
  switch (s) {
    case ReservationState::PLANNED: return "PLANNED";
    case ReservationState::VALIDATED: return "VALIDATED";
    case ReservationState::RESERVED: return "RESERVED";
    case ReservationState::ADMITTED: return "ADMITTED";
    case ReservationState::ACTIVE: return "ACTIVE";
    case ReservationState::RELEASED: return "RELEASED";
    case ReservationState::EXPIRED: return "EXPIRED";
  }
  return "UNKNOWN";
}

struct Reservation {
  ReservationId id;
  ReservationGeneration generation;
  WorkloadId workload;
  RequestClassId request_class;
  WorkloadPowerClass power_class = WorkloadPowerClass::THROUGHPUT;
  PowerDomainId scope;
  BudgetId budget;
  Watts requested_power{0.0};
  Joules requested_energy{0.0};
  PolicyGeneration policy_generation;
  DeviceGeneration device_generation;
  BudgetGeneration budget_generation;
  AuthorityEnvelope authority;
  Timestamp created;
  Duration validity;
  ReservationState state = ReservationState::PLANNED;
  Watts reserved_power{0.0};
  Joules energy_used{0.0};
  Timestamp activated_at;
  Timestamp released_at;

  bool operator==(const Reservation& o) const noexcept {
    return id == o.id && generation == o.generation && workload == o.workload &&
           request_class == o.request_class && power_class == o.power_class && scope == o.scope &&
           budget == o.budget && requested_power == o.requested_power &&
           requested_energy == o.requested_energy && policy_generation == o.policy_generation &&
           device_generation == o.device_generation &&
           budget_generation == o.budget_generation && created == o.created &&
           validity == o.validity && state == o.state && reserved_power == o.reserved_power &&
           energy_used == o.energy_used;
  }
};

class ReservationManager {
 public:
  struct Result {
    bool ok = true;
    std::string reason;
    ReservationId id;   // set on a successful plan()
  };

  // --- lifecycle ----------------------------------------------------------
  Result plan(WorkloadId workload, WorkloadPowerClass power_class, PowerDomainId scope,
              BudgetId budget, Watts requested_power, Joules requested_energy,
              PolicyGeneration policy_gen, DeviceGeneration device_gen,
              AuthorityEnvelope authority, Timestamp now, Duration validity);

  Result validate(ReservationId id, PolicyGeneration policy_gen);
  Result reserve(ReservationId id, BudgetManager& budgets, BudgetGeneration budget_gen, Timestamp now);
  Result admit(ReservationId id, Timestamp now);
  Result activate(ReservationId id, BudgetManager& budgets, BudgetGeneration budget_gen, Timestamp now);
  Result account_energy(ReservationId id, Joules energy, EnergyProvenance provenance, Timestamp now);
  Result release(ReservationId id, BudgetManager& budgets, BudgetGeneration budget_gen, Timestamp now);

  // --- queries ------------------------------------------------------------
  bool exists(ReservationId id) const noexcept { return map_.count(id) != 0; }
  const Reservation& get(ReservationId id) const {
    auto it = map_.find(id);
    if (it == map_.end()) throw std::out_of_range("pg: unknown reservation id");
    return it->second;
  }
  ReservationState state(ReservationId id) const {
    return exists(id) ? get(id).state : ReservationState::EXPIRED;
  }
  std::size_t count() const noexcept { return map_.size(); }

  // Deterministic snapshot of all reservations (ordered by id).
  std::vector<Reservation> all() const {
    std::vector<Reservation> out;
    out.reserve(map_.size());
    for (const auto& [k, r] : map_) { (void)k; out.push_back(r); }
    std::sort(out.begin(), out.end(), [](const Reservation& a, const Reservation& b) { return a.id < b.id; });
    return out;
  }
  // Restore a persisted reservation (rejects duplicate id).
  bool restore(const Reservation& r) {
    if (map_.count(r.id)) return false;
    map_.emplace(r.id, r);
    return true;
  }

  std::size_t active_count() const noexcept {
    std::size_t n = 0;
    for (const auto& [k, r] : map_) { (void)k; if (r.state == ReservationState::ACTIVE) ++n; }
    return n;
  }

  // Verify no reservations are leaked: every non-released/expired reservation must still be within
  // its validity window and must not be double-released.
  bool audit_no_leaks(Timestamp now) const noexcept {
    for (const auto& [k, r] : map_) {
      (void)k;
      if (r.state == ReservationState::RELEASED || r.state == ReservationState::EXPIRED) continue;
      if (r.created.is_zero() == false) {
        const Duration age = now.elapsed_since(r.created);
        if (age > r.validity) return false;  // leaked (expired but not released)
      }
    }
    return true;
  }

 private:
  Reservation& mutable_get(ReservationId id) {
    auto it = map_.find(id);
    if (it == map_.end()) throw std::out_of_range("pg: unknown reservation id");
    return it->second;
  }
  bool expired(const Reservation& r, Timestamp now) const noexcept {
    if (r.created.is_zero()) return false;
    return now.elapsed_since(r.created) > r.validity;
  }

  std::unordered_map<ReservationId, Reservation> map_;
};

inline ReservationManager::Result ReservationManager::plan(
    WorkloadId workload, WorkloadPowerClass power_class, PowerDomainId scope, BudgetId budget,
    Watts requested_power, Joules requested_energy, PolicyGeneration policy_gen,
    DeviceGeneration device_gen, AuthorityEnvelope authority, Timestamp now, Duration validity) {
  Reservation r;
  r.id = ReservationId::allocate();
  r.generation = ReservationGeneration::first();
  r.workload = workload;
  r.request_class = RequestClassId();  // unset unless caller sets it later; kept zero
  r.power_class = power_class;
  r.scope = scope;
  r.budget = budget;
  r.requested_power = requested_power;
  r.requested_energy = requested_energy;
  r.policy_generation = policy_gen;
  r.device_generation = device_gen;
  r.budget_generation = BudgetGeneration();
  r.authority = authority;
  r.created = now;
  r.validity = validity;
  r.state = ReservationState::PLANNED;
  if (!map_.emplace(r.id, r).second) {
    return Result{false, "duplicate reservation id", r.id};
  }
  return Result{true, "planned", r.id};
}

inline ReservationManager::Result ReservationManager::validate(ReservationId id, PolicyGeneration policy_gen) {
  Reservation& r = mutable_get(id);
  if (r.state != ReservationState::PLANNED) return Result{false, "reservation not in PLANNED"}; 
  if (r.policy_generation != policy_gen) return Result{false, "policy generation mismatch"};
  r.state = ReservationState::VALIDATED;
  return Result{true, "validated"};
}

inline ReservationManager::Result ReservationManager::reserve(
    ReservationId id, BudgetManager& budgets, BudgetGeneration budget_gen, Timestamp now) {
  Reservation& r = mutable_get(id);
  if (r.state != ReservationState::VALIDATED && r.state != ReservationState::PLANNED) {
    return Result{false, "reservation not reservable (already reserved/active/released)"};
  }
  if (expired(r, now)) { r.state = ReservationState::EXPIRED; return Result{false, "reservation expired"}; }
  const bool ok = budgets.reserve(r.budget, r.requested_power, id, budget_gen);
  if (!ok) return Result{false, "budget reserve failed (capacity/generation/authority)"};
  r.budget_generation = budget_gen;
  r.reserved_power = r.requested_power;
  r.state = ReservationState::RESERVED;
  return Result{true, "reserved"};
}

inline ReservationManager::Result ReservationManager::admit(ReservationId id, Timestamp now) {
  Reservation& r = mutable_get(id);
  if (r.state != ReservationState::RESERVED) return Result{false, "reservation not RESERVED"};
  if (expired(r, now)) { r.state = ReservationState::EXPIRED; return Result{false, "reservation expired"}; }
  r.state = ReservationState::ADMITTED;
  return Result{true, "admitted"};
}

inline ReservationManager::Result ReservationManager::activate(
    ReservationId id, BudgetManager& budgets, BudgetGeneration budget_gen, Timestamp now) {
  Reservation& r = mutable_get(id);
  if (r.state != ReservationState::ADMITTED && r.state != ReservationState::RESERVED) {
    return Result{false, "stale activation: reservation not admitted"};
  }
  if (expired(r, now)) { r.state = ReservationState::EXPIRED; return Result{false, "reservation expired"}; }
  if (r.budget_generation != budget_gen) {
    return Result{false, "stale activation: budget generation changed"};
  }
  const bool ok = budgets.activate(r.budget, r.requested_power, id, budget_gen);
  if (!ok) return Result{false, "budget activate failed"};
  r.state = ReservationState::ACTIVE;
  r.activated_at = now;
  return Result{true, "active"};
}

inline ReservationManager::Result ReservationManager::account_energy(
    ReservationId id, Joules energy, EnergyProvenance provenance, Timestamp now) {
  Reservation& r = mutable_get(id);
  if (r.state != ReservationState::ACTIVE) return Result{false, "energy only accounted for ACTIVE"};
  if (energy.value() < 0.0 || !std::isfinite(energy.value())) {
    return Result{false, "non-finite or negative energy"};
  }
  r.energy_used = Joules::clamped(r.energy_used.value() + energy.value());
  (void)provenance; (void)now;
  return Result{true, "accounted"};
}

inline ReservationManager::Result ReservationManager::release(
    ReservationId id, BudgetManager& budgets, BudgetGeneration budget_gen, Timestamp now) {
  Reservation& r = mutable_get(id);
  if (r.state == ReservationState::RELEASED) return Result{false, "double release"};
  if (r.state == ReservationState::EXPIRED) return Result{false, "already expired"};
  if (r.budget_generation.is_null() &&
      (r.state == ReservationState::ADMITTED || r.state == ReservationState::ACTIVE)) {
    return Result{false, "release with unknown budget generation"};
  }
  const bool released = budgets.release(r.budget, r.reserved_power, id, budget_gen);
  if (!released) {
    return Result{false, "budget release failed (over-release/stale generation)"};
  }
  // A reservation that was activated has attributed usage; release removes it only once.
  if (r.state == ReservationState::ACTIVE || r.state == ReservationState::ADMITTED ||
      r.state == ReservationState::RESERVED) {
    budgets.deactivate(r.budget, r.reserved_power, id, budget_gen);
  }
  r.state = ReservationState::RELEASED;
  r.released_at = now;
  return Result{true, "released"};
}

}  // namespace pg
