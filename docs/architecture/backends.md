# Provider backends

Providers isolate native APIs and inference runtimes from the portable core. Each
family registers a context pointer, metadata, and one size-versioned operation table.
The operation tables are independent because capture, model execution, presentation,
accessibility, and synthetic input have different ownership and failure rules.

## Registration and selection

Registration occurs before runtime freeze. Each family has room for eight providers.
The compute-device registry has room for 32 devices and accepts devices only from a
registered inference provider.

A provider stable ID must be unique within its family. A device stable ID is scoped to
its provider. Selection can require and prefer capability, pixel-format, precision, and
surface-import bits. Queue capacity and maximum in-flight work are also filterable.
Ties keep registration order. Explicit provider and device IDs report a distinct
selection reason.

Selection and lookup do not allocate.

Registry tests cover name ownership, capacity, compatible prefixes, malformed descriptors,
freeze behavior, and handles from another registry domain.

## Asynchronous work

Inference, accessibility, and input providers return generated tickets. Their tables
support non-blocking polling, bounded waiting, cancellation, synchronization, and
reset or release operations appropriate to that family.

Ticket failures stay specific:

- Queue full returns `SACCADE_ERROR_BUSY`.
- Output too small returns `SACCADE_ERROR_CAPACITY` with the required byte count.
- Cancellation returns `SACCADE_ERROR_CANCELLED`.
- A stale resource generation returns `SACCADE_ERROR_STALE_HANDLE`.

Input operations have no separate collect or release call. Once `poll` or `wait`
successfully writes a terminal input status, that ticket is retired and later use
returns `SACCADE_ERROR_STALE_HANDLE`. This keeps the bounded ticket table reusable.

The portable runtime does not call provider code while holding a future scheduler
queue lock. That rule matters once native providers begin completing work on framework
threads.

## Device failure recovery

The desktop owner treats a native backend failure as loss of the complete platform
pipeline, not as an operator-level retry. It disconnects local agent clients, neutralizes
synthetic input, tears capture, inference, overlay, and device objects down in ownership
order, then rebuilds the pipeline after a 250 ms delay. Repeated initialization failures
back off to an eight-second ceiling without blocking 120 Hz message processing.

Recovery state belongs to the desktop owner thread and uses no lock, atomic, allocation,
or background retry task. A successful rebuild reconnects the local agent endpoint and
clears the visible fault. If teardown cannot confirm that native ownership ended,
the application launches a clean replacement process before constructing a second GPU
stack over uncertain resources.

## Deterministic providers

`Saccade::mock` implements all five families for tests. Its configuration controls:

- The number of polls before asynchronous completion.
- Queue capacity and advertised capabilities.
- Capture dimensions and formats.
- Memory counters returned by every family.
- One-shot faults at named operation boundaries.

The delay is a poll count, not a sleep. Tests can exercise running, completion,
cancellation, timeout, queue pressure, reset, and teardown without depending on wall
clock timing. The provider records bounded observations such as submission counts,
scene epochs, visibility, and command hashes. It does not perform vision work or emit
real input.

## Scalar CPU provider

`Saccade::reference_cpu` is the first correctness provider. It accepts small borrowed
host fixtures in BGRA8, RGBA8, BGRX8, or R8 form. Its model is a 12-byte versioned
record containing an integer luma threshold and minimum component area.

The detector performs:

1. Exact integer luma conversion.
2. Four-connected bright-region labeling with fixed arrays.
3. Component bounding boxes and integer center points.
4. Reading-order sorting.
5. Deterministic IDs and Q16 confidence.
6. Explicit little-endian serialization.

The detector accepts fixtures up to 1024 by 1024 pixels, with 128 intermediate components
and 32 outputs. Those limits keep the allocation-free oracle small. Desktop accuracy and
speed come from the shipping neural detector.

This provider is deliberately synchronous. Submission creates a bounded ticket. The
first poll, or a wait with a nonzero timeout, performs the scalar detector before
reporting completion. It does not advertise the asynchronous capability. Production
inference providers must run work away from the interaction path.

## Native providers

A native provider is accepted only after it passes the same descriptor, lifecycle,
queue, cancellation, stale-handle, memory, and output-parity checks. Platform defaults
are candidates, not architectural requirements. Core ML, Metal graphs, Windows ML,
DirectML, and owned kernels can all fit behind the inference contract.

Native capture and input providers must also pass platform permission, secure-screen,
focus, display-change, and teardown tests before the corresponding desktop mode is
considered supported.

### macOS Core ML

The macOS Core ML provider loads one signed compiled bundle and imports one BGRA8
IOSurface-backed frame at a time. A dedicated worker owns synchronous Core ML prediction.
Submit and poll do not run the model on the interaction owner. Fixed provider storage
contains the ticket, compact output packet, decoded candidates, and postprocess
workspace. Framework-owned model and accelerator allocations remain opaque and require
separate measurement. The contract fixture covers execution mechanics. Model accuracy is
a separate concern.

