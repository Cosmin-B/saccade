# Concurrency and frame rates

Saccade has an interaction path and a scene path. Their deadlines and overload behavior
are different, so they cannot share one serial loop.

## Interaction path

The interaction path targets 120 Hz. It owns keyboard reduction, hint filtering,
pointer feedback, action validation, and overlay publication. A 120 Hz tick has 8.33 ms,
but the path should normally be event-driven and finish well below that budget.

The interaction path reads an immutable target scene. It does not wait for capture,
accessibility, neural inference, model compilation, or a GPU fence.

Mutable hot state belongs to one thread. Capture staging, scene construction, input
reduction, and presentation scratch storage use thread-local or thread-affine fixed
buffers. Cross-thread state is reduced to bounded ownership transfers and immutable
publication. The design does not use CAS retry loops on a steady-state path.

## Scene path

Version 0.1 targets a 30 Hz full-scope neural refresh, a 33.33 ms interval. Version 0.2
targets 60 Hz on qualified accelerated hardware, a 16.67 ms interval. Capture,
preprocessing, inference, postprocessing, and scene publication all consume that budget.

Inference is asynchronous: submission returns a ticket before the result is ready. A
worker or native runtime completes the ticket while input and presentation continue.
Polling, waiting, cancellation, and collection are explicit provider operations.

Region-bounded work means the scheduler can mark changed or high-value rectangles as
priority input and cap temporary work to known storage. It does not change the product
qualification rule. A full-scope pass must still let every visible point affect the
result. Under overload, old work is canceled or replaced rather than allowed to build an
unbounded queue.

## Bounded communication

The planned runtime uses:

- a newest-frame mailbox between capture and scene scheduling;
- bounded single-producer paths where ownership permits;
- fixed ticket tables for asynchronous provider work;
- immutable scenes published by handle or index;
- explicit counters for replacement, queue pressure, cancellation, and lateness.

Queue depth is a policy, not a throughput fix. Deep queues increase latency and memory.
The normal neural path keeps at most one frame executing and one newer frame pending.

## Current implementation

The foundation runtime serializes public calls with one mutex. Provider registration and
device selection are control-path work. Deterministic and scalar CPU providers each
protect their fixed state with a mutex, and contract tests cover them under
ThreadSanitizer.

Host imports now publish generation-safe frame handles through one atomic latest-only
mailbox. Replacement and consumption are linearizable, fixed-capacity, and allocation
free. Each transfer uses one bounded atomic exchange with no retry loop, and it runs on
the 30/60 Hz scene boundary rather than the 120 Hz interaction path. Control-path
cancellation is serialized outside the mailbox. Producer, consumer, and control
counters are single-writer cache-line-separated fields sampled only while quiescent,
not shared atomic accumulators. A stress test accounts for every handle as either
replaced or consumed.

Registry construction obtains domain IDs from a thread-local block. Refilling a block
uses one statically stored, mutex-protected cold source; normal construction does not
touch shared allocator or atomic state.

The production scene worker, immutable scene publication, scheduler, and action threads
are not implemented yet. The current code therefore proves the first capture-to-scene
handoff primitive, not a 120 Hz end-to-end loop.

## Kernel work

The first owned image kernel converts packed color to exact integer luma. Its scalar
implementation is the oracle. arm64 builds compile a NEON path, while x64 builds compile
an AVX2 path in a separate translation unit and select it only on a compatible CPU.
Tests force every available path, compare exact output, exercise vector tails, and check
that row padding is untouched.

Later CPU and GPU kernels use the same rule: optimize behind a stable tensor and output
contract, and compare against the scalar path before measuring speed. Metal and Direct3D
variants must match integer geometry and ordering before their speed matters.

Kernel tuning is measured in isolation and in the full scene path. A faster kernel that
adds copies, expands memory, blocks another queue, or changes target geometry is not an
improvement. Provider memory counters and per-stage timing make those tradeoffs visible.
