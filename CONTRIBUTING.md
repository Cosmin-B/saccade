# Contributing

Saccade is a native C++20 project with an installed C11 boundary. Changes should keep
that split visible: portable contracts belong in public headers, implementation details
stay private, and platform or framework types remain inside their provider.

## Build before changing code

```sh
cmake --preset dev
cmake --build --preset dev -j
ctest --preset dev --output-on-failure
```

Use the release preset for optimized behavior and the sanitizer presets for ownership,
undefined behavior, and data races. A provider change should run its contract test under
ASan/UBSan and TSan.

## Code rules

- Compile as strict C11 or C++20 with extensions disabled.
- Keep public structures size-versioned and reserve zeroed space for compatible growth.
- Do not expose operating-system, graphics, or model-runtime types in installed headers.
- Use fixed storage on capture, inference, interaction, and presentation paths.
- Make queue capacity, replacement, timeout, and cancellation behavior explicit.
- Do not let C++ exceptions cross an exported C function or provider callback.
- Keep ownership visible in names, descriptors, and tests.
- Add comments only where the contract is not clear from the code.

Avoid a new abstraction unless it removes repeated policy or makes a lifetime boundary
testable. Avoid an optimization unless a parity test and a measurement can describe it.

## Tests

Tests scale with the boundary being changed:

- ABI changes update layout assertions, symbol manifests, C and C++ header checks, and
  installed consumers.
- Provider changes run the common lifecycle, queue, cancellation, memory, and fault
  cases.
- Kernel changes compare exact scalar output before adding tolerance for floating-point
  provider output.
- Concurrency changes run TSan and include a bounded overload case.
- Package changes install into an empty prefix and build a separate consumer.

Small synthetic fixtures may be committed when their origin and expected output are
clear. Downloaded models, screenshots, recordings, profiler captures, notebooks, and
generated kernel trials do not belong in product Git.

## Public writing

Documentation states what the code implements and labels future contracts as such.
Keep prose plain, specific, and testable. Follow [the writing guide](docs/development/writing.md).

## Security

Read [SECURITY.md](SECURITY.md) before changing capture, permissions, provider loading,
input execution, diagnostics, signing, or updates. Do not place private screen contents
or credentials in a fixture, log, issue, or review attachment.
