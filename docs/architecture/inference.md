# Inference execution and model tooling

Saccade has a task-oriented inference API. It does not expose a general operator graph.
A provider answers whether it can compile one complete model, creates a context for one
device and queue policy, then accepts frame-scoped submissions with explicit epochs.

This boundary is deliberate. Native backends can combine owned kernels and platform
model runtimes without placing framework tensors, events, or headers in the public ABI.

## Artifact admission

Model artifacts enter through one fixed, versioned manifest. It records the graph and
artifact kind, model identity, input shape and channels, precision, maximum target and
output capacity, provider compatibility, payload span, and an optional fixed-size
signature. Provider payloads also carry model-specific postprocess calibration: the
global confidence threshold, an optional short-side confidence band, IoU threshold, and
bounded target count. Release admission requires a signature. The signed message is the exact byte
range from the manifest magic through the final payload byte; the 64-byte signature must
end the file, so no interpreted data sits outside the authenticated range. The parser
validates only bounded structure and ranges.

The native verifier uses P-256 ECDSA with SHA-256 and the fixed RFC 4754 `r || s`
representation. Apple Security and Windows CNG cache the imported public key during cold
startup. Positive and tampered-message/signature fixtures share the same bytes on both
platforms. The artifact file is mapped read-only and verified in place, avoiding a heap
copy of ONNX or compiled-model metadata. Runtime creation, provider freeze, device
selection, model admission, and session teardown then pass through one shared bootstrap.

No manifest hash or signature work occurs during capture, preprocessing, inference,
postprocessing, scene publication, or presentation. The admitted payload is then owned by
the selected provider according to its documented import contract. A compiled Core ML
bundle uses a package-relative locator and signed directory digest. Production admission
compares that digest before loading the bundle and rejects symlinks, special entries, and
unbounded directory shapes. Memory-native graph formats may use the payload directly.

## Model lifetime

Model creation is cold-path work. A provider validates the artifact, chooses precision,
packs weights, compiles pipelines, and reports required host, device, and opaque runtime
memory before interaction begins. Execution contexts own queue depth, in-flight state,
scratch storage, and device-specific compiled state.

Ahead-of-time artifacts are preferred when the target device family is known. On-device
compilation is allowed for small portable models, but its time and peak memory are part
of startup accounting. Compilation never occurs in an activation or display callback.

`query_model` is the capability query. It tests the whole graph and reports whether that
provider can execute it under the requested device and precision policy. Saccade does
not publish an operator-by-operator graph API. Whole-model selection prevents a partial
delegate from adding hidden transfers between unsupported layers.

## Buffer ownership

The fastest path keeps data on one device:

```text
native capture surface
    -> preprocess textures/tensors
    -> compiled model
    -> postprocess and suppression
    -> target buffer
    -> overlay expansion and draw
```

Provider import bits and alignment fields describe the surfaces a device can consume.
Native tensor buffers, heaps, fences, and queue events remain provider-owned. A ticket
represents completion at the portable boundary; it does not force a CPU wait or expose a
framework event object.

Intermediate tensors are not read back to the CPU. A backend may return compact target
metadata for semantic fusion and action validation, but it keeps feature maps, logits,
and suppression scratch on the device. Cross-device execution is accepted only when a
trace proves the transfer is cheaper than keeping the graph on one accelerator.

## GPU preprocessing

The macOS preprocessing contract has three outputs. A shape-compatible model can consume
the captured BGRA8 texture directly. A fixed image model receives one persistent
IOSurface-backed BGRA8 pixel buffer. A tensor model receives one persistent planar FP16
or INT8 buffer. The reusable lane fuses crop, aspect-preserving fit, integer letterbox
placement, bilinear sampling, RGB channel mapping, scale, bias, and output storage in one
Metal dispatch. The output is overwritten only after the neural coordinator retires the
runtime frame that references it.

Metal 4 is selected when supported, with a reusable command allocator, argument table,
residency set, command buffer, and shared event. Metal 3 uses one bounded command queue and
the same shader and tensor contract. The preprocessor performs no CPU pixel conversion or
per-frame Saccade-owned allocation; framework and driver allocations are outside that
claim.

## macOS Core ML provider

The macOS provider admits a signed manifest whose package-relative payload names one
compiled `.mlmodelc` bundle. Admission validates the fixed BGRA8 image input, input
dimensions, named float target-row output, named count output, candidate capacity, and
maximum target packet before a context starts. The configured Core ML compute policy is
reflected in provider and device capabilities after intersecting the selected policy with
`MLModel.availableComputeDevices`; CPU-only does not advertise GPU or Neural Engine
execution, and Intel Macs do not advertise a Neural Engine. Core ML exposes CPU-only,
CPU-plus-GPU, CPU-plus-Neural-Engine, and all-device modes, not GPU-only or
Neural-Engine-only execution, so the application uses those exact terms.

