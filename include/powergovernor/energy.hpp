// Power Governor - energy accounting with provenance.
//
// Energy is never presented as measured unless it is: every sample carries an explicit provenance
// (MEASURED, REPORTED, DERIVED, ESTIMATED, UNKNOWN) and the account keeps the per-provenance totals
// separate. Instantaneous power, integrated energy, average power, and peak power are distinct
// concepts here and are never conflated.
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include "clock.hpp"
#include "ids.hpp"
#include "policy.hpp"
#include "units.hpp"

namespace pg {

enum class EnergyProvenance : std::uint8_t {
  MEASURED, REPORTED, DERIVED, ESTIMATED, UNKNOWN
};

inline std::string_view to_string(EnergyProvenance p) noexcept {
  switch (p) {
    case EnergyProvenance::MEASURED: return "MEASURED";
    case EnergyProvenance::REPORTED: return "REPORTED";
    case EnergyProvenance::DERIVED: return "DERIVED";
    case EnergyProvenance::ESTIMATED: return "ESTIMATED";
    case EnergyProvenance::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}

// An energy sample, including where it came from.
struct EnergySample {
  Joules joules{0.0};
  EnergyProvenance provenance = EnergyProvenance::UNKNOWN;
  Timestamp at;
  WorkerBootId boot;
  ObservationGeneration generation;
};

// Accumulates energy with per-provenance accounting. Rejects negative/non-finite samples.
class EnergyMeter {
 public:
  void add(Joules j, EnergyProvenance p, Timestamp at) {
    if (j.value() < 0.0 || !std::isfinite(j.value())) {
      throw std::invalid_argument("pg::EnergyMeter::add: negative or non-finite energy");
    }
    total_ = Joules::clamped(total_.value() + j.value());
    by_provenance_[static_cast<std::size_t>(p)] = Joules::clamped(
        by_provenance_[static_cast<std::size_t>(p)].value() + j.value());
    last_ = at;
    ++samples_;
  }

  void add(const EnergySample& s) { add(s.joules, s.provenance, s.at); }

  Joules total() const noexcept { return total_; }
  Joules by(EnergyProvenance p) const noexcept { return by_provenance_[static_cast<std::size_t>(p)]; }
  Timestamp last() const noexcept { return last_; }
  std::uint64_t samples() const noexcept { return samples_; }
  bool empty() const noexcept { return samples_ == 0; }

 private:
  Joules total_{0.0};
  std::array<Joules, 5> by_provenance_{};
  Timestamp last_;
  std::uint64_t samples_ = 0;
};

// Distinguishes instantaneous, average, and peak power over a bounded window using real timestamps.
class PowerWindow {
 public:
  explicit PowerWindow(Duration capacity = Duration::seconds(1)) : capacity_(capacity) {}

  void record(Timestamp t, Watts p) {
    if (!std::isfinite(p.value())) throw std::invalid_argument("pg::PowerWindow::record: non-finite");
    if (p.value() < 0.0) throw std::invalid_argument("pg::PowerWindow::record: negative power");
    if (!samples_.empty() && t < samples_.back().t) {
      throw std::invalid_argument("pg::PowerWindow::record: out-of-order timestamp");
    }
    samples_.push_back(Sample{t, p});
    if (p.value() > peak_.value()) peak_ = p;
    trim(t);
  }

  std::size_t size() const noexcept { return samples_.size(); }
  Watts peak() const noexcept { return peak_; }
  Duration capacity() const noexcept { return capacity_; }

  // Average power over the time span of the retained samples (trapezoidal integration).
  double average_power() const noexcept {
    if (samples_.size() < 2) return samples_.empty() ? 0.0 : samples_.front().p.value();
    Joules e = integrated_energy_us();
    const double span = span_seconds();
    if (span <= 0.0) return samples_.back().p.value();
    return e.value() / span;
  }
  double span_seconds() const noexcept {
    if (samples_.size() < 2) return 0.0;
    return (samples_.back().t.elapsed_since(samples_.front().t)).seconds_f();
  }
  // Energy integrated over the retained samples (trapezoidal, in joules).
  Joules integrated_energy_us() const noexcept {
    if (samples_.size() < 2) return Joules(0.0);
    double acc = 0.0;
    for (std::size_t i = 1; i < samples_.size(); ++i) {
      const double dt = (samples_[i].t.elapsed_since(samples_[i - 1].t)).seconds_f();
      const double p = 0.5 * (samples_[i - 1].p.value() + samples_[i].p.value());
      acc += p * dt;
    }
    return Joules(acc);
  }

  void clear() noexcept { samples_.clear(); peak_ = Watts(0.0); }

 private:
  struct Sample { Timestamp t; Watts p; };
  void trim(Timestamp now) {
    while (samples_.size() > 2 && (now.elapsed_since(samples_.front().t)) > capacity_) {
      samples_.pop_front();
    }
  }
  Duration capacity_;
  std::deque<Sample> samples_;
  Watts peak_{0.0};
};

// Energy budget over a window (e.g. joules per request class, per batch, per workload).
class EnergyWindowAccount {
 public:
  explicit EnergyWindowAccount(EnergyWindow cfg = {}) : cfg_(cfg) {}

  void set_config(EnergyWindow cfg) { cfg_ = cfg; reset(); }
  const EnergyWindow& config() const noexcept { return cfg_; }

  void reset() noexcept {
    consumed_ = Joules(0.0);
    window_start_ = Timestamp();
    started_ = false;
  }

  // Remaining joules in the current window; refills when the window elapses.
  Joules remaining(Timestamp now) const {
    if (!cfg_.enforce) return Joules(0.0);         // unbounded; see can_consume
    if (started_ && now.elapsed_since(window_start_) >= cfg_.window) return cfg_.budget;
    const double left = cfg_.budget.value() - consumed_.value();
    return Joules(left < 0.0 ? 0.0 : left);
  }

  bool can_consume(Joules j, Timestamp now) const {
    if (!cfg_.enforce) return j.value() >= 0.0 && std::isfinite(j.value());
    return remaining(now).value() + 1e-9 >= j.value() && j.value() >= 0.0;
  }

  // Consume joules; refills if the window elapses. Returns false (and consumes nothing) when it
  // would overrun an enforced energy budget.
  bool consume(Joules j, Timestamp now) {
    if (!cfg_.enforce) {
      consumed_ = Joules::clamped(consumed_.value() + j.value());
      return true;
    }
    if (!started_) { window_start_ = now; started_ = true; }
    else if (now.elapsed_since(window_start_) >= cfg_.window) {
      window_start_ = now;
      consumed_ = Joules(0.0);
    }
    if (!can_consume(j, now)) return false;
    consumed_ = Joules::clamped(consumed_.value() + j.value());
    return true;
  }

  Joules consumed(Timestamp now) const {
    if (cfg_.enforce && started_ && now.elapsed_since(window_start_) >= cfg_.window) return Joules(0.0);
    return consumed_;
  }

 private:
  EnergyWindow cfg_;
  Joules consumed_{0.0};
  Timestamp window_start_;
  bool started_ = false;
};

}  // namespace pg
