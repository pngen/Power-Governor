// Power Governor - distributed proof runtime (coordinator + worker over framed TCP).
// Both run as real OS processes; the CLI run-coordinator / run-worker subcommands wire them.
#ifndef _CRT_SECURE_NO_WARNINGS
#  define _CRT_SECURE_NO_WARNINGS
#endif
#include "powergovernor/governor.hpp"
#include "powergovernor/net.hpp"
#include "powergovernor/protocol.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pgd {
using namespace pg;

struct WorkerCtx {
  WorkerId worker; WorkerBootId boot; NodeId node;
  AcceleratorId accel = AcceleratorId::from_raw(1);
  ObservationGeneration obs_gen = ObservationGeneration::first();
  CoordinatorEpoch epoch;
  SOCKET sock = INVALID_SOCKET;
};

inline bool read_line(const char* prompt, std::string& line) {
  (void)prompt;
  line.clear();
  int c;
  while ((c = std::getchar()) != EOF) {
    if (c == '\n') return true;
    line.push_back(static_cast<char>(c));
  }
  return !line.empty();
}

inline bool send_ack(SOCKET s, std::uint64_t id, bool ok, const std::string& msg) {
  AckMsg a; a.message_id = id; a.ok = ok; a.message = msg;
  return send_frame(s, Frame{MsgType::ACK, encode_ack(a)});
}

inline bool send_error(SOCKET s, std::uint64_t id, const std::string& msg) {
  AckMsg a; a.message_id = id; a.ok = false; a.message = "ERROR:" + msg;
  return send_frame(s, Frame{MsgType::ERROR, encode_ack(a)});
}

// --- worker-side ----------------------------------------------------
// Registers and then processes a script of commands from `cmds`; each response is
// prefixed to `out` so a test can assert on accepted/rejected actions.
bool worker_register(WorkerCtx& w, std::function<void(const std::string&)> emit) {
  RegisterMsg reg; reg.worker = w.worker; reg.boot = w.boot; reg.node = w.node; reg.name = "w";
  if (!send_frame(w.sock, Frame{MsgType::REGISTER, encode_register(reg)})) return false;
  Frame ack; if (!recv_frame(w.sock, ack)) return false;
  auto ra0 = decode_ack(ack);
  if (!ra0 || !ra0->ok) { emit("REGISTER rejected: " + (ra0 ? ra0->message : "malformed")); return false; }
  emit("REGISTER ok: " + ra0->message);
  try { w.epoch = CoordinatorEpoch::from_raw(std::stoull(ra0->message)); } catch (...) { w.epoch = CoordinatorEpoch::first(); }
  return true;
}