Fixed-shape models use the infrequent-reshape hint on macOS 14.4 and newer and the
fast-prediction specialization strategy on macOS 15 and newer. Each context owns one
worker, one depth-one ticket, one retained IOSurface-backed pixel
buffer view, and fixed target output storage. Submission publishes the ticket through a
release/acquire handoff and wakes the worker. Polling only reads completion state, so
Core ML prediction does not execute on the 120 Hz scene owner. Explicit wait,
cancellation, reset, and teardown may wait for an active framework call. Core ML does
not expose cancellation for a synchronous prediction already in progress, so Saccade
acknowledges cancellation only after worker ownership ends and suppresses that result.

The image bridge maps only the aspect-fitted content rectangle back to desktop
coordinates, so targets in letterbox pixels are clipped instead of stretched onto the
display. Runtime retirement returns the image lane to the preprocessor through a direct
owner-thread callback.

IOSurface import creates a Core Video view over the existing surface and does not copy
frame pixels into Saccade-owned memory. Core ML retains ownership of opaque model,
tensor, accelerator, and prediction-feature allocations. The provider decodes only the
bounded target rows and publishes the ordinary compact target packet used by fusion.
Framework allocation and residency require OS-level measurement.

The deterministic Core ML fixture covers artifact admission, runtime registration,
IOSurface import, asynchronous execution, cancellation, epoch propagation, and packet
parity through the public C ABI. Model quality and end-to-end cadence are separate
concerns.

## Windows DirectML provider

The Windows provider owns one D3D12 device and direct queue. Production Windows Graphics
Capture runs on a native D3D11 device for the same adapter. A bounded transfer owner
copies each selected WGC frame once into a shared D3D12 resource and signals a shared
fence. The public Win32 frame import retains both the resource and readiness dependency
for the runtime ticket lifetime. On the dedicated GPU worker, the provider inserts the
fence wait before preprocessing; the capture thread never accesses the DirectML queue.
Preprocessing, DirectML inference, target postprocessing, and bounded packet readback then
remain ordered on that worker-owned queue.

The provider disables ONNX Runtime telemetry on its environment before creating the
session. Model execution remains local and does not require a runtime network path.

The admitted ONNX contract has one planar FP16 or INT8 image input and one fixed FP16
`[candidate_capacity, 6]` output. Rows contain normalized x, y, width, height,
confidence, and role. An owned D3D12 dispatch removes letterboxing and packs them into
the 16-byte internal dense layout without a CPU readback. Unused rows have zero
confidence, so the GPU can process the fixed capacity without reading a candidate count
back to the CPU. Input names, candidate capacity, thresholds, normalization, and
letterbox values are part of the signed artifact contract rather than process
configuration.

Model admission creates persistent preprocessing, inference, candidate, radix,
suppression, packet, and readback resources. Context creation transfers those objects
once to the worker thread. Submission then performs a release/acquire handoff with no
mutex, allocation, or compare-and-swap loop. Cancellation suppresses output after any
already-running DirectML call retires.

DirectML receives the worker's D3D12 queue during session creation. After ONNX Runtime
submits a graph, Saccade signals one persistent fence on that same queue and waits on one
reusable event before postprocessing. Persistent run options suppress ONNX Runtime's
redundant end-of-run execution-provider synchronization. Queue order makes the Saccade
fence cover the complete graph without calling the allocating `SynchronizeBoundOutputs`
wrapper. The Saccade-owned synchronization path performs no steady-state allocation;
ONNX Runtime, DirectML, and driver behavior is measured separately.

The automatic and GPU policies select a hardware D3D12 adapter. The current CPU policy
selects Microsoft's Windows Advanced Rasterization Platform (WARP), which executes the
D3D12 and DirectML graph in software on CPU cores and advertises CPU rather than GPU
capability through the same provider contract. WARP is a functional compatibility and
validation fallback: capture, model execution, and bounded packet publication remain
available while the 120 Hz interaction path continues to use the newest immutable
scene. It is not intended to meet the 30 Hz neural target.

The version 0.1 graph contract uses a `1 x 3 x 768 x 1280` input and a fixed
`1024 x 6` candidate output. `saccade_benchmark_windows_directml_model` reports session
creation, steady-state percentiles, and process memory for a supplied artifact.

The D3D12 postprocessor uses 16 stable four-bit radix passes followed by GPU containment
and IoU masks. It reads back only the bounded target packet. Its parity fixture matches
the scalar packet byte-for-byte across empty, threadgroup-boundary, and partial-block
candidate counts. The end-to-end fixture uses a real WGC frame and a minimal ONNX graph
to prove ownership and execution; it does not claim detector accuracy.

## GPU target postprocessing

The owned target postprocessor consumes a persistent provider output buffer. The provider
registers that buffer once during initialization; a hot submission changes only the
candidate count, thresholds, and epochs. Metal 4 argument tables and residency are fixed
before the first submission. Metal 3 binds the same fixed resources through its compute
encoder.

Decoded candidates are source-local Q3 records:

```text
x, y, width, height       4 * uint16 = 8 bytes
confidence                uint16     = 2 bytes
role, source bits         2 * uint8  = 2 bytes
flags, reserved           2 * uint16 = 4 bytes
total                                  16 bytes
```

