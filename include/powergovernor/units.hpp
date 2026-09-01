// Power Governor - typed physical quantity model.
//
// Strongly-typed units for power, energy, temperature, time, frequency, and efficiency.
// Every construction and mutation validates the value: NaN and Inf are always rejected, and
// dimension-specific rules (no negative power/energy, utilization in [0,1], physically possible
// temperature) are enforced by throwing std::invalid_argument on violation.
#pragma once
#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pg {

enum class Dimension : std::uint8_t { Power, Energy, Temperature, Time, Frequency, Utilization, Efficiency };

// --- Tag types: each distinct type so Watts and Joules cannot be mixed at compile time. ---
struct WattsTag {};            struct MilliwattsTag {};
struct JoulesTag {};           struct MillijoulesTag {};
struct CelsiusTag {};
struct MillisecondsTag {};     struct SecondsTag {};
struct UtilizationTag {};
struct FrequencyMhzTag {};     struct MemoryFrequencyMhzTag {};
struct EnergyPerTokenTag {};   struct EnergyPerRequestTag {};  struct PowerEfficiencyTag {};

// --- Unit traits: physical dimension, scale to a canonical base unit, and a display name. ---
template <class Tag> struct UnitTraits;

template <> struct UnitTraits<WattsTag> {
  static constexpr Dimension dim = Dimension::Power;
  static constexpr double scale_to_base = 1.0;
  static constexpr std::string_view name = "W";
};
template <> struct UnitTraits<MilliwattsTag> {
  static constexpr Dimension dim = Dimension::Power;
  static constexpr double scale_to_base = 0.001;
  static constexpr std::string_view name = "mW";
};
template <> struct UnitTraits<JoulesTag> {
  static constexpr Dimension dim = Dimension::Energy;
  static constexpr double scale_to_base = 1.0;
  static constexpr std::string_view name = "J";
};
template <> struct UnitTraits<MillijoulesTag> {
  static constexpr Dimension dim = Dimension::Energy;
  static constexpr double scale_to_base = 0.001;
  static constexpr std::string_view name = "mJ";
};
template <> struct UnitTraits<CelsiusTag> {
  static constexpr Dimension dim = Dimension::Temperature;
  static constexpr double scale_to_base = 1.0;
  static constexpr std::string_view name = "C";
};
template <> struct UnitTraits<MillisecondsTag> {
  static constexpr Dimension dim = Dimension::Time;
  static constexpr double scale_to_base = 0.001;  // canonical time base is seconds
  static constexpr std::string_view name = "ms";
};
template <> struct UnitTraits<SecondsTag> {
  static constexpr Dimension dim = Dimension::Time;
  static constexpr double scale_to_base = 1.0;
  static constexpr std::string_view name = "s";
};
template <> struct UnitTraits<UtilizationTag> {
  static constexpr Dimension dim = Dimension::Utilization;
  static constexpr double scale_to_base = 1.0;  // fraction in [0,1]
  static constexpr std::string_view name = "frac";
};
template <> struct UnitTraits<FrequencyMhzTag> {
  static constexpr Dimension dim = Dimension::Frequency;
  static constexpr double scale_to_base = 1.0e6;  // base is Hz
  static constexpr std::string_view name = "MHz";
};
template <> struct UnitTraits<MemoryFrequencyMhzTag> {
  static constexpr Dimension dim = Dimension::Frequency;
  static constexpr double scale_to_base = 1.0e6;
  static constexpr std::string_view name = "MHz";
};
template <> struct UnitTraits<EnergyPerTokenTag> {
  static constexpr Dimension dim = Dimension::Efficiency;
  static constexpr double scale_to_base = 1.0;
  static constexpr std::string_view name = "J/token";
};
template <> struct UnitTraits<EnergyPerRequestTag> {
  static constexpr Dimension dim = Dimension::Efficiency;
  static constexpr double scale_to_base = 1.0;
  static constexpr std::string_view name = "J/req";
};
template <> struct UnitTraits<PowerEfficiencyTag> {
  static constexpr Dimension dim = Dimension::Efficiency;
  static constexpr double scale_to_base = 1.0;
  static constexpr std::string_view name = "work/W";
};

