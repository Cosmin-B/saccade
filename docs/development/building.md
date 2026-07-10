# Building Saccade

Saccade uses CMake and Ninja directly. Configuration does not fetch packages or run
code generators from the network.

## Toolchains

The portable source requires:

- CMake 3.25 or newer;
- Ninja 1.11 or newer;
- Clang 16, GCC 13, or MSVC 2022 with current updates;
- a 64-bit target.

Public headers are compiled as strict C11 and C++20. Runtime sources use C++20.
Compiler extensions are disabled in both lanes.

## Presets

`dev` builds an unoptimized tree with tests and warnings as errors. `release` uses
the compiler's release optimization settings. `profile` keeps optimization and debug
symbols for platform profilers.

`asan-ubsan` enables AddressSanitizer and UndefinedBehaviorSanitizer. `tsan` enables
ThreadSanitizer by itself because it cannot share a process with the other sanitizer
runtimes.

```sh
cmake --list-presets
cmake --preset dev
cmake --build --preset dev -j
ctest --preset dev --output-on-failure
```

Build trees are written below `build/` and can be removed without affecting source.

## Options

The main cache options are:

| Option | Default | Purpose |
|---|---:|---|
| `SACCADE_BUILD_STATIC` | `ON` | Build static libraries. |
| `SACCADE_BUILD_SHARED` | `OFF` | Build shared libraries with an explicit export list. |
| `SACCADE_BUILD_TESTS` | `ON` | Build CTest targets. |
| `SACCADE_BUILD_BENCHMARKS` | `OFF` | Build maintained benchmarks. |
| `SACCADE_BUILD_TOOLS` | `ON` | Build maintained diagnostic tools. |
| `SACCADE_BACKEND_MOCK` | `ON` | Build deterministic contract-test providers. |
| `SACCADE_BACKEND_REFERENCE_CPU` | `ON` | Build the scalar CPU parity provider. |
| `SACCADE_ENABLE_TRACING` | `OFF` | Compile runtime tracing points. |
| `SACCADE_WARNINGS_AS_ERRORS` | `ON` | Treat project warnings as build failures. |

Override an option at configure time:

```sh
cmake --preset dev -DSACCADE_BUILD_SHARED=ON
```

The mock and scalar CPU providers are separate static targets. They are used by the
development suite and are not part of the installed public package.

## Install and consume

The default install contains the static core, public headers, and a relocatable CMake
package:

```sh
cmake -S . -B build/install -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/prefix" \
  -DSACCADE_BUILD_TESTS=OFF
cmake --build build/install --target install
```

A consumer can then use:

```cmake
find_package(Saccade 0.1 CONFIG REQUIRED)
target_link_libraries(my_program PRIVATE Saccade::core)
```

Set `SACCADE_BUILD_STATIC=OFF` and `SACCADE_BUILD_SHARED=ON` for a shared-only package.
That package also provides `Saccade::shared`. When both forms are built, the package
exports `Saccade::core` for static linkage and `Saccade::shared` for shared linkage.

CTest builds and runs strict C11 and C++20 consumers against static-only, shared-only,
and dual installs.

## Test lanes

ABI tests compile the public headers independently as C11 and C++20. Package tests
repeat that check from an installed prefix. Unit tests
exercise bounded storage, handle generations, provider registration, and deterministic
fault paths. Provider contracts cover every family, and the scalar CPU lane checks exact
image and serialized target output.

Run a narrow lane with a regular expression:

```sh
ctest --preset dev -R '^abi\.' --output-on-failure
```

Run sanitizer tests in their own build tree:

```sh
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan -j
ctest --preset asan-ubsan --output-on-failure
```

`quality.repository`, `quality.text`, and `quality.documentation` reject build artifacts,
downloaded model formats, dependency downloads, text damage, unfinished prose, and broken
local links. `abi.manifest.layout` and `abi.manifest.symbols` protect the versioned ABI.

## Source-tree rules

Keep generated and machine-specific material out of the repository. This includes
downloaded models, datasets, screenshots, recordings, notebooks, profiler captures,
compiler output, and temporary kernel variants.

A small synthetic test fixture may be committed when its origin and license are
recorded, its behavior is deterministic, and the test needs it. Product code must not
depend on a local capture, model cache, or network download.

Maintained documentation describes the contract that exists in code. Exploratory
notes and machine-local state stay out of the source tree.
