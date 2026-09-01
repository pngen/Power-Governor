// Power Governor - strong identity and generation model.
//
// Identities and generations are distinct strongly-typed values rather than raw integers or
// loosely-typed strings. Zero is reserved as the invalid/null value for every id and generation.
// Generations roll independently per authority domain; each id type and generation type has its own
// independent monotonic allocator so generated values are unique within their type.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace pg {

// -------------------------------------------------------------------------
// Strong identifier: wraps a uint64 tag. value 0 == invalid/unset.
// -------------------------------------------------------------------------
template <class Tag>
class StrongId {
 public:
  using tag_type = Tag;

  StrongId() noexcept = default;
  explicit StrongId(std::uint64_t v) noexcept : value_(v) {}

  static StrongId invalid() noexcept { return StrongId(); }
  static StrongId from_raw(std::uint64_t v) noexcept { return StrongId(v); }
  static StrongId allocate() noexcept { return StrongId(next_.fetch_add(1, std::memory_order_relaxed)); }

  bool is_valid() const noexcept { return value_ != 0; }
  std::uint64_t raw() const noexcept { return value_; }

  bool operator==(StrongId o) const noexcept { return value_ == o.value_; }
  bool operator!=(StrongId o) const noexcept { return value_ != o.value_; }
  bool operator<(StrongId o) const noexcept { return value_ < o.value_; }

  std::size_t hash() const noexcept { return std::hash<std::uint64_t>{}(value_); }
  std::string to_string() const { return std::to_string(value_); }

 private:
  inline static std::atomic<std::uint64_t> next_{1};
  std::uint64_t value_ = 0;
};

// -------------------------------------------------------------------------
// Independent generation counter for a single authority domain. value 0 == null.
// -------------------------------------------------------------------------
template <class Tag>
class Generation {
 public:
  using tag_type = Tag;

  Generation() noexcept = default;
  explicit Generation(std::uint64_t v) noexcept : value_(v) {}

  static Generation null() noexcept { return Generation(); }
  static Generation first() noexcept { return Generation(1); }
  static Generation from_raw(std::uint64_t v) noexcept { return Generation(v); }

  bool is_null() const noexcept { return value_ == 0; }
  std::uint64_t raw() const noexcept { return value_; }
  Generation next() const noexcept { return Generation(value_ + 1); }

  bool operator==(Generation o) const noexcept { return value_ == o.value_; }
  bool operator!=(Generation o) const noexcept { return value_ != o.value_; }
  bool operator<(Generation o) const noexcept { return value_ < o.value_; }
  bool operator<=(Generation o) const noexcept { return value_ <= o.value_; }
  bool operator>(Generation o) const noexcept { return value_ > o.value_; }
  bool operator>=(Generation o) const noexcept { return value_ >= o.value_; }

  std::size_t hash() const noexcept { return std::hash<std::uint64_t>{}(value_); }
  std::string to_string() const { return std::to_string(value_); }

 private:
  std::uint64_t value_ = 0;
};

// -------------------------------------------------------------------------
// 128-bit device UUID.
// -------------------------------------------------------------------------
class Uuid {
 public:
  Uuid() noexcept = default;
  explicit Uuid(const std::array<std::uint8_t, 16>& b) noexcept : bytes_(b) {}
  Uuid(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) noexcept {
    write_u32(0, a); write_u32(4, b); write_u32(8, c); write_u32(12, d);
  }

  static Uuid zero() noexcept { return Uuid(); }
  static Uuid from_bytes(const std::array<std::uint8_t, 16>& b) noexcept { return Uuid(b); }
  static Uuid from_counter(std::uint64_t n) noexcept {
    Uuid u;
    for (int i = 0; i < 8; ++i) {
      u.bytes_[i] = static_cast<std::uint8_t>((n >> (i * 8)) & 0xFFu);
      u.bytes_[8 + i] = static_cast<std::uint8_t>((i + 1) & 0xFFu);
    }
    return u;
  }
  // Parse canonical hex "00112233-4455-6677-8899-aabbccddeeff" (dashes/colons optional).
  static Uuid from_hex(std::string_view s);

  bool is_zero() const noexcept { return all_zero(); }
  bool operator==(Uuid o) const noexcept { return bytes_ == o.bytes_; }
  bool operator!=(Uuid o) const noexcept { return bytes_ != o.bytes_; }
  bool operator<(Uuid o) const noexcept { return bytes_ < o.bytes_; }
  std::size_t hash() const noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    for (std::uint8_t b : bytes_) { h ^= b; h *= 1099511628211ULL; }
    return static_cast<std::size_t>(h);
  }

  const std::array<std::uint8_t, 16>& bytes() const noexcept { return bytes_; }
  std::string to_hex() const;
  std::string to_string() const { return to_hex(); }

 private:
  bool all_zero() const noexcept {
    for (auto b : bytes_) if (b != 0) return false;
    return true;
  }
  void write_u32(std::size_t off, std::uint32_t v) noexcept {
    bytes_[off]     = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    bytes_[off + 1] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    bytes_[off + 2] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    bytes_[off + 3] = static_cast<std::uint8_t>(v & 0xFFu);
  }

  std::array<std::uint8_t, 16> bytes_{};
};

