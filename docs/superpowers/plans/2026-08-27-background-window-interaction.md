# Background Window Interaction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans task-by-task. All native implementation workers must load `native-systems-c-cpp` before project actions.

**Goal:** Add safe explicit-window observation and semantic background actions on macOS, with an explicit activation-gated fallback for coordinate input.

**Architecture:** A heap-retained ScreenCaptureKit session state removes late-callback lifetime hazards. One pinned explicit-window session publishes coherent window-owned scenes, while a pure backend policy selects background AXPress, activation-required refusal, or explicit activation followed by fresh target re-resolution.

**Tech Stack:** C++20, Objective-C++ ARC, ScreenCaptureKit, macOS Accessibility, CoreGraphics, AppKit, C11 agent wire protocol, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-08-27-background-window-interaction-design.md`

## Global Constraints

- Preserve the stable bundle ID, Apple Development signing identity, hardened runtime, and minimal entitlements.
- Do not add Apple Events entitlement or use private APIs, injection, event rewriting, or WindowServer hacks.
- Keep target paths bounded and allocation-free; heap retention is restricted to cold ScreenCaptureKit/AX session lifetimes.
- Never silently activate a window or reuse coordinates across activation.
- Never log, persist, or transmit screenshots, AX text, raw input, pointer coordinates, or pointer trajectories.
- Preserve unrelated working-tree changes and the existing Privacy & Input Safety behavior.

---

### Task 1: Capture callback lifetime and retirement

**Files:**
- Create: `platform/macos/screen_capture_lifecycle.hpp`
- Create: `platform/macos/screen_capture_lifecycle.mm`
- Create: `tests/platform/macos/screen_capture_lifecycle.mm`
- Modify: `platform/macos/screen_capture.mm`
- Modify: `platform/macos/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces a session-local callback state with immutable `{slot, generation, source_id}`, operation tokens, callback admission/borrowing, start/stop completion, queue drain, lease accounting, retirement, and final-release predicates.
- ScreenCaptureKit output and completion blocks retain this state; no callback retains or dereferences provider `Impl`.

- [ ] Write deterministic tests for late start completion after cancellation, late callbacks after A->B slot switching, stop timeout, shutdown, repeated sessions, duplicate tokens, window disappearance, held-frame destroy refusal, and final release.
- [x] Run the standalone/focused test and confirm failure because the lifecycle interface is absent.
- [x] Implement the minimal state machine and ARC retention boundary.
- [x] Close callback admission, detach inline context before destruction, make start/stop timeout retire the session, and keep retiring slots unavailable.
- [x] Run the focused lifecycle and explicit-capture tests. Broader sanitizer work is deferred unless a later change specifically reaches those mechanisms.

### Task 2: Explicit-window identity and scene selection

**Files:**
- Create: `platform/macos/explicit_window_session.hpp`
- Create: `platform/macos/explicit_window_session.cpp`
- Create: `tests/platform/macos/explicit_window_session.cpp`
- Modify: `platform/macos/scene_capture.hpp`
- Modify: `platform/macos/scene_capture.cpp`
- Modify: `platform/macos/desktop_pipeline.hpp`
- Modify: `platform/macos/desktop_pipeline.mm`
- Modify: `src/interaction/interaction_state.hpp`
- Modify: `include/saccade/saccade_scene.h`

**Interfaces:**
- Consumes the lifecycle-safe screen-capture provider.
- Produces one pinned explicit-window session and a coherent `{PacketView, InteractionState}` snapshot carrying selected PID/window/session identity.

- [ ] Write fake-backend tests for exact selection, background visibility, disappearance, PID/window reuse, bounds change, rapid A->B->A, permission loss, disconnect, and shutdown.
- [x] Confirm failure before adding the session implementation.
- [x] Implement exact public identity cross-checking and one-window capture selection.
- [x] Route explicit capture through the existing inference/scene path, stamp target ownership, and reject warming or mismatched scenes.
- [x] Confirm foreground and explicit session epochs cannot collide or leak targets across retirement.