// Dispatch a single worker command. Returns false on "quit" or fatal connection loss.
bool worker_dispatch(WorkerCtx& w, const std::string& cmd, std::function<void(const std::string&)> emit) {
  if (cmd.rfind("quit", 0) == 0) return false;
  if (cmd.rfind("observe ", 0) == 0) {
    long mw, centi; if (std::sscanf(cmd.c_str(), "observe %ld %ld", &mw, &centi) != 2) { emit("observe: bad args"); return true; }
    PowerObsMsg m; m.worker = w.worker; m.boot = w.boot; m.epoch = w.epoch; m.accel = w.accel;
    m.device_gen = DeviceGeneration::first(); m.obs_gen = w.obs_gen; w.obs_gen = w.obs_gen.next();
    m.timestamp_ns = 1000000; m.has_power = true; m.power_mw = mw;
    m.has_temp = true; m.temp_centi = centi; m.has_util = true; m.utilization_bp = 5000;
    m.provenance = 1; m.source = "fake";
    if (!send_frame(w.sock, Frame{MsgType::POWER_OBSERVATION, encode_power_obs(m)})) { emit("observe: closed"); return false; }
    Frame r; if (!recv_frame(w.sock, r)) { emit("observe: connection closed"); return false; }
    auto ra = decode_ack(r); emit("observe -> " + (ra ? (ra->ok ? "ACCEPT" : ra->message) : "bad ack"));
  } else if (cmd.rfind("reserve ", 0) == 0) {
    unsigned long long budget; long mw; if (std::sscanf(cmd.c_str(), "reserve %llu %ld", &budget, &mw) != 2) { emit("reserve: bad args"); return true; }
    ReserveMsg m; m.budget = BudgetId::from_raw(budget); m.requested_mw = mw; m.requested_mj = 0;
    m.budget_gen = BudgetGeneration::first(); m.reservation_gen = ReservationGeneration::first();
    m.policy_gen = PolicyGeneration::first(); m.device_gen = DeviceGeneration::first();
    m.workload = WorkloadId::from_raw(w.worker.raw()); m.cls = 1; m.scope = PowerDomainId::from_raw(1);
    m.epoch = w.epoch; m.boot = w.boot; m.timestamp_ns = 1000000; m.validity_ns = 5000000000LL;
    if (!send_frame(w.sock, Frame{MsgType::RESERVE, encode_reserve(m)})) { emit("reserve: closed"); return false; }
    Frame r; if (!recv_frame(w.sock, r)) { emit("reserve: connection closed"); return false; }
    auto ra = decode_ack(r); emit("reserve -> " + (ra ? (ra->ok ? "ACCEPT:" + ra->message : ra->message) : "bad ack"));
  } else if (cmd.rfind("activate ", 0) == 0) {
    unsigned long long res, budget; long mw; if (std::sscanf(cmd.c_str(), "activate %llu %llu %ld", &res, &budget, &mw) != 3) { emit("activate: bad args"); return true; }
    BudgetActionMsg m; m.reservation = ReservationId::from_raw(res); m.budget = BudgetId::from_raw(budget); m.amount_mw = mw;
    m.budget_gen = BudgetGeneration::first(); m.epoch = w.epoch; m.boot = w.boot;
    if (!send_frame(w.sock, Frame{MsgType::ACTIVATE, encode_budget_action(m)})) { emit("activate: closed"); return false; }
    Frame r; if (!recv_frame(w.sock, r)) { emit("activate: connection closed"); return false; }
    auto ra = decode_ack(r); emit("activate -> " + (ra ? (ra->ok ? "ACCEPT" : ra->message) : "bad ack"));
  } else if (cmd.rfind("release ", 0) == 0) {
    unsigned long long res, budget; long mw; if (std::sscanf(cmd.c_str(), "release %llu %llu %ld", &res, &budget, &mw) != 3) { emit("release: bad args"); return true; }
    BudgetActionMsg m; m.reservation = ReservationId::from_raw(res); m.budget = BudgetId::from_raw(budget); m.amount_mw = mw;
    m.budget_gen = BudgetGeneration::first(); m.epoch = w.epoch; m.boot = w.boot;
    if (!send_frame(w.sock, Frame{MsgType::RELEASE, encode_budget_action(m)})) { emit("release: closed"); return false; }
    Frame r; if (!recv_frame(w.sock, r)) { emit("release: connection closed"); return false; }
    auto ra = decode_ack(r); emit("release -> " + (ra ? (ra->ok ? "ACCEPT" : ra->message) : "bad ack"));
  } else if (cmd.rfind("decision ", 0) == 0) {
    unsigned long long budget; long mw; if (std::sscanf(cmd.c_str(), "decision %llu %ld", &budget, &mw) != 2) { emit("decision: bad args"); return true; }
    ReserveMsg m; m.budget = BudgetId::from_raw(budget); m.requested_mw = mw; m.cls = 1;
    m.epoch = w.epoch; m.boot = w.boot; m.workload = WorkloadId::from_raw(w.worker.raw());
    if (!send_frame(w.sock, Frame{MsgType::DECISION_REQUEST, encode_reserve(m)})) { emit("decision: closed"); return false; }
    Frame r; if (!recv_frame(w.sock, r)) { emit("decision: connection closed"); return false; }
    auto ra = decode_ack(r); emit("decision -> " + (ra ? (ra->ok ? "ACCEPT:" + ra->message : ra->message) : "bad ack"));
  } else if (cmd.rfind("stale_observe ", 0) == 0) {
    unsigned long long epoch, boot; long mw;
    if (std::sscanf(cmd.c_str(), "stale_observe %llu %llu %ld", &epoch, &boot, &mw) != 3) { emit("stale_observe: bad args"); return true; }
    PowerObsMsg m; m.worker = w.worker; m.boot = WorkerBootId::from_raw(boot); m.epoch = CoordinatorEpoch::from_raw(epoch);
    m.accel = w.accel; m.device_gen = DeviceGeneration::first(); m.obs_gen = w.obs_gen; w.obs_gen = w.obs_gen.next();
    m.timestamp_ns = 1000000; m.has_power = true; m.power_mw = mw; m.has_temp = true; m.temp_centi = 3700;
    m.provenance = 1; m.source = "fake";
    if (!send_frame(w.sock, Frame{MsgType::POWER_OBSERVATION, encode_power_obs(m)})) { emit("stale_observe: closed"); return false; }
    Frame r; if (!recv_frame(w.sock, r)) { emit("stale_observe: connection closed"); return false; }
    auto ra = decode_ack(r); emit("stale_observe -> " + (ra ? (ra->ok ? "ACCEPT" : ra->message) : "bad ack"));
  } else if (cmd.rfind("stale_reserve ", 0) == 0) {
    unsigned long long epoch, boot, budget; long mw;
    if (std::sscanf(cmd.c_str(), "stale_reserve %llu %llu %llu %ld", &epoch, &boot, &budget, &mw) != 4) { emit("stale_reserve: bad args"); return true; }
    ReserveMsg m; m.budget = BudgetId::from_raw(budget); m.requested_mw = mw; m.requested_mj = 0;
    m.budget_gen = BudgetGeneration::first(); m.reservation_gen = ReservationGeneration::first();
    m.policy_gen = PolicyGeneration::first(); m.device_gen = DeviceGeneration::first();
    m.workload = WorkloadId::from_raw(w.worker.raw()); m.cls = 1; m.scope = PowerDomainId::from_raw(1);
    m.epoch = CoordinatorEpoch::from_raw(epoch); m.boot = WorkerBootId::from_raw(boot);
    m.timestamp_ns = 1000000; m.validity_ns = 5000000000LL;
    if (!send_frame(w.sock, Frame{MsgType::RESERVE, encode_reserve(m)})) { emit("stale_reserve: closed"); return false; }
    Frame r; if (!recv_frame(w.sock, r)) { emit("stale_reserve: connection closed"); return false; }
    auto ra = decode_ack(r); emit("stale_reserve -> " + (ra ? (ra->ok ? "ACCEPT:" + ra->message : ra->message) : "bad ack"));
  } else if (cmd.rfind("stale_release ", 0) == 0) {
    unsigned long long epoch, boot, res, budget; long mw;
    if (std::sscanf(cmd.c_str(), "stale_release %llu %llu %llu %llu %ld", &epoch, &boot, &res, &budget, &mw) != 5) { emit("stale_release: bad args"); return true; }
    BudgetActionMsg m; m.reservation = ReservationId::from_raw(res); m.budget = BudgetId::from_raw(budget); m.amount_mw = mw;
    m.budget_gen = BudgetGeneration::first(); m.epoch = CoordinatorEpoch::from_raw(epoch); m.boot = WorkerBootId::from_raw(boot);
    if (!send_frame(w.sock, Frame{MsgType::RELEASE, encode_budget_action(m)})) { emit("stale_release: closed"); return false; }
    Frame r; if (!recv_frame(w.sock, r)) { emit("stale_release: connection closed"); return false; }
    auto ra = decode_ack(r); emit("stale_release -> " + (ra ? (ra->ok ? "ACCEPT" : ra->message) : "bad ack"));
  } else { emit("unknown cmd"); }
  return true;
}

