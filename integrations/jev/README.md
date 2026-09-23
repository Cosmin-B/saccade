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

## Calibrated game healing on macOS

The optional `saccade-game-healer` command reads **visible, fixed-position health
bars** from a small macOS screen capture and uses native Saccade input to hover
a frame and press a game key bound to a mouseover healing action. It does not
read game memory or private combat data. The fast healing choice is local and
threshold-based; Jev's network decision call is not in the time-critical action
path. This is a configurable healing adapter, not general game understanding.

Install the screen reader extra with
`python -m pip install './integrations/jev[game]'`. Give the terminal macOS
Screen Recording permission, and run Saccade with its native input permissions.
Use a stable game UI layout. Calibrate each bar's on-screen rectangle, a hover
point inside that rectangle, and representative filled and empty RGB colors.
Screen coordinates are macOS display coordinates; the capture reader handles a
uniform Retina backing scale. In-game healing bindings must act on the hovered frame;
the configured `keyUsage` is a USB HID keyboard usage. For example, usage 4 is
the A key and usage 5 is B. Replace every sample coordinate and color below
with values from your own UI before use:

```json
{
  "process": "ExampleGame",
  "bars": [
    {"name": "tank", "rect": [100, 200, 120, 12], "hover": [150, 206],
     "fill": [20, 180, 20], "empty": [30, 30, 30]}
  ],
  "heals": [
    {"name": "emergency", "keyUsage": 5, "below": 0.3},
    {"name": "regular", "keyUsage": 4, "below": 0.8}
  ]
}
```

Save this as a local JSON file. The default run prints the selected ally and
heal without sending input. Add `--execute` only after the readings agree with
the visible bars:

```sh
saccade-game-healer healer.json --mcp build/dev/tools/saccade-mcp
saccade-game-healer healer.json --mcp build/dev/tools/saccade-mcp --execute
```

The run lasts at most 60 seconds or 30 emitted heal inputs by default. It
requires the configured game executable in the foreground, no active user input, recent and
unambiguous captures, and the same heal decision after hover. A physical
takeover, focus change, stale capture, or failed Saccade action stops the run.
`input-sent` means the guarded key action completed; it does not prove a spell
cast, healing effect, cooldown readiness, or encounter success. Fullscreen
capture and input behavior depend on the game and macOS permissions. This
adapter has synthetic tests but has not been calibrated or exercised against a
live game session.

### Local voice commands

Install `python -m pip install './integrations/jev[game,voice]'` to use
[Parakeet Redux](https://moondream.ai/blog/introducing-parakeet-redux-and-ultra)
through Moondream Photon and your microphone. Add `--voice` to
the healer command. It starts paused and accepts only these exact spoken
commands: “Saccade start healing”, “Saccade resume healing”, “Saccade pause
healing”, and “Saccade stop healing”. Microphone access is required. `--voice`
does not imply `--execute`; both flags are needed to emit input. The model may
download on first use. Voice is for session control, not urgent casts: it
transcribes local four-second clips, and commands may be missed when speech
crosses a clip boundary. The microphone audio and transcripts are not sent to
TypeSafe by this feature.
