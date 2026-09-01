// Power Governor - explicit hierarchical power budgets and budget accounting.
//
// A budget is a typed allocation of power capacity within a domain. Budgets form a tree
// (node -> accelerators -> workload classes -> reservations). The BudgetManager enforces two hard
// invariants:
//   1. a reservation/usage may never exceed a budget's own available capacity, and
//   2. a child may never exceed an authoritative parent's capacity.
// Every mutation also carries the BudgetGeneration the operation was created under; a mutation with
// a stale generation (because the budget was reissued) is rejected, preventing generation rollback.
#pragma once
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "ids.hpp"
#include "units.hpp"

namespace pg {

// A budget definition (config). Accounting state lives in BudgetManager.
struct PowerBudget {
  BudgetId id;
  BudgetGeneration generation;
  PowerDomainId domain;
  PolicyGeneration policy_generation;
  std::optional<BudgetId> parent;
  std::string name;

  Watts hard_max{0.0};
  Watts soft_target{0.0};
  Watts min_reserve{0.0};       // reserve held against exhaustion
  Watts burst_allowance{0.0};
  Duration burst_window;
  Duration recovery{Duration::milliseconds(500)};
  int priority = 50;
  bool authoritative = true;    // false = informational, no capacity enforcement

  bool operator==(const PowerBudget& o) const noexcept {
    return id == o.id && generation == o.generation && domain == o.domain &&
           policy_generation == o.policy_generation && parent == o.parent && name == o.name &&
           hard_max == o.hard_max && soft_target == o.soft_target &&
           min_reserve == o.min_reserve && burst_allowance == o.burst_allowance &&
           burst_window == o.burst_window && recovery == o.recovery &&
           priority == o.priority && authoritative == o.authoritative;
  }
};

// Budget accounting with hierarchical enforcement. Not thread-safe by itself; the Governor
// serializes access.
class BudgetManager {
 public:
  // Install or update a budget. Updates require a strictly newer generation (no rollback).
  bool install(const PowerBudget& b);

  bool has(BudgetId id) const noexcept { return states_.count(id) != 0; }
  const PowerBudget& get(BudgetId id) const {
    auto it = states_.find(id);
    if (it == states_.end()) throw std::out_of_range("pg: unknown budget id");
    return it->second.cfg;
  }

  // Reserve power along the whole ancestor chain. Rejects stale generation, negative amount,
  // insufficient capacity, and unknown budgets. All budgets on the chain are validated before any
  // mutation, so a failure never leaves partial state.
  bool reserve(BudgetId id, Watts amount, ReservationId rid, BudgetGeneration budget_gen);

  // Reduce reserved capacity along the chain. Rejects over-release and stale generation.
  bool release(BudgetId id, Watts amount, ReservationId rid, BudgetGeneration budget_gen);

  // Attribute measured usage along the chain. Rejects if it would exceed available capacity.
  bool activate(BudgetId id, Watts amount, ReservationId rid, BudgetGeneration budget_gen);

  // Remove attributed usage along the chain. Rejects underflow (accounting divergence).
  bool deactivate(BudgetId id, Watts amount, ReservationId rid, BudgetGeneration budget_gen);

  // Current available capacity for a budget, bounded by every ancestor on the chain.
  Watts available(BudgetId id) const;
  Watts reserved(BudgetId id) const noexcept;
  Watts used(BudgetId id) const noexcept;
  Watts hard_max(BudgetId id) const noexcept;

  // Full chain from id up to the root (inclusive of id).
  std::vector<BudgetId> chain(BudgetId id) const;

  bool contains(BudgetId id) const noexcept { return has(id); }
  std::size_t size() const noexcept { return states_.size(); }

  // Cross-check: booked (reserved+used) never exceeds hard_max for any budget.
  bool accounting_consistent() const noexcept;

  // Ordered snapshot of all installed budget definitions (for persistence/snapshot).
  std::vector<PowerBudget> snapshots() const {
    std::vector<PowerBudget> out;
    out.reserve(states_.size());
    for (const auto& [id, s] : states_) { (void)id; out.push_back(s.cfg); }
    return out;
  }

  void clear() noexcept { states_.clear(); }

 private:
  struct State {
    PowerBudget cfg;
    Watts reserved{0.0};
    Watts used{0.0};
  };

  Watts own_available(const State& s) const noexcept {
    const double avail = s.cfg.hard_max.value() - s.reserved.value() - s.used.value();
    return Watts::clamped(avail);
  }
  bool chain_has_capacity(BudgetId id, Watts amount) const noexcept;

