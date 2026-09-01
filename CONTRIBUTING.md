# Contributing to Power Governor

Thank you for your interest in contributing to Power Governor. This project is
published by Summon Software Labs under the Apache License, Version 2.0.

## License and copyright

By contributing, you agree that your contributions will be licensed to the
project under the Apache License, Version 2.0, consistent with the LICENSE and
NOTICE files in this repository. There is no Contributor License Agreement
(CLA) and no copyright assignment: you retain copyright in your contribution
and grant the project a perpetual, worldwide, non-exclusive, royalty-free
license to use it under the terms of the Apache License, Version 2.0.

## Attribution

Please do not remove or alter the copyright notice or the NOTICE file. Any
distribution of derivative works must carry the attribution notices contained
in the NOTICE file, as required by Section 4 of the Apache License.

## Getting started

- Open a pull request against `main`. Keep changes focused and small.
- Build and test locally before pushing:
  - Release: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`, then `cmake --build build`.
  - Tests: `.uild\powergovernor_tests.exe` (or the equivalent `ctest` invocation).
  - CUDA (optional): build with `-DPG_ENABLE_CUDA=ON` (requires an sm_120 NVIDIA Toolkit).
- The suite must pass with zero compiler warnings (`/W4 /WX` on MSVC).

## Engineering conventions

- Use the strongly-typed unit and identity models (see `include/powergovernor`); do
  not add untyped doubles for power/energy quantities or raw ints for identities.
- New policy/decision logic must be deterministic and explainable.
- Add a regression test for every defect and every new behavior.
- Do not degrade the real-vs-synthetic evidence classification: never present
  estimated or synthetic telemetry as measured, and never claim a hardware
  control was applied unless the backend confirms it.

## Code of conduct

Be respectful and constructive. Reviews focus on the code and its behavior.
