# Saccade

Saccade is a native keyboard-driven pointer system under active development. The
finished desktop application will combine captured pixels, accessibility data, and
compact vision models to find useful controls without sending screen contents over
the network.

This repository currently contains the portable foundation, not a finished desktop
application. Implemented pieces include:

- a C++20 runtime with a versioned C11 ABI;
- typed C11/C++20 host-frame import with bounded leases and newest-frame replacement;
- separate inference, capture, overlay, accessibility, and input provider contracts;
- bounded errors, generated handles, and fixed-capacity provider and device registries;
- deterministic providers for lifecycle and fault testing;
- a scalar CPU detector that serves as a small parity oracle;
- exact luma conversion with portable scalar, arm64 NEON, and x64 AVX2 paths;
- static and shared CMake packages with clean C and C++ consumer tests.

Native macOS and Windows adapters, the scene scheduler, the interaction engine, and
the shipping vision model are not implemented yet. The public
[version 0.1 contract](docs/product/version-0.1.md) records the intended desktop
behavior without claiming that it already ships.

## Build from source

You need CMake 3.25 or newer, Ninja, and a compiler with C11 and C++20 support.

```sh
cmake --preset dev
cmake --build --preset dev -j
ctest --preset dev --output-on-failure
```

The build does not download dependencies. Release, sanitizer, thread-sanitizer, and
profiling presets use the same command shape. See
[building Saccade](docs/development/building.md) for options and installation.

## Design targets

Input handling, pointer feedback, and overlay presentation have a 120 Hz target.
Neural scene refresh runs independently, with a 30 Hz full-scope target for version
0.1 and a 60 Hz accelerated target for version 0.2. Missing a neural deadline must
not stall input or presentation.

These are qualification targets, not benchmark claims for the current foundation.
[Memory and resolution](docs/architecture/memory.md) gives the formulas and concrete
4K and 8K costs. [Concurrency](docs/architecture/concurrency.md) explains how the two
rates remain independent.

## Repository layout

- `include/saccade/` contains the installed C11 headers.
- `src/` contains the portable runtime and private C++20 implementation.
- `backends/` contains the deterministic and scalar CPU providers.
- `abi/` records the public symbol and layout manifests.
- `tests/` contains ABI, provider, sanitizer, package, and source-quality checks.
- `cmake/` and `scripts/` contain maintained build checks.

Downloaded models, captures, datasets, notebooks, generated kernels, and profiler
output stay outside this repository.

## Platform scope

Version 0.1 targets macOS 14 or newer on Apple Silicon and Windows 11 24H2 or newer
on x64 and arm64. Both applications must satisfy the same behavior contract. Linux
fits the provider architecture but is not part of the first release claim.

Start with the [architecture overview](docs/architecture/overview.md), the
[provider contract](docs/architecture/backends.md), and the [ABI guide](docs/architecture/abi.md).

## License

See [LICENSE](LICENSE).
