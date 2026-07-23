# Memory and resolution

Resolution, queue depth, tensor shape, and retained history are separate memory costs.
Frame rate changes bandwidth and compute pressure. It does not increase steady-state
memory unless more frames are kept in flight.

## Captured frames

For a packed image, one frame costs:

```text
frame bytes = width * height * bytes per pixel
resident capture bytes = frame bytes * queue depth
```

BGRA8 and RGBA8 use four bytes per pixel. Common full-frame costs are:

| Resolution | Pixels | One frame | Two frames | Three frames |
|---|---:|---:|---:|---:|
| 1920 x 1080 | 2,073,600 | 7.91 MiB | 15.82 MiB | 23.73 MiB |
| 2560 x 1440 | 3,686,400 | 14.06 MiB | 28.12 MiB | 42.19 MiB |
| 3840 x 2160 | 8,294,400 | 31.64 MiB | 63.28 MiB | 94.92 MiB |
| 5120 x 2880 | 14,745,600 | 56.25 MiB | 112.50 MiB | 168.75 MiB |
| 7680 x 4320 | 33,177,600 | 126.56 MiB | 253.12 MiB | 379.69 MiB |

Row padding, native-surface metadata, and driver residency add to these values.
Imported GPU surfaces count under `device_imported`. Provider-created textures count
under `device_owned`. The same bytes must not be reported in both categories.

The macOS stream requests three ScreenCaptureKit buffers because three is the framework
minimum. Saccade does not copy them into another image queue: its fixed lease slots
retain those same IOSurfaces while one frame is acquired and one newer frame may be
pending. `CVMetalTextureCache` creates a texture view over the retained surface. The
provider reports the exact `IOSurfaceGetAllocSize` sum as imported memory and keeps
`copied_bytes` at zero.

Full-scope and full-resolution are separate. A full-scope neural pass covers the complete
selected desktop but may ask the capture provider to fit it into the model manifest's
input box. Native resolution is retained for bounded refinement.

## Frame rate and bandwidth

Reading each packed frame once gives a lower bound:

```text
bytes per second = frame bytes * frames per second
```

At 4K BGRA8, that is about 0.95 GiB/s at 30 Hz and 1.85 GiB/s at 60 Hz. At 8K it is
about 3.71 GiB/s at 30 Hz and 7.42 GiB/s at 60 Hz. Copies, color conversion, cache
misses, and intermediate tensors add traffic. Zero-copy import avoids a frame copy but
does not make the surface consume zero memory or bandwidth.

The 120 Hz interaction path does not read a full image on every tick. It consumes the
latest immutable scene and small input state. Capture and neural work advance on the
independent scene clock.

## Display topology

The complete 16-display catalog occupies 1,296 bytes.

Each 80-byte display record shares a fixed epoch and count header. Hotplug, scale,
rotation, work-area, and safe-area changes replace that snapshot. Elapsed time and refresh
rate do not change its size.

## Overlay memory

One surface-local overlay target uses 48 bytes. One style uses 64 bytes and is shared by
many targets. Expanded instances use an 8-byte rectangle and 4-byte metadata entry.
Version 1 emits five static instances per target and at most one active instance:

```text
input bytes  = 64 + target count * 48 + style count * 64
output bytes = (target count * 5 + active target present) * (8 + 4)
```

At 10,000 targets with one style, the input is 480,128 bytes and one output slot is
600,012 bytes. Three output slots reserve 1,800,036 bytes. Static expansion occurs on
scene publication. Elapsed time and display rate do not increase these capacities.

The Metal renderer retains one validated maximum-size packet and one packet per slot.
With all 16 style records reserved, its requested fixed buffers are:

```text
packet buffers = (64 + 10,000 * 48 + 16 * 64) * 4
instance buffers = (10,000 * 5 + 1) * (8 + 4) * 3
small slot buffers = (parameters 16 + indirect args 16 + display constants 16) * 3
resident R8 glyph atlas = 512 * 256 = 131,072 bytes
requested renderer buffers = 3,855,604 bytes
```

Metal rounds buffer allocations to implementation granularity. The glyph atlas changes
only when font or alphabet settings change and does not scale with target count, display
rate, or process lifetime. `OverlaySurfaceMemoryStats` keeps the measured renderer value
separate from the estimated Core Animation drawable pool. D3D12 keeps the same 128 KiB
atlas in a default-heap texture and uses a transient upload only at initialization or a
settings commit.

Metal 4 command-allocator residency is framework-owned and reported separately. Metal 3
exposes no corresponding allocator counter.

The presentation estimate is:

```text
drawable pool bytes = backing width * backing height * 4 * drawable count
```

Saccade requests three BGRA8 drawables. That is 23.73 MiB at 1080p, 94.92 MiB at 4K,
168.75 MiB at 5K, and 379.69 MiB at 8K per display. Core Animation may add metadata,
alignment, compression state, or other opaque residency, so these figures are planning
bounds. Exact framework allocation requires operating-system measurement. Adding displays
adds their surface-local renderer and drawable pools. The formula itself is independent of elapsed
time and refresh rate. Driver and compositor residency still require operating-system
measurement.

