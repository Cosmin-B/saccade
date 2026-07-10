# Version 0.1 product contract

This document defines the desktop behavior required for a version 0.1 release. It is an
acceptance contract, not a statement that the current foundation already implements the
features below. The README lists what exists today.

Version 0.1 targets macOS 14 or newer on Apple Silicon and Windows 11 24H2 or newer on
x64 and arm64. A feature is complete only when both applications pass the same shared
behavior fixture and their native platform checks. Linux remains a future platform.

## Activation and scope

Saccade runs as a menu bar application on macOS and a notification-area application on
Windows. Capture and inference stop while the application is idle.

The release includes:

- configurable global hotkeys for every action and mode;
- one command that suspends or resumes all Saccade hotkeys;
- full-desktop targeting across every attached display as the default scope;
- active-window targeting;
- a command that changes scope while hints are visible;
- stable desktop coordinates across mixed scale factors and rotated displays;
- display hotplug, sleep, wake, and dock-transition handling;
- 4K and 8K operation without changing command semantics.

Activation freezes visible hint labels. New damage may update target geometry before an
action, but a label cannot jump to another target during the same session.

The signed Windows build is tested with normal and elevated application windows at every
integrity level permitted by UIAccess. The signed macOS build uses granted Accessibility
permission. Neither platform acts on a login, lock, consent, password, or other secure
screen.

## Target sources

Every targeting session offers three sources:

- Pixel finds text and controls from captured pixels across the selected scope.
- Semantic reads the operating-system accessibility tree for the foreground window.
- Grid places targets at predictable positions when detection does not cover the desired
  point.

Pixel is the default. A command can switch source while hints are visible. Fusion may
combine evidence, but the overlay presents one deduplicated scene.

Pixel settings include confidence, minimum target size, text sensitivity, merge policy,
and duplicate suppression. Semantic mode reports an inaccessible or incomplete tree
instead of pretending that no targets exist. Grid settings include rows, columns,
margins, and monitor scope.

## Hints and target adjustment

Every target has a visible region, a prefix-free keyboard hint, and a safe action point.
The initial safe point is the center of an eroded interior region, not an unverified
model coordinate.

The release supports:

- sorted or randomized hint assignment;
- priority near the pointer or the center of the active scope;
- a configurable hint alphabet and physical-key priority;
- labels from the active keyboard language or an explicitly selected language;
- four-direction edge snapping;
- nine fixed positions arranged as a three-by-three target grid;
- fine keyboard movement inside or outside target bounds;
- unrestricted pointer movement when no detected target is suitable;
- configurable label placement, font, size, weight, color, outline, and glow;
- high-contrast, light, dark, and user-defined themes.

Labels use each display's scale and color space. A label cannot cover its own safe point
or prevent a neighboring label from being typed.

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

Every action can have its own activation hotkey. Version 0.1 includes:

- pointer move and hover;
- left, right, middle, and double click;
- click and hold;
- drag and drop between two selected targets;
- vertical and horizontal scrolling in step and continuous forms;
- text-range selection between two targets;
- repeated clicks over Multi or Path selections;
- modifier-assisted clicks;
- final pointer placement at the target, original position, or a configured anchor.

Drag, hold, and continuous scroll use a physical action ledger. Abort, timeout,
permission loss, backend failure, and process shutdown release every synthetic button
and stop every continuous stream.

A model can propose a target region and safe-point candidate. Deterministic code checks
the frame, transform, focus, visibility, security, and session epochs before an input
provider receives the action plan.

## Window navigation

Window mode includes:

- directional activation based on window geometry;
- forward and backward cycling through overlapping windows;
- activation of the next window behind the foreground window;
- hint-based selection of visible windows;
- restoration and activation of minimized windows when the operating system permits it;
- exclusion of Saccade windows from candidates;
- stable ordering across displays and scale factors.

Version 0.1 activates and cycles windows on the current desktop. It does not move another
application's windows between macOS Spaces or Windows virtual desktops. It uses public
workspace and window APIs only.

## Settings

The settings application includes a visual keyboard editor. Users can assign commands,
inspect conflicts, and restore defaults without editing a configuration file. Bindings
store physical key usages plus logical-label fallbacks, so one settings document can
move between operating systems and keyboard layouts.

Settings cover:

- action, mode, navigation, suspend, confirm, and abort bindings;
- hint alphabet, language, priority, placement, and sorting;
- target-source and detector tuning;
- desktop, active-window, monitor, and mixed-DPI scope behavior;
- final pointer position and movement speed;
- theme, font, size, appearance, animation, and reduced motion;
- automatic, CPU-only, GPU-only, accelerator, and named-device compute policies;
- import and export of the complete versioned settings document;
- per-page reset and full reset.

The menu bar or notification-area menu opens settings, suspends hotkeys, reports faults,
offers a restart command, and quits cleanly.

## Diagnostics and debuggers

User-facing diagnostics report:

- capture, accessibility, and input permissions;
- displays, coordinate transforms, and active scope;
- graphics adapters and selected providers;
- model identity, precision, and fallback policy;
- host and device memory high-water marks;
- queue pressure, replaced frames, cancellation, and late results;
- the most recent bounded local trace.

Developer tools include a provider and device explorer, frame and transform inspector,
target-scene inspector, overlay debugger, input-plan dry run, deterministic replay,
memory audit, latency trace, and backend fault controls. Debug captures are explicit,
local, bounded, and excluded from normal operation.

## Privacy and updates

Normal operation has no network path. Saccade has no account, cloud inference,
telemetry, screenshot history, recognized-text history, or interaction history. A
packaged release works offline after installation.

Update checks are manual and explicit. An accepted update uses signed packages, verifies
the installed version, and supports rollback. Model and accelerator files are shipped
with the package or supplied by the operating system; activation never downloads them.

## Performance contract

Input reduction, pointer feedback, and overlay presentation target 120 Hz. The version
0.1 neural path targets a complete 30 Hz refresh of the selected full scope on qualified
hardware. Region priority is allowed for scheduling and overload recovery but does not
replace full-scope qualification.

Inference never blocks the interaction path. The scheduler keeps bounded work, replaces
superseded frames, and records missed deadlines. CPU execution is always available in a
supported release. A GPU or accelerator path ships only with measured accuracy, memory,
latency, power, startup, and fallback evidence.

Version 0.2 keeps the 120 Hz interaction path and raises qualified accelerated
full-scope inference to 60 Hz.

## Cross-platform release rule

The maintained feature evidence has one macOS result and one Windows result for every
behavior above. A version 0.1 release cannot omit a platform result, waive it, or use a
deterministic test provider as native evidence. The applications share settings,
commands, target semantics, action semantics, and replay fixtures even where their
native APIs differ.
