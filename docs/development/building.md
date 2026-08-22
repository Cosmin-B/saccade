# Building Saccade

Saccade uses CMake and Ninja directly. Configuration does not fetch packages or run
code generators from the network.

## Toolchains

The portable source requires:

- CMake 3.30 or newer.
- Ninja 1.11 or newer.
- Clang 16, GCC 13, or MSVC 2022 with current updates.
- A 64-bit target.

The native macOS build uses the Xcode 26 SDK so the same binary can compile the Metal 4
path while retaining a macOS 14 deployment target and Metal 3 runtime fallback. The
installed public package does not expose Apple SDK types.

Public headers are compiled as strict C11 and C++20. Runtime sources use C++20.
Compiler extensions are disabled in both lanes.

### Version 0.1 release toolchains

The macOS arm64 release configuration uses Xcode 26.6 (build 17F113) with the macOS 26
SDK and a macOS 14 deployment target. The Windows x64 and arm64 package lanes use
Visual Studio 2022 17.14.36, MSVC 19.44.35228 from toolset 14.44.35207, Windows SDK
10.0.26100.0, and Ninja 1.13.2. The release configuration selects that exact compiler
toolset. The arm64 package can be cross-built from x64.

Packaged Windows inference uses the current stable
`Microsoft.Windows.AI.MachineLearning` 2.1.74 package, which bundles ONNX Runtime
1.24.6 commit `800ac32`. CMake verifies its package version and architecture-specific
runtime hashes before compiling. Xcode, Visual Studio, the Windows SDK, CMake, Ninja,
and WiX are build tools. They do not enter the
application package. Runtime libraries, shaders, and model assets do enter the package
and are hash-checked as release inputs.

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
| `SACCADE_BUILD_APPLICATION` | static native builds | Build the menu-bar or notification-area application. |
| `SACCADE_DISTRIBUTION_BUILD` | `OFF` | Reject incomplete, ad-hoc, or unsigned desktop packages. |
| `SACCADE_BACKEND_METAL` | Apple hosts | Build the native Metal backend. |
| `SACCADE_BACKEND_D3D12` | Windows hosts | Build the native Direct3D 12 backend. |
| `SACCADE_ENABLE_TRACING` | `OFF` | Compile runtime tracing points. |
| `SACCADE_WARNINGS_AS_ERRORS` | `ON` | Treat project warnings as build failures. |
| `SACCADE_WINDOWS_UIACCESS` | `OFF` | Embed UIAccess for signed secure-location Windows releases. |
| `SACCADE_MSVC_STATIC_RUNTIME` | `OFF` | Statically link the MSVC runtime for a self-contained Windows package. |
| `SACCADE_MODEL_ARTIFACT` | empty | Signed platform model artifact bundled with the application. |
| `SACCADE_COREML_MODEL_BUNDLE` | empty | Compiled Core ML bundle copied into the macOS application. |
| `SACCADE_MACOS_CODESIGN_IDENTITY` | `-` | Stable Apple Development or Developer ID identity. |
| `SACCADE_MACOS_NOTARY_PROFILE` | empty | Keychain profile used by `notarytool`. |
| `SACCADE_MACOS_HARDENED_RUNTIME` | `ON` | Sign the completed macOS bundle with hardened runtime. |
| `SACCADE_WINDOWS_SIGNTOOL` | auto | Explicit `signtool.exe` path when it is not on `PATH`. |
| `SACCADE_WINDOWS_SIGNING_CERTIFICATE_SHA1` | empty | Authenticode certificate-store thumbprint. |
| `SACCADE_WINDOWS_TIMESTAMP_URL` | DigiCert | RFC 3161 timestamp service. |

Override an option at configure time:

```sh
cmake --preset dev -DSACCADE_BUILD_SHARED=ON
```

## Package a model

The application accepts a signed platform artifact named `saccade.model`. The packaging
tool writes the exact runtime contract and converts OpenSSL's DER ECDSA signature to the
fixed 64-byte P-256 representation consumed by both native verifiers.

