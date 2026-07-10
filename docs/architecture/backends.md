# Provider backends

Providers isolate native APIs and inference runtimes from the portable core. Each
family registers a context pointer, metadata, and one size-versioned operation table.
The operation tables are independent because capture, model execution, presentation,
accessibility, and synthetic input have different ownership and failure rules.

## Registration and selection

Registration occurs before runtime freeze. Each family has room for eight providers.
The compute-device registry has room for 32 devices and accepts devices only from a
registered inference provider.

A provider stable ID must be unique within its family. A device stable ID is scoped to
its provider. Selection can require and prefer capability, pixel-format, precision, and
surface-import bits. Queue capacity and maximum in-flight work are also filterable.
Ties keep registration order. Explicit provider and device IDs report a distinct
selection reason.

Selection and lookup do not allocate. Registry tests also cover name ownership,
capacity, compatible prefixes, malformed descriptors, freeze behavior, and handles
from another registry domain.

## Asynchronous work

Inference, accessibility, and input providers return generated tickets. Their tables
support non-blocking polling, bounded waiting, cancellation, synchronization, and
reset or release operations appropriate to that family.

Queue full is reported as `SACCADE_ERROR_BUSY`. Output too small is reported as
`SACCADE_ERROR_CAPACITY` together with the required byte count. A canceled ticket
reports `SACCADE_ERROR_CANCELLED`. Stale resource generations report
`SACCADE_ERROR_STALE_HANDLE`.

Input operations have no separate collect or release call. Once `poll` or `wait`
successfully writes a terminal input status, that ticket is retired and later use
returns `SACCADE_ERROR_STALE_HANDLE`. This keeps the bounded ticket table reusable.

The portable runtime does not call provider code while holding a future scheduler
queue lock. That rule matters once native providers begin completing work on framework
threads.

## Deterministic providers

`Saccade::mock` implements all five families for tests. Its configuration controls:

- the number of polls before asynchronous completion;
- queue capacity and advertised capabilities;
- capture dimensions and formats;
- memory counters returned by every family;
- one-shot faults at named operation boundaries.

The delay is a poll count, not a sleep. Tests can exercise running, completion,
cancellation, timeout, queue pressure, reset, and teardown without depending on wall
clock timing. The provider records bounded observations such as submission counts,
scene epochs, visibility, and command hashes. It does not perform vision work or emit
real input.

## Scalar CPU provider

`Saccade::reference_cpu` is the first correctness provider. It accepts small borrowed
host fixtures in BGRA8, RGBA8, BGRX8, or R8 form. Its model is a 12-byte versioned
record containing an integer luma threshold and minimum component area.

The detector performs:

1. exact integer luma conversion;
2. four-connected bright-region labeling with fixed arrays;
3. component bounding boxes and integer center points;
4. reading-order sorting;
5. deterministic IDs and Q16 confidence;
6. explicit little-endian serialization.

The detector supports fixtures up to 1024 by 1024 pixels, 128 intermediate components,
and 32 outputs. Those limits keep the oracle simple and allocation-free. It is not the
shipping neural detector and is not used to claim desktop accuracy or speed.

This provider is deliberately synchronous. Submission creates a bounded ticket. The
first poll, or a wait with a nonzero timeout, performs the scalar detector before
reporting completion. It does not advertise the asynchronous capability. Production
inference providers must run work away from the interaction path.

## Native providers

A native provider is accepted only after it passes the same descriptor, lifecycle,
queue, cancellation, stale-handle, memory, and output-parity checks. Platform defaults
are candidates, not architectural requirements. Core ML, Metal graphs, Windows ML,
DirectML, and owned kernels can all fit behind the inference contract.

Native capture and input providers must also pass platform permission, secure-screen,
focus, display-change, and teardown tests before the corresponding desktop mode is
considered supported.
