# Concurrency and scheduler deadlines

Saccade has an interaction path and a scene path. Their deadlines and overload behavior
are different, so they cannot share one serial loop.

## Interaction path

The interaction path owns keyboard reduction, hint filtering, pointer feedback, action
validation, and overlay publication. Its scheduler deadline is independent from the scene
path, and the path should normally be event-driven.

The interaction path reads an immutable target scene. It does not wait for capture,
accessibility, neural inference, model compilation, or a GPU fence.

Mutable hot state belongs to one thread. Capture staging, scene construction, input
reduction, and presentation scratch storage use thread-local or thread-affine fixed
buffers. Cross-thread state is reduced to bounded ownership transfers and immutable
publication. The design does not use CAS retry loops on a steady-state path.

## Scene path

The scene path has a separate neural-refresh deadline. Capture, preprocessing, inference,
postprocessing, and scene publication consume that budget when a neural refresh is
scheduled.

Inference is asynchronous: submission returns a ticket before the result is ready. A
worker or native runtime completes the ticket while input and presentation continue.
Polling, waiting, cancellation, and collection are explicit provider operations.

Region-bounded work means the scheduler can mark changed or high-value rectangles as
priority input and cap temporary work to known storage. It does not change the product
full-scope rule. A full-scope pass must still let every visible point affect the
result. Under overload, old work is canceled or replaced before a queue can build behind
the current scene.

## Bounded communication

The runtime design uses:

- A newest-frame mailbox between capture and scene scheduling.
- Bounded single-producer paths where ownership permits.
- Fixed ticket tables for asynchronous provider work.
- Immutable scenes published by handle or index.
- Explicit counters for replacement, queue pressure, cancellation, and lateness.

Queue depth is a policy, not a throughput fix. Deep queues increase latency and memory.
The normal neural path keeps at most one frame executing and one newer frame pending.

## Current implementation

Runtime creation, destruction, provider registration, and the rare global registry-domain
refill pass through one CAS-and-wait gate. This gate is never entered by frame publication,
frame release, capture callbacks, provider tickets, inference kernels, input, or overlay
presentation. Their scheduler thread owns each provider context, so the context needs no
internal serialization.

Host imports now publish generation-safe frame handles through one atomic latest-only
mailbox. Replacement and consumption are linearizable, fixed-capacity, and allocation
free. Each transfer uses one bounded atomic exchange with no retry loop, and it runs on
the scene boundary, outside the interaction path. Control-path
cancellation is serialized outside the mailbox. Producer, consumer, and control
counters are single-writer cache-line-separated fields sampled only while quiescent,
not shared atomic accumulators. A stress test accounts for every handle as either
replaced or consumed.

Registry construction obtains domain IDs from a thread-local block. Refilling a block
uses the same statically stored cold gate. Normal construction does not touch shared
allocator or atomic state. Repository checks permit CAS only in that gate and reject
mutex types everywhere.

The Windows desktop owner and DirectML worker each join the MMCSS `Games` task at high
relative priority. The owner also ensures that the process priority is at least
`ABOVE_NORMAL_PRIORITY_CLASS`. Each thread registers and reverts its own MMCSS handle
during cold lifetime transitions. The qualifier reads both registrations and rejects a
run when either is absent. The 120 Hz loop and DirectML command loop perform no scheduling
call, allocation, lock, compare-and-swap, or reference-count update.

Destructive-interference spacing is an architecture constant. Apple arm64 uses 128 bytes,
while the x86_64 path uses 64. Ordinary 64-byte buffer alignment serves a different
purpose and provides no destructive-interference spacing.

The portable dual-rate scheduler is implemented as thread-owned state. It emits at most
one interaction tick after a delayed wake, skips stale catch-up ticks, and tracks the two
deadlines independently. The scene side admits exactly one running item and one
newest pending timestamp. Another deadline replaces pending work without a queue. Scene
completion immediately promotes pending work.

The macOS scene capture set is owned by its scheduler thread. It reconciles fixed-capacity
display streams against topology epochs, acquires frames without waiting, and refuses to
tear down a stream while its frame lease is outstanding. Capture callbacks continue to
replace pending framework frames independently.

Immutable scene publication, neural ticket coordination, target fusion, hint reduction,
action-plan construction, physical input reduction, and the shared session owner are
implemented with fixed storage. The session owner keeps one immutable scene active while
labels are frozen, reduces prefix and selection events, validates current epochs, and
invokes a direct native-executor callback. The interaction-thread scene coordinator polls
one native accessibility ticket without waiting, consumes the newest neural scene,
publishes newer semantic-only snapshots between neural frames, fuses exact epoch matches,
and rejects late semantic results. It writes the fusion result directly into a scene-store
slot and allocates nothing after initialization. Native application event loops poll the
interaction path, platform capture, input leases, overlay presentation, and local agent
channels without waiting for neural completion.

