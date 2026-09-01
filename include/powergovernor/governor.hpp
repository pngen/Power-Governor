// Power Governor - the orchestrator.
//
// The Governor binds a backend, a domain tree, a policy, hierarchical budgets, transactional
// reservations, energy/thermal/burst accounting, and the deterministic decision engine into one
// thread-safe runtime. It makes explicit, bounded, explainable power decisions and fences
// distributed state through an authority gate.
//
// Locking: a single std::shared_mutex guards the mutable governance state. Backend sampling,
// hardware control, and filesystem/persistence work are performed outside the lock; only fast
// in-memory state transitions and decision evaluation hold it. This means no global/lock is ever
// held across a CUDA, NVML, socket, callback, or durable-write call.
#pragma once
#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "authority.hpp"
#include "backend.hpp"
#include "budget.hpp"
#include "burst.hpp"
#include "change_history.hpp"
#include "clock.hpp"
#include "decision_engine.hpp"
#include "energy.hpp"
#include "freshness.hpp"
#include "index.hpp"
#include "perf_state.hpp"
#include "persistence.hpp"
#include "policy.hpp"
#include "power_domain.hpp"
#include "reservation.hpp"
#include "thermal.hpp"
#include "units.hpp"
#include "util.hpp"
#include "workload_class.hpp"

namespace pg {

struct GovernorConfig {
  GovernorId id;
  NodeId node;
  WorkerBootId boot;
  CoordinatorEpoch epoch;
};

// Per-device live governance state.
struct DeviceState {
  AcceleratorId accel;
  BackendCapabilities caps;

  // Latest accepted observation.
  std::optional<BackendSample> latest;
  Timestamp last_observed;                 // when the latest sample was accepted
  ObservationGeneration obs_gen;           // advances on each accepted observation

  DeviceGeneration device_gen;
  PowerWindow power_window = PowerWindow(Duration::seconds(2));
  EnergyMeter meter;
  ThermalGovernor thermal;
  BurstBudget burst;
  PerformanceStateSet perf;
  std::optional<Watts> hardware_cap;       // hardware limit reported by the backend / policy

  bool is_thermally_constrained() const noexcept { return thermal.state() == ThermalState::THERMALLY_CONSTRAINED; }
};

class Governor {
 public:
  Governor(Clock& clock, ControlBackend& backend, GovernorConfig cfg = {})
      : clock_(clock), backend_(backend), cfg_(cfg),
        authority_(cfg.epoch, cfg.boot), engine_(DecisionGeneration::first()),
        history_(4096) {
    policy_ = PowerPolicy::defaults(PolicyId::allocate(), "default");
    policy_.generation = PolicyGeneration();  // no policy installed yet; first install advances to 1
  }

  // ---- Domain tree -------------------------------------------------------
  PowerDomainId add_domain(PowerDomainId id, PowerDomainType type,
                           std::optional<PowerDomainId> parent, std::string name, bool physical = false) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    return domains_.add(id, type, parent, std::move(name), physical);
  }
  bool has_domain(PowerDomainId id) const { std::shared_lock<std::shared_mutex> lk(mu_); return domains_.contains(id); }
  const PowerDomainTree& domains() const { return domains_; }

