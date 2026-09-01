// Power Governor - NVIDIA/CUDA (sm_120) control backend.
//
// This backend reports real hardware telemetry where the local APIs permit it (CUDA runtime for
// device identity/capability, NVML for power/temperature/clocks/utilization/energy) and confirms a
// vendor-supported power-limit change only when NVML reports it succeeded. It never fabricates a
// hardware fact or claims a control was applied unless the backend confirms it.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include "backend.hpp"

namespace pg {

// Result of a bounded real CUDA workload probe (defined in pg_cuda_backend.cu).
struct CudaProbeResult {
  bool ok = false;
  int device_count = 0;
  int compute_major = 0;
  int compute_minor = 0;
  unsigned long long free_bytes = 0;
  unsigned long long total_bytes = 0;
  double elapsed_seconds = 0.0;
  bool verified = false;
  unsigned long long uuid_low = 0;
  unsigned long long uuid_high = 0;
  char name[256] = {0};
};

extern "C" bool pg_rtx_probe(std::int32_t n_elems, CudaProbeResult* out);

// A ControlBackend backed by CUDA + NVML on the first (well, device 0) NVIDIA accelerator.
class NvidiaCudaBackend final : public ControlBackend {
 public:
  NvidiaCudaBackend();
  ~NvidiaCudaBackend() override;

  BackendCapabilities capabilities() const override { return caps_; }
  std::string name() const override { return "cuda/nvml"; }
  std::string device_name() const { return name_; }
  bool is_rtx5090() const { return (compute_major_ == 12 && compute_minor_ == 0); }
  int compute_major() const { return compute_major_; }
  int compute_minor() const { return compute_minor_; }

  BackendSample sample() override;
  ControlResult set_power_limit(std::optional<Watts> limit) override;
  ControlResult restore_power_limit() override;
  std::optional<Watts> applied_limit() const noexcept override { return applied_limit_; }

  // Run a bounded real CUDA workload (saxpy) and verify against the CPU reference.
  CudaProbeResult run_probe(std::int32_t n_elems);

 private:
  BackendCapabilities caps_;
  std::string name_;
  int compute_major_ = 0;
  int compute_minor_ = 0;
  std::uint64_t device_pointer_ = 0;
  std::optional<Watts> applied_limit_;
};

}  // namespace pg