  std::unordered_map<BudgetId, State> states_;
};

inline bool BudgetManager::install(const PowerBudget& b) {
  auto it = states_.find(b.id);
  if (it == states_.end()) {
    states_[b.id] = State{b, Watts(0.0), Watts(0.0)};
    return true;
  }
  // No generation rollback: an update must strictly supersede.
  if (b.generation <= it->second.cfg.generation) {
    return false;
  }
  // Preserve accounting across a re-issue of the same budget id.
  it->second.cfg = b;
  return true;
}

inline std::vector<BudgetId> BudgetManager::chain(BudgetId id) const {
  std::vector<BudgetId> out;
  std::optional<BudgetId> cur = id;
  std::size_t guard = 0;
  while (cur && cur->is_valid()) {
    out.push_back(*cur);
    auto it = states_.find(*cur);
    if (it == states_.end()) break;
    cur = it->second.cfg.parent;
    if (++guard > 64) break;  // defensive; an acyclic tree will never reach this
  }
  return out;
}

inline bool BudgetManager::chain_has_capacity(BudgetId id, Watts amount) const noexcept {
  for (const BudgetId b : chain(id)) {
    auto it = states_.find(b);
    if (it == states_.end()) return false;
    if (double(own_available(it->second).value()) + 1e-9 < amount.value()) return false;
  }
  return true;
}

inline bool BudgetManager::reserve(BudgetId id, Watts amount, ReservationId rid, BudgetGeneration budget_gen) {
  (void)rid;
  auto it = states_.find(id);
  if (it == states_.end()) return false;
  if (it->second.cfg.generation != budget_gen) return false;   // stale budget generation
  if (!it->second.cfg.authoritative) return false;
  if (amount.value() < 0.0 || !std::isfinite(amount.value())) return false;
  if (!chain_has_capacity(id, amount)) return false;
  for (const BudgetId b : chain(id)) {
    auto bit = states_.find(b);
    if (bit != states_.end()) bit->second.reserved = Watts::clamped(bit->second.reserved.value() + amount.value());
  }
  return true;
}

inline bool BudgetManager::release(BudgetId id, Watts amount, ReservationId rid, BudgetGeneration budget_gen) {
  (void)rid;
  auto it = states_.find(id);
  if (it == states_.end()) return false;
  if (it->second.cfg.generation != budget_gen) return false;   // stale budget generation
  if (amount.value() < 0.0 || !std::isfinite(amount.value())) return false;
  for (const BudgetId b : chain(id)) {
    auto bit = states_.find(b);
    if (bit == states_.end()) return false;
    if (bit->second.reserved.value() + 1e-9 < amount.value()) return false;  // over-release
  }
  for (const BudgetId b : chain(id)) {
    auto bit = states_.find(b);
    if (bit != states_.end()) {
      bit->second.reserved = Watts::clamped(bit->second.reserved.value() - amount.value());
    }
  }
  return true;
}

inline bool BudgetManager::activate(BudgetId id, Watts amount, ReservationId rid, BudgetGeneration budget_gen) {
  (void)rid;
  auto it = states_.find(id);
  if (it == states_.end()) return false;
  if (it->second.cfg.generation != budget_gen) return false;
  if (amount.value() < 0.0 || !std::isfinite(amount.value())) return false;
  if (!chain_has_capacity(id, amount)) return false;
  for (const BudgetId b : chain(id)) {
    auto bit = states_.find(b);
    if (bit != states_.end()) bit->second.used = Watts::clamped(bit->second.used.value() + amount.value());
  }
  return true;
}

inline bool BudgetManager::deactivate(BudgetId id, Watts amount, ReservationId rid, BudgetGeneration budget_gen) {
  (void)rid;
  auto it = states_.find(id);
  if (it == states_.end()) return false;
  if (it->second.cfg.generation != budget_gen) return false;
  if (amount.value() < 0.0 || !std::isfinite(amount.value())) return false;
  for (const BudgetId b : chain(id)) {
    auto bit = states_.find(b);
    if (bit == states_.end()) return false;
    if (bit->second.used.value() + 1e-9 < amount.value()) return false;  // underflow divergence
  }
  for (const BudgetId b : chain(id)) {
    auto bit = states_.find(b);
    if (bit != states_.end()) bit->second.used = Watts::clamped(bit->second.used.value() - amount.value());
  }
  return true;
}

inline Watts BudgetManager::available(BudgetId id) const {
  Watts best(0.0);
  auto it = states_.find(id);
  if (it == states_.end()) return Watts(0.0);
  const Watts own = own_available(it->second);
  // Bound by each ancestor.
  Watts avail = own;
  for (const BudgetId b : chain(id)) {
    auto bit = states_.find(b);
    if (bit == states_.end()) return Watts(0.0);
    const Watts a = own_available(bit->second);
    avail = Watts(std::min(avail.value(), a.value()));
  }
  return avail;
}

inline Watts BudgetManager::reserved(BudgetId id) const noexcept {
  auto it = states_.find(id);
  return it == states_.end() ? Watts(0.0) : it->second.reserved;
}
inline Watts BudgetManager::used(BudgetId id) const noexcept {
  auto it = states_.find(id);
  return it == states_.end() ? Watts(0.0) : it->second.used;
}
inline Watts BudgetManager::hard_max(BudgetId id) const noexcept {
  auto it = states_.find(id);
  return it == states_.end() ? Watts(0.0) : it->second.cfg.hard_max;
}

inline bool BudgetManager::accounting_consistent() const noexcept {
  for (const auto& [id, s] : states_) {
    const double booked = s.reserved.value() + s.used.value();
    if (booked + 1e-9 > s.cfg.hard_max.value()) return false;
    (void)id;
  }
  return true;
}

}  // namespace pg