// ---- Validation helpers -------------------------------------------------
namespace detail {
inline void reject_non_finite(double v, std::string_view unit) {
  if (!std::isfinite(v)) {
    throw std::invalid_argument("pg: " + std::string(unit) + " quantity must be finite (got NaN/Inf)");
  }
}
template <class Tag>
inline void validate_unit(double v) {
  reject_non_finite(v, UnitTraits<Tag>::name);
  if constexpr (UnitTraits<Tag>::dim == Dimension::Utilization) {
    if (v < 0.0 || v > 1.0) {
      throw std::invalid_argument("pg: utilization must be in [0,1]");
    }
  } else if constexpr (UnitTraits<Tag>::dim == Dimension::Temperature) {
    if (v < -273.15) {
      throw std::invalid_argument("pg: temperature below absolute zero");
    }
  } else if constexpr (UnitTraits<Tag>::dim == Dimension::Power ||
                       UnitTraits<Tag>::dim == Dimension::Energy ||
                       UnitTraits<Tag>::dim == Dimension::Efficiency) {
    if (v < 0.0) {
      throw std::invalid_argument("pg: " + std::string(UnitTraits<Tag>::name) + " must not be negative");
    }
  }
}
}  // namespace detail

// ---- Generic strongly-typed quantity ------------------------------------
template <class Tag>
class Quantity {
 public:
  using tag_type = Tag;
  static constexpr Dimension dim = UnitTraits<Tag>::dim;
  static constexpr double scale_to_base = UnitTraits<Tag>::scale_to_base;
  static constexpr std::string_view unit_name = UnitTraits<Tag>::name;

  Quantity() noexcept = default;
  explicit Quantity(double v) : value_(v) { detail::validate_unit<Tag>(v); }

  // value in this unit's own scale
  double value() const noexcept { return value_; }
  // value in the canonical base unit of the dimension (watts, joules, seconds, Hz, fraction, ...)
  double as_base() const noexcept { return value_ * scale_to_base; }

  // Convert to a different unit of the same physical dimension (e.g. Watts -> Milliwatts).
  template <class Other>
  Quantity<Other> to() const noexcept {
    static_assert(UnitTraits<Tag>::dim == UnitTraits<Other>::dim,
                  "pg::convert_to: units must share a physical dimension");
    return Quantity<Other>((as_base() / UnitTraits<Other>::scale_to_base));
  }

  Quantity& operator+=(Quantity o) noexcept { value_ += o.value_; return *this; }
  Quantity& operator-=(Quantity o) noexcept { value_ -= o.value_; return *this; }
  Quantity operator+(Quantity o) const noexcept { return Quantity(value_ + o.value_); }
  Quantity operator-(Quantity o) const noexcept { return Quantity(value_ - o.value_); }
  Quantity operator*(double f) const noexcept { return Quantity(value_ * f); }
  Quantity operator/(double f) const noexcept { return Quantity(value_ / f); }

  bool operator==(Quantity o) const noexcept { return value_ == o.value_; }
  bool operator!=(Quantity o) const noexcept { return value_ != o.value_; }
  bool operator<(Quantity o) const noexcept { return value_ < o.value_; }
  bool operator<=(Quantity o) const noexcept { return value_ <= o.value_; }
  bool operator>(Quantity o) const noexcept { return value_ > o.value_; }
  bool operator>=(Quantity o) const noexcept { return value_ >= o.value_; }

  bool is_zero() const noexcept { return value_ == 0.0; }
  bool is_positive() const noexcept { return value_ > 0.0; }
  bool is_non_negative() const noexcept { return value_ >= 0.0; }
  bool is_finite() const noexcept { return std::isfinite(value_); }

