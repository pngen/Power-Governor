// Power Governor - append-only change history.
//
// Meaningful state transitions are recorded as immutable, sequentially numbered entries. Historical
// decisions and transitions are never rewritten; a change history is a log, a digest of what
// happened, not a mutable store of "current" state.
#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "clock.hpp"

namespace pg {

enum class ChangeKind : std::uint8_t {
  POLICY_CHANGED, BUDGET_CHANGED, RESERVATION_PLANNED, RESERVATION_RESERVED,
  RESERVATION_ACTIVATED, RESERVATION_RELEASED, RESERVATION_EXPIRED,
  BURST_EXHAUSTED, BURST_REFILLED, DEVICE_THERMALLY_CONSTRAINED, DEVICE_RECOVERED,
  WORKLOAD_CLASS_CHANGED, DECISION_CHANGED, TELEMETRY_STALE, POWER_CONTROL_CAPABILITY_CHANGED,
  AUTHORITY_ROLLED, RECOVERED, OBSERVATION_ACCEPTED, OBSERVATION_REJECTED
};

inline std::string_view to_string(ChangeKind k) noexcept {
  switch (k) {
    case ChangeKind::POLICY_CHANGED: return "POLICY_CHANGED";
    case ChangeKind::BUDGET_CHANGED: return "BUDGET_CHANGED";
    case ChangeKind::RESERVATION_PLANNED: return "RESERVATION_PLANNED";
    case ChangeKind::RESERVATION_RESERVED: return "RESERVATION_RESERVED";
    case ChangeKind::RESERVATION_ACTIVATED: return "RESERVATION_ACTIVATED";
    case ChangeKind::RESERVATION_RELEASED: return "RESERVATION_RELEASED";
    case ChangeKind::RESERVATION_EXPIRED: return "RESERVATION_EXPIRED";
    case ChangeKind::BURST_EXHAUSTED: return "BURST_EXHAUSTED";
    case ChangeKind::BURST_REFILLED: return "BURST_REFILLED";
    case ChangeKind::DEVICE_THERMALLY_CONSTRAINED: return "DEVICE_THERMALLY_CONSTRAINED";
    case ChangeKind::DEVICE_RECOVERED: return "DEVICE_RECOVERED";
    case ChangeKind::WORKLOAD_CLASS_CHANGED: return "WORKLOAD_CLASS_CHANGED";
    case ChangeKind::DECISION_CHANGED: return "DECISION_CHANGED";
    case ChangeKind::TELEMETRY_STALE: return "TELEMETRY_STALE";
    case ChangeKind::POWER_CONTROL_CAPABILITY_CHANGED: return "POWER_CONTROL_CAPABILITY_CHANGED";
    case ChangeKind::AUTHORITY_ROLLED: return "AUTHORITY_ROLLED";
    case ChangeKind::RECOVERED: return "RECOVERED";
    case ChangeKind::OBSERVATION_ACCEPTED: return "OBSERVATION_ACCEPTED";
    case ChangeKind::OBSERVATION_REJECTED: return "OBSERVATION_REJECTED";
  }
  return "UNKNOWN";
}

struct ChangeEntry {
  std::size_t seq = 0;
  Timestamp at;
  ChangeKind kind = ChangeKind::POLICY_CHANGED;
  std::string message;
};

class ChangeHistory {
 public:
  explicit ChangeHistory(std::size_t max_entries = 4096) : max_(max_entries ? max_entries : 1) {}

  void append(ChangeKind kind, std::string message, Timestamp at) {
    entries_.push_back(ChangeEntry{next_++, at, kind, std::move(message)});
    if (entries_.size() > max_) entries_.erase(entries_.begin());
  }

  const std::vector<ChangeEntry>& entries() const noexcept { return entries_; }
  std::size_t size() const noexcept { return entries_.size(); }
  void clear() noexcept { entries_.clear(); }

 private:
  std::size_t next_ = 1;
  std::size_t max_;
  std::vector<ChangeEntry> entries_;
};

}  // namespace pg
