#include "powergovernor/governor.hpp"
#include "powergovernor/units.hpp"
#include <cstdio>
int main() {
  pg::TestClock clock;
  pg::FakeBackend backend;
  pg::GovernorConfig cfg;
  cfg.node = pg::NodeId::from_raw(1);
  cfg.boot = pg::WorkerBootId::from_raw(1);
  cfg.epoch = pg::CoordinatorEpoch::first();
  pg::Governor gov(clock, backend, cfg);
  std::printf("downstream consumer OK: %s\n", pg::Watts(250.0).to_string().c_str());
  return 0;
}
