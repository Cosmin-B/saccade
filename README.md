# Saccade

Saccade is a native keyboard-driven pointer system for macOS and Windows. It combines
captured pixels, accessibility data, and a compact local vision model to find useful
controls without sending screen contents over the network.

## What it does

- Targets controls across the full desktop, active window, one display, or a grid.
- Fuses neural, accessibility, pixel, and grid evidence into one immutable scene.
- Assigns prefix-free keyboard hints with Single, Dual, Multi, and Path selection.
- Moves, clicks, holds, drags, scrolls, selects text, repeats actions, and navigates
  windows through validated native input plans.
- Presents mixed-scale overlays on every display without activating its own windows.
- Provides native settings, bounded diagnostics, a debugger host, a local CLI, and an MCP
  stdio adapter.
- Rejects stale scenes, wrong-window actions, protected surfaces, permission loss, and
  physical-user overrides before input execution.

The complete version 0.1 behavior is defined by the
[version 0.1 product contract](docs/product/version-0.1.md).

## Architecture

Saccade uses C++20 internally and exposes versioned C11 headers. Hot paths use bounded
storage, thread-local or thread-owned state, newest-frame replacement, and fixed-point
desktop coordinates. Input, capture, accessibility, inference, overlay, and agent
services have separate provider contracts.

The native pixel path stays on the GPU where the platform permits it:

```text
native capture -> GPU preprocess -> local inference -> GPU target postprocess
              -> immutable scene -> GPU overlay -> validated native input
```

macOS uses ScreenCaptureKit, Metal 3/4, Core ML, Accessibility, and `CGEvent`. Windows
uses Windows Graphics Capture, D3D12, DirectML through Windows ML, UI Automation,
DirectComposition, and `SendInput`.

Input reduction, pointer feedback, and overlay presentation target 120 Hz. Full-scope
neural refresh independently targets 30 Hz for version 0.1.

Production model artifacts are signed outside this repository and supplied explicitly to
the packaging build. A releasable desktop package is required to contain the application,
local agent tools, platform accelerator runtime, compiled shaders, and configured model
assets.

## Build

You need CMake 3.30 or newer, Ninja, and a compiler with C11 and C++20 support.

```sh
cmake --preset dev
cmake --build --preset dev -j
ctest --preset dev --output-on-failure
```

Configuration performs no network download. Release, sanitizer, thread-sanitizer, and
profiling presets use the same command shape. See
[building Saccade](docs/development/building.md) for platform toolchains, model
packaging, tests, benchmarks, and installation.

## Repository layout

- `include/saccade/` contains the installed C11 API.
- `src/` contains the portable runtime and private C++20 implementation.
- `platform/` contains private operating-system integration.
- `backends/` contains native GPU and deterministic reference providers.
- `apps/` contains the native desktop process hosts.
- `tools/` contains the local CLI, MCP adapter, and release probe.
- `abi/` records public symbol and layout manifests.
- `tests/` and `benchmarks/` contain maintained correctness checks and measurement tools.

Downloaded models, captures, datasets, notebooks, generated kernels, and profiler output
stay outside this repository.

## Platform scope

Version 0.1 targets macOS 14 or newer on Apple Silicon and Windows 11 24H2 or newer on
x64 and arm64. Both applications implement the same behavior contract.

Start with the [architecture overview](docs/architecture/overview.md),
[provider contract](docs/architecture/backends.md),
[memory model](docs/architecture/memory.md),
[inference path](docs/architecture/inference.md),
[overlay path](docs/architecture/overlay.md),
[local agent protocol](docs/architecture/agent-protocol.md), and
[ABI guide](docs/architecture/abi.md).

## License

See [LICENSE](LICENSE).
