# Local agent protocol

Saccade exposes immutable target generations and validated desktop actions to local
clients. The channel is offline and carries structured targets, queries, action batches,
and completions. Version 0.1 does not carry screenshots, crops, feature maps, recognized
text history, or interaction history.

The public wire ABI is declared in
[`saccade_agent.h`](https://github.com/Cosmin-B/saccade/blob/main/include/saccade/saccade_agent.h).
Every top-level message has
an exact structure size, API version, and message kind. Variable records use bounded
offset, count, and stride fields. The maximum scene contains 10,000 compact 88-byte agent
target records and fits inside the 1 MiB message ceiling.

```text
CLI or local adapter
  -> authenticated local channel
  -> capability handshake
  -> observe, query, or bounded action batch
  -> immutable target generation
  -> background Accessibility action, or explicit activation policy
  -> shared ActionPlanner validation when coordinate input is permitted
  -> typed completion and final physical-input state
```

## Ownership and memory

The desktop owner thread polls each local channel. It never hands mutable runtime state to
a transport worker. Windows uses an overlapped named pipe secured to the interactive user.
macOS uses a mode-0600 per-user local socket, verifies the peer user and process ID, and
requires the same signing team when the application has a production signature.

Each channel receives fixed storage:

- One caller-owned 1 MiB request buffer.
- One caller-owned 1 MiB response buffer.
- One service-owned action-plan workspace.

Processing does not allocate, lock, enter a CAS retry loop, or retain completed messages.
Memory follows the configured message and scene capacities. Resolution history, request
count, and elapsed time do not change it.

The CLI writes the binary completion directly to standard output by default. `--json` is
an explicit human-readable view rendered incrementally through a stack string builder.
The CLI never allocates a full JSON document.

`saccade-mcp` is a thin stdio adapter over the same binary client. It implements MCP
initialization, ping, tool discovery, and three tools: `saccade_observe`, `saccade_query`,
and `saccade_act`. Each input line is one bounded JSON-RPC message. Parsing uses fixed token
storage, responses stream directly to standard output, and the adapter connects to the
native service only when a tool is called. MCP does not create a second action or security
path. Capability negotiation and every generation, permission, and physical-state check
remain in the binary service.

MCP represents every 64-bit generation, target, window, display, process, permission, and
physical-sequence value as a decimal JSON string. Inputs also accept exactly representable
JSON integers for small values. This avoids silent rounding in JSON decoders that use
IEEE-754 numbers beyond 53 bits. Observe and query can restrict results to the active
window, one exact public macOS window, a stable display, the desktop, or a desktop-Q8 rectangle. They can select pixel,
semantic, grid, or fused sources. Returned targets include a compact observation index,
stable IDs, role, capabilities, flags, source bits, confidence, ordering, bounds, safe
point, and available text. An unspecified active-window request resolves to the current
native window identity and its desktop-Q8 bounds. Every successful observation also
returns the process, window, and display identities plus scene, frame, capture-time,
transform, topology, and permission epochs used to construct it. Action calls expose
dry-run and matching generation, process, window, display, transform, permission, and
physical-state preconditions. An exact-window action must opt into that scope and can
separately opt into foreground activation; neither behavior is inferred.

At the MCP boundary, `processId` owns the returned scene and `windowId` is its exact native
window. In active-window observations that process is foreground. Exact-window observations
retain the selected background owner while reporting a distinct scene kind internally.

## Observation fields

The C API's `capture_time_ns` is the monotonic timestamp associated with the captured frame
used to build the scene. For a multi-display scene it is the earliest timestamp among the
frames included in that scene. It is not the scene publication time, action time, or a
freshness deadline. Zero means that the capture timestamp was unavailable. The agent copies
this value into `SaccadeAgentGeneration.capture_time_ns`. `scene_epoch` identifies the
immutable publication. `frame_id` identifies the capture used to build it.

`SaccadeAgentScopeKind` is the exact set `ACTIVE_WINDOW`, `WINDOW`, `DISPLAY`, `DESKTOP`, and `RECT`.
An active-window scope with `stable_id == 0` resolves to the current native window and its
desktop-Q8 bounds. A `WINDOW` scope requires a nonzero current public `CGWindowID`. It pins
the owner PID, window ID, bounds, visibility, capture source, and session without activating
the application. A display scope matches `stable_id`. A rectangle scope intersects target
bounds. Desktop scope includes the desktop scene. `source_mode` accepts `PIXEL`, `SEMANTIC`,
`GRID`, and `FUSED`. Pixel matches neural or pixel source bits, semantic matches accessibility
source bits, grid matches grid source bits, and fused accepts the published scene records.

`SaccadeAgentFreshnessPolicy` has the exact values `LATEST_VALID`, `AFTER_GENERATION`, and
`FORCE_REFRESH`. The current service supports the first two with zero freshness flags.
`FORCE_REFRESH`, `REQUIRE_DAMAGE_CHECK`, and `REQUIRE_NEURAL_REFRESH` are rejected as
unsupported. `AFTER_GENERATION` retains one read while asking the scene owner for a
generation strictly greater than `after_generation`. Its absolute deadline is derived once
from `timeout_ns`; later owner ticks cannot extend it. The read returns the newer generation,
a typed timeout at that deadline, or cancellation when the connection or input owner stops it.

Target records and returned text use offsets inside the completion packet. `targets_offset`
points to the first target record, `target_stride` gives the record spacing, and each target's
`text_offset` is an absolute byte offset to its UTF-8 text. Counts and offsets are bounded by
the message's `total_size`. A completion can set the incomplete-source flag when a source
packet was bounded or interrupted. A truncation flag means the requested output capacity was
too small or target text exceeded the available response lane.

## Capabilities

The first message negotiates observation, pointer, keyboard, clipboard, window, and
settings capabilities. A connection can use only the intersection of its request and the
platform policy. The desktop applications currently grant observation, pointer, keyboard,
and window actions. Clipboard and settings access remain denied.

The service checks capabilities again for every request. A valid denied, stale, timed-out,
or unsupported request receives a typed completion record. The transport rejects malformed
framing.

## Generations and actions

Observe serializes the latest valid scene directly into caller storage. Query filters by
stable ID, role, source, capability, geometry, confidence, text, and implemented relations.
Both requests may wait for a generation strictly newer than a supplied generation. The
owner-thread service keeps one bounded pending read while the local channel holds the same
request bytes. A different request is refused until the pending read finishes or is
cancelled. Observe and query completions retain the scene's incomplete-source flag, so an
agent can distinguish a complete target set from a bounded or interrupted semantic traversal.
Target text lives in the scene packet's bounded trailing UTF-8 lane. Observe and query
completions append only text belonging to returned targets and expose absolute byte offsets
from each target record. Exact, prefix, and substring matching are case-sensitive UTF-8
byte operations. Locale state and normalization allocation never enter the service path.

Action batches record generation, process, window, transform, permission, and physical-state
preconditions. Every immediate input action is converted to the same `ActionRequest` and
`ActionPlan` used by the interactive application. The service reacquires state between
actions, allowing a hold followed by release while rejecting intervening scene, process,
window, or permission changes. Abort, window cycle, and physical-state query use explicit
owner-thread callbacks and do not fabricate input plans.

On macOS, an exact-window target reports one of three dispositions. A current Accessibility
target that supports `AXPress` is background-actionable and executes on the bounded
Accessibility worker without activation or `CGEvent`. A visual-only click is
activation-required. It is refused unless the action explicitly allows activation; when
allowed, Saccade activates the exact PID/window, waits for a new foreground scene, resolves
the same target again inside that window, and only then builds a `CGEvent` plan. Other
actions are unsupported in background mode. Disabled, non-actionable, and secure targets
are refused before activation or `AXPress`. A changed PID, window, bounds, visibility,
generation, transform, permission epoch, or target identity emits no input. Accessibility
`cannot complete` is outcome-unconfirmed and is never retried automatically.

An action batch may request next-generation verification. Input is dispatched exactly once.
The service first checks for a generation published synchronously by the action. If none
exists yet, one fixed slot retains the completed action results while scene acquisition asks
for a generation newer than the action generation. Later owner ticks resume only that read;
they do not rebuild the plan or call an action backend again. The final completion includes
the newer generation or a typed timeout at the original deadline. Verification requires the
connection to have explicitly negotiated observation in addition to the action capability.
A disconnect or physical-input override cancels the pending read and never retries input.

The v0.1 action set is pointer move and hover, click, semantic invoke, hold and release,
drag and drop, vertical or horizontal scroll, physical key and modifier chord, text entry,
text-range selection, window activation and cycling, input abort, and physical-state query.
Pointer actions use the target's validated safe point by default. A client may supply an
in-target point pair. The planner rejects coordinates outside the selected target, so the
option cannot become unrestricted desktop input.

Version 0.1 rejects nonzero-duration agent actions. Native executors retain timed plans
until completion, so asynchronous timed actions require separately owned operation storage,
pollable completions, and cancellation. They cannot reuse the immediate-action workspace.

Physical input, client disconnect, session loss, secure input, timeout, backend failure,
and shutdown retain the existing guaranteed-release paths.

## Platform endpoints

Windows names the endpoint `Saccade.Agent.v1.<session-id>` under the local named-pipe
namespace. It accepts one client, uses message framing provided by the pipe, and performs
overlapped reads and writes without blocking the interaction owner.

macOS places `saccade-agent-v1.sock` in the Darwin per-user temporary directory. Frames use
a four-byte size followed by one protocol message. Partial nonblocking reads and writes
advance incrementally on owner ticks. A replacement process retries an endpoint held by
the previous process every 50 ms. Shutdown removes the pathname only when its device and
inode still match the listener that created it, so an older process cannot unlink a newer
listener. The platform test exercises this handoff across two processes and verifies that
the replacement accepts a client after the first owner exits.
