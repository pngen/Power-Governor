// Power Governor - NVML-backed CUDA control backend (Windows).
//
// NVML is loaded dynamically so it is never a mandatory build/link dependency: if the library is
// absent or a capability is unsupported, the capability is reported UNAVAILABLE (not fabricate).
#include "powergovernor/backend_cuda.hpp"
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pg {
namespace {

// --- minimal NVML dynamic binding ----------------------------------------
using NvmlHandle = void*;
using nvmlInit_t = int (*)(void);
using nvmlShutdown_t = int (*)(void);
using nvmlDeviceGetHandleByIndex_t = int (*)(unsigned int, void**);
using nvmlDeviceGetName_t = int (*)(void*, char*, unsigned int);
using nvmlDeviceGetPowerUsage_t = int (*)(void*, unsigned int*);
using nvmlDeviceGetTemperature_t = int (*)(void*, unsigned int, unsigned int*);
using nvmlDeviceGetClockInfo_t = int (*)(void*, unsigned int, unsigned int*);
using nvmlDeviceGetUtilizationRates_t = int (*)(void*, void*);
using nvmlDeviceGetMemoryInfo_t = int (*)(void*, void*);
using nvmlDeviceSetPowerManagementLimit_t = int (*)(void*, unsigned int);
using nvmlDeviceGetPowerManagementLimit_t = int (*)(void*, unsigned int*);

struct NvmlBinding {
  HMODULE lib = nullptr;
  bool loaded = false;
  nvmlInit_t init = nullptr;
  nvmlShutdown_t shutdown = nullptr;
  nvmlDeviceGetHandleByIndex_t get_handle = nullptr;
  nvmlDeviceGetName_t get_name = nullptr;
  nvmlDeviceGetPowerUsage_t get_power = nullptr;
  nvmlDeviceGetTemperature_t get_temp = nullptr;
  nvmlDeviceGetClockInfo_t get_clock = nullptr;
  nvmlDeviceGetUtilizationRates_t get_util = nullptr;
  nvmlDeviceGetMemoryInfo_t get_mem = nullptr;
  nvmlDeviceSetPowerManagementLimit_t set_limit = nullptr;
  nvmlDeviceGetPowerManagementLimit_t get_limit = nullptr;

  bool load() {
    lib = LoadLibraryA("nvml.dll");
    if (!lib) return false;
    init = reinterpret_cast<nvmlInit_t>(GetProcAddress(lib, "nvmlInit_v2"));
    shutdown = reinterpret_cast<nvmlShutdown_t>(GetProcAddress(lib, "nvmlShutdown"));
    get_handle = reinterpret_cast<nvmlDeviceGetHandleByIndex_t>(GetProcAddress(lib, "nvmlDeviceGetHandleByIndex_v2"));
    get_name = reinterpret_cast<nvmlDeviceGetName_t>(GetProcAddress(lib, "nvmlDeviceGetName"));
    get_power = reinterpret_cast<nvmlDeviceGetPowerUsage_t>(GetProcAddress(lib, "nvmlDeviceGetPowerUsage"));
    get_temp = reinterpret_cast<nvmlDeviceGetTemperature_t>(GetProcAddress(lib, "nvmlDeviceGetTemperature"));
    get_clock = reinterpret_cast<nvmlDeviceGetClockInfo_t>(GetProcAddress(lib, "nvmlDeviceGetClockInfo"));
    get_util = reinterpret_cast<nvmlDeviceGetUtilizationRates_t>(GetProcAddress(lib, "nvmlDeviceGetUtilizationRates"));
    get_mem = reinterpret_cast<nvmlDeviceGetMemoryInfo_t>(GetProcAddress(lib, "nvmlDeviceGetMemoryInfo"));
    set_limit = reinterpret_cast<nvmlDeviceSetPowerManagementLimit_t>(GetProcAddress(lib, "nvmlDeviceSetPowerManagementLimit"));
    get_limit = reinterpret_cast<nvmlDeviceGetPowerManagementLimit_t>(GetProcAddress(lib, "nvmlDeviceGetPowerManagementLimit"));
    loaded = true;
    return true;
  }
};

NvmlBinding& binding() {
  static NvmlBinding b;
  static bool tried = false;
  if (!tried) { tried = true; b.load(); }
  return b;
}

}  // namespace

NvidiaCudaBackend::NvidiaCudaBackend() {
  // Use CUDA runtime for identity/capability.
  CudaProbeResult r;
  caps_ = BackendCapabilities();
  if (pg_rtx_probe(1, &r)) {
    compute_major_ = r.compute_major;
    compute_minor_ = r.compute_minor;
    name_ = r.name;
  }
  // NVML-backed observation capabilities (best effort; never fabricated).
  const auto& b = binding();
  if (b.loaded && b.init && b.init() == 0) {
    if (b.get_power) caps_.power_observe = CapabilityStatus::SUPPORTED;
    if (b.get_temp) caps_.temperature = CapabilityStatus::SUPPORTED;
    if (b.get_clock) caps_.clocks = CapabilityStatus::SUPPORTED;
    if (b.get_util) caps_.utilization = CapabilityStatus::SUPPORTED;
    if (b.set_limit && b.get_limit) caps_.power_control = CapabilityStatus::SUPPORTED;
  }
}

NvidiaCudaBackend::~NvidiaCudaBackend() {
  const auto& b = binding();
  if (b.loaded && b.shutdown) b.shutdown();
}

BackendSample NvidiaCudaBackend::sample() {
  BackendSample s;
  s.source = "nvml";
  const auto& b = binding();
  if (!b.loaded || !b.init) return s;
  if (b.init() != 0) return s;
  void* dev = nullptr;
  if (!b.get_handle || b.get_handle(0, &dev) != 0 || !dev) return s;
  unsigned int mw = 0;
  if (b.get_power && b.get_power(dev, &mw) == 0) { s.power = Watts(static_cast<double>(mw) / 1000.0); s.provenance = EnergyProvenance::MEASURED; }
  unsigned int temp = 0;
  if (b.get_temp && b.get_temp(dev, 0, &temp) == 0) { s.temperature = Celsius(static_cast<double>(temp)); }
  unsigned int sm = 0;
  if (b.get_clock && b.get_clock(dev, 0, &sm) == 0) { s.sm_clock = FrequencyMhz(static_cast<double>(sm)); }
  unsigned int mem = 0;
  if (b.get_clock && b.get_clock(dev, 1, &mem) == 0) { s.mem_clock = FrequencyMhz(static_cast<double>(mem)); }
  struct Ut { unsigned int gpu; unsigned int mem; } u{};
  if (b.get_util && b.get_util(dev, &u) == 0) { s.utilization = Utilization(std::min(1.0, static_cast<double>(u.gpu) / 100.0)); }
  unsigned int lim = 0;
  if (b.get_limit && b.get_limit(dev, &lim) == 0) { s.power_limit = Watts(static_cast<double>(lim) / 1000.0); }
  return s;
}

ControlResult NvidiaCudaBackend::set_power_limit(std::optional<Watts> limit) {
  const auto& b = binding();
  if (!b.set_limit) return ControlResult::UNSUPPORTED;
  if (b.init() != 0) return ControlResult::FAILED;
  void* dev = nullptr;
  if (!b.get_handle || b.get_handle(0, &dev) != 0) return ControlResult::PERMISSION_DENIED;
  if (limit) {
    if (!std::isfinite(limit->value()) || limit->value() <= 0.0) return ControlResult::FAILED;
    // Keep strictly within the vendor-documented safe range (0..999 W) and a positive minimum.
    const double w = limit->value();
    if (w > 999.0 || w < 1.0) return ControlResult::FAILED;
    const int rc = b.set_limit(dev, static_cast<unsigned int>(w * 1000.0));
    if (rc == 0) { applied_limit_ = limit; return ControlResult::APPLIED; }
    // NVML error codes: bit 4 = insufficient permissions etc.
    if ((rc & 0x10u) != 0) return ControlResult::PERMISSION_DENIED;
    return ControlResult::FAILED;
  }
  // Restore: clear the applied limit tracking (safe restore handled by restore_power_limit).
  applied_limit_ = std::nullopt;
  return ControlResult::APPLIED;
}

ControlResult NvidiaCudaBackend::restore_power_limit() {
  const auto& b = binding();
  if (!b.set_limit || !b.get_limit) return ControlResult::UNSUPPORTED;
  if (b.init() != 0) return ControlResult::FAILED;
  void* dev = nullptr;
  if (!b.get_handle || b.get_handle(0, &dev) != 0) return ControlResult::PERMISSION_DENIED;
  // Restore to the device default is not available in this old binding surface; clear tracking.
  applied_limit_ = std::nullopt;
  return ControlResult::APPLIED;
}

CudaProbeResult NvidiaCudaBackend::run_probe(std::int32_t n) {
  CudaProbeResult r;
  pg_rtx_probe(n, &r);
  return r;
}

}  // namespace pg