```sh
openssl ecparam -name prime256v1 -genkey -noout -out model-signing-key.pem
key_xy=$(python3 tools/model/package_artifact.py public-key \
  --key model-signing-key.pem)

python3 tools/model/package_artifact.py directml \
  --key model-signing-key.pem --onnx detector.onnx \
  --output build/model/windows/saccade.model \
  --precision fp16 --width 1280 --height 768 \
  --input-name input --candidates-name candidates \
  --candidates 1024 --targets 160
```

Both graphs expose normalized six-value target rows. DirectML consumes FP16 rows.
Core ML exposes Float32 rows independently of the compiled graph's storage precision.
On Windows, an owned D3D12 dispatch converts those rows into the narrow Q3 candidate
workspace before sorting and suppression. The model graph does not manufacture
byte-packed native records. Core ML uses the same row meaning from a compiled bundle:

```sh
python3 tools/model/package_artifact.py coreml \
  --key model-signing-key.pem --locator SaccadeDetector.mlmodelc \
  --model-bundle /absolute/path/SaccadeDetector.mlmodelc \
  --output build/model/macos/saccade.model \
  --precision fp16 --input-name input \
  --rows-name target_rows --count-name target_count \
  --width 1280 --height 768 --candidates 1024 --targets 160
```

`--precision` records the Core ML bundle's storage precision in the signed artifact.
Use `fp32` only when the compiled bundle was deliberately exported at full precision.
The Core ML packager reads the compiled bundle metadata and rejects mismatched feature
names, dimensions, row shapes, or scalar-count shape before signing the artifact. When
the tools and application are enabled together, each desktop app target runs its native
runtime probe against the signed artifact before linking the application.

Configure release builds with `SACCADE_MODEL_PUBLIC_KEY_XY=$key_xy` and the matching
`SACCADE_MODEL_ARTIFACT`. The macOS build also receives
`SACCADE_COREML_MODEL_BUNDLE=/absolute/path/SaccadeDetector.mlmodelc`. Signing keys stay
outside the source and build trees.

The macOS target signs only after every model and shader resource has entered the
bundle. The default `-` identity creates a sealed ad-hoc development bundle, but its
designated requirement is tied to that exact executable. Screen Recording and
Accessibility grants therefore do not survive a rebuild. Development builds that need
grant continuity set `SACCADE_MACOS_CODESIGN_IDENTITY` to a stable Apple Development
identity. Distribution builds use a Developer ID Application identity and request a
trusted timestamp. Hardened runtime is enabled in every signed configuration unless
explicitly disabled.

Windows assistive-technology releases set `SACCADE_WINDOWS_UIACCESS=ON`, are
Authenticode-signed, and install below `Program Files`. Development builds leave it off
because Windows grants UIAccess only to trusted signed binaries in secure locations.
Windows release packages also set `SACCADE_MSVC_STATIC_RUNTIME=ON`, so the
app and local tools do not depend on a separately installed Visual C++ redistributable.

Set `SACCADE_DISTRIBUTION_BUILD=ON` only for a releasable package. Configuration then
requires the signed private model artifact and its public verification key. macOS also
requires a compiled Core ML bundle, non-ad-hoc Developer ID identity, hardened runtime,
and notary keychain profile. CPack signs and strictly verifies the generated DMG before
submitting it, then staples and validates the notarization. Windows also
requires the packaged ML runtime, UIAccess, a certificate thumbprint, and timestamp URL.
The application and generated MSI are both Authenticode-signed. Missing release inputs
fail configuration before an inert package can be produced.

## Desktop packages

Application builds generate a CPack configuration with one required `Application`
component. The component contains only the native application, accelerator runtimes,
shaders, and configured signed model assets. The separately installable C/C++ SDK is not
part of a desktop package.

Tests, benchmarks, the runtime probe, source documentation,
and compiler support files are excluded. Package size is dominated by the configured
model and the Microsoft ONNX Runtime and DirectML libraries.

On macOS, package the configured application as a disk image:

```sh
cmake --build build/release --target saccade_app
(cd build/release && cpack -G DragNDrop)
```

Windows release packaging uses WiX .NET CLI 4.0.6 and its matching UI
extension. Install that exact pair once, then generate the per-machine MSI:

