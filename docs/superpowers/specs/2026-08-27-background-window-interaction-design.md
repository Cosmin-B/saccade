# Background Window Interaction Design

## Goal

Allow an authenticated Saccade agent client to observe one explicitly selected macOS window without foregrounding it, report whether each target supports a true background action, and execute only actions whose current semantic or activation policy proves they affect that exact window.

## Public API boundary

The agent protocol adds an exact-window scope whose `stable_id` is a current public `CGWindowID`. The service resolves and pins the window's owner PID, bounds, visibility, capture source, and a new session epoch. Completion generation fields describe that selected scene owner; foreground identity remains an internal input-policy fact. The agent API minor version advances with enum/flag additions while message layouts remain unchanged.

Targets in an explicit-window completion report one of three dispositions: background-capable, activation-required, or unsupported. An action can opt into activation with a dedicated flag. Results report whether the backend was background Accessibility, activation plus CGEvent, a dry-run that would activate, or an indeterminate Accessibility outcome. Existing active-window scope retains foreground-only semantics.

## Capture lifecycle

Each ScreenCaptureKit stream owns one cold-path heap-retained Objective-C++ session state. It carries immutable slot/session/source identity, lifecycle operation tokens, callback admission and borrowing, frame-lease accounting, stop state, and its serial callback queue. The callback context points to the stream only while admission is open; retirement closes admission, drains borrowers, and detaches that context before inline stream or frame storage can be destroyed or reused.

Lifecycle is `constructed -> start-pending -> running -> stop-pending -> draining -> retired`. Cancellation, timeout, window disappearance, permission loss, disconnect, and shutdown first close frame admission. Start and stop completions carry immutable session generation and operation tokens; stale or duplicate completions are idempotent. A late successful start after cancellation schedules exactly one stop. A retiring slot remains unavailable, bounding retained sessions to the existing 16 slots. Destruction never releases an acquired frame and cannot report success before native stop, callback-queue drain, callback quiescence, and frame-lease release are separately satisfied.

## Explicit-window scene

The macOS pipeline owns one `ExplicitWindowSession`, matching the single authenticated socket client. Selection cross-checks ScreenCaptureKit and Accessibility/CGWindow public identity. The session is never rebound after disappearance or identity change. A different selection retires the prior session before a new one becomes observable.

The existing capture, inference, and scene publication path is reused; there is no second GPU/runtime stack. Explicit mode uses a single desktop-independent window stream, stamps all visual targets with the pinned window ID, and requests Accessibility data for that same window. Foreground and explicit publications are distinct session kinds and never masquerade as one another. The scene/state acquisition callback returns a coherent snapshot; warming, stale, or mismatched sessions return a typed pending/timeout/stale result instead of falling back to the foreground scene.

Bounds, visibility, PID, window ID, capture source, transform, topology, permission epoch, and session epoch are revalidated before publication and action. A change terminally retires the v1 explicit session and requires fresh selection. This deliberately avoids following potentially stale window transforms.

## Background action backends

Accessibility target collection retains a bounded, generation-scoped semantic locator for every explicit-window target that advertises `kAXPressAction`. The locator binds the AX element to session epoch, scene generation, target ID, PID, exact containing AX window, CG window, role, identifier, geometry, enabled/hidden/secure state, and action set. Locators retire with their scene/session and retain no captured text or pixels.

At action time the provider revalidates the public window identity and locator. A single unmodified invoke or equivalent left click can use `AXUIElementPerformAction(..., kAXPressAction)` without activating the application. `kAXErrorCannotComplete` is reported as outcome-indeterminate and is never retried automatically.

A coordinate-only target returns activation-required unless the request explicitly allows activation. With permission, Saccade activates the exact requested window, waits for a fresh foreground scene for the same PID/window, re-resolves the target, and only then executes the existing validated CGEvent plan. It never activates and reuses old coordinates. Failed activation, ambiguity, disappearance, generation change, or identity mismatch emits no CGEvent.

AX work and activation waiting are asynchronous. The Accessibility worker admits one bounded action, and the authenticated socket retains one request in a processing state until the service produces a completion. Fresh-scene reads and post-action checks derive one absolute deadline on first admission, survive polling without extending it, and can be cancelled on physical input or disconnect. A completed action is never dispatched a second time while its post-action scene is pending. The desktop loop continues advancing while the request is pending.

If macOS has accepted a window-activation request and Saccade is then cancelled, times out, or loses the window identity before the foreground scene arrives, Saccade emits no CGEvent and reports the activation outcome as unconfirmed. It does not claim the window activated or replay the retained request.

## Permissions and privacy

Screen Recording authorizes selected-window capture. Accessibility authorizes semantic observation and AX actions. Input Monitoring/Post Events are required only for the deliberate activation-plus-CGEvent fallback, not for AXPress itself. Apple Events is unrelated and no entitlement is added.

The feature uses only ScreenCaptureKit, Accessibility, CoreGraphics, and AppKit public APIs. It does not use injection, event rewriting, private WindowServer APIs, process credentials, or application-specific bypasses. Diagnostics remain bounded numeric counters/reason bits and never contain screenshots, AX text, raw input, pointer trajectories, or credentials.

## Failure behavior

- Missing or ambiguous window/element identity: stale or unsupported; no action.
- Visual target without activation permission: activation-required; no activation or input.
- Accessibility permission loss: permission denied and session retirement.
- Window disappearance, PID reuse, bounds change, or session/generation mismatch: stale and session retirement.
- AX `cannot complete`: outcome-indeterminate; no retry.
- Capture start/stop timeout: slot remains retiring and callback-safe; never reused early.
- Disconnect/shutdown: close all admission, neutralize synthetic input, retire explicit capture and AX locators.

## Verification

Deterministic non-live tests cover late completions, cancellation, rapid stream switching, timeout, shutdown, repeated explicit sessions, exact-window selection, background AXPress, activation-required refusal, activation fallback with fresh re-resolution, stale/PID-reused windows, and no-wrong-window guarantees. Live tests use only Saccade-owned harmless controls. During active development, validation stays limited to the tests and signed build target directly affected by a change; broader suites and sanitizers are reserved for changes that specifically reach those mechanisms.