Scene fusion consumes up to four already-validated desktop-Q8 packets in explicit
priority order. Accepted targets are indexed into five size-adaptive spatial levels.
Each new candidate probes only its own level and neighboring cells. A duplicate keeps
the higher-priority geometry and identity while merging provenance and capabilities.
Disabled or secure evidence dominates the merge and clears all action capabilities.
The kernel writes directly into the final immutable packet and owns no allocator path.

Native accessibility traversal remains outside the interaction owner. Windows uses one
MTA thread and Win32 events. macOS uses one pthread and Mach semaphores. Both expose one
in-flight query and one retained snapshot, publish completion with release/acquire state,
and perform framework traversal only on their worker. Coordinator polling never enters UI
Automation or Accessibility and never waits.

macOS action admission also owns a public-API secure-surface preflight. It combines the
console/login session state, frontmost process, focused Accessibility role and optional
subrole, protected-content state, ancestor window classification, and secure event input.
An activation and the final native input boundary take a fresh sample. While an action,
session, or synthetic lease is active, the owner refreshes the sample at the 250 ms
permission cadence. The ordinary idle path only reads the cached disposition.
Any missing, malformed, mismatched, untrusted, bounded-traversal, secure-text, system
dialog, or system-floating-window evidence blocks the action and follows the existing
release-all path. Diagnostics expose the bounded reason bits and sample epoch.

## Kernel work

The first owned image kernel converts packed color to exact integer luma. Its scalar
implementation is the oracle. Arm64 builds compile a NEON path, while x64 builds compile
an AVX2 path in a separate translation unit and select it only on a compatible CPU.
Tests force every available path, compare exact output, exercise vector tails, and check
that row padding is untouched.

Later CPU and GPU kernels use the same rule: optimize behind a stable tensor and output
contract, and compare against an exact fixture before measuring speed. The implemented
Metal preprocessor has one materialized output lane and no per-frame Saccade-owned
allocation. Metal and Direct3D variants must match integer geometry and ordering before
their speed matters.

Kernel tuning is measured in isolation and in the full scene path. A faster kernel that
adds copies, expands memory, blocks another queue, or changes target geometry is not an
improvement. Provider memory counters and per-stage timing make those tradeoffs visible.

## Overlay publication

The scene-rate overlay composer maps desktop-Q8 targets through one direct per-display
transform, clips them to the backing surface, resolves glyph indices, and places labels
with a fixed spatial hash. It emits a scene-index to surface-index table so active hint
updates do not search after per-display filtering. The maintained 10,000-target benchmark
measures this publication separately from GPU expansion and presentation.

## Physical Input Override

Native input emitters mark every synthetic event with one shared 64-bit value. Native
monitors ignore that value, so Saccade cannot trigger its own hotkeys or continuously
abort its own drag. macOS installs a listen-only `CGEventTap` after Accessibility
permission is granted. Windows owns low-level keyboard and mouse hooks on its shell
thread. Both notify a direct, allocation-free callback for physical user activity.

The application callback checks whether a synthetic action is active and invokes the
existing physical-input override or release path on the thread that owns the executor.
The monitor itself does not acquire locks, mutate an action plan, wait for inference, or
call an operating-system input injection API.

The fixed-storage interaction controller is that callback's portable ownership point.
It cancels the frozen-label session first, then asks the native executor adapter whether
its physical ledger has a lease and releases it only when needed. Hotkey actions, mode
changes, repeat action, and shell-only commands remain direct calls on the same owner
thread. `start_interaction_command` and `observe_interaction_input` fit the existing
router and monitor callback shapes without an event queue or shared action state.

The implemented scalar overlay expander validates immutable packets and produces the
same 8-byte rectangle and 4-byte metadata streams required from native compute kernels.
It is allocation-free and is the parity oracle.

The renderer contract owns three slots and requires static expansion only when a scene or
transform epoch changes. End-to-end profiling includes physical display timing in addition
to component timing. [Overlay packets and presentation](overlay.md) specifies the target
handoff.

macOS surface lifecycle and diagnostics are main-thread owned. The display-link callback
runs on the registered main run loop and records callback duration, missed presentation
deadlines, missing scenes, busy slots, failures, and the last rendered epochs. The frame
source is a direct function pointer plus context, not an allocating callable wrapper.
Initialized surfaces and surface sets are also destroyed on their owning main thread.
Debug builds assert that lifecycle contract.

The display catalog is thread owned. Its macOS collector rejects calls away from the
main thread before changing observations or topology state. The Windows collector
requires per-monitor DPI awareness and publishes physical desktop pixels. Rare display,
work-area, safe-area, scale, refresh, and rotation changes publish a new immutable epoch
without a shared atomic in the interaction loop.
