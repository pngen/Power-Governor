#include "powergovernor/governor.hpp"
#include <cstdio>
int main() {
  pg::TestClock clock;
  pg::FakeBackend backend;
  pg::GovernorConfig cfg;
  cfg.boot = pg::WorkerBootId::from_raw(1);
  cfg.epoch = pg::CoordinatorEpoch::first();
  pg::Governor gov(clock, backend, cfg);
  std::printf("Power Governor basic example ok\n");
  return 0;
}
