// Power Governor CLI.
#include "powergovernor/governor.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

namespace pgd { int run_worker_main(int, char**); int run_coordinator_main(int, char**); }

using namespace pg;

static void usage() {
  std::printf("usage: powergovernor <command> [args]\n");
  std::printf("commands: devices power thermal policy budgets reserve release decision explain\n");
  std::printf("         energy efficiency burst history snapshot save recover benchmark\n");
  std::printf("         run-worker run-coordinator version team\n");
}

int main(int argc, char** argv) {
  if (argc < 2) { usage(); return 1; }
  const std::string cmd = argv[1];

  if (cmd == "run-coordinator") return pgd::run_coordinator_main(argc, argv);
  if (cmd == "run-worker") return pgd::run_worker_main(argc, argv);
  if (cmd == "version") { std::printf("Power Governor 1.0.0\n"); return 0; }
  if (cmd == "team") { std::printf("Summon Software Labs\n"); return 0; }

  // In-process demo commands use a synthetic fake backend by default.
  TestClock clock(Timestamp::from_ns(1000000000LL));
  FakeBackend backend;
  GovernorConfig cfg; cfg.id = GovernorId::allocate(); cfg.node = NodeId::from_raw(1);
  cfg.boot = WorkerBootId::from_raw(1); cfg.epoch = CoordinatorEpoch::first();
  Governor gov(clock, backend, cfg);
  PowerPolicy pol = PowerPolicy::defaults(); pol.device_hard_cap = Watts(402.0);
  pol.fleet_hard_cap = Watts(5000.0); pol.generation = PolicyGeneration::first();
  if (!gov.install_policy(pol)) { std::printf("error: policy install failed\n"); return 1; }
  PowerBudget node; node.id = BudgetId::from_raw(1); node.generation = BudgetGeneration::first();
  node.domain = PowerDomainId::from_raw(1); node.hard_max = Watts(450.0); node.name = "node-budget";
  gov.install_budget(node);
  PowerBudget acc; acc.id = BudgetId::from_raw(2); acc.generation = BudgetGeneration::first();
  acc.domain = PowerDomainId::from_raw(2); acc.parent = node.id; acc.hard_max = Watts(402.0); acc.name = "acc-budget";
  gov.install_budget(acc);

  backend.set_power(Watts(92.0)); backend.set_temperature(Celsius(37.0));
  backend.set_utilization(Utilization(0.13));
  if (cmd == "power" || cmd == "thermal" || cmd == "snapshot" || cmd == "decision" || cmd == "explain")
    gov.poll(clock.now());

  if (cmd == "devices") {
    auto caps = backend.capabilities();
    std::printf("device: NVIDIA GeForce RTX 5090 (compute_cap 12.0 / sm_120)\n");
    std::printf("power_observe=%s power_control=%s temperature=%s utilization=%s\n",
      std::string(to_string(caps.power_observe)).c_str(), std::string(to_string(caps.power_control)).c_str(),
      std::string(to_string(caps.temperature)).c_str(), std::string(to_string(caps.utilization)).c_str());
    return 0;
  } else if (cmd == "power") {
    auto s = backend.sample();
    if (s.power) std::printf("power: %.1f W\n", s.power->value());
    std::printf("total energy: %.3f J\n", gov.total_energy().value());
    return 0;
  } else if (cmd == "thermal") {
    std::printf("thermal: NORMAL (synthetic)\n");
    return 0;
  } else if (cmd == "policy") {
    const auto& p = gov.current_policy();
    std::printf("policy gen=%s device_cap=%s\n", p.generation.to_string().c_str(),
      p.device_hard_cap ? p.device_hard_cap->to_string().c_str() : "none");
    return 0;
  } else if (cmd == "budgets") {
    std::printf("budget 1 node-budget hard_max=450W\n");
    std::printf("budget 2 acc-budget hard_max=402W (parent=1)\n");
    return 0;
  } else if (cmd == "decision") {
    const Watts req(argc > 2 ? std::atof(argv[2]) : 250.0);
    Decision d = gov.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1), WorkloadPowerClass::THROUGHPUT, req, Joules(0), 1.0, clock.now());
    std::printf("decision: %s\n", std::string(to_string(d.verdict)).c_str());
    std::printf("json: %s\n", d.json.c_str());
    std::printf("digest: %s\n", d.digest.c_str());
    return 0;
  } else if (cmd == "explain") {
    Decision d = gov.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1), WorkloadPowerClass::THROUGHPUT, Watts(250.0), Joules(0), 1.0, clock.now());
    std::printf("why: %s\n", d.explanation.why.c_str());
    std::printf("what_would_change: %s\n", d.explanation.what_would_change.c_str());
    std::printf("binding_kind: %s\n", d.explanation.binding_kind.c_str());
    return 0;
  } else if (cmd == "energy") {
    std::printf("energy total: %.3f J\n", gov.total_energy().value());
    return 0;
  } else if (cmd == "efficiency") {
    std::printf("efficiency: Joules/request not available without a work counter\n");
    return 0;
  } else if (cmd == "burst") {
    std::printf("burst: disabled by default policy\n");
    return 0;
  } else if (cmd == "history") {
    for (const auto& e : gov.history().entries()) {
      std::printf("%zu %s %s\n", e.seq, std::string(to_string(e.kind)).c_str(), e.message.c_str());
    }
    return 0;
  } else if (cmd == "snapshot") {
    std::printf("state digest: %s\n", gov.state_digest().c_str());
    return 0;
  } else if (cmd == "save") {
    const char* file = argc > 2 ? argv[2] : "state.bin";
    std::printf("save %s -> %s\n", file, gov.save(file) ? "ok" : "failed");
    return 0;
  } else if (cmd == "recover") {
    const char* file = argc > 2 ? argv[2] : "state.bin";
    std::printf("recover %s -> %s\n", file, gov.load(file) ? "ok" : "failed");
    return 0;
  } else if (cmd == "benchmark") {
    std::printf("run: powergovernor_bench (see benchmarks/)\n");
    return 0;
  } else { usage(); return 1; }
}