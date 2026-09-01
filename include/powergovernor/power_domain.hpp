// Power Governor - hierarchical power domains.
//
// Domains form a tree (fleet -> rack/group -> node -> accelerator -> workload class -> reservation)
// but the abstraction also works when only node/device telemetry exists: every level is optional
// and a domain may have no physical backing behind it. Child allocations must never silently
// exceed authoritative parent capacity; that invariant is enforced by the budget manager.
#pragma once
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "ids.hpp"
#include "units.hpp"

namespace pg {

enum class PowerDomainType : std::uint8_t {
  FLEET, RACK, NODE, ACCELERATOR, WORKLOAD_CLASS, RESERVATION
};

inline std::string_view to_string(PowerDomainType t) noexcept {
  switch (t) {
    case PowerDomainType::FLEET: return "FLEET";
    case PowerDomainType::RACK: return "RACK";
    case PowerDomainType::NODE: return "NODE";
    case PowerDomainType::ACCELERATOR: return "ACCELERATOR";
    case PowerDomainType::WORKLOAD_CLASS: return "WORKLOAD_CLASS";
    case PowerDomainType::RESERVATION: return "RESERVATION";
  }
  return "UNKNOWN";
}

struct PowerDomain {
  PowerDomainId id;
  PowerDomainType type = PowerDomainType::NODE;
  std::optional<PowerDomainId> parent;
  std::vector<PowerDomainId> children;
  std::string name;
  bool physical = false;   // whether a real hardware/telemetry source backs this domain

  PowerDomain() = default;
  PowerDomain(PowerDomainId id_, PowerDomainType type_,
              std::optional<PowerDomainId> parent_, std::string name_) noexcept
      : id(id_), type(type_), parent(parent_), name(std::move(name_)) {}
};

// The domain tree. Owns the set of domains and answers hierarchy queries. It does no accounting
// itself; accounting lives in the budget manager, which binds a budget to a domain.
class PowerDomainTree {
 public:
  PowerDomainId add(PowerDomainId id, PowerDomainType type,
                    std::optional<PowerDomainId> parent, std::string name, bool physical = false) {
    if (!id.is_valid()) throw std::invalid_argument("pg: domain id must be valid");
    if (nodes_.count(id)) throw std::invalid_argument("pg: duplicate domain id");
    if (parent && parent->is_valid() && !nodes_.count(*parent)) {
      throw std::invalid_argument("pg: parent domain does not exist");
    }
    PowerDomain d(id, type, parent, std::move(name));
    d.physical = physical;
    nodes_.emplace(id, std::move(d));
    if (parent && parent->is_valid()) nodes_[*parent].children.push_back(id);
    return id;
  }

  bool contains(PowerDomainId id) const noexcept { return nodes_.count(id) != 0; }
  std::size_t size() const noexcept { return nodes_.size(); }

  const PowerDomain& get(PowerDomainId id) const {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) throw std::out_of_range("pg: unknown domain id");
    return it->second;
  }

  // The chain of ancestors from deepest to most general.
  std::vector<PowerDomainId> ancestors(PowerDomainId id) const {
    std::vector<PowerDomainId> out;
    auto it = nodes_.find(id);
    if (it == nodes_.end()) throw std::out_of_range("pg: unknown domain id");
    std::optional<PowerDomainId> cur = it->second.parent;
    while (cur && cur->is_valid()) {
      out.push_back(*cur);
      auto pit = nodes_.find(*cur);
      if (pit == nodes_.end()) break;
      cur = pit->second.parent;
    }
    return out;
  }

  const std::unordered_map<PowerDomainId, PowerDomain>& all() const noexcept { return nodes_; }

 private:
  std::unordered_map<PowerDomainId, PowerDomain> nodes_;
};

}  // namespace pg
