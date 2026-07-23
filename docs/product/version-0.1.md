# Version 0.1 product contract

Version 0.1 defines the desktop behavior shared by macOS 14 or newer on Apple Silicon and
Windows 11 24H2 or newer on x64 and arm64.

## Activation and scope

Saccade runs as a menu bar application on macOS and a notification-area application on
Windows. Capture and inference stop while the application is idle.

Global commands are reduced through one physical-key command schema on both platforms.
Physical keyboard or pointer activity neutralizes any active synthetic hold, drag, or
continuous stream before another action is considered.

The release contract includes:

- Configurable global hotkeys for every action and mode.
- One command that suspends or resumes all Saccade hotkeys.
- Full-desktop targeting across every attached display as the default scope.
- Active-window targeting.
- A command that changes scope while hints are visible.
- Stable desktop coordinates across mixed scale factors and rotated displays.
- Display hotplug, sleep, wake, and dock-transition handling.
- 4K and 8K operation without changing command semantics.

Activation freezes visible hint labels. New damage may update target geometry before an
action, but a label cannot jump to another target during the same activation. Changing
scope ends that activation and starts a new one with a newly frozen target set and label
map.

On Windows, UIAccess builds may act at integrity levels allowed by the installation.
On macOS, input requires Accessibility permission. Neither platform acts on a login, lock,
consent, password, or other secure screen.

## Target sources

Every targeting session offers four sources:

- Pixel finds text and controls from captured pixels across the selected scope.
- Semantic reads the operating-system accessibility tree for the foreground window.
- Grid places targets at predictable positions when detection does not cover the desired
  point.
- Fused correlates frame-matched Pixel and Semantic evidence into one deduplicated scene.

Pixel is the default source.

A source switch ends the current activation and starts a new one, keeping frozen labels
stable. Fused waits for matching capture, transform, and topology epochs before combining
evidence. The overlay presents one deduplicated scene.

Pixel and Fused settings include confidence, minimum target size, text sensitivity, merge
policy, and duplicate suppression. Disabling the merge policy keeps overlapping source
records separate. Semantic mode reports an inaccessible or incomplete tree and never
turns that failure into an empty successful scene. Grid settings include rows, columns,
margins, and monitor scope.

## Hints and target adjustment

Every target has a visible region, a prefix-free keyboard hint, and a safe action point.
The initial safe point is the center of an eroded interior region, not an unverified
model coordinate.

The release supports:

- Sorted or randomized hint assignment.
- Priority near the pointer or the center of the active scope.
- A configurable hint alphabet and physical-key priority.
- Labels from the active keyboard language or an explicitly selected language.
- Four-direction edge snapping.
- Nine fixed positions arranged as a three-by-three target grid.
- Fine keyboard movement inside or outside target bounds.
- Unrestricted pointer movement when no detected target is suitable.
- Configurable label placement, font, size, weight, color, outline, and glow.
- High-contrast, light, dark, and user-defined themes.

Labels use each display's scale. A label cannot cover its own safe point or prevent a
neighboring label from being typed.

## Selection modes

The shared selection reducer implements four modes:

- Single selects one target and performs or advances the active action.
- Dual collects exactly two targets.
- Multi collects a bounded set and completes on confirmation.
- Path collects two or more anchors and includes ordered targets between them.

Backspace removes the most recent selection. Escape cancels the session. Confirmation,
timeout, display changes, permission loss, and focus changes have defined transitions in
every mode.

## Pointer, scroll, and text actions

Every action can have its own activation hotkey. The action set includes:

- Pointer move and hover.
- Left, right, middle, and double click.
- Click and hold.
- Drag and drop between two selected targets.
- Vertical and horizontal scrolling in step and continuous forms.
- Text-range selection between two targets.
- Repeated clicks over Multi or Path selections.
- Modifier-assisted clicks.
- Final pointer placement at the target, original position, or a configured anchor.

Drag, hold, and continuous scroll use a physical action ledger. Abort, timeout,
permission loss, backend failure, and process shutdown release every synthetic button
and stop every continuous stream.

A model can propose a target region and safe-point candidate. Deterministic code checks
the frame, transform, focus, visibility, security, and session epochs before an input
provider receives the action plan.

