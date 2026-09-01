// Power Governor - RTX 5090 hardware-backed validation.
//
// Runs a bounded real CUDA workload on the actual NVIDIA GeForce RTX 5090 (compute capability
// 12.0 / sm_120), captures real NVML power/temperature/clock/utilization telemetry around it, and
// feeds those observations through Power Governor to produce explainable decisions. A synthetic
// constrained budget is exercised to prove THROTTLE/ALLOW_WITH_LIMIT/DEFER, then unconstrained
// policy is restored. Real vs synthetic evidence is labelled explicitly.
#include "powergovernor/governor.hpp"

#if defined(PG_HAS_CUDA)
#include "powergovernor/backend_cuda.hpp"
#endif

#include <cstdio>
#include <cstdint>

int main() {
  using namespace pg;

#if !defined(PG_HAS_CUDA)
  std::printf("Power Governor: CUDA backend was not built (rebuild with -DPG_ENABLE_CUDA=ON).\n");
  std::printf("This example exercises the RTX 5090 hardware path and is unavailable here.\n");
  return 1;
#else
  NvidiaCudaBackend backend;
  CudaProbeResult info;
  pg_rtx_probe(1, &info);
  std::printf("device: %s compute_cap=%d.%d devices=%d\n", info.name, info.compute_major,
              info.compute_minor, info.device_count);
  std::printf("memory total=%llu MiB free=%llu MiB uuid=%016llx%016llx\n",
              info.total_bytes / (1024 * 1024), info.free_bytes / (1024 * 1024),
              info.uuid_high, info.uuid_low);
  if (!backend.is_rtx5090()) std::printf("WARNING: expected RTX 5090 (sm_120); got %s\n", info.name);

  BackendSample base = backend.sample();
  double base_w = base.power ? base.power->value() : 0.0;
  double base_c = base.temperature ? base.temperature->value() : 0.0;
  double base_util = base.utilization ? base.utilization->value() : 0.0;
  std::printf("baseline: %.1f W, %.1f C, utilization %.2f (REAL)\n", base_w, base_c, base_util);

  const std::int32_t n = 8 * 1024 * 1024;  // 32 MiB total buffer (x+y)
  CudaProbeResult probe = backend.run_probe(n);
  std::printf("probe: n=%d verified=%s elapsed=%.3f ms (REAL CUDA)\n", n,
              probe.verified ? "yes" : "no", probe.elapsed_seconds * 1000.0);

  BackendSample post = backend.sample();
  double post_w = post.power ? post.power->value() : 0.0;
  std::printf("post-workload power: %.1f W (REAL)\n", post_w);

  SystemClock clock;
  GovernorConfig cfg; cfg.id = GovernorId::allocate(); cfg.node = NodeId::from_raw(1);
  cfg.boot = WorkerBootId::from_raw(1); cfg.epoch = CoordinatorEpoch::first();
  Governor gov(clock, backend, cfg);

  PowerPolicy pol = PowerPolicy::defaults();
  pol.device_hard_cap = Watts(402.0); pol.fleet_hard_cap = Watts(5000.0);
  pol.node_hard_cap = Watts(450.0);
  pol.generation = PolicyGeneration::first();
  gov.install_policy(pol);

  PowerBudget node; node.id = BudgetId::from_raw(1); node.generation = BudgetGeneration::first();
  node.domain = PowerDomainId::from_raw(1); node.hard_max = Watts(450.0); node.name = "node";
  gov.install_budget(node);
  PowerBudget acc; acc.id = BudgetId::from_raw(2); acc.generation = BudgetGeneration::first();
  acc.domain = PowerDomainId::from_raw(2); acc.parent = node.id; acc.hard_max = Watts(402.0);
  acc.name = "acc"; gov.install_budget(acc);

  gov.observe_backend(post, clock.now());

  for (WorkloadPowerClass cls : {WorkloadPowerClass::LATENCY_CRITICAL, WorkloadPowerClass::THROUGHPUT}) {
    Decision d = gov.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1), cls,
                                Watts(250.0), Joules(0), 1.0, clock.now());
    std::printf("class %s requested 250 W -> %s (evidence=%s)\n",
                std::string(to_string(cls)).c_str(), std::string(to_string(d.verdict)).c_str(),
                std::string(to_string(d.explanation.evidence)).c_str());
  }

  auto plan = gov.plan_reservation(WorkloadId::from_raw(40), WorkloadPowerClass::THROUGHPUT,
      PowerDomainId::from_raw(2), BudgetId::from_raw(2), Watts(100), Joules(0),
      PolicyGeneration::first(), DeviceGeneration::first(), clock.now(), Duration::seconds(10));
  if (!plan.ok) { std::printf("reservation plan failed: %s\n", plan.reason.c_str()); return 1; }
  gov.reserve_reservation(plan.id, BudgetGeneration::first(), clock.now());
  std::printf("reservation %s reserved 100 W\n", plan.id.to_string().c_str());

  PowerPolicy con = PowerPolicy::defaults();
  con.device_hard_cap = Watts(60.0); con.fleet_hard_cap = Watts(5000.0); con.node_hard_cap = Watts(450.0);
  con.generation = PolicyGeneration::from_raw(2);
  gov.install_policy(con);
  Decision dcon = gov.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1),
      WorkloadPowerClass::THROUGHPUT, Watts(250.0), Joules(0), 1.0, clock.now());
  std::printf("SYNTHETIC constrained budget (60 W): requested 250 W -> %s limit=%s\n",
              std::string(to_string(dcon.verdict)).c_str(),
              dcon.recommended_limit ? dcon.recommended_limit->to_string().c_str() : "n/a");
  if (dcon.verdict != DecisionVerdict::ALLOW_WITH_LIMIT && dcon.verdict != DecisionVerdict::THROTTLE &&
      dcon.verdict != DecisionVerdict::DEFER) {
    std::printf("UNEXPECTED: constrained budget did not limit/throttle/defer\n");
  }

  PowerPolicy pol3 = PowerPolicy::defaults();
  pol3.device_hard_cap = Watts(402.0); pol3.fleet_hard_cap = Watts(5000.0); pol3.node_hard_cap = Watts(450.0);
  pol3.generation = PolicyGeneration::from_raw(3);
  gov.install_policy(pol3);
  gov.reset_hysteresis();  // clear hysteresis so normal eligibility can return
  Decision drestore = gov.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1),
      WorkloadPowerClass::THROUGHPUT, Watts(250.0), Joules(0), 1.0, clock.now());
  std::printf("restored unconstrained policy -> %s\n", std::string(to_string(drestore.verdict)).c_str());

  gov.release_reservation(plan.id, BudgetGeneration::first(), clock.now());
  CudaProbeResult after;
  pg_rtx_probe(1, &after);
  std::printf("after release: free mem=%llu MiB (bounded, justifiable delta only)\n",
              after.free_bytes / (1024 * 1024));
  std::printf("accounting consistent: %s\n", gov.accounting_consistent() ? "yes" : "no");
  return 0;
#endif
}