bool worker_run(const WorkerCtx& ctx0, const std::vector<std::string>& cmds,
                 std::function<void(const std::string&)> emit) {
  WorkerCtx w = ctx0;
  if (!worker_register(w, emit)) return false;
  for (const auto& cmd : cmds) {
    if (!worker_dispatch(w, cmd, emit)) break;
  }
  return true;
}
// --- coordinator-side ------------------------------------------------
struct CoordShared {
  Governor* gov;
  std::mutex mu;
  CoordinatorEpoch epoch;
  std::map<WorkerId, WorkerBootId> workers;
  std::map<WorkerId, ObservationGeneration> obs_hwm;
  std::atomic<bool> shutdown{false};
};

inline bool fence_obs(const CoordShared& cs, CoordinatorEpoch epoch, WorkerBootId boot,
                    ObservationGeneration og, WorkerId w, std::string& why) {
  if (epoch != cs.epoch) { why = "STALE_EPOCH"; return false; }
  auto it = cs.workers.find(w);
  if (it == cs.workers.end() || it->second != boot) { why = "STALE_BOOT"; return false; }
  auto hi = cs.obs_hwm.find(w);
  if (hi != cs.obs_hwm.end() && og < hi->second) { why = "STALE_OBSERVATION"; return false; }
  return true;
}