## Window navigation

Window mode includes:

- Directional activation based on window geometry.
- Forward and backward cycling through overlapping windows.
- Activation of the next window behind the foreground window.
- Hint-based selection of visible windows.
- Restoration and activation of minimized windows when the operating system permits it.
- Exclusion of Saccade windows from candidates.
- Deterministic geometry ordering across displays and scale factors. Overlap cycling
  follows the operating system's current z-order.

Saccade activates and cycles windows on the current desktop through public workspace
and window APIs. Moving another application's windows between macOS Spaces or Windows
virtual desktops is outside this contract.

## Local agent control

Local agents can observe the latest immutable target generation, query its structured
targets, and submit bounded action batches through an authenticated platform channel.
The binary C11 wire ABI is shared by macOS and Windows. It exposes no platform handle or
C++ library type.

Observation, pointer, keyboard, clipboard, window, and settings capabilities are
negotiated independently. The applications grant observation, pointer, keyboard, and
window actions. Protocol version 0.1 omits images and crops.

Agent actions use the same generation, focus, transform, permission, safe-point, and
physical-input validation as interactive actions. A client disconnect, physical user
override, timeout, session loss, permission loss, secure surface, backend failure, or
application shutdown cannot leave a synthetic key or button held.

## Settings

The settings application includes a visual keyboard editor. Users can assign commands,
inspect conflicts, and restore defaults without editing a configuration file. Bindings
store physical key usages plus logical-label fallbacks, so one settings document can
move between operating systems and keyboard layouts.

Settings cover:

- Action, mode, navigation, suspend, confirm, and abort bindings.
- Hint alphabet, language, priority, placement, and sorting.
- Target-source and detector tuning.
- Desktop, active-window, monitor, and mixed-DPI scope behavior.
- Final pointer position and movement speed.
- Theme, font, size, appearance, animation, and reduced motion.
- Automatic, CPU-only, CPU-plus-GPU, CPU-plus-accelerator, and named-device compute policies.
- Import and export of the complete versioned settings document.
- Per-page reset and full reset.

The menu bar or notification-area menu opens settings, suspends hotkeys, reports faults,
offers a restart command, and quits cleanly.

## Diagnostics and debuggers

User-facing diagnostics report:

- Capture, accessibility, and input permissions.
- Displays, coordinate transforms, and active scope.
- Graphics adapters and selected providers.
- Model identity, precision, and fallback policy.
- Host and device memory high-water marks.
- Queue pressure, replaced frames, cancellation, and late results.
- Overlay slot pressure, presentation deadlines, and active instance counts.
- The most recent bounded local trace.

One bounded debugger host presents views for providers and devices, frames and transforms,
target scenes and fusion, overlays, memory, timing, and GPU counters. It also provides
input-plan dry run, deterministic replay, and backend fault controls. Debug captures are
explicit, local, bounded, and excluded from normal operation.

## Privacy and updates

Normal operation has no network path. Saccade has no account, cloud inference,
telemetry, screenshot history, recognized-text history, or interaction history. A
packaged release works offline after installation.

The release has no in-application update client. A user installs a signed replacement
package explicitly. Installation, repair, and removal remain offline-capable. Model and
accelerator files are supplied with the package or by the operating system. Activation
never downloads them.

## Performance contract

Input reduction, pointer feedback, and overlay presentation target 120 Hz. The version 0.1
neural path targets a complete 30 Hz refresh of the selected full scope on supported
hardware. Region priority may schedule work and recover from overload, while every visible
point can still affect a full-scope result.

Inference never blocks the interaction path. The scheduler keeps bounded work, replaces
superseded frames, and records missed deadlines. Providers report latency, memory, startup,
fallback, and precision through the common diagnostics contract.

The overlay accepts the same fixed target packet on every platform. Static instances are
expanded when a scene or transform epoch changes. A display tick updates only active
state and submits one bounded draw. Native backends do not read neural intermediates
back to the CPU for presentation.

## Cross-platform behavior

The macOS and Windows applications share settings, commands, target semantics, action
semantics, and replay fixtures even where their native APIs differ.