## Tensor memory

A dense tensor costs:

```text
tensor bytes = width * height * channels * bytes per element
```

For a feature pyramid, sum that expression for every level and multiply by the number
of simultaneous activation sets. FP32 uses four bytes per element, FP16 uses two, and
INT8 uses one. Framework workspaces and compiled graphs are opaque when their exact
owner is unknown. Provider-owned counters do not reveal their size or allocation behavior.
Operating-system residency completes the accounting.

Full-scope inference does not require a native-resolution tensor throughout the model.
A provider may tile, downsample, or build compact features, but every visible pixel must
still be able to affect the published output.

The implemented Metal and D3D12 preprocessors each own one output lane, not one tensor per
capture slot. At 1536 x 1024, three planar channels occupy exactly 9,437,184 bytes in FP16
or 4,718,592 bytes in INT8. D3D12 sends the 96-byte parameter block as root constants and
reuses one descriptor heap, output buffer, allocator, command list, and fence. WGC exposes
its capture surface through native D3D11. The live path copies that surface once into an
on-demand shared D3D12 slot and passes a retained fence/value dependency to the inference
worker. Slots are fixed-capacity, committed only as needed, and reusable after ticket
retirement. Transfer statistics report copied and committed bytes explicitly. No capture
pixels cross into host memory. A direct-texture model can bypass the materialized tensor.
An owned first neural layer may later fuse preprocessing and remove the output lane.

## Target postprocess memory

Candidate count and final render instance count are different capacities. The neural
decoder emits normalized rows that the GPU packs into 16-byte candidates. Postprocessing
selects at most the model's target cap
and publishes 80-byte scene records. Overlay expansion can later turn those targets into
several 12-byte render instances per target.

For candidate capacity `C`, target cap `T`, 256-candidate radix blocks, and 32-bit
suppression words, requested postprocess storage is:

```text
imported candidates = C * 16
radix entries       = C * 16 * 2
local ranks         = C * 4
block histograms    = ceil(C / 256) * 16 * 4
suppression masks   = T * ceil(T / 32) * 4
suppressed bits     = ceil(T / 32) * 4
target packet       = 104 + T * 80
pass constants      = 16 * 256
counters            = 16
```

At `C = 65,536` and `T = 1,024`, the imported candidate buffer is 1 MiB and the
suppression mask is 128 KiB. At the public 10,000-target ceiling the mask
alone would be 12,520,000 bytes, which is why a model's neural selection cap is kept
separate from the renderer's instance capacity. Saccade-owned workspace capacity remains
constant with time. Framework and driver residency are measured separately.

## Scene fusion memory

Fusion writes directly into the final 10,000-record scene packet. Its fixed workspace
contains 32,768 32-bit bucket heads and five compact 8-byte index nodes per possible
output target:

```text
bucket heads       = 32,768 * 4        = 131,072 bytes
spatial nodes      = 10,000 * 5 * 8    = 400,000 bytes
fusion workspace                           531,072 bytes
target records      = 104 + 10,000 * 80 = 800,104 bytes
bounded UTF-8 lane                            16,384 bytes
maximum packet                               816,488 bytes
```

Input packets remain borrowed and are not copied into a combined candidate array.
Memory is independent of source count, frame rate, and process lifetime.

The interaction-thread coordinator retains one maximum semantic packet so a native
accessibility snapshot can be released before fusion. Its owned storage is therefore:

```text
semantic packet      816,488 bytes
fusion workspace     531,072 bytes
alignment padding         24 bytes
coordinator storage 1,347,584 bytes
```

The newest neural packet remains borrowed from its scene store. Fusion writes directly
into the output scene-store slot, so the coordinator adds no intermediate output packet
and no per-update allocation.

## Time and retained history

A newest-frame mailbox has constant memory. When a new frame arrives, it replaces the
pending frame and keeps the queue depth fixed. With one frame in inference and one pending,
steady-state frame count is two regardless of how long Saccade runs.

Recording is different:

```text
history bytes = frame bytes * frames per second * retained seconds
```

Ten seconds of uncompressed 4K BGRA8 at 30 Hz would require about 9.27 GiB. Normal
operation therefore keeps no screenshot history. Test recordings and profiler captures
belong outside the product source tree and are loaded explicitly.

## Accounting contract

The provider accounting contract requires:

- Committed and reserved host bytes.
- Imported and provider-owned device bytes.
- Opaque framework residency.
- Bytes copied across ownership domains.
- A high-water total.

Reports are sampled at defined lifecycle points. Diagnostics compare provider-owned
counters with operating-system measurements and record any unavoidable opaque difference.
The deterministic providers return configured counters, and the scalar CPU provider
reports its fixed storage and cumulative serialized output bytes. Current Core ML
counters cover Saccade-owned storage and the active imported surface, not Core ML model
or workspace residency. OS-level residency is required for complete accounting.