inline bool fence_epoch_boot(const CoordShared& cs, CoordinatorEpoch epoch, WorkerBootId boot, std::string& why) {
  if (epoch != cs.epoch) { why = "STALE_EPOCH"; return false; }
  for (const auto& [w, b] : cs.workers) { (void)w; if (b == boot) return true; }
  why = "STALE_BOOT";
  return false;
}

inline void handle_worker(CoordShared& cs, SOCKET peer) {
  Frame f;
  if (!recv_frame(peer, f)) { close_socket(peer); return; }
  if (f.type != MsgType::REGISTER) { send_error(peer, 0, "expected REGISTER"); close_socket(peer); return; }
  auto reg = decode_register(f);
  if (!reg) { send_error(peer, 0, "malformed register"); close_socket(peer); return; }
  {
    std::lock_guard<std::mutex> lk(cs.mu);
    cs.workers[reg->worker] = reg->boot;
    cs.obs_hwm[reg->worker] = ObservationGeneration();
  }
  // reply with the current epoch so the worker can fence its own messages
  { std::lock_guard<std::mutex> lk(cs.mu); send_ack(peer, reg->worker.raw(), true, std::to_string(cs.epoch.raw())); }

  while (!cs.shutdown.load()) {
    Frame m;
    if (!recv_frame(peer, m)) break;
    if (m.type == MsgType::POWER_OBSERVATION) {
      auto po = decode_power_obs(m);
      if (!po) { send_error(peer, 0, "malformed observation"); continue; }
      std::string why; bool ok;
      { std::lock_guard<std::mutex> lk(cs.mu);
        ok = fence_obs(cs, po->epoch, po->boot, po->obs_gen, po->worker, why);
        if (ok) { cs.obs_hwm[po->worker] = po->obs_gen; }
      }
      if (!ok) { send_error(peer, po->obs_gen.raw(), why); continue; }
      BackendSample s;
      if (po->has_power) s.power = Watts(double(po->power_mw) / 1000.0);
      if (po->has_temp) s.temperature = Celsius(double(po->temp_centi) / 100.0);
      if (po->has_util) s.utilization = Utilization(double(po->utilization_bp) / 10000.0);
      s.throttled = po->throttled; s.provenance = EnergyProvenance::REPORTED; s.source = "worker";
      cs.gov->observe_backend(s, Timestamp(po->timestamp_ns));
      send_ack(peer, po->obs_gen.raw(), true, "accepted");
    } else if (m.type == MsgType::THERMAL_OBSERVATION) {
      auto th = decode_thermal_obs(m);
      if (!th) { send_error(peer, 0, "malformed thermal"); continue; }
      std::string why; bool ok;
      { std::lock_guard<std::mutex> lk(cs.mu); ok = fence_obs(cs, th->epoch, th->boot, th->obs_gen, th->worker, why); if (ok) cs.obs_hwm[th->worker] = th->obs_gen; }
      if (!ok) { send_error(peer, th->obs_gen.raw(), why); continue; }
      send_ack(peer, th->obs_gen.raw(), true, "accepted");
    } else if (m.type == MsgType::RESERVE) {
      auto rr = decode_reserve(m);
      if (!rr) { send_error(peer, 0, "malformed reserve"); continue; }
      std::string why; bool ok;
      { std::lock_guard<std::mutex> lk(cs.mu); ok = fence_epoch_boot(cs, rr->epoch, rr->boot, why); }
      if (!ok) { send_error(peer, 0, why); continue; }
      // create + reserve on the governor
      auto plan = cs.gov->plan_reservation(rr->workload, WorkloadPowerClass::THROUGHPUT, rr->scope,
          rr->budget, Watts(double(rr->requested_mw)/1000.0), Joules(double(rr->requested_mj)/1000.0),
          PolicyGeneration::first(), DeviceGeneration::first(), Timestamp(rr->timestamp_ns), Duration::nanoseconds(rr->validity_ns));
      if (!plan.ok) { send_error(peer, 0, "plan:" + plan.reason); continue; }
      auto res = cs.gov->reserve_reservation(plan.id, rr->budget_gen, Timestamp(rr->timestamp_ns));
      if (!res.ok) { send_error(peer, 0, "reserve:" + res.reason); continue; }
      send_ack(peer, rr->workload.raw(), true, std::to_string(plan.id.raw()));
    } else if (m.type == MsgType::ACTIVATE || m.type == MsgType::RELEASE) {
      auto ba = decode_budget_action(m);
      if (!ba) { send_error(peer, 0, "malformed action"); continue; }
      std::string why; bool ok;
      { std::lock_guard<std::mutex> lk(cs.mu); ok = fence_epoch_boot(cs, ba->epoch, ba->boot, why); }
      if (!ok) { send_error(peer, 0, why); continue; }
      auto act = (m.type == MsgType::ACTIVATE) ? cs.gov->activate_reservation(ba->reservation, ba->budget_gen, Timestamp()) : cs.gov->release_reservation(ba->reservation, ba->budget_gen, Timestamp());
      send_ack(peer, ba->reservation.raw(), act.ok, act.reason);
    } else if (m.type == MsgType::DECISION_REQUEST) {
      auto dr = decode_reserve(m);
      if (!dr) { send_error(peer, 0, "malformed decision"); continue; }
      std::string why; bool ok;
      { std::lock_guard<std::mutex> lk(cs.mu); ok = fence_epoch_boot(cs, dr->epoch, dr->boot, why); }
      if (!ok) { send_error(peer, 0, why); continue; }
      auto dec = cs.gov->decide_for(dr->budget, dr->workload, WorkloadPowerClass::THROUGHPUT, Watts(double(dr->requested_mw)/1000.0), Joules(0), 1.0, Timestamp());
      send_ack(peer, dr->workload.raw(), true, std::string(to_string(dec.verdict)) + " " + dec.digest);
    } else if (m.type == MsgType::SNAPSHOT_REQUEST) {
      std::lock_guard<std::mutex> lk(cs.mu); send_ack(peer, 0, true, cs.gov->state_digest());
    } else { send_error(peer, 0, "unknown message"); }
  }
  close_socket(peer);
}