```bat
dotnet tool install --global wix --version 4.0.6
wix extension add -g WixToolset.UI.wixext/4.0.6
cmake --build build\release --target saccade_app
cd build\release
cpack -G WIX
```

The MSI uses one stable upgrade identity across version 0.1 packages and installs the
x64 or ARM64 payload below `Program Files\Saccade`. Windows Installer provides repair,
same-version replacement, major upgrade, and removal. Per-machine installation and
UIAccess installation requires an elevated installer process. The MSI refuses Windows
builds older than 26100 (Windows 11 24H2). CPack writes a SHA-256 checksum file next to
both the MSI and DMG.

Uninstall removes the installed application, shortcuts, and packaged files. It does not
remove per-user settings: macOS keeps `~/Library/Application Support/Saccade/settings.bin`
and Windows keeps `%LOCALAPPDATA%\Saccade\settings.bin`. Reinstalling Saccade therefore
restores the previous settings. Delete that file or its containing `Saccade` directory
manually when a clean settings reset is wanted.

## Runtime probe

`SACCADE_BUILD_TOOLS=ON` builds `saccade_runtime_probe`. It performs a noninteractive
release smoke without capture, hotkeys, input, overlays, or UI: native P-256 verification,
read-only artifact mapping, provider initialization, model and execution-context creation,
memory reporting, and ordered teardown.

On macOS, pass the signed artifact and the directory containing its compiled Core ML
bundle:

```sh
build/release/tools/saccade_runtime_probe \
  build/model/macos/saccade.model \
  build/model/macos
```

On Windows, pass the signed artifact and the directory containing the packaged DXIL
shaders:

```bat
build\release\tools\saccade_runtime_probe.exe ^
  build\model\windows\saccade.model ^
  build\release\apps\windows
```

The probe writes one bounded JSON object containing provider, device, model, tensor,
capability, import, output-capacity, and memory fields. A build without the configured
public key exits before opening the artifact.

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
repeat that check from an installed prefix. Unit tests exercise bounded storage, handle
generations, provider registration, and deterministic fault paths. Provider contracts
cover every family, and the scalar CPU lane checks exact image and serialized target
output. The image-kernel lane compares scalar and SIMD output, exact-sized vector tails,
row padding, first-use dispatch, and steady-state allocation counts.

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

MSVC supports the address sanitizer only. Keep that lane test-only with the dynamic
runtime. CMake's MSVC manifest wrapper cannot embed the shipping GUI manifest while
linking the ASan executable. The ordinary Release lane still builds and packages the
application:

```bat
cmake -S . -B build\asan -G Ninja -DCMAKE_BUILD_TYPE=Debug ^
  -DSACCADE_SANITIZERS=address -DSACCADE_BUILD_APPLICATION=OFF ^
  -DSACCADE_MSVC_STATIC_RUNTIME=OFF
cmake --build build\asan
ctest --test-dir build\asan --output-on-failure -LE live
```

Apple Clang builds with an exported compile database also provide an opt-in static
analyzer lane. It analyzes every unique production translation unit and treats analyzer
findings as failures:

```sh
cmake --preset dev
cmake --build build/dev --target saccade_clang_analyzer
```

`quality.repository`, `quality.text`, and `quality.documentation` reject build artifacts,
downloaded model formats, dependency downloads, text damage, unfinished prose, and broken
local links. `build.target_architecture` checks host and target architecture selection.
`core.no_allocator_symbols` rejects process-allocator references in the built core.
`abi.manifest.layout` and `abi.manifest.symbols` protect the versioned ABI.

## Benchmarks

Maintained benchmarks use release optimization and fixed storage outside measured
loops. Build all of them with:

```sh
cmake -S . -B build/benchmark-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSACCADE_BUILD_BENCHMARKS=ON
cmake --build build/benchmark-release -j
```

