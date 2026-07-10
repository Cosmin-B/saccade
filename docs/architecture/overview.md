# Architecture

Saccade separates desktop interaction from scene understanding. Keyboard input,
pointer feedback, and overlay presentation have a tighter deadline than capture or
neural inference. A late scene update can be replaced. An input event cannot.

The repository currently implements the portable ABI, provider contracts, bounded
registries, deterministic providers, and a scalar CPU detector. The capture-to-action
pipeline below is the contract those pieces are being built to support.

## Data flow

```text
capture ------> frame lease ------> image kernels ------> inference
   |                 |                    |                   |
   +--- damage ------+                    +--- priority ------+
                                                              |
accessibility ---- semantic snapshot -------------------------+
                                                              |
                                                              v
input <--------- validated action plan <------- immutable target scene
                         |
overlay <----------------+
```

Capture providers produce leased frames and damage regions. Image kernels handle
format conversion, resize, comparison, and postprocessing. Inference and accessibility
results carry the epochs needed to decide whether they are still current. Fusion
produces an immutable target scene. Input receives a bounded action plan, not a raw
model prediction.

Frame leases, target fusion, action planning, and native adapters are still future
implementation work. Their contracts remain separate from the provider ABI so they do
not force platform types into the portable core.

## Two clocks

The interaction clock covers keyboard handling, pointer motion, action state, and
overlay presentation. Its design target is 120 Hz.

The scene clock covers capture, accessibility refresh, image preparation, inference,
and target fusion. Version 0.1 targets a 30 Hz full-scope neural refresh on qualified
hardware. Version 0.2 raises the accelerated target to 60 Hz. The clocks exchange
bounded snapshots. Neither waits for the other.

A full-scope pass means every visible point can affect the result. It does not mean
that every model layer operates on a native-resolution RGB tensor. Tiling, compact
feature pyramids, and low-channel native-pixel stems are valid when they preserve the
same output contract.

## Provider families

Five provider families keep unrelated lifetime rules apart:

1. Inference providers own devices, models, execution contexts, and tickets.
2. Capture providers own streams, frame acquisition, and damage reporting.
3. Overlay providers own presentation resources without taking application focus.
4. Accessibility providers own semantic queries, snapshots, and window identity.
5. Input providers execute validated pointer, scroll, drag, and text plans.

Each family has its own size-versioned C operations table. Registration is explicit.
Provider and device metadata is copied into fixed-capacity storage, so caller-owned
names may be released after registration. The registry freezes before execution and
uses domain-tagged handles rather than exposing storage addresses.

The deterministic providers implement all five families for contract tests. The
scalar CPU provider implements the inference family and exact image fixtures. Neither
is evidence that a native platform backend is complete.

## ABI boundary

Installed headers compile as strict C11 and C++20. They use fixed-width values, byte
spans, opaque 64-bit handles, and structures beginning with `struct_size` and
`api_version`. The C ABI is the source of truth.

One frame descriptor map emits C11 `_Generic` dispatch and C++20 overloads. Both forms
call explicit C symbols, so the convenience layer adds no runtime type switch.

Platform SDK and inference-framework headers do not enter the installed include graph.
The current frame-import functions validate their descriptors and return
`SACCADE_ERROR_UNSUPPORTED` until frame ownership and provider routing are connected.

## Memory and concurrency

Registries, handle tables, ticket tables, diagnostic text, test-provider state, and
scalar detector scratch storage have fixed capacities. Contract tests replace global
`operator new` and verify that provider operations do not allocate after construction.

Memory reports separate host commitment, host reservation, imported device memory,
provider-owned device memory, framework residency, copied bytes, and high-water use.
The [memory guide](memory.md) covers resolution and queue-depth scaling.

Public runtime calls are serialized today. Provider test doubles also protect their
state with a mutex. The 120 Hz scheduler, worker topology, and newest-frame mailboxes
are not present yet; their required behavior is described in [concurrency](concurrency.md).

## Platform boundary

The macOS adapter will use public ScreenCaptureKit, Accessibility, CGEvent, Metal, and
window APIs. The Windows adapter will use public Windows Graphics Capture, UI
Automation, SendInput, Direct3D, and DirectComposition APIs. Platform code will be
translated into the same provider and coordinate contracts.

Version 0.1 includes activation and cycling on the current desktop. It does not promise
to move another application's windows between macOS Spaces or Windows virtual desktops.
It does not interact with login, lock, consent, or other secure screens.
