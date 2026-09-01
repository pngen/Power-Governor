// Power Governor - benchmark of completed useful operations.
#include "powergovernor/governor.hpp"
#include "powergovernor/protocol.hpp"
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
using namespace pg;
using SteadyClock = std::chrono::steady_clock;
namespace {
double ms(SteadyClock::time_point a, SteadyClock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }
template <class F> double bench(std::size_t n, F f) { auto a = SteadyClock::now(); for (std::size_t i=0;i<n;++i) f(); auto b = SteadyClock::now(); return ms(a,b); }
}
int main() {
  TestClock clk(Timestamp::from_ns(1000000000LL)); FakeBackend fb;
  GovernorConfig cfg; cfg.id = GovernorId::allocate(); cfg.node = NodeId::from_raw(1);
  cfg.boot = WorkerBootId::from_raw(1); cfg.epoch = CoordinatorEpoch::first();
  Governor gov(clk, fb, cfg);
  PowerPolicy pol = PowerPolicy::defaults();
  pol.device_hard_cap = Watts(402.0); pol.fleet_hard_cap = Watts(5000.0);
  pol.generation = PolicyGeneration::first(); gov.install_policy(pol);
  PowerBudget node; node.id = BudgetId::from_raw(1); node.generation = BudgetGeneration::first();
  node.domain = PowerDomainId::from_raw(1); node.hard_max = Watts(450.0); node.name = "node"; gov.install_budget(node);
  PowerBudget acc; acc.id = BudgetId::from_raw(2); acc.generation = BudgetGeneration::first();
  acc.domain = PowerDomainId::from_raw(2); acc.parent = node.id; acc.hard_max = Watts(402.0); acc.name = "acc"; gov.install_budget(acc);
  fb.set_power(Watts(120.0)); fb.set_temperature(Celsius(45.0)); gov.poll(clk.now());
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  constexpr std::size_t N = 30000;
  std::printf("Power Governor benchmarks (Release).\n");
  { double t = bench(N, [&]() { gov.poll(clk.now()); });
    std::printf("observation ingestion     %8.0f ops/s   (%zu obs, %.1f ms)\n", N/(t/1000.0), N, t); }
  { double t = bench(N, [&]() { (void)gov.decide_for(BudgetId::from_raw(2), WorkloadId::from_raw(1), WorkloadPowerClass::THROUGHPUT, Watts(250), Joules(0), 1.0, clk.now()); });
    std::printf("power-decision evaluation %8.0f ops/s   (%zu decisions, %.1f ms)\n", N/(t/1000.0), N, t); }
  { double t = bench(N, [&]() { (void)gov.budget_available(BudgetId::from_raw(2)); });
    std::printf("budget allocation (query) %8.0f ops/s   (%zu queries, %.1f ms)\n", N/(t/1000.0), N, t); }
  { double t = bench(N, [&]() {
      auto p = gov.plan_reservation(WorkloadId::from_raw(9), WorkloadPowerClass::THROUGHPUT, PowerDomainId::from_raw(2),
          BudgetId::from_raw(2), Watts(20), Joules(0), PolicyGeneration::first(), DeviceGeneration::first(), clk.now(), Duration::seconds(2));
      if (p.ok) gov.release_reservation(p.id, BudgetGeneration::first(), clk.now()); });
    std::printf("reservation create/release %8.0f ops/s   (%zu pairs, %.1f ms)\n", N/(t/1000.0), N, t); }
  { double t = bench(N, [&]() { gov.account_energy(WorkloadId::from_raw(1), Joules(1.0), EnergyProvenance::DERIVED, clk.now()); });
    std::printf("energy accounting         %8.0f ops/s   (%zu adds, %.1f ms)\n", N/(t/1000.0), N, t); }
  { double t = bench(N, [&]() { (void)gov.state_digest(); });
    std::printf("state digest (serialize)  %8.0f ops/s   (%zu digests, %.1f ms)\n", N/(t/1000.0), N, t); }
  { PowerObsMsg m; m.worker = WorkerId::from_raw(1); m.boot = WorkerBootId::from_raw(2);
    m.epoch = CoordinatorEpoch::first(); m.accel = AcceleratorId::from_raw(3);
    m.device_gen = DeviceGeneration::from_raw(4); m.obs_gen = ObservationGeneration::from_raw(5);
    m.has_power = true; m.power_mw = 92000; m.has_temp = true; m.temp_centi = 3700;
    m.provenance = 1; m.source = "fake";
    Frame f{MsgType::POWER_OBSERVATION, encode_power_obs(m)};
    double t = bench(N, [&]() { auto bytes = serialize_frame(f); auto fr = deserialize_frame(bytes.data(), bytes.size()); (void)fr; });
    std::printf("protocol encode/decode     %8.0f ops/s   (%zu frames, %.1f ms)\n", N/(t/1000.0), N, t); }
  { constexpr std::size_t T = 4, M = 50000;
    auto a = SteadyClock::now(); std::vector<std::thread> th;
    for (std::size_t t=0;t<T;++t) th.emplace_back([&]() {
      for (std::size_t i=0;i<M/T;++i) {
        auto p = gov.plan_reservation(WorkloadId::from_raw(55), WorkloadPowerClass::THROUGHPUT, PowerDomainId::from_raw(2),
            BudgetId::from_raw(2), Watts(10), Joules(0), PolicyGeneration::first(), DeviceGeneration::first(), clk.now(), Duration::seconds(2));
        if (p.ok) gov.release_reservation(p.id, BudgetGeneration::first(), clk.now());
      } });
    for (auto& thd : th) thd.join();
    double t = ms(a, SteadyClock::now());
    std::printf("concurrent reservations    %8.0f ops/s   (%zu ops, 4 threads, %.1f ms)\n", M/(t/1000.0), M, t); }
  return 0;
}