| Area | Maintained measurements |
|---|---|
| Overlay and coordinates | Packet validation, target expansion, composition, transforms, and an offscreen draw. |
| Interaction | Hint assignment, prefix resolution, Path selection, action planning, and the 120 Hz scheduler budget. |
| Scene fusion | Semantic and neural candidate fusion, publication latency, and the 30 Hz scene budget. |
| Native inference | Core ML and DirectML creation, dispatch latency, tensor bytes, memory, and recovery. |
| Capture and preprocessing | Capture, GPU import, fused resize, color conversion, freshness, and transfer bytes. |
| Full scope | Capture through target publication while the independent interaction clock runs. |
| Longevity | Process, handle, accelerator, and Saccade-owned memory over a fixed cadence. |

Use each executable's `--help` output for narrow diagnostic controls. Compare results
only on the same machine, build, power mode, and thermal state. Synthetic, replay, and
live-capture modes measure different portions of the pipeline.

Windows inference benchmarks require `SACCADE_WINDOWS_ML_ROOT` to name an extracted
`Microsoft.Windows.AI.MachineLearning` 2.1.74 package. CMake rejects other versions and
verifies the package plus architecture-specific ONNX Runtime and DirectML hashes.
Executables load those libraries only from their own directory.

### Windows performance tools

The headless D3D12 memory tool uses a resident synthetic BGRA texture and exercises
preprocessing, DirectML inference, and GPU target postprocessing without creating a
window or reading the desktop. It samples process-private bytes, process handles, local
and nonlocal DXGI usage, and Saccade-owned buffers once per second after warm-up. The
short CTest lane runs automatically when `SACCADE_MODEL_ARTIFACT` names an admitted
DirectML artifact. The one-hour longevity mode is explicit:

```bat
build\benchmark-release\benchmarks\saccade_benchmark_windows_d3d12_memory_soak.exe ^
  detector.model build\benchmark-release\backends\d3d12 ^
  --duration=3600 --frames-per-second=30 --minimum-measured-millihz=29000 ^
  --private-budget-mb=64 --handle-budget=8 ^
  --video-budget-mb=64 --saccade-budget-mb=64 --monotonic-samples=8 ^
  --source-width=3840 --source-height=2160
```

The tool fails when any explicit growth budget is crossed, when a metric grows
continuously beyond its noise floor, or when measured throughput is below 29 Hz. The
longevity mode exercises the version 0.1 neural cadence of 30 full passes per second and
reports its actual pass count, elapsed nanoseconds, and measured millihertz at exit.
`--frames-per-second=0` is the separate uncapped stress mode. Sampling storage is fixed
for the full hour. Source dimensions are explicit in every result and are bounded by the
Q3 desktop-coordinate range, including 7,680 by 4,320 input.

The Windows full-scope tool has separate `live` and `replay` modes. `live` measures WGC
capture timestamps through scene publication when the compositor supplies a desktop frame.
`replay` leases one WGC desktop surface per display and submits fresh runtime handles from
those unchanged native textures. It performs no pixel copy and exercises newest-frame
replacement, preprocessing, DirectML, postprocessing, full-desktop coordination, scene
publication, and the concurrent interaction scheduler. Replay latency is processing
latency from submission, not capture freshness.

```bat
scripts\run_windows_full_scope.cmd detector.model 60 live
scripts\run_windows_full_scope.cmd detector.model 60 replay
```

Both modes report their name, scene refresh rate, p50/p95/p99 batch and full-scope
latency, deadline misses, replacements, interaction ticks, target count, and memory
high-water marks. Run both modes when characterizing a release build. Replay throughput
does not represent live capture latency. Replay exits successfully only when refresh is at
least 29 Hz, batch and full-scope p95 are within 33.33 ms, and the larger of the batch or
full-scope deadline-miss rates is at most 1,000 parts per million.

The same qualification result requires measured interaction cadence of at least 115 Hz.
The output reports `interaction_refresh_millihz`, `minimum_interaction_millihz`, and
`interaction_ticks` so the 120 Hz interaction clock is visible beside the 30 Hz scene
rate. Live runs remain subject to capture freshness and the full-scope latency gates.

The CPU model diagnostic compares MLAS, hybrid XNNPACK, and strict XNNPACK using the same
FP32 graph. Strict mode rejects any graph partition assigned to the default CPU provider.
It is a fallback diagnostic, not the Windows release path.

