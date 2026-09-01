// Power Governor - injectable, deterministic clock.
//
// Everything that needs wall time goes through a pg::Clock so tests and the distributed protocol
// can be fully deterministic. No test relies on sleeping; time is advanced explicitly.
#pragma once
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "ids.hpp"
#include "units.hpp"

namespace pg {

// A point in time measured as nanoseconds since the Unix epoch. value 0 means "never observed".
class Timestamp {
 public:
  Timestamp() noexcept = default;
  explicit Timestamp(std::int64_t ns) noexcept : ns_(ns) {}

  static Timestamp from_ns(std::int64_t ns) { return Timestamp(ns); }
  static Timestamp from_seconds(double s) { return Timestamp(static_cast<std::int64_t>(s * 1.0e9)); }

  std::int64_t ns() const noexcept { return ns_; }
  double seconds_f() const noexcept { return static_cast<double>(ns_) / 1.0e9; }
  bool is_zero() const noexcept { return ns_ == 0; }

  Duration elapsed_since(Timestamp a) const { return Duration(ns_ > a.ns_ ? ns_ - a.ns_ : 0); }

  Timestamp operator+(Duration d) const { return Timestamp(ns_ + d.ns()); }
  Timestamp operator-(Duration d) const { return Timestamp(ns_ - d.ns()); }

  bool operator==(Timestamp o) const noexcept { return ns_ == o.ns_; }
  bool operator!=(Timestamp o) const noexcept { return ns_ != o.ns_; }
  bool operator<(Timestamp o) const noexcept { return ns_ < o.ns_; }
  bool operator<=(Timestamp o) const noexcept { return ns_ <= o.ns_; }
  bool operator>(Timestamp o) const noexcept { return ns_ > o.ns_; }
  bool operator>=(Timestamp o) const noexcept { return ns_ >= o.ns_; }

  std::string to_string() const {
    if (ns_ == 0) return "never";
    return std::to_string(ns_) + "ns";
  }

 private:
  std::int64_t ns_ = 0;
};

// -------------------------------------------------------------------------
// Clock abstraction.
// -------------------------------------------------------------------------
class Clock {
 public:
  virtual ~Clock() = default;
  virtual Timestamp now() const = 0;
};

// Real wall-clock in nanoseconds since the Unix epoch, monotonic within a run.
class SystemClock final : public Clock {
 public:
  Timestamp now() const override {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return Timestamp(ns);
  }
};

// Deterministic test clock. Time only advances when advance()/set() is called.
class TestClock final : public Clock {
 public:
  explicit TestClock(Timestamp t = Timestamp()) : t_(t) {}

  Timestamp now() const override { return t_; }
  void set(Timestamp t) noexcept { t_ = t; }
  void advance(Duration d) noexcept { t_ = t_ + d; }

 private:
  Timestamp t_;
};

}  // namespace pg
