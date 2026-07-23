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
  -> shared ActionPlanner validation
  -> native input executor
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
window, a stable display, the desktop, or a desktop-Q8 rectangle. They can select pixel,
semantic, grid, or fused sources. Returned targets include a compact observation index,
stable IDs, role, capabilities, flags, source bits, confidence, ordering, bounds, safe
point, and available text. An unspecified active-window request resolves to the current
native window identity and its desktop-Q8 bounds. Every successful observation also
returns the process, window, and display identities plus scene, frame, capture-time,
transform, topology, and permission epochs used to construct it. Action calls expose
dry-run and matching generation, process, window, display, transform, permission, and
physical-state preconditions.

At the MCP boundary, `processId` is the current foreground process and `windowId` is its
current native window. Keeping both lets an action reject a same-application window switch
that process focus alone cannot detect.

## Observation fields

The C API's `capture_time_ns` is the monotonic timestamp associated with the captured frame
used to build the scene. For a multi-display scene it is the earliest timestamp among the
frames included in that scene. It is not the scene publication time, action time, or a
freshness deadline. Zero means that the capture timestamp was unavailable. The agent copies
this value into `SaccadeAgentGeneration.capture_time_ns`. `scene_epoch` identifies the
immutable publication. `frame_id` identifies the capture used to build it.

`SaccadeAgentScopeKind` is the exact set `ACTIVE_WINDOW`, `DISPLAY`, `DESKTOP`, and `RECT`.
An active-window scope with `stable_id == 0` resolves to the current native window and its
desktop-Q8 bounds. A display scope matches `stable_id`. A rectangle scope intersects target
bounds. Desktop scope includes the desktop scene. `source_mode` accepts `PIXEL`, `SEMANTIC`,
`GRID`, and `FUSED`. Pixel matches neural or pixel source bits, semantic matches accessibility
source bits, grid matches grid source bits, and fused accepts the published scene records.

`SaccadeAgentFreshnessPolicy` has the exact values `LATEST_VALID`, `AFTER_GENERATION`, and
`FORCE_REFRESH`. The current service supports the first two with zero freshness flags.
`FORCE_REFRESH`, `REQUIRE_DAMAGE_CHECK`, and `REQUIRE_NEURAL_REFRESH` are rejected as
unsupported. `AFTER_GENERATION` returns a generation strictly greater than
`after_generation`, or a typed timeout when the deadline expires.

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
owner-thread service evaluates that predicate once and returns a typed timeout while the
generation is unchanged. A fixed-storage client can resubmit the read-only request across
later owner ticks until its monotonic deadline. Observe and query completions retain the
scene's incomplete-source flag, so an agent can distinguish a complete target set from a
bounded or interrupted semantic traversal.
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

An action batch may request next-generation verification. Input is dispatched exactly once.
The service first checks for a generation published synchronously by the action. If
none exists yet, the completion records that every action finished and that verification
is pending. The client then sends bounded read-only observations until a later generation
arrives or the original action deadline expires. It patches that generation into the
original completion without replaying the action. Verification requires the connection to
have explicitly negotiated observation in addition to the action capability. A transport
loss during this phase fails verification and never retries input.

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
