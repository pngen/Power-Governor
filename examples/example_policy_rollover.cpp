#include "powergovernor/policy.hpp"
#include <cstdio>
using namespace pg;
int main() {
  std::printf("=== Policy generation rollover ===\n");
  PowerPolicy p = PowerPolicy::defaults();
  std::printf("gen=%llu\n", (unsigned long long)p.generation.raw());
  p.device_hard_cap = Watts(300); p.bump_generation();
  std::printf("after cap 300 & bump: gen=%llu\n", (unsigned long long)p.generation.raw());
  p.device_hard_cap = Watts(500); p.bump_generation();
  std::printf("after cap 500 & bump: gen=%llu\n", (unsigned long long)p.generation.raw());
  PowerPolicy old = p; old.generation = p.generation; // same
  std::printf("policy != generation differently: %s\n", (old == p) ? "same" : "different");
  return 0;
}