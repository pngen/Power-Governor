# Power Governor

**Power Governor** is a production C++20 systems runtime that answers one question about a
compute accelerator:

> **How much power and energy may this accelerator consume now, what performance state is
> justified, which thermal and policy constraints bind execution, what work should be admitted
> or throttled, and how can power behavior remain stable, explainable, and authoritative under
> changing load?**

It is a governance and decision runtime, not a monitoring dashboard, a power-meter wrapper, an
NVML demo, a fan-control utility, an overclocking tool, a synthetic benchmark harness, or a
simplistic watts-cap setter.

Maximum performance is not always the correct execution state. Power Governor makes explicit,
bounded decisions about the tradeoff between power, energy, performance, latency, thermal
headroom, capacity, policy, fairness, efficiency, and hardware health. A device is never
presumed to have unlimited power headroom merely because compute capacity is available.

## Systems boundary

Power Governor owns runtime policy for accelerator **power budgets**, **energy accounting**, **thermal
headroom**, **performance-state selection**, **throttling**, **power-cap recommendations**, **workload
power classes**, **admission constraints derived from power**, **fleet/node/device power envelopes**,
**power-budget allocation**, **transient oversubscription policy**, **energy-efficiency objectives**,
**thermal recovery**, **power-policy generations**, **authority and fencing**, and **safe recovery**
after restart. It consumes telemetry from CUDA, NVML where available, device/runtime APIs, the GPU
Fleet Agent, Accelerator Health, schedulers, workload metadata, and external policy, and exposes
decisions to schedulers, admission systems, Resource Broker, Latency Governor, Replica Fabric, Model
Residency, GPU Fleet Agent, and accelerator health/lifecycle systems.

It deliberately does **not** become a workload scheduler, a generic telemetry collector, a
thermal-control firmware replacement, a fan controller, an overclocking/undervolting system, or a
datacenter power-distribution controller.

## Strong identity and generation model

Identities are strongly typed rather than raw integers or loosely typed strings, including
**GovernorId**, **NodeId**, **AcceleratorId**, **DeviceUuid**, **WorkerId**, **WorkerBootId**,
**WorkloadId**, **RequestClassId**, **BudgetId**, **PolicyId**, **ObservationId**, **DecisionId**,
**PowerDomainId**, **EnergyWindowId**, **ThermalDomainId**, and **ReservationId**. Generations roll
independently per authority domain (**CoordinatorEpoch**, **GovernorGeneration**, **PolicyGeneration**,
**DeviceGeneration**, **ObservationGeneration**, **BudgetGeneration**, **DecisionGeneration**,
**ReservationGeneration**, **WorkloadGeneration**, **ThermalGeneration**); unrelated generations are
never collapsed into one generic counter.

## Typed power / energy / thermal model

Core quantities use strongly typed units (**Watts**, **Milliwatts**, **Joules**, **Millijoules**,
**Celsius**, **Milliseconds**, **Seconds**, **Utilization**, **FrequencyMhz**, **MemoryFrequencyMhz**,
**EnergyPerToken**, **EnergyPerRequest**, **PowerEfficiency**). Construction and mutation validate
ranges and reject negative physical power, invalid temperature values, NaN, Inf, impossible
utilization, integer overflow, and duration overflow.

## Power domains and budget hierarchy

Power domains form a hierarchy (fleet, rack/group, node, accelerator, workload class, workload
reservation), but the abstraction works when only node/device telemetry exists. Budgets support a
hard maximum, a soft target, a minimum reserve, a burst allowance with a window and recovery, a
policy generation, a scope/domain, a priority, and an owner/authority. Child allocations can only be
reserved if every parent on the chain has capacity, so a child can never silently exceed an
authoritative parent.

## Workload power classes

Typed classes (**LATENCY_CRITICAL**, **THROUGHPUT**, **BATCH**, **BACKGROUND**, **BEST_EFFORT**,
**ENERGY_OPTIMIZED**, **THERMALLY_SENSITIVE**, **CUSTOM**) carry a power priority, an energy target, a
maximum power share, burst eligibility, latency sensitivity, throttling tolerance, and a minimum
service level. These influence governance without turning Power Governor into the scheduler.

## Reservations, energy accounting, and burst

Reservations are transactional (plan, validate, reserve, admit, activate, account, release). The
runtime prevents double reservation, leaked reservation, stale reservation activation, negative
capacity, parent-budget overflow, release twice, and generation rollback.

Energy accounting tracks per-device, per-workload, per-class, reservation, burst, idle, shared, and
unknown/unattributed energy with explicit provenance (**MEASURED**, **REPORTED**, **DERIVED**,
**ESTIMATED**, **UNKNOWN**); estimated energy is never presented as measured, and instantaneous power,
integrated energy, average power, and peak power remain distinct. Burst accounting is a bounded,
refillable token bucket that can never bypass a hard hardware/policy limit; oversubscription is
explicit and bounded by policy.

## Thermal governance and performance-state policy

Thermal state (**UNKNOWN**, **COOL**, **NORMAL**, **WARM**, **HOT**, **THERMALLY_CONSTRAINED**,
**RECOVERING**) is tracked separately from power, with hysteresis that requires consecutive hot
samples before declaring constrained and a recovery margin before leaving it. Performance states
(**ECO**, **EFFICIENT**, **BALANCED**, **PERFORMANCE**, **MAX_PERFORMANCE**, **THROTTLED**,
**RECOVERY**) separate requested, effective, observed, and inferred throttling; the runtime may
recommend a state through a backend and reports accurately when control is unavailable.