inline int coordinator_loop(std::uint16_t port, void (*emit)(const std::string&)) {
  pg_net_init();
  TestClock clock; FakeBackend backend;
  GovernorConfig cfg; cfg.id = GovernorId::allocate(); cfg.node = NodeId::from_raw(1);
  cfg.boot = WorkerBootId::from_raw(1); cfg.epoch = CoordinatorEpoch::from_raw(1);
  Governor gov(clock, backend, cfg);
  CoordShared cs; cs.gov = &gov; cs.epoch = CoordinatorEpoch::from_raw(1);
  TcpListener listener;
  if (!listener.listen("127.0.0.1", port)) { if (emit) emit("LISTEN_FAIL"); return 1; }
  if (emit) emit("READY " + std::to_string(port));
  // background accept thread
  std::thread accepter([&]() {
    while (!cs.shutdown.load()) {
      SOCKET peer; if (!listener.accept(peer)) break;
      std::thread(handle_worker, std::ref(cs), peer).detach();
    }
  });
  // control command loop on stdin
  std::string line;
  while (read_line(nullptr, line)) {
    if (line.rfind("install_budget ", 0) == 0) {
      unsigned long long id, parent; double hmax, soft;
      if (std::sscanf(line.c_str(), "install_budget %llu %llu %lf %lf", &id, &parent, &hmax, &soft) != 4) { if (emit) emit("BAD install_budget"); continue; }
      PowerBudget b; b.id = BudgetId::from_raw(id); b.generation = BudgetGeneration::first();
      b.domain = PowerDomainId::from_raw(1); b.parent = parent ? std::optional<BudgetId>(BudgetId::from_raw(parent)) : std::nullopt;
      b.hard_max = Watts(hmax); b.soft_target = Watts(soft); b.name = "budget";
      std::lock_guard<std::mutex> lk(cs.mu); bool ok = gov.install_budget(b); if (emit) emit(ok ? "BUDGET installed" : "BUDGET rejected");
    } else if (line.rfind("set_policy ", 0) == 0) {
      double cap; if (std::sscanf(line.c_str(), "set_policy %lf", &cap) != 1) { if (emit) emit("BAD set_policy"); continue; }
      PowerPolicy p = PowerPolicy::defaults(); p.device_hard_cap = Watts(cap); p.generation = PolicyGeneration::first();
      std::lock_guard<std::mutex> lk(cs.mu); bool ok = gov.install_policy(p); if (emit) emit(ok ? "POLICY installed" : "POLICY rejected");
    } else if (line.rfind("roll_epoch", 0) == 0) {
      std::lock_guard<std::mutex> lk(cs.mu); cs.epoch = cs.epoch.next(); if (emit) emit("EPOCH " + std::to_string(cs.epoch.raw()));
    } else if (line.rfind("snapshot", 0) == 0) {
      std::lock_guard<std::mutex> lk(cs.mu); if (emit) emit("SNAPSHOT " + gov.state_digest());
    } else if (line.rfind("save ", 0) == 0) {
      std::string file = line.substr(5); bool ok = gov.save(file); if (emit) emit(ok ? "SAVED" : "SAVE_FAIL");
    } else if (line.rfind("quit", 0) == 0) { break; }
    else { if (emit) emit("unknown control"); }
  }
  cs.shutdown.store(true);
  listener.close(); if (accepter.joinable()) accepter.join();
  return 0;
}

