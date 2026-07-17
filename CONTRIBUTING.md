# Contributing

Saccade is a native C++20 project with an installed C11 boundary. Changes should keep
that split visible: portable contracts belong in public headers, implementation details
stay private, and platform or framework types remain inside their provider.

## Contribution access

Code changes are accepted only from explicitly invited repository collaborators. GitHub
pull-request creation is restricted accordingly. Public users may report reproducible bugs
or propose product behavior through the repository issue forms. Security reports must use
the private process described in [SECURITY.md](SECURITY.md).

Every commit submitted for merge must include a `Signed-off-by` trailer created with
`git commit -s`. The sign-off certifies the contribution under the
[Developer Certificate of Origin 1.1](https://developercertificate.org/). Saccade uses the
DCO instead of a contributor license agreement.

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
- Keep hot mutable state thread-owned and prefer thread-local or thread-affine arenas.
- Build runtime text with bounded stack-backed builders and typed appends. Do not use
  `printf`-family formatting on runtime paths.
- Keep builder spill paths cold and explicit. A future spill path must use a Saccade
  allocator; hot builders expose truncation instead of allocating.
- Name private data members with a trailing underscore.
- Use direct function pointers or inline callback templates; do not use `std::function`.
- Prefer wait-free single-writer handoffs and do not use CAS retry loops on runtime paths.
- Make queue capacity, replacement, timeout, and cancellation behavior explicit.
- Do not let C++ exceptions cross an exported C function or provider callback.
- Keep ownership visible in names, descriptors, and tests.
- Add comments only where the contract is not clear from the code.

Avoid a new abstraction unless it removes repeated policy or makes a lifetime boundary
testable. Avoid an optimization unless a parity test and a measurement can describe it.

## Formatting

Format production C, C++, and Objective-C++ with the repository `.clang-format` file.
The 120-column limit is intentional: use the available horizontal space instead of
creating narrow staircases of continuation lines. Treat a function body as short semantic
paragraphs. Separate setup, validation, fast paths, state derivation, branching,
publication, and statistics with one empty line. Separate consecutive control blocks
unless they are one tightly coupled decision. Do not add empty lines between statements
that form one operation.

The formatter preserves those deliberate empty lines, keeps definition return types and
short parameter lists attached, and removes trailing whitespace. Run it on files you
change before building:

```sh
clang-format -i path/to/file.cpp
```

At major implementation milestones, format the complete native source manifest rather
than only the files changed by the latest patch:

```sh
rg --files apps backends benchmarks include platform src tests tools \
  | rg '\.(c|cc|cpp|cxx|h|hh|hpp|hxx|m|mm)$' \
  | sort -u \
  | xargs clang-format -i
git diff --check
```

Follow the exhaustive pass with the normal build and test presets on every supported
host affected by the milestone.

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
