// Power Governor - deterministic bounded binary protocol (framed TCP).
//
// Every frame is: magic(u32) protocol_major(u16) msg_type(u16) payload_len(u32) payload(crc32(u32)).
// All payload fields are fixed-width little-endian integers; physical quantities are carried as
// fixed-point (milliwatts, millijoules, centi-degrees, nanoseconds) so NaN/Inf can never appear on
// the wire. The reader rejects malformed lengths, oversized or truncated frames, trailing garbage,
// unknown types, invalid enums, and unsupported protocol versions. Authority fencing (epoch, boot,
// generation) is carried per message so the recipient can reject stale messages deterministically.
#pragma once
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "ids.hpp"
#include "util.hpp"

// windows.h (via winsock headers) defines ERROR as a macro; avoid the collision.
#if defined(ERROR)
#  undef ERROR
#endif

namespace pg {

constexpr std::uint32_t FRAME_MAGIC = 0x43544750u;   // bytes: 'P','G','T','C'
constexpr std::uint16_t PROTOCOL_MAJOR = 1;
constexpr std::size_t MAX_FRAME = 16 * 1024 * 1024;  // 16 MiB
constexpr std::uint32_t HEADER_SIZE = 4 + 2 + 2 + 4;  // magic, major, type, len
constexpr std::uint32_t TRAILER_SIZE = 4;             // crc32

enum class MsgType : std::uint16_t {
  HELLO = 1, REGISTER = 2, POWER_OBSERVATION = 3, THERMAL_OBSERVATION = 4,
  SET_POLICY = 5, SET_BUDGET = 6, RESERVE = 7, ACTIVATE = 8, RELEASE = 9,
  DECISION_REQUEST = 10, DECISION_RESPONSE = 11, THROTTLE_REPORT = 12,
  SNAPSHOT_REQUEST = 13, SNAPSHOT_RESPONSE = 14, ACK = 15, ERROR = 16
};

inline bool is_valid_msg_type(std::uint16_t t) noexcept {
  return t >= 1 && t <= 16;
}

struct Frame {
  MsgType type = MsgType::HELLO;
  std::vector<std::uint8_t> payload;
};

struct FrameResult {
  bool ok = false;
  Frame frame;
  std::string error;
  std::size_t consumed = 0;
};

// ---- Writer ----------------------------------------------------------------
class WireWriter {
 public:
  void u8(std::uint8_t v) { buf_.push_back(v); }
  void u16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v)); u8(static_cast<std::uint8_t>(v >> 8)); }
  void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (i * 8))); }
  void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(v >> (i * 8))); }
  void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
  void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
  void bool_(bool b) { u8(b ? 1 : 0); }
  void str(std::string_view s) {
    if (s.size() > std::numeric_limits<std::uint32_t>::max()) throw std::overflow_error("pg wire: string too long");
    u32(static_cast<std::uint32_t>(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
  }
  void f32(double v) { i64(static_cast<std::int64_t>(v * 1000.0)); }  // 3 decimals fixed
  const std::vector<std::uint8_t>& data() const noexcept { return buf_; }
  std::size_t size() const noexcept { return buf_.size(); }
  std::vector<std::uint8_t> take() { return std::move(buf_); }

 private:
  std::vector<std::uint8_t> buf_;
};

// ---- Reader ----------------------------------------------------------------
class WireReader {
 public:
  WireReader(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}
  explicit WireReader(const std::vector<std::uint8_t>& v) : p_(v.data()), n_(v.size()) {}

  bool u8(std::uint8_t& v) { if (need(1)) { v = p_[pos_++]; return true; } return false; }
  bool u16(std::uint16_t& v) { std::uint8_t a, b; if (u8(a) && u8(b)) { v = static_cast<std::uint16_t>(a | (b << 8)); return true; } return false; }
  bool u32(std::uint32_t& v) { std::uint32_t r = 0; for (int i = 0; i < 4; ++i) { std::uint8_t b; if (!u8(b)) return false; r |= static_cast<std::uint32_t>(b) << (i * 8); } v = r; return true; }
  bool u64(std::uint64_t& v) { std::uint64_t r = 0; for (int i = 0; i < 8; ++i) { std::uint8_t b; if (!u8(b)) return false; r |= static_cast<std::uint64_t>(b) << (i * 8); } v = r; return true; }
  bool i64(std::int64_t& v) { std::uint64_t u; if (!u64(u)) return false; v = static_cast<std::int64_t>(u); return true; }
  bool i32(std::int32_t& v) { std::uint32_t u; if (!u32(u)) return false; v = static_cast<std::int32_t>(u); return true; }
  bool bool_(bool& b) { std::uint8_t v; if (!u8(v)) return false; b = v != 0; return true; }
  bool str(std::string& s) {
    std::uint32_t len; if (!u32(len)) return false;
    if (len > n_ - pos_) return false;
    s.assign(reinterpret_cast<const char*>(p_ + pos_), len);
    pos_ += len; return true;
  }
  bool f32(double& v) { std::int64_t x; if (!i64(x)) return false; v = static_cast<double>(x) / 1000.0; return true; }
  bool bytes(std::vector<std::uint8_t>& b) {
    std::uint32_t len; if (!u32(len)) return false;
    if (len > n_ - pos_) return false;
    b.assign(p_ + pos_, p_ + pos_ + len);
    pos_ += len; return true;
  }
  std::size_t remaining() const noexcept { return n_ - pos_; }
  bool at_end() const noexcept { return pos_ == n_; }

 private:
  bool need(std::size_t k) const noexcept { return pos_ + k <= n_; }
  const std::uint8_t* p_;
  std::size_t n_;
  std::size_t pos_ = 0;
};

// ---- Frame encode / decode -------------------------------------------------
inline Frame encode_frame(MsgType type, const std::vector<std::uint8_t>& payload) {
  Frame f;
  f.type = type;
  f.payload = payload;
  return f;
}

inline std::vector<std::uint8_t> serialize_frame(const Frame& f) {
  if (f.payload.size() > MAX_FRAME) throw std::length_error("pg wire: frame exceeds MAX_FRAME");
  std::vector<std::uint8_t> out;
  WireWriter w;
  w.u32(FRAME_MAGIC);
  w.u16(PROTOCOL_MAJOR);
  w.u16(static_cast<std::uint16_t>(f.type));
  w.u32(static_cast<std::uint32_t>(f.payload.size()));
  out = w.take();
  out.insert(out.end(), f.payload.begin(), f.payload.end());
  const std::uint32_t crc = crc32(out.data(), out.size());
  std::uint8_t tmp[4];
  std::memcpy(tmp, &crc, 4);  // little-endian via native
  for (int i = 0; i < 4; ++i) out.push_back(tmp[i]);
  return out;
}

inline FrameResult deserialize_frame(const std::uint8_t* data, std::size_t n) {
  FrameResult r;
  if (n < HEADER_SIZE + TRAILER_SIZE) { r.error = "frame too small"; return r; }
  WireReader hr(data, n);
  std::uint32_t magic; std::uint16_t major, type; std::uint32_t len;
  if (!hr.u32(magic) || !hr.u16(major) || !hr.u16(type) || !hr.u32(len)) {
    r.error = "malformed header"; return r;
  }
  if (magic != FRAME_MAGIC) { r.error = "bad magic"; return r; }
  if (major != PROTOCOL_MAJOR) { r.error = "unsupported protocol version"; return r; }
  if (!is_valid_msg_type(type)) { r.error = "invalid message type"; return r; }
  if (len > MAX_FRAME) { r.error = "oversized frame"; return r; }
  const std::size_t total = HEADER_SIZE + static_cast<std::size_t>(len) + TRAILER_SIZE;
  if (n < total) { r.error = "truncated payload"; return r; }
  const std::uint8_t* payload = data + HEADER_SIZE;
  const std::uint32_t stored_crc = *reinterpret_cast<const std::uint32_t*>(data + HEADER_SIZE + len);
  const std::uint32_t calc_crc = crc32(data, HEADER_SIZE + len);
  if (stored_crc != calc_crc) { r.error = "crc mismatch"; return r; }
  r.ok = true;
  r.consumed = total;
  r.frame.type = static_cast<MsgType>(type);
  r.frame.payload.assign(payload, payload + len);
  return r;
}

// ---- Typed message payloads ------------------------------------------------
struct RegisterMsg {
  WorkerId worker; WorkerBootId boot; NodeId node; std::string name;
};
struct PowerObsMsg {
  WorkerId worker; WorkerBootId boot; CoordinatorEpoch epoch; AcceleratorId accel;
  DeviceGeneration device_gen; ObservationGeneration obs_gen;
  std::int64_t timestamp_ns = 0;
  std::int64_t power_mw = 0; bool has_power = false;
  bool has_temp = false; std::int32_t temp_centi = 0;
  std::uint32_t sm_mhz = 0; std::uint32_t mem_mhz = 0;
  std::uint16_t utilization_bp = 0; bool has_util = false;
  std::int64_t energy_mj = 0; bool has_energy = false;
  bool throttled = false; std::uint8_t provenance = 0; std::string source;
};
struct ThermalObsMsg {
  WorkerId worker; WorkerBootId boot; CoordinatorEpoch epoch; AcceleratorId accel;
  ThermalGeneration therm_gen; ObservationGeneration obs_gen;
  std::int64_t timestamp_ns = 0; bool has_temp = false; std::int32_t temp_centi = 0;
  std::uint8_t state = 0;
};
struct BudgetMsg {
  BudgetId budget; BudgetGeneration generation; PowerDomainId domain;
  BudgetId parent; std::string name;
  std::int64_t hard_max_mw = 0; std::int64_t soft_mw = 0; std::int64_t min_reserve_mw = 0;
  std::int64_t burst_mw = 0; std::int64_t burst_window_ns = 0; std::int64_t recovery_ns = 0;
  std::int32_t priority = 50; bool authoritative = true; std::uint64_t policy_gen = 0;
};
struct ReserveMsg {
  ReservationId reservation; BudgetId budget;
  std::int64_t requested_mw = 0; std::int64_t requested_mj = 0;
  BudgetGeneration budget_gen; ReservationGeneration reservation_gen;
  PolicyGeneration policy_gen; DeviceGeneration device_gen;
  WorkloadId workload; std::uint8_t cls = 0; PowerDomainId scope;
  CoordinatorEpoch epoch; WorkerBootId boot;
  std::int64_t timestamp_ns = 0; std::int64_t validity_ns = 0;
};
struct BudgetActionMsg {
  ReservationId reservation; BudgetId budget; std::int64_t amount_mw = 0;
  BudgetGeneration budget_gen; CoordinatorEpoch epoch; WorkerBootId boot;
};
struct AckMsg {
  std::uint64_t message_id = 0; bool ok = true; std::string message;
};

// Register
inline std::vector<std::uint8_t> encode_register(const RegisterMsg& m) {
  WireWriter w; w.u64(m.worker.raw()); w.u64(m.boot.raw()); w.u64(m.node.raw()); w.str(m.name);
  return w.take();
}
inline std::optional<RegisterMsg> decode_register(const Frame& f) {
  RegisterMsg m; WireReader r(f.payload);
  std::uint64_t a, b, c; if (!r.u64(a) || !r.u64(b) || !r.u64(c)) return std::nullopt;
  m.worker = WorkerId::from_raw(a); m.boot = WorkerBootId::from_raw(b); m.node = NodeId::from_raw(c);
  if (!r.str(m.name)) return std::nullopt;
  if (!r.at_end()) return std::nullopt;  // trailing garbage
  return m;
}

// Power observation
inline std::vector<std::uint8_t> encode_power_obs(const PowerObsMsg& m) {
  WireWriter w;
  w.u64(m.worker.raw()); w.u64(m.boot.raw()); w.u64(m.epoch.raw()); w.u64(m.accel.raw());
  w.u64(m.device_gen.raw()); w.u64(m.obs_gen.raw()); w.i64(m.timestamp_ns);
  w.bool_(m.has_power); w.i64(m.power_mw);
  w.bool_(m.has_temp); w.i32(m.temp_centi);
  w.u32(m.sm_mhz); w.u32(m.mem_mhz);
  w.bool_(m.has_util); w.u16(m.utilization_bp);
  w.bool_(m.has_energy); w.i64(m.energy_mj);
  w.bool_(m.throttled); w.u8(m.provenance); w.str(m.source);
  return w.take();
}
inline std::optional<PowerObsMsg> decode_power_obs(const Frame& f) {
  PowerObsMsg m; WireReader r(f.payload);
  std::uint64_t a, b, ce, c, d, e;
  if (!r.u64(a) || !r.u64(b) || !r.u64(ce) || !r.u64(c) || !r.u64(d) || !r.u64(e)) return std::nullopt;
  m.worker = WorkerId::from_raw(a); m.boot = WorkerBootId::from_raw(b); m.epoch = CoordinatorEpoch::from_raw(ce); m.accel = AcceleratorId::from_raw(c);
  m.device_gen = DeviceGeneration::from_raw(d); m.obs_gen = ObservationGeneration::from_raw(e);
  if (!r.i64(m.timestamp_ns)) return std::nullopt;
  if (!r.bool_(m.has_power) || !r.i64(m.power_mw)) return std::nullopt;
  if (!r.bool_(m.has_temp) || !r.i32(m.temp_centi)) return std::nullopt;
  if (!r.u32(m.sm_mhz) || !r.u32(m.mem_mhz)) return std::nullopt;
  if (!r.bool_(m.has_util) || !r.u16(m.utilization_bp)) return std::nullopt;
  if (!r.bool_(m.has_energy) || !r.i64(m.energy_mj)) return std::nullopt;
  if (!r.bool_(m.throttled) || !r.u8(m.provenance)) return std::nullopt;
  if (!r.str(m.source)) return std::nullopt;
  if (!r.at_end()) return std::nullopt;
  return m;
}

// Thermal observation
inline std::vector<std::uint8_t> encode_thermal_obs(const ThermalObsMsg& m) {
  WireWriter w;
  w.u64(m.worker.raw()); w.u64(m.boot.raw()); w.u64(m.epoch.raw()); w.u64(m.accel.raw());
  w.u64(m.therm_gen.raw()); w.u64(m.obs_gen.raw()); w.i64(m.timestamp_ns);
  w.bool_(m.has_temp); w.i32(m.temp_centi); w.u8(m.state);
  return w.take();
}
inline std::optional<ThermalObsMsg> decode_thermal_obs(const Frame& f) {
  ThermalObsMsg m; WireReader r(f.payload);
  std::uint64_t a, b, ce, c, d, e;
  if (!r.u64(a) || !r.u64(b) || !r.u64(ce) || !r.u64(c) || !r.u64(d) || !r.u64(e)) return std::nullopt;
  m.worker = WorkerId::from_raw(a); m.boot = WorkerBootId::from_raw(b); m.epoch = CoordinatorEpoch::from_raw(ce); m.accel = AcceleratorId::from_raw(c);
  m.therm_gen = ThermalGeneration::from_raw(d); m.obs_gen = ObservationGeneration::from_raw(e);
  if (!r.i64(m.timestamp_ns)) return std::nullopt;
  if (!r.bool_(m.has_temp) || !r.i32(m.temp_centi)) return std::nullopt;
  std::uint8_t st; if (!r.u8(st)) return std::nullopt;
  if (st > 6) return std::nullopt;  // invalid ThermalState enum
  m.state = st;
  if (!r.at_end()) return std::nullopt;
  return m;
}

// Budget
inline std::vector<std::uint8_t> encode_budget(const BudgetMsg& m) {
  WireWriter w;
  w.u64(m.budget.raw()); w.u64(m.generation.raw()); w.u64(m.domain.raw());
  w.u64(m.parent.raw()); w.str(m.name);
  w.i64(m.hard_max_mw); w.i64(m.soft_mw); w.i64(m.min_reserve_mw);
  w.i64(m.burst_mw); w.i64(m.burst_window_ns); w.i64(m.recovery_ns);
  w.i32(m.priority); w.bool_(m.authoritative); w.u64(m.policy_gen);
  return w.take();
}
inline std::optional<BudgetMsg> decode_budget(const Frame& f) {
  BudgetMsg m; WireReader r(f.payload);
  std::uint64_t a, b, c, d;
  if (!r.u64(a) || !r.u64(b) || !r.u64(c) || !r.u64(d)) return std::nullopt;
  m.budget = BudgetId::from_raw(a); m.generation = BudgetGeneration::from_raw(b); m.domain = PowerDomainId::from_raw(c);
  m.parent = BudgetId::from_raw(d);
  if (!r.str(m.name)) return std::nullopt;
  if (!r.i64(m.hard_max_mw) || !r.i64(m.soft_mw) || !r.i64(m.min_reserve_mw)) return std::nullopt;
  if (!r.i64(m.burst_mw) || !r.i64(m.burst_window_ns) || !r.i64(m.recovery_ns)) return std::nullopt;
  if (!r.i32(m.priority) || !r.bool_(m.authoritative)) return std::nullopt;
  if (!r.u64(m.policy_gen)) return std::nullopt;
  if (!r.at_end()) return std::nullopt;
  return m;
}

// Reserve
inline std::vector<std::uint8_t> encode_reserve(const ReserveMsg& m) {
  WireWriter w;
  w.u64(m.reservation.raw()); w.u64(m.budget.raw());
  w.i64(m.requested_mw); w.i64(m.requested_mj);
  w.u64(m.budget_gen.raw()); w.u64(m.reservation_gen.raw()); w.u64(m.policy_gen.raw());
  w.u64(m.device_gen.raw()); w.u64(m.workload.raw()); w.u8(m.cls);
  w.u64(m.scope.raw()); w.u64(m.epoch.raw()); w.u64(m.boot.raw());
  w.i64(m.timestamp_ns); w.i64(m.validity_ns);
  return w.take();
}
inline std::optional<ReserveMsg> decode_reserve(const Frame& f) {
  ReserveMsg m; WireReader r(f.payload);
  std::uint64_t res, budget, bgen, rgen, pgen, dgen, wl, scopeA, epochA, bootA;
  std::int64_t reqmw, reqmj, valns;
  if (!r.u64(res) || !r.u64(budget) || !r.i64(reqmw) || !r.i64(reqmj) || !r.u64(bgen) ||
      !r.u64(rgen) || !r.u64(pgen) || !r.u64(dgen) || !r.u64(wl) || !r.u8(m.cls) ||
      !r.u64(scopeA) || !r.u64(epochA) || !r.u64(bootA) || !r.i64(m.timestamp_ns) || !r.i64(valns)) {
    return std::nullopt;
  }
  if (m.cls > 7) return std::nullopt;  // invalid WorkloadPowerClass enum
  m.reservation = ReservationId::from_raw(res); m.budget = BudgetId::from_raw(budget);
  m.requested_mw = reqmw; m.requested_mj = reqmj;
  m.budget_gen = BudgetGeneration::from_raw(bgen); m.reservation_gen = ReservationGeneration::from_raw(rgen);
  m.policy_gen = PolicyGeneration::from_raw(pgen); m.device_gen = DeviceGeneration::from_raw(dgen);
  m.workload = WorkloadId::from_raw(wl); m.scope = PowerDomainId::from_raw(scopeA);
  m.epoch = CoordinatorEpoch::from_raw(epochA); m.boot = WorkerBootId::from_raw(bootA);
  m.validity_ns = valns;
  if (!r.at_end()) return std::nullopt;
  return m;
}
// BudgetAction (activate/release share the shape)
inline std::vector<std::uint8_t> encode_budget_action(const BudgetActionMsg& m) {
  WireWriter w;
  w.u64(m.reservation.raw()); w.u64(m.budget.raw()); w.i64(m.amount_mw);
  w.u64(m.budget_gen.raw()); w.u64(m.epoch.raw()); w.u64(m.boot.raw());
  return w.take();
}
inline std::optional<BudgetActionMsg> decode_budget_action(const Frame& f) {
  BudgetActionMsg m; WireReader r(f.payload);
  std::uint64_t a, b, d, e, g;
  if (!r.u64(a) || !r.u64(b) || !r.i64(m.amount_mw) || !r.u64(d) || !r.u64(e) || !r.u64(g)) return std::nullopt;
  m.reservation = ReservationId::from_raw(a); m.budget = BudgetId::from_raw(b);
  m.budget_gen = BudgetGeneration::from_raw(d); m.epoch = CoordinatorEpoch::from_raw(e);
  m.boot = WorkerBootId::from_raw(g);
  if (!r.at_end()) return std::nullopt;
  return m;
}

inline std::vector<std::uint8_t> encode_ack(const AckMsg& m) {
  WireWriter w; w.u64(m.message_id); w.bool_(m.ok); w.str(m.message);
  return w.take();
}
inline std::optional<AckMsg> decode_ack(const Frame& f) {
  AckMsg m; WireReader r(f.payload);
  if (!r.u64(m.message_id) || !r.bool_(m.ok) || !r.str(m.message)) return std::nullopt;
  if (!r.at_end()) return std::nullopt;
  return m;
}

}  // namespace pg
