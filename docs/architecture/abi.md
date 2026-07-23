# C ABI

Saccade uses C++20 internally and exposes an installed C11 interface. C is the binary
boundary. C++ callers use the same symbols plus small inline conveniences. Layouts may
still change before the version 0.1 contract is frozen.

## Headers

| Header | Contents |
| --- | --- |
| `<saccade/saccade.h>` | Runtime, results, frame descriptors, and typed frame imports |
| `<saccade/saccade_backend.h>` | Five provider operation tables and registration functions |
| `<saccade/saccade_scene.h>` | Immutable target-scene packet and target record layouts |
| `<saccade/saccade_input.h>` | Validated input-plan headers, commands, and execution status |
| `<saccade/saccade_overlay.h>` | Target, style, and expanded-instance packet layouts |
| `<saccade/saccade_agent.h>` | Local observation, query, action, and completion wire records |

The headers do not include C++ standard-library, operating-system, graphics, or model
runtime headers. Each is compiled independently as C11 and C++20 in every test lane.

## Versions and structure sizes

Every extensible structure begins with:

```c
uint32_t struct_size;
uint32_t api_version;
```

Callers set `struct_size` to the bytes they provide and set `api_version` to
`SACCADE_API_VERSION`. The implementation reads only the known prefix, accepts a larger
top-level structure from a compatible API major, and requires known reserved bytes to
be zero. Embedded provider metadata and operation tables must stay within the containing
versioned descriptor.

Descriptor reads use byte copies before typed access. This permits a valid descriptor
at an address that is not naturally aligned and prevents a short rejected prefix from
touching the next page.

## Handles and ownership

Public objects use 64-bit opaque handles. Runtime object handles include a generation.
When the generation space is exhausted, the slot retires before it can wrap to an old value.
Provider and device handles also include a registry domain, so a handle from one
runtime cannot resolve in another registry.

Frame handles encode a runtime domain, a 32-bit lease generation, and a bounded slot.
Passing a frame handle to another runtime is rejected as stale even when both runtimes
currently use the same local slot.

Provider registration copies metadata and operation tables. A provider's `context`
pointer remains caller-owned and must outlive every registered operation that can use
it. Names are copied into bounded registry storage.

Host imports borrow their byte span without copying it. Their ownership rules are direct:

- Bytes remain valid until `saccade_frame_release` returns.
- Releasing a pending frame removes it from the newest-frame mailbox first.
- Destroying the parent runtime releases every outstanding frame handle.

Native surface imports retain the platform object for the complete frame-lease lifetime.
The Win32 capture import takes one COM reference to the borrowed texture. IOSurface import
looks up and retains the named surface. A platform import returns
`SACCADE_ERROR_UNSUPPORTED` when called on another operating system.

## Errors

Functions return a `SaccadeResult`. A failure may also set bounded thread-local UTF-8
text returned by `saccade_last_error()`. The returned span remains valid until the next
Saccade call on the same thread. It is not a process log and must not contain captured
screen data.

C++ exceptions are translated at exported runtime entry points. Provider operation
implementations keep exceptions inside their own boundary. The maintained providers route
every C callback through a common exception guard and translate an unexpected exception to
`SACCADE_ERROR_BACKEND`. Permission denial remains distinct as
`SACCADE_ERROR_PERMISSION`, never an empty successful result.

## Typed frame imports

C11 callers can write:

```c
SaccadeHostFrameDesc frame = {0};
SaccadeFrameHandle handle = 0;
SaccadeResult result = saccade_frame_import(runtime, &frame, &handle);
```

The `_Generic` expression resolves the explicit host, IOSurface, or Win32 capture C
symbol at compile time. C++20 receives overloads generated from the same type map.
Unsupported descriptor types fail during compilation. There is no runtime default.

Host import validates the byte span, creates a generation-safe lease in fixed storage,
and publishes the handle to a latest-only mailbox. A newer host import replaces the
mailbox's ownership of the older pending frame without invalidating the older caller
handle. The caller releases each successful import with `saccade_frame_release`.

## Overlay packet

The overlay packet is an in-process, native-endian byte block with explicit offsets and
fixed record strides. It contains no pointers or platform handles. Version 1 uses a
64-byte header, 48-byte targets, 64-byte styles, 8-byte rectangles, and 4-byte metadata.
The 88-byte `SaccadeOverlayFrameDesc` references the packet and carries the display-rate
active target index separately. Its active flag is opt-in, so a zero-initialized descriptor
does not select target zero.

Packet parsing copies records from bytes before typed access. The validator checks every
offset, count, stride, reserved field, style reference, rectangle, and glyph count before
publishing a view. Version 1 also requires the canonical exact packet size and rejects
gaps or trailing bytes. The [overlay architecture](overlay.md) defines expansion and GPU
use.

## Manifests

`abi/symbols-v1.txt` is the export allowlist. Shared-library tests inspect the binary
and require every listed C symbol while rejecting visible private C++ symbols.

`abi/layout-v1.def` records the 64-bit size and field-offset contract for every public
fixed-width alias and structure. A strict C11 test turns each entry into a compile-time
assertion, and a source inventory check rejects an unlisted public type. Changing either
manifest requires an explicit ABI review and an API version decision.