inline void default_emit(const std::string& s) { std::printf("%s\n", s.c_str()); std::fflush(stdout); }

// CLI entry: "run-worker --coordinator HOST:PORT --boot N --node N --worker N"
int run_worker_main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  WorkerId wid = WorkerId::from_raw(1);
  WorkerBootId boot = WorkerBootId::from_raw(1);
  NodeId node = NodeId::from_raw(1);
  for (int i = 1; i < argc - 1; ++i) {
    std::string a = argv[i];
    if (a == "--coordinator") { std::string v = argv[++i]; auto c = v.find(':'); if (c != std::string::npos) { host = v.substr(0, c); port = static_cast<std::uint16_t>(std::stoi(v.substr(c + 1))); } }
    else if (a == "--boot") { boot = WorkerBootId::from_raw(std::stoull(argv[++i])); }
    else if (a == "--node") { node = NodeId::from_raw(std::stoull(argv[++i])); }
    else if (a == "--worker") { wid = WorkerId::from_raw(std::stoull(argv[++i])); }
  }
  if (port == 0) { default_emit("ERROR no port"); return 1; }
  SOCKET s = connect_socket(host, port);
  if (s == INVALID_SOCKET) { default_emit("ERROR connect"); return 1; }
  WorkerCtx ctx; ctx.worker = wid; ctx.boot = boot; ctx.node = node; ctx.sock = s;
  if (!pgd::worker_register(ctx, default_emit)) { close_socket(s); return 1; }
  std::string line;
  while (pgd::read_line(nullptr, line)) {
    if (!line.empty() && !pgd::worker_dispatch(ctx, line, default_emit)) break;
  }
  close_socket(s);
  return 0;
}

// CLI entry: "run-coordinator --port N"
int run_coordinator_main(int argc, char** argv) {
  std::uint16_t port = 0;
  for (int i = 1; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--port") { port = static_cast<std::uint16_t>(std::stoi(argv[++i])); }
  }
  if (port == 0) { default_emit("ERROR no port"); return 1; }
  return coordinator_loop(port, default_emit);
}

}  // namespace pgd