inline Uuid Uuid::from_hex(std::string_view s) {
  Uuid u;
  int nibbles = 0;
  for (char c : s) {
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else if (c == '-' || c == ':') continue;
    else throw std::invalid_argument("pg::Uuid::from_hex: invalid character");
    if (nibbles >= 32) throw std::invalid_argument("pg::Uuid::from_hex: too many hex digits");
    const int byte = nibbles / 2;
    const int shift = (nibbles % 2 == 0) ? 4 : 0;
    u.bytes_[byte] = static_cast<std::uint8_t>(u.bytes_[byte] | (v << shift));
    ++nibbles;
  }
  if (nibbles != 32) throw std::invalid_argument("pg::Uuid::from_hex: expected 32 hex digits");
  return u;
}

inline std::string Uuid::to_hex() const {
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    out.push_back(hex[bytes_[i] >> 4]);
    out.push_back(hex[bytes_[i] & 0x0Fu]);
  }
  return out;
}

// -------------------------------------------------------------------------
// Identity and generation tag types (distinct so the types never collapse).
// -------------------------------------------------------------------------
struct GovernorIdTag {};         struct NodeIdTag {};           struct AcceleratorIdTag {};
struct WorkerIdTag {};           struct WorkerBootIdTag {};      struct WorkloadIdTag {};
struct RequestClassIdTag {};     struct BudgetIdTag {};          struct PolicyIdTag {};
struct ObservationIdTag {};      struct DecisionIdTag {};        struct PowerDomainIdTag {};
struct EnergyWindowIdTag {};     struct ThermalDomainIdTag {};   struct ReservationIdTag {};

struct CoordinatorEpochTag {};   struct GovernorGenerationTag {};  struct PolicyGenerationTag {};
struct DeviceGenerationTag {};   struct ObservationGenerationTag {};  struct BudgetGenerationTag {};
struct DecisionGenerationTag {}; struct ReservationGenerationTag {};  struct WorkloadGenerationTag {};
struct ThermalGenerationTag {};  struct EnergyGenerationTag {};

// Concrete aliases
using GovernorId = StrongId<GovernorIdTag>;
using NodeId = StrongId<NodeIdTag>;
using AcceleratorId = StrongId<AcceleratorIdTag>;
using WorkerId = StrongId<WorkerIdTag>;
using WorkerBootId = StrongId<WorkerBootIdTag>;
using WorkloadId = StrongId<WorkloadIdTag>;
using RequestClassId = StrongId<RequestClassIdTag>;
using BudgetId = StrongId<BudgetIdTag>;
using PolicyId = StrongId<PolicyIdTag>;
using ObservationId = StrongId<ObservationIdTag>;
using DecisionId = StrongId<DecisionIdTag>;
using PowerDomainId = StrongId<PowerDomainIdTag>;
using EnergyWindowId = StrongId<EnergyWindowIdTag>;
using ThermalDomainId = StrongId<ThermalDomainIdTag>;
using ReservationId = StrongId<ReservationIdTag>;

using CoordinatorEpoch = Generation<CoordinatorEpochTag>;
using GovernorGeneration = Generation<GovernorGenerationTag>;
using PolicyGeneration = Generation<PolicyGenerationTag>;
using DeviceGeneration = Generation<DeviceGenerationTag>;
using ObservationGeneration = Generation<ObservationGenerationTag>;
using BudgetGeneration = Generation<BudgetGenerationTag>;
using DecisionGeneration = Generation<DecisionGenerationTag>;
using ReservationGeneration = Generation<ReservationGenerationTag>;
using WorkloadGeneration = Generation<WorkloadGenerationTag>;
using ThermalGeneration = Generation<ThermalGenerationTag>;
using EnergyGeneration = Generation<EnergyGenerationTag>;

}  // namespace pg

// std::hash specializations
namespace std {
template <class Tag> struct hash<pg::StrongId<Tag>> {
  std::size_t operator()(const pg::StrongId<Tag>& id) const noexcept { return id.hash(); }
};
template <class Tag> struct hash<pg::Generation<Tag>> {
  std::size_t operator()(const pg::Generation<Tag>& g) const noexcept { return g.hash(); }
};
template <> struct hash<pg::Uuid> {
  std::size_t operator()(const pg::Uuid& u) const noexcept { return u.hash(); }
};
}  // namespace std
