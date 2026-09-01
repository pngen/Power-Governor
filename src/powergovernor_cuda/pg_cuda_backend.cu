// Power Governor - bounded real CUDA workload probe (sm_120 / RTX 5090).
//
// This is a genuine CUDA execution on the available accelerator, not a simulation. It allocates a
// bounded buffer, copies deterministic input host->device, runs a saxpy kernel for a bounded amount
// of useful work, synchronizes, copies the result device->host, and verifies against a CPU
// reference. No power/thermal numbers are fabricated here: power/thermal telemetry is captured by
// the NVML-backed wrapper (nvidia_cuda_backend.cpp) around this probe.
#include <cuda_runtime.h>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <vector>

#include "powergovernor/backend_cuda.hpp"

namespace {

__global__ void pg_saxpy(float* y, const float* x, float a, std::int32_t n) {
  const std::int32_t i = static_cast<std::int32_t>(blockIdx.x) * static_cast<std::int32_t>(blockDim.x)
                       + static_cast<std::int32_t>(threadIdx.x);
  if (i < n) y[i] = a * x[i] + y[i];
}

}  // namespace

extern "C" bool pg_rtx_probe(std::int32_t n_elems, pg::CudaProbeResult* out) {
  if (!out || n_elems <= 0) return false;
  *out = pg::CudaProbeResult();
  int devices = 0;
  if (cudaGetDeviceCount(&devices) != cudaSuccess || devices <= 0) return false;
  out->device_count = devices;

  cudaDeviceProp prop;
  if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess) return false;
  out->compute_major = prop.major;
  out->compute_minor = prop.minor;
  std::snprintf(out->name, sizeof(out->name), "%s", prop.name);

  std::size_t free_mem = 0, total_mem = 0;
  cudaMemGetInfo(&free_mem, &total_mem);
  out->free_bytes = static_cast<unsigned long long>(free_mem);
  out->total_bytes = static_cast<unsigned long long>(total_mem);

  const cudaUUID_t uuid = prop.uuid;
  out->uuid_low = 0;
  out->uuid_high = 0;
  for (int i = 0; i < 8; ++i) out->uuid_low = (out->uuid_low << 8) | uuid.bytes[i];
  for (int i = 8; i < 16; ++i) out->uuid_high = (out->uuid_high << 8) | uuid.bytes[i];

  const float a = 1.5f;
  const std::int32_t n = n_elems;
  float* x_d = nullptr;
  float* y_d = nullptr;
  if (cudaMalloc(&x_d, static_cast<std::size_t>(n) * sizeof(float)) != cudaSuccess) return false;
  if (cudaMalloc(&y_d, static_cast<std::size_t>(n) * sizeof(float)) != cudaSuccess) { cudaFree(x_d); return false; }
  std::vector<float> x(n), y(n), yref(n);
  for (std::int32_t i = 0; i < n; ++i) { x[i] = 0.0001f * static_cast<float>(i % 97); y[i] = 0.25f; }
  for (std::int32_t i = 0; i < n; ++i) yref[i] = a * x[i] + y[i];

  cudaEvent_t t0, t1;
  cudaEventCreate(&t0);
  cudaEventCreate(&t1);

  cudaMemcpy(x_d, x.data(), static_cast<std::size_t>(n) * sizeof(float), cudaMemcpyHostToDevice);
  cudaMemcpy(y_d, y.data(), static_cast<std::size_t>(n) * sizeof(float), cudaMemcpyHostToDevice);

  const int threads = 256;
  const int blocks = (n + threads - 1) / threads;
  cudaEventRecord(t0);
  pg_saxpy<<<blocks, threads>>>(y_d, x_d, a, n);
  cudaEventRecord(t1);
  cudaDeviceSynchronize();

  float elapsed_ms = 0.0f;
  cudaEventElapsedTime(&elapsed_ms, t0, t1);
  out->elapsed_seconds = static_cast<double>(elapsed_ms) / 1000.0;

  std::vector<float> yh(n);
  cudaMemcpy(yh.data(), y_d, static_cast<std::size_t>(n) * sizeof(float), cudaMemcpyDeviceToHost);

  bool verified = true;
  for (std::int32_t i = 0; i < n; ++i) { if (std::fabs(yh[i] - yref[i]) > 1e-4f) { verified = false; break; } }
  out->verified = verified;

  cudaEventDestroy(t0);
  cudaEventDestroy(t1);
  cudaFree(x_d);
  cudaFree(y_d);
  out->ok = true;
  return true;
}
