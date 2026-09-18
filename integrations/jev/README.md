# Jev support for Saccade

This optional Python client connects TypeSafe Jev to Saccade's native MCP service.
Saccade observes and executes; Jev chooses one locally constructed action ID.
The client checks fresh evidence, executes once, and observes the result.

The native runtime remains offline. Running this client sends the goal, bounded
UI text and geometry, and candidate descriptions to `https://api.typesafe.ai`.
It does not send screenshots. Secure and redacted target text is excluded.
Caller-provided text payloads are included in candidate descriptions; do not use
secret payloads in this client. Credentials are read from `TYPESAFE_API_KEY`, never
printed, and removed from the MCP child environment. Output contains status,
opaque candidate IDs, confidence, and generations, without UI text.

## Run

Python 3.9 or newer is sufficient; the runtime has no third-party dependencies.
From the repository root:

```sh
cmake --preset dev
cmake --build --preset dev --target saccade_mcp

# Check the configured key and live API with synthetic data; no desktop access.
PYTHONPATH=integrations/jev python3 -m saccade_jev --check-provider

# Requires the Saccade desktop application running with its native permissions.
# Select an action and validate the native plan without emitting input.
PYTHONPATH=integrations/jev python3 -m saccade_jev \
  'Click Save' --kind click --mcp build/dev/tools/saccade-mcp

# Execute one action, then require an exact visible target text postcondition.
PYTHONPATH=integrations/jev python3 -m saccade_jev \
  'Click Save' --kind click --success-text Saved --execute \
  --mcp build/dev/tools/saccade-mcp
```

Alternatively, install `integrations/jev` in a virtual environment with
`python -m pip install ./integrations/jev`, then use `saccade-jev`.
Windows uses the same Python client with the path to `saccade-mcp.exe`.
The client is not bundled into native application installers.

`--model` defaults to `jev-latest`. `--minimum-confidence` defaults to 0.8;
this is a configurable conservative starting policy, not a calibrated guarantee.
A correct choice can fall below it. `--check-provider` verifies the API contract
and synthetic selection, independently of the desktop execution threshold.

## Actions and workflows

The default allowed set is click and semantic invoke. Repeated `--kind` flags
choose an explicit set. Candidate construction intersects these actions with
observed target capabilities and excludes disabled, occluded, secure, redacted,
and text-truncated targets.

| Kind | Caller-owned arguments |
| --- | --- |
| `click`, `invoke`, `move`, `hover`, `window` | Observed target identity |
| `text` | `--text` contains the exact text to insert |
| `scroll` | `--scroll-y-q8`; 256 Q8 units are one logical pixel |
| `key`, `key-chord` | `--key-usage` USB HID usage and `--modifiers` Saccade bits |
| `drag`, `text-select` | `--secondary-target` identifies another observed target |

Keyboard actions are bound to the active window rather than a fabricated target.
The Python `ActionSpec` also exposes horizontal scrolling. Jev never generates
text, coordinates, keystrokes, or executable arguments. The application supplies
them before inference.

`--window-id` pins an exact native window. In that scope the client offers only
supported clicks/invocations. Background Accessibility actions are preferred;
`--allow-activation` explicitly permits targets requiring foreground activation.
Actual platform support remains governed by Saccade's native service.

A local workflow is a JSON array of up to 32 stages. Each stage provides a goal,
one to sixteen action specifications, and a required exact-text postcondition:

```json
[
  {
    "goal": "Click Save",
    "actions": [{"kind": "click"}],
    "expectText": "Saved"
  },
  {
    "goal": "Close the confirmation using Done",
    "actions": [{"kind": "invoke"}],
    "expectText": "Editor"
  }
]
```

```sh
PYTHONPATH=integrations/jev python3 -m saccade_jev \
  --workflow workflow.json --execute --mcp build/dev/tools/saccade-mcp
```

Each stage dispatches at most once and advances only after its postcondition
passes. A postcondition already satisfied in the initial observation skips that
stage. A dry run stops after the first stage requiring input. Uncertainty, no
candidates, a stop decision, or a failed postcondition stops the workflow with a
nonzero exit code. There is no automatic retry of an action or unbounded goal loop.
Choose postconditions specific to the workflow; a generic label can be present
before the intended change. Python callers can provide a stronger synchronous
`verify(observation)` callback, including an independent application-state check.

## Validation boundary

- Every candidate has an opaque, per-decision ID and immutable serialized arguments.
  Unknown/stale IDs, malformed probabilities, and nonfinite confidence fail closed.
- Observations are limited to 64 targets, candidate sets to 128, provider request
  and response bodies to 64 KiB, and workflows to 64 KiB. Incomplete observations
  or exceeded budgets stop the decision instead of silently discarding evidence.
- Freshness checks compare the entire returned scene's target evidence, window,
  process, display, topology, transforms, permission state, and physical input.
  Publication/frame clocks and confidence may advance without changing the scene.
  An unchanged scene may be rebound to its fresh generation; changed evidence
  stops before dispatch. The native generation guard closes the remaining race.
- Native execution retains generation, process, window, display, transform,
  permission, and physical-sequence preconditions. Input is dispatched once.
  MCP timeout/cancellation poisons the connection. Neither transport nor controller
  retries input; an uncertain outcome must be inspected before another attempt.
- A newer generation confirms fresh observation, not task success. Without a
  caller-supplied postcondition, success is reported only as `executed`.
- Provider calls have a timeout, reject redirects, and do not retry automatically.

## Platform support

The client connects to Saccade on macOS and Windows. It uses Saccade's fused
local observations and does not add a Linux desktop backend, DOM extraction,
screenshot transport, or remote perception. Hold/release leases, window cycling,
explicit point pairs, and long-running actions are not offered as Jev candidates.

The provider follows the [TypeSafe quickstart](https://docs.typesafe.ai/introduction/quickstart)
and [HTTP API](https://docs.typesafe.ai/api).