### macOS performance tools

The macOS full-scope tool uses live ScreenCaptureKit frames. ScreenCaptureKit requests
display-rate updates, atlas admission starts
2 ms before each 30 Hz neural boundary, and latency begins at WindowServer's native
mach-absolute display event. It covers Metal image preprocessing, IOSurface import,
Core ML, target publication, and full-desktop scene commit.

Adaptive displays may present unchanged desktops at 30 Hz while idle. Run the separate
nonactivating stimulus so every selected display has a changing captured region without
moving focus or accepting input, then run the full-scope tool after other GPU or Neural
Engine workloads have stopped:

```sh
build/benchmark-release/benchmarks/saccade_benchmark_macos_display_stimulus 63 &
build/benchmark-release/benchmarks/saccade_benchmark_macos_full_scope \
  detector.model /path/to/compiled-model-root \
  build/benchmark-release/backends/metal/saccade_overlay.metallib 60
wait
```

Two explicit 60-second modes exercise memory behavior:

```sh
build/benchmark-release/benchmarks/saccade_benchmark_macos_full_scope \
  detector.model /path/to/compiled-model-root \
  build/benchmark-release/backends/metal/saccade_overlay.metallib 60 stable-cached

build/benchmark-release/benchmarks/saccade_benchmark_macos_display_stimulus 63 &
build/benchmark-release/benchmarks/saccade_benchmark_macos_full_scope \
  detector.model /path/to/compiled-model-root \
  build/benchmark-release/backends/metal/saccade_overlay.metallib 60 continuous-change
wait
```

`stable-cached` admits one physical frame per display, then exercises bounded atlas
reuse, exact inference, publication, and interaction without requiring the compositor
to emit unchanged frames. `continuous-change` requires physical capture delivery to
keep pace with neural publication. Both sample owned and resident memory once per
second after a ten-second baseline. They require zero post-warmup owned high-water
growth and bound final and peak resident growth. Stable cached reuse does not substitute
for a physical idle-display capture soak.

The report includes live capture pressure, batch and full-scope percentiles, interaction
ticks, native capture age, cross-display timestamp skew, Metal atlas latency, target
count, process residency, and separate inference, preprocessing, and capture memory
high-water marks. Four warmup scenes and the cold first-scene latency are reported
separately. The sustained sample uses the same 29 Hz, 33.33 ms p95, and
1,000-parts-per-million miss-rate thresholds as Windows.

## Contained live tests

The default test lane excludes every test that registers system-wide input, creates a
desktop window, posts native input, captures the desktop, or presents an overlay. Live
tests are split into five explicit categories:

- `registration` registers global hotkeys.
- `owned-window` creates only a test-owned Accessibility or UI Automation window.
- `workflow` posts pointer, button, scroll, and text input only to a test-owned window,
  checks stale-plan and physical-override behavior, and restores the pointer.
- `capture` reads the current desktop or an owned capture surface.
- `overlay` creates click-through, nonactivating overlay surfaces.

The runner requires the selected category twice so an accidental CTest invocation cannot
cross the live boundary. After reviewing the capture tests, run:

```sh
SACCADE_ALLOW_LIVE_TESTS=capture cmake \
    -DBUILD_DIR=build/dev \
    -DCATEGORY=capture \
    -P scripts/run_live_tests.cmake
```

Use `CATEGORY=all` and `SACCADE_ALLOW_LIVE_TESTS=all` only for a deliberate acceptance run.
Pass `-DREPORT_FILE=<path>` to write a JUnit report. Input translation and executor unit
tests use capture sinks and remain in the default headless lane. Only the guarded
`workflow` category posts native input.

## Source-tree rules

Keep generated and machine-specific material out of the repository. This includes
downloaded models, datasets, screenshots, recordings, notebooks, profiler captures,
compiler output, and temporary kernel variants.

A small synthetic test fixture may be committed when its origin and license are
recorded, its behavior is deterministic, and the test needs it. Product code must not
depend on a local capture, model cache, or network download.

Maintained documentation describes the contract that exists in code. Exploratory
notes and machine-local state stay out of the source tree.
