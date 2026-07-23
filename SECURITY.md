# Security

Saccade handles screen content, accessibility metadata, local model inference, overlays,
and synthetic input. A mistake at any of those boundaries can expose private data or
trigger the wrong action. Reports about permission handling, capture exclusion, action
validation, provider loading, local agent authentication, and package integrity are
security reports.

The repository contains the portable runtime plus native macOS and Windows providers for
capture, accessibility, accelerated inference, overlay presentation, and input. ABI
validation, stale handles, provider lifetime, package exports, model admission, and
diagnostic data handling are also in scope.

Use GitHub's private vulnerability reporting for this repository. Include the
affected commit, operating system, hardware, reproduction steps, and any relevant
logs. Remove screen contents, account names, tokens, and signing material before
attaching a file.

Do not open a public issue for an unpatched vulnerability.

## Required trust boundaries

- Native capture and accessibility permissions must be requested separately.
- Native input must be emitted only from a validated action plan and cancelled when
  focus, geometry, or permission state changes.
- Provider registration is explicit. Release builds do not scan the filesystem for
  plugins or download executable components at runtime.
- Public handles carry a generation and are checked before use.
- C++ exceptions do not cross the C ABI.
- Diagnostic output must not contain captured pixels, recognized text, or raw
  accessibility values unless the user starts an explicit local debugging session.