  // Clamp-toward-zero helper for accumulator results that may dip infinitesimally below zero
  // due to floating point rounding. Returns a non-negative quantity of the same unit.
  static Quantity clamped(double v) noexcept {
    const double c = (std::isfinite(v) && v >= 0.0) ? v : 0.0;
    return Quantity(c);
  }

  std::string to_string() const;  // defined at bottom of header

 private:
  double value_ = 0.0;
};

template <class Tag>
inline std::string Quantity<Tag>::to_string() const {
  return std::to_string(value_) + std::string(unit_name);
}

// ---- Concrete quantity aliases ------------------------------------------
using Watts = Quantity<WattsTag>;
using Milliwatts = Quantity<MilliwattsTag>;
using Joules = Quantity<JoulesTag>;
using Millijoules = Quantity<MillijoulesTag>;
using Celsius = Quantity<CelsiusTag>;
using Milliseconds = Quantity<MillisecondsTag>;
using Seconds = Quantity<SecondsTag>;
using Utilization = Quantity<UtilizationTag>;
using FrequencyMhz = Quantity<FrequencyMhzTag>;
using MemoryFrequencyMhz = Quantity<MemoryFrequencyMhzTag>;
using EnergyPerToken = Quantity<EnergyPerTokenTag>;
using EnergyPerRequest = Quantity<EnergyPerRequestTag>;
using PowerEfficiency = Quantity<PowerEfficiencyTag>;

// ---- Relative time: exact integer nanoseconds (overflows are rejected). ----
class Duration {
 public:
  static constexpr std::int64_t kMaxNs = std::int64_t(4611686018427387903LL);  // ~146 years
  Duration() noexcept = default;
  explicit Duration(std::int64_t ns) : ns_(ns) { validate(ns_); }

  static Duration nanoseconds(std::int64_t ns) { return Duration(ns); }
  static Duration microseconds(std::int64_t us) { return Duration(check_mul(us, 1000LL)); }
  static Duration milliseconds(std::int64_t ms) { return Duration(check_mul(ms, 1000000LL)); }
  static Duration seconds(std::int64_t s) { return Duration(check_mul(s, 1000000000LL)); }

  std::int64_t ns() const noexcept { return ns_; }
  double seconds_f() const noexcept { return double(ns_) / 1.0e9; }

  Duration operator+(Duration o) const { return Duration(ns_ + o.ns_); }
  Duration operator-(Duration o) const { return Duration(ns_ - o.ns_); }
  Duration operator*(double f) const { return Duration(std::int64_t(double(ns_) * f)); }
  bool operator==(Duration o) const noexcept { return ns_ == o.ns_; }
  bool operator!=(Duration o) const noexcept { return ns_ != o.ns_; }
  bool operator<(Duration o) const noexcept { return ns_ < o.ns_; }
  bool operator<=(Duration o) const noexcept { return ns_ <= o.ns_; }
  bool operator>(Duration o) const noexcept { return ns_ > o.ns_; }
  bool operator>=(Duration o) const noexcept { return ns_ >= o.ns_; }

  bool is_zero() const noexcept { return ns_ == 0; }
  std::string to_string() const { return std::to_string(double(ns_) / 1.0e9) + "s"; }

  static Duration zero() noexcept { return Duration(); }

 private:
  static std::int64_t check_mul(std::int64_t a, std::int64_t b) {
    if (a == 0 || b == 0) return 0;
    if (a > 0 && b > 0 && a > kMaxNs / b) throw std::overflow_error("pg::Duration overflow");
    if (a < 0) throw std::invalid_argument("pg: negative duration");
    return a * b;
  }
  static void validate(std::int64_t ns) {
    if (ns < 0) throw std::invalid_argument("pg: duration must be non-negative");
    if (ns > kMaxNs) throw std::overflow_error("pg::Duration overflow");
  }
  std::int64_t ns_ = 0;
};

}  // namespace pg