### Task 3: Background action policy and semantic locator

**Files:**
- Create: `src/agent/background_action_policy.hpp`
- Create: `src/agent/background_action_policy.cpp`
- Create: `tests/agent/background_action_policy.cpp`
- Modify: `platform/macos/accessibility_provider.hpp`
- Modify: `platform/macos/accessibility_provider.cpp`
- Modify: `tests/platform/macos/accessibility_provider.cpp`
- Modify: `src/interaction/action_planner.cpp`

**Interfaces:**
- Consumes explicit-session identity, current target record, request action/flags, and current foreground identity.
- Produces `foreground_input`, `background_ax`, `activation_required`, `activation_input`, or `unsupported` plus result flags.
- Accessibility provider produces one bounded generation-scoped locator table and one polled AX action.

- [ ] Write policy tests for semantic AX success, visual activation-required, explicit activation selection, unsupported modifiers/actions, stale identity, and no-wrong-window behavior.
- [x] Confirm failure before implementing policy.
- [x] Add bounded AX locator retention and action-time PID/window/generation/action validation.
- [x] Perform AXPress on the AX worker; treat `kAXErrorCannotComplete` as outcome-indeterminate without retry.
- [ ] Add controlled AppKit tests that prove AXPress does not call activation or CGEvent.

### Task 4: Agent protocol, async response, and activation fallback

**Files:**
- Modify: `include/saccade/saccade_agent.h`
- Modify: `src/agent/service.hpp`
- Modify: `src/agent/service.cpp`
- Modify: `tests/agent/service.cpp`
- Modify: `platform/macos/agent_socket.hpp`
- Modify: `platform/macos/agent_socket.mm`
- Modify: `tests/platform/macos/agent_socket.mm`
- Modify: `src/input/execution_preflight.cpp`
- Modify: `tests/input/execution_preflight.cpp`
- Modify: `tools/agent_client.cpp`
- Modify: `tools/agent_cli.cpp`
- Modify: `tools/mcp_server.cpp`

**Interfaces:**
- Adds exact WINDOW scope, target disposition flags, ALLOW_ACTIVATION, typed activation-required/outcome-indeterminate results, and backend result flags without changing wire struct sizes.
- Socket `processing` state re-polls one retained authenticated request while the desktop loop advances.

- [ ] Write agent-service tests for no foreground fallback, strict explicit preconditions, background AX, activation-required, dry-run would-activate, and stale generation.
- [ ] Write socket tests for pending request progress/disconnect cancellation and verify RED.
- [x] Bump the agent API minor and implement scope-aware coherent scene acquisition.
- [x] Implement bounded pending reads, one-shot action polling, and exact activation -> fresh scene -> re-resolve -> CGEvent sequence.
- [x] Extend CLI/MCP schemas and JSON output with WINDOW scope, activation policy, disposition, and backend reporting.

### Task 5: Integration, privacy, ABI, and signed artifact

**Files:**
- Modify only canonical ABI manifest/documentation files required by semantic header changes.
- Inspect all feature diffs for accidental logging or entitlement changes.

- [ ] Run only the focused tests and build target affected by the final integration changes.
- [ ] Run ABI layout/symbol/header/installed-consumer validation and semantically inspect generated manifest changes.
- [ ] Run sanitizer or harmless live checks only when a later change specifically reaches those mechanisms.
- [ ] Search feature paths for screen/AX/input logging, persistence, telemetry, and Apple Events entitlement leakage.
- [ ] Build with `SACCADE_MACOS_CODESIGN_IDENTITY`, hardened runtime, and the established release workflow.
- [ ] Verify nested and outer signatures, designated requirement, team/bundle identity, and entitlements.
- [ ] Preserve the existing Applications backup, replace `/Applications/Saccade.app` with the whole verified bundle, relaunch it, and verify process/signing path.