### Windows UI Automation

The Windows accessibility provider enumerates visible top-level windows and traverses
UI Automation's control view on one dedicated MTA thread. The caller-facing path has
one fixed in-flight request and uses release/acquire publication plus Win32 events for
sleep and explicit waits. A 10,000-record snapshot arena is reserved once with
`VirtualAlloc`. Queries do not allocate Saccade-owned memory. Completed snapshots are
ordinary `SaccadeTargetPacketHeader` and `SaccadeTargetRecord` bytes in desktop-Q8
coordinates, with runtime-derived target IDs and monitor IDs. UI Automation's own COM
allocations remain confined to the worker.

### macOS Accessibility

The macOS accessibility provider enumerates layer-zero Core Graphics windows and maps a
selected public window ID to its owning application's AX window by process and geometry.
One pthread owns synchronous Accessibility calls and wakes through Mach semaphores.
Traversal is depth-first through public `AXChildren` values with a retained, bounded
10,000-entry stack and a 100,000-element visit ceiling. Position, size, role, subrole,
enabled, hidden, and identifier attributes are fetched together. Provider messaging has
a 500 ms timeout. TCC denial reports `SACCADE_ERROR_PERMISSION` and never an empty scene.

The macOS provider reserves one 1,040,000-byte `mmap` arena at initialization: 800,000
bytes for target records and 240,000 bytes for retained traversal entries. It allocates
no Saccade-owned memory per query. Capacity, depth, malformed-child, or visit limits set
`SACCADE_TARGET_PACKET_INCOMPLETE`. Fusion preserves that flag in the published scene.
Secure and disabled AX elements remain visible as safety evidence but carry no action
capabilities.

Overlay providers consume the versioned packet from
`<saccade/saccade_overlay.h>`. Native providers expand targets on the GPU when the scene
or transform epoch changes, read dynamic active selection from
`SaccadeOverlayFrameDesc` at display rate, and issue one instanced draw. The scalar
expander is the parity oracle, not the preferred presentation path. See
[overlay packets and presentation](overlay.md).

### macOS capture and presentation

The current macOS implementation contains a Metal 4 compute-to-render path with a Metal
3 fallback, one-draw fragment rendering, main-run-loop display-link scheduling,
nonactivating per-display panels, and a main-thread display collector backed by the
portable fixed-point catalog. Its ScreenCaptureKit provider captures displays or
desktop-independent windows, excludes the current application, and publishes only
the newest pending frame. It creates a Metal texture view directly from each IOSurface.
Zero capture limits preserve native resolution. Nonzero limits form an
aspect-preserving model-input box. Accessibility, scene publication, and input remain
separate adapter stages. See [coordinates and display topology](coordinates.md).

### Windows capture and inference

The Windows GPU owner selects a physical adapter explicitly and creates one D3D12 device
and direct queue. Production Windows Graphics Capture uses a native D3D11 device on the
same adapter because WGC's public surface contract is D3D11. Display and visible
top-level window sources use the same bounded free-threaded frame pools. Acquisition
drains queued frames to the newest, borrows the capture texture until explicit release,
and reports dirty regions without a CPU pixel copy. Content-size changes advance the
stream transform epoch and recreate the pool after the outstanding texture lease is
returned.

Live capture crosses the API boundary through an on-demand ring of shared resources. A
free D3D12 texture is opened by the producer D3D11 device, `CopyResource` performs one
GPU-local copy, and the producer signals a shared fence value. The imported frame retains
the D3D12 texture and `{fence, value}` dependency. The capture owner never submits work
to the DirectML queue. The inference worker waits on that dependency and then runs one
Shader Model 6 preprocessing dispatch for source crop, aspect fit, letterbox, bilinear
resize, channel scale, channel bias, and planar FP16 or INT8 output. Slot retirement is
coupled to frame retirement, so capture storage remains bounded and reusable.

The preprocessing root signature, pipeline, two-entry descriptor heap, output buffer,
allocator, command list, and completion fence are persistent. The 96-byte parameters are
root constants. Exact GPU-readback tests cover channel order, FP16 conversion, INT8
quantization, crop, and letterbox output. Benchmarks report live transfer bytes separately
from preprocessing so the required GPU copy cannot be mistaken for zero-copy import.

Inference runtimes are selected at the whole-model boundary. Their graph, tensor,
workspace, and fence types remain private to the provider. Optional runtimes must pass
binary-size, memory, parity, zero-copy, startup, and device-loss gates before becoming a
platform default. See [inference execution and model tooling](inference.md).
