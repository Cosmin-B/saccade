# C ABI

Saccade uses C++20 internally and exposes an installed C11 interface. C is the stable
binary boundary. C++ callers use the same symbols plus small inline conveniences.

## Headers

`<saccade/saccade.h>` contains the runtime, results, frame descriptors, and typed frame
import facade. `<saccade/saccade_backend.h>` contains the five provider operation tables
and registration functions.

The headers do not include C++ standard-library, operating-system, graphics, or model
runtime headers. Both are compiled independently as C11 and C++20 in every test lane.

## Versions and structure sizes

Every extensible structure begins with:

```c
uint32_t struct_size;
uint32_t api_version;
```

Callers set `struct_size` to the bytes they provide and set `api_version` to
`SACCADE_API_VERSION`. The implementation reads only the known prefix, accepts a larger
top-level structure from a compatible API major, and requires known reserved bytes to
be zero. Embedded provider metadata and operation tables cannot claim bytes beyond the
containing versioned descriptor.

Descriptor reads use byte copies before typed access. This permits a valid descriptor
at an address that is not naturally aligned and prevents a short rejected prefix from
touching the next page.

## Handles and ownership

Public objects use 64-bit opaque handles. Runtime object handles include a generation;
when the generation space is exhausted, the slot retires instead of wrapping to an old
value. Provider and device handles also include a registry domain, so a handle from one
runtime cannot resolve in another registry.

Provider registration copies metadata and operation tables. A provider's `context`
pointer remains caller-owned and must outlive every registered operation that can use
it. Names are copied into bounded registry storage.

The scalar CPU provider borrows frame bytes registered through its private C++ fixture
API. Those bytes must remain valid until the frame is released. Native frame ownership
will be defined by the frame-lease layer before the public import calls return a handle.

## Errors

Functions return a `SaccadeResult`. A failure may also set bounded thread-local UTF-8
text returned by `saccade_last_error()`. The returned span remains valid until the next
Saccade call on the same thread. It is not a process log and must not contain captured
screen data.

C++ exceptions are translated at exported runtime entry points. Provider operation
implementations are required to keep exceptions inside their own boundary. The maintained
providers route every C callback through a common exception guard and translate an
unexpected exception to `SACCADE_ERROR_BACKEND`.

## Typed frame imports

C11 callers can write:

```c
SaccadeHostFrameDesc frame = {0};
SaccadeFrameHandle handle = 0;
SaccadeResult result = saccade_frame_import(runtime, &frame, &handle);
```

The `_Generic` expression resolves the explicit host, IOSurface, or D3D11 C symbol at
compile time. C++20 receives overloads generated from the same type map. Unsupported
descriptor types fail during compilation rather than falling through a runtime default.

The current import symbols validate descriptors but return
`SACCADE_ERROR_UNSUPPORTED`. This keeps the ABI callable while frame leases and provider
routing are implemented.

## Manifests

`abi/symbols-v1.txt` is the export allowlist. Shared-library tests inspect the binary
and require every listed C symbol while rejecting visible private C++ symbols.

`abi/layout-v1.def` records the 64-bit size and field-offset contract for every public
fixed-width alias and structure. A strict C11 test turns each entry into a compile-time
assertion, and a source inventory check rejects an unlisted public type. Changing either
manifest requires an explicit ABI review and an API version decision.
