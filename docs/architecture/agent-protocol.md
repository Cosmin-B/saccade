# Local agent protocol

Saccade exposes immutable target generations and validated desktop actions to local
clients. The channel is offline and carries structured targets, queries, action batches,
and completions. Version 0.1 does not carry screenshots, crops, feature maps, recognized
text history, or interaction history.

The public wire ABI is declared in `saccade/saccade_agent.h`. Every top-level message has
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

Each channel receives caller-owned static storage with one 1 MiB request buffer and one
1 MiB response buffer. The service owns one fixed action-plan workspace. Processing does
not allocate, lock, enter a CAS retry loop, or retain completed messages. Memory therefore
depends on the configured message and scene capacities, not resolution history, request
count, or elapsed time.

The CLI writes the binary completion directly to standard output by default. `--json` is
an explicit human-readable view rendered incrementally through a stack string builder;
the CLI never allocates a full JSON document.

`saccade-mcp` is a thin stdio adapter over the same binary client. It implements MCP
initialization, ping, tool discovery, and three tools: `saccade_observe`, `saccade_query`,
and `saccade_act`. Each input line is one bounded JSON-RPC message. Parsing uses fixed token
storage, responses stream directly to standard output, and the adapter connects to the
native service only when a tool is called. MCP does not create a second action or security
path; capability negotiation and every generation, permission, and physical-state check
remain in the binary service.

MCP represents every 64-bit generation, target, window, display, process, permission, and
physical-sequence value as a decimal JSON string. Inputs also accept exactly representable
JSON integers for small values. This avoids the silent rounding that JavaScript JSON
decoders apply beyond 53 bits. Observe and query can restrict results to the active window,
a stable display, the desktop, or a desktop-Q8 rectangle and can select pixel, semantic,
grid, or fused sources. Returned targets include a compact observation index, stable IDs,
role, capabilities, flags, source bits, confidence, ordering, bounds, safe point, and
available text. An unspecified active-window request resolves to the current native window
identity and its desktop-Q8 bounds. Every successful observation also returns the exact
process, window, and display identities plus scene, damage, transform, topology, and
permission epochs used to construct it. Action calls expose dry-run and matching
generation, process, window, display, transform, permission, and physical-state
preconditions.

At the MCP boundary, `processId` is the current foreground process and `windowId` is its
current native window. Keeping both lets an action reject a same-application window switch
instead of treating process focus alone as sufficient.

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
stable ID, role, source, capability, geometry, confidence, text, and implemented relations. Both requests may wait
for a generation strictly newer than a supplied generation. The owner-thread service evaluates that predicate once
and returns a typed timeout while the generation is unchanged. The fixed-storage client resubmits only the read-only
request across later owner ticks until its monotonic deadline. This keeps scene publication live instead of blocking
the thread that must publish the requested generation. Observe and query completions retain the scene's
incomplete-source flag so an agent can distinguish a complete target set from a bounded or interrupted semantic
traversal.
Target text lives in the scene packet's bounded trailing UTF-8 lane. Observe and query
completions append only text belonging to returned targets and expose absolute byte offsets
from each target record. Exact, prefix, and substring matching are case-sensitive UTF-8
byte operations; locale state and normalization allocation never enter the service path.

Action batches record generation, focus, transform, permission, and physical-state
preconditions. Every immediate input action is converted to the same `ActionRequest` and
`ActionPlan` used by the interactive application. The service reacquires state between
actions, allowing a hold followed by release while rejecting intervening scene, focus, or
permission changes. Abort, window cycle, and physical-state query use explicit owner-thread
callbacks and do not fabricate input plans.

An action batch may request next-generation verification. Input is dispatched exactly
once. The service first checks for a generation published synchronously by the action; if
none exists yet, the completion records that every action finished and that verification
is pending. The client then sends bounded read-only observations until a later generation
arrives or the original action deadline expires. It patches that generation into the
original completion without replaying the action. Verification therefore requires the
connection to have explicitly negotiated observation in addition to the action
capability. A transport loss during this phase fails verification and never retries input.

The v0.1 action set is pointer move and hover, click, semantic invoke, hold and release,
drag and drop, vertical or horizontal scroll, physical key and modifier chord, text entry,
text-range selection, window activation and cycling, input abort, and physical-state query.
Pointer actions use the target's validated safe point by default. A client may supply an
in-target point pair; the planner rejects coordinates outside the selected target rather
than turning that option into unrestricted desktop input.

Version 0.1 rejects nonzero-duration agent actions. Native executors retain timed plans
until completion, so asynchronous timed actions require separately owned operation storage,
pollable completions, and cancellation rather than reuse of the immediate-action workspace.

Physical input, client disconnect, session loss, secure input, timeout, backend failure,
and shutdown retain the existing guaranteed-release paths.

## Platform endpoints

Windows names the endpoint `Saccade.Agent.v1.<session-id>` under the local named-pipe
namespace. It accepts one client, uses message framing provided by the pipe, and performs
overlapped reads and writes without blocking the 120 Hz owner.

macOS places `saccade-agent-v1.sock` in the Darwin per-user temporary directory. Frames use
a four-byte size followed by one protocol message. Partial nonblocking reads and writes
advance incrementally on owner ticks.