## Decision engine and explainability

Decisions are deterministic and never a black-box model. Verdicts include **ALLOW**,
**ALLOW_WITH_LIMIT**, **THROTTLE**, **DEFER**, **DRAIN_RECOMMENDED**, **REVALIDATION_REQUIRED**,
**REJECT**, and **UNKNOWN**. Every decision exposes exact reasons, the binding constraint (device cap,
budget, or thermal), the evidence provenance, the telemetry freshness, what would have to change for
a different outcome, a deterministic JSON document, and a stable digest. Bounded hysteresis with a
minimum stable duration and threshold bands prevents power-state oscillation.

## Authority, fencing, freshness, and distributed proof

Distributed mutations and observations are fenced with the coordinator epoch, the worker boot id, and
per-domain generation high-water marks. A worker restarted with a fresh **WorkerBootId** fences every
report and reservation from the prior incarnation. Stale epoch, boot, or generation messages are
rejected deterministically. Every observation carries an id, timestamp, generation, source identity,
boot id, and device generation, with a freshness state (**CURRENT**, **AGING**, **STALE**,
**EXPIRED**); stale power/thermal observations do not drive authoritative decisions.

The distributed proof runs a **real coordinator** and **two worker OS processes** over framed TCP. It
registers both workers, publishes observations, installs authoritative budgets, reserves power on
both, captures authority envelopes, kills one worker as a real OS process, rolls the coordinator
epoch, restarts the worker with a fresh boot, and proves stale observations, reservations, releases,
and activations are rejected while the surviving worker remains valid and unaffected.

## Persistence and recovery

State persists as a versioned deterministic binary record with an explicit schema version, fixed
little-endian byte order, bounded lengths/counts, and CRC-32. Corruption, truncation, trailing
garbage, duplicate ids, invalid enums/generations, impossible budget hierarchy, and NaN/Inf are all
rejected. Writes are atomic (temporary file then an atomic replace). Recovered live telemetry is
**not** treated as current: a fresh observation is required before authoritative decisions.

## RTX 5090 hardware-backed validation

On the real NVIDIA GeForce RTX 5090 (compute capability 12.0 / sm_120), the **rtx5090** example
identifies the device, confirms sm_120, captures real NVML power/temperature/clock/utilization
telemetry, allocates bounded device memory, runs a real bounded CUDA workload, synchronizes, and
verifies the result against a CPU reference. The observations are fed through Power Governor; two
workload power classes are exercised, a bounded reservation is made, a synthetic constrained budget
below the observed unconstrained requirement is shown to return **ALLOW_WITH_LIMIT**/**THROTTLE**,
unconstrained policy is restored, and resources are released with memory returning to a justified
bounded delta. Real hardware telemetry and real CUDA execution are reported separately from
synthetic budget constraints and synthetic control paths.

## CLI and examples

The **powergovernor** CLI supports **devices**, **power**, **thermal**, **policy**, **budgets**,
**reserve**, **release**, **decision**, **explain**, **energy**, **efficiency**, **burst**, **history**,
**snapshot**, **save**, **recover**, **run-worker**, **run-coordinator**, and **benchmark**, with JSON
output where useful. The **examples/** directory provides runnable programs: basic observation,
device power budget, hierarchical budget, workload reservation, workload power classes, energy
accounting, burst allowance, thermal constraint, policy generation rollover, stale telemetry,
throttling decision, recovery from a constrained state, persistence/recovery, and the RTX 5090 CUDA
validation.

## Build, install, and use

Requirements: CMake 3.25+, a C++20 compiler (MSVC on Windows, with /W4 /WX for a zero-warning build),
and optionally an NVIDIA CUDA Toolkit (12.8+) with an sm_120-capable accelerator.

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build --output-on-failure

To install and consume as a package:

    cmake --install build --prefix <prefix>
    find_package(PowerGovernor CONFIG REQUIRED)
    target_link_libraries(myapp PRIVATE PowerGovernor::powergovernor)

The named target exposes the header-only core. When CUDA is enabled, the optional
**PowerGovernor::cuda_backend** target provides the real NVIDIA/NVML backend.

## Benchmark summary

A Release build benchmark reports throughput for observation ingestion, decision evaluation, budget
allocation, reservation create/release, energy accounting, state serialization, protocol
encode/decode, and concurrent reservations. Representative throughputs on the reference hardware
(see **benchmarks/bench_main.cpp** for exact workloads and units):

| operation | typical throughput |
| --- | --- |
| observation ingestion | ~190k ops/s |
| power-decision evaluation | ~32k ops/s |
| budget allocation query | ~12M ops/s |
| reservation create/release | ~76k ops/s |
| energy accounting | ~22M ops/s |

## Materially relevant limitations

- Power Governor consumes telemetry and applies control only through vendor-supported APIs. It does
  not perform voltage manipulation, overvolting, undervolting, BIOS or firmware modification, unsafe
  fan control, unsafe thermal experiments, or disabling hardware safety mechanisms.
- Hardware power-limit mutation may not be supported, or may require privileges that are not
  available; in that case the control plane is validated with a synthetic backend while physical
  telemetry remains real. Power Governor never claims a hardware cap was applied unless the backend
  confirms it.
- The abstraction works when only node/device telemetry is available; it does not assume physical
  rack-level power controls exist in every deployment.
- Energy attribution is exact only where input telemetry permits; unknown/unattributed energy is
  preserved explicitly rather than fabricated.
- The reference RTX 5090 validation is single-accelerator; multi-GPU validation is not claimed.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
