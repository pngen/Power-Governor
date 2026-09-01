#include "powergovernor/energy.hpp"
#include <cstdio>
using namespace pg;
int main() {
  std::printf("=== Energy accounting with provenance ===\n");
  EnergyMeter meter;
  meter.add(Joules(5.0), EnergyProvenance::MEASURED, Timestamp::from_ns(1));
  meter.add(Joules(2.0), EnergyProvenance::REPORTED, Timestamp::from_ns(2));
  meter.add(Joules(1.0), EnergyProvenance::ESTIMATED, Timestamp::from_ns(3));
  std::printf("total=%.2f J  measured=%.2f  reported=%.2f  estimated=%.2f\n",
              meter.total().value(), meter.by(EnergyProvenance::MEASURED).value(),
              meter.by(EnergyProvenance::REPORTED).value(), meter.by(EnergyProvenance::ESTIMATED).value());
  std::printf("estimated energy is never presented as measured (provenance kept separate)\n");
  PowerWindow w(Duration::seconds(1));
  w.record(Timestamp::from_ns(0), Watts(100)); w.record(Timestamp::from_ns(500000000LL), Watts(300));
  std::printf("peak=%.1f W average=%.1f W integrated=%.2f J\n", w.peak().value(), w.average_power(), w.integrated_energy_us().value());
  return 0;
}
