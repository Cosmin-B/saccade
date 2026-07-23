# Architecture

Saccade separates desktop interaction from scene understanding. Keyboard input,
pointer feedback, and overlay presentation have a tighter deadline than capture or
neural inference. A late scene update can be replaced. An input event cannot.

One desktop owner advances separate interaction and scene scheduler deadlines, publishes
immutable scenes, freezes hint sessions, validates actions, and composes per-display
overlays. Capture, inference, accessibility, presentation, and input remain separate
providers with bounded ownership.
The local agent service reads the same scene and submits actions through the same checks
as interactive input.

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
                         +------> overlay packet ------> compute expansion ------> overlay
```

Capture providers produce leased frames and damage regions. The current CPU image kernel
converts R8, BGRA8, BGRX8, and RGBA8 input to exact R8 luma. The Metal and D3D12 GPU paths
fuse source cropping, aspect fit, letterboxing, bilinear sampling, channel normalization,
and planar FP16 or INT8 output. Metal also emits the IOSurface-backed BGRA image required
by Core ML. Inference and accessibility results carry the epochs needed to decide
whether they are still current.
The interaction-thread scene coordinator polls one semantic ticket without waiting,
selects the newest source epoch, fuses exactly matching neural and semantic packets, and
publishes directly into the final immutable scene store. Input receives a bounded action
plan, not a raw model prediction.

Native capture-to-model workers, process event loops, and the portable desktop owner keep
their contracts separate from the provider ABI so platform types do not enter the
portable core. The process hosts admit a configured model manifest, initialize the native
provider, and own capture, inference, fusion, interaction, and presentation through one
ordered lifecycle. Production admission additionally requires the payload checks defined
in the [inference policy](inference.md). Builds without required model inputs remain inert
and report a bounded fault.

## Two clocks

The interaction clock covers keyboard handling, pointer motion, action state, and
overlay presentation. It advances at up to 120 Hz without waiting for scene production.

The scene clock covers capture, accessibility refresh, image preparation, inference,
and target fusion. Version 0.1 targets a 30 Hz full-scope neural refresh on supported
hardware. The clocks exchange bounded snapshots. Neither waits for the other.

Isolated model timing, deterministic replay, offscreen rendering, and source scaling
measure components. End-to-end timing runs from capture to scene publication and from
input to physical presentation.

A full-scope pass lets every visible point affect the result. Individual model layers may
use tiling, compact feature pyramids, or low-channel native-pixel stems while preserving
the same output contract.

## Provider families

Five provider families keep unrelated lifetime rules apart:

1. Inference providers own devices, models, execution contexts, and tickets.
2. Capture providers own streams, frame acquisition, and damage reporting.
3. Overlay providers own presentation resources without taking application focus.
4. Accessibility providers own semantic queries, snapshots, and window identity.
5. Input providers execute validated pointer, scroll, drag, and text plans.

Each family has its own size-versioned C operations table. Registration is explicit.
Provider and device metadata is copied into fixed-capacity storage, so caller-owned
names may be released after registration. The registry freezes before execution, uses
domain-tagged handles, and exposes no storage addresses.

The deterministic providers implement all five families for contract tests. The
scalar CPU provider implements the inference family and exact image fixtures. Neither
substitutes for a native platform backend.

The [overlay path](overlay.md) defines target and instance records, scene-rate compute
expansion, display scheduling, and fixed memory. The [inference policy](inference.md)
defines compiled-model ownership, native buffer rules, mixed precision, and backend
requirements. [Coordinates and display topology](coordinates.md) defines direct
capture, desktop, surface, and window mapping. The [local agent protocol](agent-protocol.md)
defines observation, query, action batching, capability negotiation, and platform channels.

## ABI boundary

Installed headers compile as strict C11 and C++20. They use fixed-width values, byte
spans, opaque 64-bit handles, and structures beginning with `struct_size` and
`api_version`. The C ABI is the source of truth.

One frame descriptor map emits C11 `_Generic` dispatch and C++20 overloads. Both forms
call explicit C symbols, so the convenience layer adds no runtime type switch.

Platform SDK and inference-framework headers do not enter the installed include graph.
Host frames enter a fixed-capacity lease pool and newest-frame mailbox. macOS capture
frames retain their CVPixelBuffer and IOSurface through a generation-safe lease and expose
a direct Metal texture view. Platform types remain outside the installed headers.

## Memory and concurrency

Registries, handle tables, ticket tables, diagnostic text, test-provider state, and
scalar detector scratch storage have fixed capacities. Contract tests replace global
`operator new`, reject allocator references in the core archive, and verify selected
Saccade-owned C++ paths. They do not observe Objective-C, Core ML, ONNX Runtime, COM,
driver, or compositor allocations.

Memory reports separate host commitment, host reservation, imported device memory,
provider-owned device memory, framework residency, copied bytes, and high-water use.
The [memory guide](memory.md) covers resolution and queue-depth scaling.

Runtime and provider hot paths are scheduler-owned. Only cold global lifecycle mutation
uses the bounded CAS gate. Repository checks reject mutexes and reject CAS anywhere else.
The newest-frame exchange is allocation-free and uses one atomic exchange per ownership
transfer. The 120 Hz scheduler and worker topology are described in
[concurrency](concurrency.md).

## Platform boundary

The macOS display collector uses public AppKit and Core Graphics APIs. The Metal backend
implements bounded compute expansion, rasterization, and presentation through public
Metal, QuartzCore, and nonactivating AppKit panels. ScreenCaptureKit supplies bounded
native display and window frames. Accessibility and CGEvent provide semantic targets
and validated input. The Windows adapter publishes per-monitor-v2 physical geometry,
translates validated plans into fixed `SendInput` batches, and uses Windows Graphics Capture for
display and visible top-level window sources. Production WGC frames are copied once on
the GPU into a bounded shared D3D12 resource, accompanied by a shared-fence dependency,
and retired after inference. The capture owner does not access the inference queue. The
D3D12 worker consumes the dependency and implements preprocessing, DirectML inference,
and GPU target postprocessing. It owns a high-priority MMCSS registration for its lifetime.
The application owner owns a separate registration for the 120 Hz loop and composes fixed
overlay packets. The Windows overlay provider expands them on the GPU, renders indirectly,
and presents serially through nonactivating DirectComposition surfaces. A fixed 16-display
set reconciles them against topology epochs. UI Automation remains on a dedicated MTA
worker and publishes bounded desktop-Q8 target packets without
blocking the display-rate owner. Platform code is translated into the same provider
and coordinate contracts.

Version 0.1 activates and cycles windows on the current desktop. Moving another
application's windows between macOS Spaces or Windows virtual desktops is outside the
contract. Login, lock, consent, and other secure screens block interaction.