  // ---- Policy ------------------------------------------------------------
  // Install a policy; a stale or equal generation is rejected (no rollback).
  bool install_policy(const PowerPolicy& p) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    if (!p.id.is_valid()) return false;
    if (p.generation <= policy_.generation) return false;
    // Ensure the policy carries valid class rules.
    if (p.class_rules.empty()) return false;
    policy_ = p;
    authority_.set_policy(policy_.generation);
    history_.append(ChangeKind::POLICY_CHANGED, "policy gen " + policy_.generation.to_string(), clock_.now());
    return true;
  }
  const PowerPolicy& current_policy() const { std::shared_lock<std::shared_mutex> lk(mu_); return policy_; }

  // ---- Budgets -----------------------------------------------------------
  bool install_budget(const PowerBudget& b) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    const bool ok = budgets_.install(b);
    if (ok) history_.append(ChangeKind::BUDGET_CHANGED, "budget " + b.id.to_string(), clock_.now());
    return ok;
  }
  bool has_budget(BudgetId id) const { std::shared_lock<std::shared_mutex> lk(mu_); return budgets_.has(id); }
  Watts budget_available(BudgetId id) const { std::shared_lock<std::shared_mutex> lk(mu_); return budgets_.available(id); }

  // ---- Reservations (through ReservationManager) -------------------------
  ReservationManager::Result plan_reservation(WorkloadId wl, WorkloadPowerClass cls, PowerDomainId scope,
                                              BudgetId budget, Watts power, Joules energy,
                                              PolicyGeneration pol, DeviceGeneration dev,
                                              Timestamp now, Duration validity) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    AuthorityEnvelope env = make_envelope();
    auto r = reservations_.plan(wl, cls, scope, budget, power, energy, pol, dev, env, now, validity);
    if (r.ok) {
      res_index_.add(r.id, budget, wl, ReservationState::PLANNED);
      history_.append(ChangeKind::RESERVATION_PLANNED, "reservation " + r.id.to_string() + " planned", now);
    }
    return r;
  }

  ReservationManager::Result validate_reservation(ReservationId id, PolicyGeneration pol, Timestamp now) {
    (void)now;
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto r = reservations_.validate(id, pol);
    if (r.ok) update_index_state(id, ReservationState::VALIDATED);
    return r;
  }
  ReservationManager::Result reserve_reservation(ReservationId id, BudgetGeneration bg, Timestamp now) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto r = reservations_.reserve(id, budgets_, bg, now);
    if (r.ok) update_index_state(id, ReservationState::RESERVED);
    return r;
  }
  ReservationManager::Result admit_reservation(ReservationId id, Timestamp now) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto r = reservations_.admit(id, now);
    if (r.ok) update_index_state(id, ReservationState::ADMITTED);
    return r;
  }
  ReservationManager::Result activate_reservation(ReservationId id, BudgetGeneration bg, Timestamp now) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto r = reservations_.activate(id, budgets_, bg, now);
    if (r.ok) update_index_state(id, ReservationState::ACTIVE);
    return r;
  }
  ReservationManager::Result release_reservation(ReservationId id, BudgetGeneration bg, Timestamp now) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    auto r = reservations_.release(id, budgets_, bg, now);
    if (r.ok) update_index_state(id, ReservationState::RELEASED);
    return r;
  }
  ReservationState reservation_state(ReservationId id) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return reservations_.state(id);
  }
  const Reservation& reservation(ReservationId id) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return reservations_.get(id);
  }
  BudgetManager& budget_manager() { return budgets_; }

  // ---- Observations ------------------------------------------------------
  ObservationId observe_backend(const BackendSample& sample, Timestamp now) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    return ingest_locked(sample, now);
  }

  // Sample the backend outside the lock, then ingest inside.
  ObservationId poll(Timestamp now) {
    Timestamp t = now.is_zero() ? clock_.now() : now;
    BackendSample sample = backend_.sample();        // no lock held
    std::unique_lock<std::shared_mutex> lk(mu_);
    return ingest_locked(sample, t);
  }

  // ---- Decisions ---------------------------------------------------------
  // Fill decision input from device state + budget and evaluate.
  Decision decide_for(BudgetId budget, WorkloadId wl, WorkloadPowerClass cls, Watts requested,
                      Joules requested_energy, double requested_duration_s, Timestamp now) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    return decide_locked(budget, wl, cls, requested, requested_energy, requested_duration_s, now);
  }

  // ---- Energy / accounting -----------------------------------------------
  void account_energy(WorkloadId wl, Joules energy, EnergyProvenance prov, Timestamp now) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    workload_meter_[wl].add(energy, prov, now);
    for (auto& [id, ds] : devices_) {
      (void)id; ds.meter.add(energy, prov, now);
    }
    total_meter_.add(energy, prov, now);
  }
  Joules total_energy() const { std::shared_lock<std::shared_mutex> lk(mu_); return total_meter_.total(); }

  // ---- Hardware control (outside the lock) -------------------------------
  ControlResult set_power_limit(std::optional<Watts> limit) {
    ControlResult r = backend_.set_power_limit(limit);   // no lock held
    std::unique_lock<std::shared_mutex> lk(mu_);
    if (r == ControlResult::APPLIED) {
      history_.append(ChangeKind::POWER_CONTROL_CAPABILITY_CHANGED,
                      limit ? "power limit set to " + limit->to_string() : "power limit cleared", clock_.now());
    }
    return r;
  }
  ControlResult restore_power_limit() {
    ControlResult r = backend_.restore_power_limit();
    return r;
  }
  std::optional<Watts> applied_limit() const { return backend_.applied_limit(); }

  // ---- Queries -----------------------------------------------------------
  std::size_t active_reservations() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return reservations_.active_count();
  }
  std::vector<ReservationId> reservations_by_budget(BudgetId b) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return res_index_.by_budget(b);
  }
  bool accounting_consistent() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return budgets_.accounting_consistent() && reservations_.audit_no_leaks(clock_.now());
  }
  const ChangeHistory& history() const { return history_; }
  GovernorConfig config() const { return cfg_; }

  // ---- Persistence -------------------------------------------------------
  bool save(const std::string& path) {
    std::vector<std::uint8_t> blob;
    {
      std::shared_lock<std::shared_mutex> lk(mu_);
      blob = encode_state();
    }
    PersistEnvelope env; env.blob = blob;
    std::vector<std::uint8_t> bytes = encode_envelope(env);
    std::string err;
    if (!persist_write(path, bytes, err)) return false;
    lock_holder lk(mu_);
    history_.append(ChangeKind::RECOVERED, "state saved to " + path, clock_.now());
    return true;
  }

  bool load(const std::string& path) {
    std::vector<std::uint8_t> bytes;
    std::string err;
    if (!persist_read(path, bytes, err)) return false;
    EnvelopeResult er = decode_envelope(bytes.data(), bytes.size());
    if (!er.ok) return false;
    std::unique_lock<std::shared_mutex> lk(mu_);
    if (!decode_state(er.envelope.blob)) return false;
    history_.append(ChangeKind::RECOVERED, "state recovered from " + path, clock_.now());
    return true;
  }

  // Clear the decision hysteresis (used to prove eligibility returns once a constraint is removed).
  void reset_hysteresis() {
    std::unique_lock<std::shared_mutex> lk(mu_);
    engine_.reset_hysteresis();
  }

  // Stable digest of the persisted state (deterministic), values independent of wall clock.
  std::string state_digest() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    const std::vector<std::uint8_t> b = encode_state();
    return digest_hex(std::string(reinterpret_cast<const char*>(b.data()), b.size()));
  }

  // Recovered telemetry must never be treated as CURRENT: on load we clear live device state.
  void invalidate_live_telemetry() {
    std::unique_lock<std::shared_mutex> lk(mu_);
    for (auto& [id, ds] : devices_) {
      (void)id;
      ds.latest.reset();
      ds.last_observed = Timestamp();
      ds.obs_gen = ObservationGeneration();
      ds.thermal.reset();
    }
  }

 private:
  using lock_holder = std::unique_lock<std::shared_mutex>;

  AuthorityEnvelope make_envelope() const noexcept {
    AuthorityEnvelope e;
    e.epoch = authority_.epoch();
    e.boot = authority_.boot();
    e.policy = authority_.policy();
    e.budget = authority_.budget();
    e.reservation = authority_.reservation();
    e.observation = authority_.observation();
    e.decision = authority_.decision();
    e.device = authority_.device();
    return e;
  }

  // --- ingestion (must hold lock) ----------------------------------------
  ObservationId ingest_locked(const BackendSample& sample, Timestamp now) {
    const AcceleratorId accel = default_accel_;
    DeviceState& ds = devices_[accel];
    ds.accel = accel;
    ds.caps = backend_.capabilities();
    const ObservationGeneration og = ds.obs_gen.next();
    ds.obs_gen = og;
    ObservationId id = ObservationId::allocate();

    if (sample.power) {
      ds.latest = sample;
      ds.last_observed = now;
      ds.power_window.record(now, *sample.power);
      ds.hardware_cap = sample.power_limit;
      // Assume device generation is advanced by the source; use the telemetry generation.
      if (sample.power_limit) { /* keep */ }
    }
    if (sample.temperature) {
      ds.thermal.observe(*sample.temperature, now);
      if (ds.thermal.state() == ThermalState::THERMALLY_CONSTRAINED) {
        history_.append(ChangeKind::DEVICE_THERMALLY_CONSTRAINED, "device thermally constrained", now);
      } else if (ds.thermal.state() == ThermalState::RECOVERING) {
        history_.append(ChangeKind::DEVICE_RECOVERED, "device recovering", now);
      }
    } else if (sample.power) {
      // No temperature in this sample: do not fabricate thermal facts, keep prior observation.
    }
    // energy_total is a cumulative hardware counter and is intentionally not folded into the
    // provenance-aware meter here (that would double count). It is retained in the sample for
    // window/history use by callers.
    authority_.advance_observation(og);
    history_.append(ChangeKind::OBSERVATION_ACCEPTED, "power observation " + id.to_string(), now);
    return id;
  }

  // --- decision (must hold lock) -----------------------------------------
  Decision decide_locked(BudgetId budget, WorkloadId wl, WorkloadPowerClass cls, Watts requested,
                         Joules requested_energy, double requested_duration_s, Timestamp now) {
    (void)wl;
    DecisionInput in;
    in.power_class = cls;
    in.rule = policy_.rule_for(cls);
    in.policy = policy_;
    in.policy_current = true;
    in.requested_power = requested;
    in.requested_energy = requested_energy;
    in.requested_duration_s = requested_duration_s;
    in.requested_perf = requested_perf_for(cls);

    const DeviceState* ds = device_ptr_locked();
    // Compute the authoritative hardware ceiling: the min of fleet/node/device caps and any
    // backend-reported hardware limit.
    std::optional<Watts> cap;
    auto tighten = [&cap](const std::optional<Watts>& v) {
      if (v && v->is_non_negative()) cap = cap ? Watts(std::min(cap->value(), v->value())) : v;
    };
    tighten(policy_.fleet_hard_cap);
    tighten(policy_.node_hard_cap);
    tighten(policy_.device_hard_cap);
    if (ds) tighten(ds->hardware_cap);
    in.hardware_cap = cap;

    if (ds) {
      in.power_known = ds->latest && ds->latest->power.has_value();
      if (in.power_known) in.current_power = *ds->latest->power;
      in.average_known = ds->power_window.size() >= 2;
      in.average_power = Watts(ds->power_window.average_power());
      in.thermal_known = ds->thermal.state() != ThermalState::UNKNOWN;
      in.thermal_state = ds->thermal.state();
      if (in.hardware_cap && in.hardware_cap->is_positive()) {
        const std::optional<Celsius> temp = ds->latest && ds->latest->temperature
            ? ds->latest->temperature : ds->thermal.last_temperature();
        if (temp) in.thermal_ceiling = ds->thermal.permitted_power(*in.hardware_cap, temp);
      }
      in.hardware_throttled = ds->latest && ds->latest->throttled;
      in.burst_available = ds->burst.can_burst(
          Joules(requested.value() * std::max(requested_duration_s, 0.0)), now);
      in.burst_utilization = ds->burst.utilization(now);
      in.power_freshness = freshness_of_locked(ds, now);
      in.evidence = ds->latest ? ds->latest->provenance : EnergyProvenance::UNKNOWN;
    } else {
      in.power_freshness = FreshnessState::EXPIRED;
      in.evidence = EnergyProvenance::UNKNOWN;
    }

    if (budget.is_valid() && budgets_.has(budget)) {
      in.budget_available = budgets_.available(budget);
      in.budget_applied = budget;
      in.parent_budgets = budgets_.chain(budget);
    }
    if (ds && policy_.soft_target) in.soft_target = policy_.soft_target;
    in.active_reservations = reservations_.active_count();

    Decision d = engine_.evaluate(in, now);
    if (is_more_restrictive(d.verdict)) {
      history_.append(ChangeKind::DECISION_CHANGED,
                      "decision " + std::string(to_string(d.verdict)) + " " + d.digest, now);
    }
    return d;
  }

  static bool is_more_restrictive(DecisionVerdict v) {
    return v == DecisionVerdict::THROTTLE || v == DecisionVerdict::DEFER ||
           v == DecisionVerdict::REJECT || v == DecisionVerdict::ALLOW_WITH_LIMIT ||
           v == DecisionVerdict::REVALIDATION_REQUIRED;
  }

  static PerformanceState requested_perf_for(WorkloadPowerClass cls) {
    switch (cls) {
      case WorkloadPowerClass::LATENCY_CRITICAL: return PerformanceState::MAX_PERFORMANCE;
      case WorkloadPowerClass::ENERGY_OPTIMIZED: return PerformanceState::ECO;
      default: return PerformanceState::BALANCED;
    }
  }

  FreshnessState freshness_of_locked(const DeviceState* ds, Timestamp now) const noexcept {
    if (!ds || ds->last_observed.is_zero()) return FreshnessState::EXPIRED;
    FreshnessClassifier fc({policy_.observation_freshness, policy_.observation_stale, policy_.observation_expired});
    return fc.classify(now, ds->last_observed);
  }

  const DeviceState* device_ptr_locked() const noexcept {
    if (devices_.empty()) return nullptr;
    return &devices_.begin()->second;
  }

  // --- persistence encode / decode ---------------------------------------
  std::vector<std::uint8_t> encode_state() const {
    BinWriter w;
    w.u64(cfg_.epoch.raw());
    w.u64(cfg_.boot.raw());
    w.u64(cfg_.node.raw());
    w.u64(policy_.generation.raw());
    w.str(policy_.name);
    // policy scalar knobs
    w.bool_(policy_.fleet_hard_cap.has_value());
    if (policy_.fleet_hard_cap) w.f64(policy_.fleet_hard_cap->value());
    w.bool_(policy_.node_hard_cap.has_value());
    if (policy_.node_hard_cap) w.f64(policy_.node_hard_cap->value());
    w.bool_(policy_.device_hard_cap.has_value());
    if (policy_.device_hard_cap) w.f64(policy_.device_hard_cap->value());
    w.bool_(policy_.soft_target.has_value());
    if (policy_.soft_target) w.f64(policy_.soft_target->value());

    // budgets
    const auto budgets = budgets_.snapshots();
    w.u32(static_cast<std::uint32_t>(budgets.size()));
    for (const auto& b : budgets) {
      w.u64(b.id.raw());
      w.u64(b.generation.raw());
      w.u64(b.domain.raw());
      w.u64(b.parent && b.parent->is_valid() ? b.parent->raw() : 0);
      w.str(b.name);
      w.f64(b.hard_max.value());
      w.f64(b.soft_target.value());
      w.f64(b.min_reserve.value());
      w.f64(b.burst_allowance.value());
      w.i64(b.burst_window.ns());
      w.i64(b.recovery.ns());
      w.i32(b.priority);
      w.bool_(b.authoritative);
    }

    // reservations
    const auto reses = reservations_.all();
    w.u32(static_cast<std::uint32_t>(reses.size()));
    for (const auto& r : reses) {
      w.u64(r.id.raw());
      w.u64(r.budget.raw());
      w.u64(r.workload.raw());
      w.u8(static_cast<std::uint8_t>(r.power_class));
      w.f64(r.requested_power.value());
      w.f64(r.requested_energy.value());
      w.u64(r.policy_generation.raw());
      w.u64(r.device_generation.raw());
      w.u64(r.budget_generation.raw());
      w.u64(r.authority.epoch.raw());
      w.u64(r.authority.boot.raw());
      w.i64(r.created.ns());
      w.i64(r.validity.ns());
      w.u8(static_cast<std::uint8_t>(r.state));
      w.f64(r.reserved_power.value());
      w.f64(r.energy_used.value());
    }

    w.u32(static_cast<std::uint32_t>(workload_meter_.size()));
    for (const auto& [wl, meter] : workload_meter_) {
      w.u64(wl.raw());
      w.f64(meter.total().value());
    }
    return w.take();
  }

  bool decode_state(const std::vector<std::uint8_t>& blob) {
    BinReader r(blob);
    std::uint64_t epoch, boot, node, polgen;
    std::string polname;
    if (!r.u64(epoch) || !r.u64(boot) || !r.u64(node) || !r.u64(polgen)) return false;
    if (!r.str(polname)) return false;
    authority_.reset_epoch(CoordinatorEpoch::from_raw(epoch));
    authority_.set_boot(WorkerBootId::from_raw(boot));
    cfg_.node = NodeId::from_raw(node);
    PowerPolicy p = PowerPolicy::defaults();
    p.id = PolicyId::allocate();
    p.name = polname;
    p.generation = PolicyGeneration::from_raw(polgen);
    // policy scalar knobs
    bool b1; if (!r.bool_(b1)) return false;
    if (b1) { double v; if (!r.f64(v)) return false; p.fleet_hard_cap = Watts(v); }
    bool b2; if (!r.bool_(b2)) return false;
    if (b2) { double v; if (!r.f64(v)) return false; p.node_hard_cap = Watts(v); }
    bool b3; if (!r.bool_(b3)) return false;
    if (b3) { double v; if (!r.f64(v)) return false; p.device_hard_cap = Watts(v); }
    bool b4; if (!r.bool_(b4)) return false;
    if (b4) { double v; if (!r.f64(v)) return false; p.soft_target = Watts(v); }
    policy_ = p;
    authority_.set_policy(policy_.generation);

    std::uint32_t nbudgets; if (!r.u32(nbudgets)) return false;
    if (nbudgets > 100000) return false;
    for (std::uint32_t i = 0; i < nbudgets; ++i) {
      std::uint64_t bid, bgen, dom, parent; std::string name;
      double hmax, soft, reserve, burst; std::int64_t bw, rec; std::int32_t pri; bool auth;
      if (!r.u64(bid) || !r.u64(bgen) || !r.u64(dom) || !r.u64(parent)) return false;
      if (!r.str(name)) return false;
      if (!r.f64(hmax) || !r.f64(soft) || !r.f64(reserve) || !r.f64(burst)) return false;
      if (!r.i64(bw) || !r.i64(rec)) return false;
      if (!r.i32(pri) || !r.bool_(auth)) return false;
      if (hmax < 0.0 || soft < 0.0 || reserve < 0.0 || burst < 0.0) return false;
      PowerBudget b;
      b.id = BudgetId::from_raw(bid); b.generation = BudgetGeneration::from_raw(bgen);
      b.domain = PowerDomainId::from_raw(dom); b.parent = BudgetId::from_raw(parent);
      b.name = name; b.hard_max = Watts(hmax); b.soft_target = Watts(soft);
      b.min_reserve = Watts(reserve); b.burst_allowance = Watts(burst);
      b.burst_window = Duration::nanoseconds(bw); b.recovery = Duration::nanoseconds(rec);
      b.priority = pri; b.authoritative = auth;
      if (!budgets_.install(b)) return false;   // duplicate ID / generation rollback -> reject
    }
    // validate budget hierarchy: every parent must exist; no cycles.
    if (!validate_budget_hierarchy()) return false;

    std::uint32_t nres; if (!r.u32(nres)) return false;
    if (nres > 100000) return false;
    for (std::uint32_t i = 0; i < nres; ++i) {
      std::uint64_t rid, bid, wl, rpol, rdev, rbg, repoch, rboot;
      double power, energy, reserved, energy_used;
      std::uint8_t cls, state;
      std::int64_t created, validity;
      if (!r.u64(rid) || !r.u64(bid) || !r.u64(wl) || !r.u8(cls)) return false;
      if (!r.f64(power) || !r.f64(energy)) return false;
      if (!r.u64(rpol) || !r.u64(rdev) || !r.u64(rbg)) return false;
      if (!r.u64(repoch) || !r.u64(rboot)) return false;
      if (!r.i64(created) || !r.i64(validity)) return false;
      if (!r.u8(state)) return false;
      if (!r.f64(reserved) || !r.f64(energy_used)) return false;
      if (cls > 7 || state > 6) return false;              // invalid enums
      if (!std::isfinite(power) || !std::isfinite(energy) || power < 0.0 ||
          !std::isfinite(reserved) || reserved < 0.0 || !std::isfinite(energy_used) || energy_used < 0.0) {
        return false;                                      // NaN/Inf / negative rejection
      }
      Reservation rr;
      rr.id = ReservationId::from_raw(rid);
      rr.budget = BudgetId::from_raw(bid);
      rr.workload = WorkloadId::from_raw(wl);
      rr.power_class = static_cast<WorkloadPowerClass>(cls);
      rr.requested_power = Watts(power);
      rr.requested_energy = Joules(energy);
      rr.policy_generation = PolicyGeneration::from_raw(rpol);
      rr.device_generation = DeviceGeneration::from_raw(rdev);
      rr.budget_generation = BudgetGeneration::from_raw(rbg);
      rr.authority.epoch = CoordinatorEpoch::from_raw(repoch);
      rr.authority.boot = WorkerBootId::from_raw(rboot);
      rr.created = Timestamp::from_ns(created);
      rr.validity = Duration::nanoseconds(validity);
      rr.state = static_cast<ReservationState>(state);
      rr.reserved_power = Watts(reserved);
      rr.energy_used = Joules(energy_used);
      if (!reservations_.restore(rr)) return false;        // duplicate id rejection
      res_index_.add(rr.id, rr.budget, rr.workload, rr.state);
      // Rebuild budget accounting: a restored, non-released reservation re-claims capacity.
      if ((rr.state == ReservationState::RESERVED || rr.state == ReservationState::ACTIVE ||
           rr.state == ReservationState::ADMITTED) && rr.reserved_power.is_positive() &&
          budgets_.has(rr.budget)) {
        budgets_.reserve(rr.budget, rr.reserved_power, rr.id, rr.budget_generation);
      }
    }

    std::uint32_t nwl; if (!r.u32(nwl)) return false;
    if (nwl > 100000) return false;
    for (std::uint32_t i = 0; i < nwl; ++i) {
      std::uint64_t wl; double en; if (!r.u64(wl) || !r.f64(en)) return false;
      if (en < 0.0 || !std::isfinite(en)) return false;
      workload_meter_[WorkloadId::from_raw(wl)] = EnergyMeter();
      workload_meter_[WorkloadId::from_raw(wl)].add(Joules(en), EnergyProvenance::DERIVED, clock_.now());
    }
    if (!r.at_end()) return false;  // trailing garbage in blob
    // Recovered telemetry is not current.
    invalidate_live_telemetry_locked();
    return true;
  }

  bool validate_budget_hierarchy() const {
    for (const auto& b : budgets_.snapshots()) {
      if (b.parent && b.parent->is_valid() && !budgets_.has(*b.parent)) return false;
    }
    // cycle detection
    for (const auto& b : budgets_.snapshots()) {
      std::optional<BudgetId> cur = b.parent;
      std::size_t guard = 0;
      while (cur && cur->is_valid()) {
        if (*cur == b.id) return false;   // cycle
        const auto st = budgets_.snapshots();
        bool found = false;
        for (const auto& c : st) if (c.id == *cur) { found = true; cur = c.parent; break; }
        if (!found) break;
        if (++guard > 256) return false;
      }
    }
    return true;
  }

  void update_index_state(ReservationId id, ReservationState st) {
    const Reservation& r = reservations_.get(id);
    res_index_.set_state(id, r.budget, r.workload, st);
  }

  void invalidate_live_telemetry_locked() {
    for (auto& [id, ds] : devices_) {
      (void)id; ds.latest.reset(); ds.last_observed = Timestamp();
      ds.obs_gen = ObservationGeneration(); ds.thermal.reset();
    }
  }

  // --- members ------------------------------------------------------------
  Clock& clock_;
  ControlBackend& backend_;
  GovernorConfig cfg_;
  mutable std::shared_mutex mu_;
  PowerDomainTree domains_;
  BudgetManager budgets_;
  ReservationManager reservations_;
  ReservationIndex res_index_;
  PowerPolicy policy_;
  DecisionEngine engine_;
  AuthorityGate authority_;
  ChangeHistory history_;
  std::unordered_map<AcceleratorId, DeviceState> devices_;
  std::unordered_map<WorkloadId, EnergyMeter> workload_meter_;
  EnergyMeter total_meter_;
  AcceleratorId default_accel_ = AcceleratorId::from_raw(1);
};

}  // namespace pg
