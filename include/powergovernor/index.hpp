// Power Governor - secondary indexes for O(1)-ish lookup on primary query paths.
//
// The governor keeps canonical maps; these secondary indexes answer the common queries without a
// full scan of every reservation. Lookups by BudgetId, WorkloadId, and ReservationState are
// maintained incrementally.
#pragma once
#include <unordered_map>
#include <vector>
#include "ids.hpp"
#include "reservation.hpp"

namespace pg {

class ReservationIndex {
 public:
  void add(ReservationId id, BudgetId budget, WorkloadId workload, ReservationState state) {
    if (!id.is_valid()) return;
    members_[id] = {budget, workload, state};
    by_budget_[budget].push_back(id);
    by_workload_[workload].push_back(id);
    by_state_[state].push_back(id);
  }

  void set_state(ReservationId id, BudgetId budget, WorkloadId workload, ReservationState state) {
    auto it = members_.find(id);
    if (it == members_.end()) { add(id, budget, workload, state); return; }
    remove_from_lists(it->second, id);
    it->second = {budget, workload, state};
    by_budget_[budget].push_back(id);
    by_workload_[workload].push_back(id);
    by_state_[state].push_back(id);
  }

  void remove(ReservationId id) {
    auto it = members_.find(id);
    if (it == members_.end()) return;
    remove_from_lists(it->second, id);
    members_.erase(it);
  }

  std::vector<ReservationId> by_budget(BudgetId b) const { return lookup(by_budget_, b); }
  std::vector<ReservationId> by_workload(WorkloadId w) const { return lookup(by_workload_, w); }
  std::vector<ReservationId> by_state(ReservationState s) const { return lookup(by_state_, s); }
  std::size_t size() const noexcept { return members_.size(); }
  bool contains(ReservationId id) const noexcept { return members_.count(id) != 0; }

 private:
  struct Meta { BudgetId budget; WorkloadId workload; ReservationState state; };

  static void remove_entry(std::vector<ReservationId>& v, ReservationId id) {
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (*it == id) { v.erase(it); break; }
    }
  }
  void remove_from_lists(const Meta& m, ReservationId id) {
    remove_entry(by_budget_[m.budget], id);
    remove_entry(by_workload_[m.workload], id);
    remove_entry(by_state_[m.state], id);
  }
  template <class K>
  static std::vector<ReservationId> lookup(const std::unordered_map<K, std::vector<ReservationId>>& m,
                                           const K& key) {
    auto it = m.find(key);
    return it == m.end() ? std::vector<ReservationId>() : it->second;
  }

  std::unordered_map<ReservationId, Meta> members_;
  std::unordered_map<BudgetId, std::vector<ReservationId>> by_budget_;
  std::unordered_map<WorkloadId, std::vector<ReservationId>> by_workload_;
  std::unordered_map<ReservationState, std::vector<ReservationId>> by_state_;
};

}  // namespace pg