Q3 covers a surface up to 8191.875 pixels with one-eighth-pixel precision. Publication
widens coordinates to signed Q8 scene records. The narrow candidate layout is an internal
model-output contract; the public 80-byte target record remains independent.

The GPU graph is deterministic:

```text
persistent candidate buffer (16 bytes each)
    -> global/banded confidence predicate + 64-bit ordering key
    -> 16 stable 4-bit radix rounds
       histogram -> one-workgroup block scan -> scatter
    -> pairwise containment/IoU suppression masks
    -> ordered mask reduction + target packet emission
    -> compact packet readback for fusion
```

The ordering key is confidence descending, y ascending, x ascending, then original
candidate index. The scalar oracle uses the same order and suppression rules. Metal 3
and Metal 4 packets must match it byte for byte. The scan never waits for another
workgroup inside a dispatch, so it does not rely on inter-workgroup forward progress on
Apple GPUs. Exact dispatch sizes consume a candidate shape already validated at model
load; shaders do not repeat capacity or bounds validation.

The optional confidence band lowers the threshold only when the candidate's shorter Q3
side falls in the signed half-open interval `[minimum, maximum)`. The predicate runs
while radix keys are generated, so rejected candidates do not consume sort or suppression
capacity. A confidence-ordered `maximum_targets` cap then bounds dense frames without a
CPU sort or provider-specific policy.

Neural targets are executable only when eroding every edge by one source pixel leaves a
nonempty interior. The target remains visible when it is too small, but its actionable bit
and capabilities are cleared. Normal targets use the center of that eroded interior as the
safe point. The scalar, Metal, and D3D12 paths share this exact predicate and their packet
outputs are checked byte for byte.

The application scene filter is a user override layered after provider admission. Its
default is the minimum nonzero Q16 value, so it does not silently replace the signed
model calibration. Raising it intentionally can hide lower-confidence targets without
repacking the model artifact.

For 65,536 candidate capacity and a 1,024-target neural cap, the graph encodes 51 compute
dispatches and 50 buffer dependency barriers. `saccade_benchmark_metal_target_postprocess`
compares Metal 3 and Metal 4 across supplied candidate capacities and reports latency plus
framework-opaque allocator residency. Optimization decisions use the complete
capture-to-target path.

## Scheduling

Each provider context executes one neural frame at a time. The full-desktop coordinator
keeps one newest pending frame per display, snapshots up to 16 displays at a 30 Hz scene
deadline, and runs that fixed batch sequentially on the owned accelerator lane. Results
are mapped into desktop coordinates and reduced to one bounded confidence-ordered scene;
an individual display can no longer replace another display in the same neural tick.
Frames arriving while a batch runs populate the next per-display snapshot. The provider
receives frame, model, session, source-transform, and topology epochs; the aggregate scene
also carries one desktop-transform epoch, so late source output cannot become actionable.
The owner records both batch-start-to-commit latency and oldest-captured-frame-to-commit
full-scope latency for every published scene. The latest samples and cumulative, maximum,
and missed-period counters are available in diagnostics. Capture timestamps already share
the platform monotonic clock with the scheduler, so provider threads perform no timing
calls and share no statistics state.

Preprocessing, inference, postprocessing, and target publication share the 30 Hz scene
budget. Input and overlay presentation continue at 120 Hz while a neural ticket is
running.

Cancellation, device loss, and reset are explicit. A backend may use native fences to
chain accelerators without a CPU wakeup, but it must still retire tickets and release
all imported resources through the provider contract.

## Precision and quantization

Quantization is a per-operation decision recorded in the model artifact. One global
"INT8 model" switch is too blunt for compact vision networks. The toolchain may combine
INT4 or INT8 weights, integer activations, FP16 compute, and selected FP32 operations.

Every candidate backend and model is evaluated for:

- target precision, recall, ordering, and safe-point stability;
- full-scope p50, p95, and p99 latency;
- model bytes, packed-weight bytes, activation high-water, and compile peak;
- startup and cache reuse;
- power and thermal behavior;
- fallback parity on macOS and Windows.

Weight-only quantization can reduce package size while making latency worse if weights
are dequantized into float at runtime. Static activation quantization can help an NPU but
needs representative calibration frames. Provider selection follows end-to-end behavior,
not the nominal bit width.

## Runtime policy

Owned GPU kernels are preferred for fixed high-frequency stages that Saccade can measure
and tune end to end. Platform runtimes provide graph execution and hardware coverage.
Each provider must preserve explicit tensor ownership, asynchronous completion, bounded
queues, and the common target-row contract without exposing framework types through the
public ABI.

## Backend requirements

An inference backend reports:

- exact input/output shapes, layouts, data types, and alignment requirements;
- supported native surface imports and whether each path copies;
- compile, first-run, steady-state, cancellation, and teardown latency;
- host, device, imported, workspace, packed-weight, and opaque memory;
- threads created, their ownership, and behavior under system pressure;
- device-loss recovery and deterministic fallback;
- per-frame allocation and synchronization behavior;
- package size and license obligations.

Provider selection considers the complete capture-to-target path, including input and
presentation isolation, startup, memory, and deployment.
