# Overlay packets and presentation

The overlay receives targets, not drawing calls. A target describes one actionable
screen region, its hint glyphs, and a style. The backend expands that compact input into
flat GPU instances. This keeps target discovery independent from Metal, Direct3D,
Vulkan, or a CPU renderer.

## Packet contract

`<saccade/saccade_overlay.h>` defines five fixed records:

| Record | Size | Purpose |
|---|---:|---|
| `SaccadeOverlayPacketHeader` | 64 bytes | Version, epochs, counts, strides, and byte offsets |
| `SaccadeOverlayTarget` | 48 bytes | Stable ID, local target and label positions, confidence, style, and sixteen glyphs |
| `SaccadeOverlayStyle` | 64 bytes | Colors and Q13.3 geometry shared by targets |
| `SaccadeOverlayRect` | 8 bytes | Q13.3 expanded rectangle stream |
| `SaccadeOverlayInstanceMeta` | 4 bytes | Packed target index, style index, and instance kind |

The packet contains no pointers. Its header stores byte offsets from the start of the
packet, so a provider can validate it once and upload it as one block. Version 1 accepts
at most 10,000 targets and 16 styles. Reserved fields and unknown flags must be zero.

`SaccadeOverlayFrameDesc` references that immutable packet and carries the dynamic active
target index. Its scene and transform epochs must match the packet header. Keeping active
selection outside the packet lets the display callback change one 32-bit value without
rewriting or racing the scene-rate target and style data. A zeroed frame has no active
target. Setting `SACCADE_OVERLAY_FRAME_HAS_ACTIVE_TARGET` makes the index meaningful.
Changing either epoch publishes a new packet snapshot. Reusing both epochs means the
packet bytes are unchanged; native backends retain the validated snapshot and do not
read replacement payload bytes at display rate.

`SACCADE_OVERLAY_STYLE_ANIMATED` is the only version 1 style flag. The settings resolver
sets it only when animation is enabled and reduced motion is disabled. Unknown style
flags remain invalid.

Version 1 has one canonical byte layout: the 64-byte header, the target array, then the
style array, with no gaps or trailing bytes. Exact layout keeps the capacity formula
enforceable and gives every provider the same upload range.

Each packet belongs to one presentation surface. Scene publication maps desktop
coordinates through the current transform epoch, clips to that surface, and writes
unsigned Q13.3 local coordinates. The format has one-eighth-pixel precision and a range
of 8191.875 pixels, which covers an 8K-wide surface. Full desktop coordinates remain in
the semantic and action scene. A surface beyond that range must be split into smaller
presentation regions or use a later packet version; coordinates never wrap or saturate.
The fixed-point mapping and display epoch rules are defined in
[coordinates and display topology](coordinates.md).

The target record also carries the resolved label origin. Collision avoidance and label
placement happen once during scene publication; the display shader does not repeat that
work.

The portable composer builds a direct label lookup from frozen hint records, maps and
clips each target through the current desktop-to-surface transform, and uses a fixed
spatial hash to reject label overlap without quadratic scans. It retains both
scene-to-overlay and overlay-to-scene index tables. A display tick can therefore resolve
the active target with one indexed load even when other displays filtered earlier scene
records. Unsupported glyphs and labels longer than the packet's sixteen-glyph capacity fail
publication explicitly rather than rendering a misleading key.

## Targets and instances

One target expands to five static instances:

```text
4 thin outline strips
1 combined label quad
```

The label fragment draws its background and up to sixteen glyphs while reading the original
target and style buffers. Glyph indices address a fixed 8 by 4 R8 atlas. CoreText on
macOS and the native Windows text rasterizer rebuild that atlas when the alphabet, font
family, or weight changes. The 128 KiB image remains GPU-resident between settings
changes and is not copied into scene packets. Fixed expansion gives each target a stable
output offset and lets a compute kernel write records without a prefix sum.

Four full-edge outline strips cost more vertices than one full rectangle, but they avoid
fragment work over the target interior. The strips overlap only at their corners and stay
nondegenerate for every accepted target. Combining the label background and glyphs removes
separate glyph instances and avoids glyph overdraw. The active target named by the frame descriptor
uses one analytic fill-and-outline instance.

```text
static instances = target count * 5
total instances  = static instances + active target present
```

Expanded geometry and metadata are separate buffers. Rectangles have an 8-byte stride
from an aligned buffer base, and metadata stays densely packed without a padded instance
structure.

| Targets | Input packet, one style | Total instances | Output bytes | Three slots |
|---:|---:|---:|---:|---:|
| 100 | 4,928 | 501 | 6,012 | 18,036 |
| 10,000 | 480,128 | 50,001 | 600,012 | 1,800,036 |

The draw count stays at one. Target count changes compute, vertex, and fragment work
inside that draw. Visible pixel coverage and overlap usually predict fragment cost
better than the raw target count.

## GPU path

The version 0.1 steady-state contract is:

```text
scene worker, 30/60 Hz
        |
        | publish validated packet and epochs
        v
target/style buffer -----------------------------------+
        |                                               |
        | compute only when either epoch changes        |
        v                                               |
static rect/meta buffers: target index * 5              |
                                                        |
display callback, up to 120 Hz                          |
        |                                               |
        | write active index, inverse size, and time    |
        v                                               |
active-instance compute                                 |
        |                                               |
        +--> indirect instance count                    |
                                                        v
                         compute-to-render barrier -> one instanced draw -> present
```

The target steady state expands static data when a new scene is published, not on every
display callback. A display callback then updates one active record and the indirect
instance count, without CPU expansion, static packet copy, or GPU readback.

Both application pipelines now compose per-display packets only when the scene, selection,
style, glyph atlas, or topology changes. A publication wakes each display for the reveal;
afterward, static displays sleep and only a display containing an animated active target
keeps ticking. macOS uses one display link per surface. Windows intentionally keeps
DirectComposition, swapchain, and window ownership on the application event thread, which
services ready displays serially without cross-thread packet exchange. Offscreen renderer
profiling measures GPU work; display-backed profiling adds drawable acquisition,
display-link cadence, and compositor behavior.
Animated styles use the same draw: the fragment shader derives a bounded 120 ms scene
reveal and active-target pulse from presentation time. Reduced motion leaves the style
bit unset, so the same shader returns constant opacity without a separate CPU path,
timer, allocation, or command stream.

Metal 4 uses an explicit dispatch-to-vertex/fragment barrier and one command allocator
per in-flight slot. Metal 3 uses encoder ordering. D3D12 uses explicit UAV-to-SRV and
indirect-argument resource transitions. The packet and output strides do not change
between the macOS and Windows backends.

The scalar implementation in `src/overlay/packet.cpp` is the geometry oracle and a
compatibility path. It validates offsets before reading, takes the active index separately,
reports required output before writing, and allocates no memory. GPU kernels must match
its rectangles and metadata in parity checks.

## Display scheduling

The macOS backend uses `CAMetalDisplayLink` on the macOS 14 floor. Its callback supplies
the exact drawable, the deadline for calling `present`, and the estimated presentation
time. Saccade commits the command buffer and calls plain `present` before the deadline;
time-targeted presentation methods are invalid for display-link drawables. The
low-latency request is one frame. Metal 4 is preferred on supported macOS 26 systems;
Metal 3 is the older system fallback.

The renderer owns three slots. A slot contains command memory, uniforms, and expanded
instances for one in-flight frame. It is reset only after its completion event retires.
The display callback never waits for a slot. If all slots are busy, it records the miss
and drops that presentation attempt.

Each display owns one borderless, nonactivating `NSPanel` and one transparent
`CAMetalLayer`. Panels ignore mouse input by default, join every Space, remain available
beside full-screen applications, and do not enter the normal window cycle. Explicit
interaction mode can temporarily enable pointer input without changing renderer
ownership. A fixed 16-slot surface set adds, updates, and removes panels from immutable
display topology epochs.

The implemented fragment path draws target strips, rounded label backgrounds, up to
sixteen linearly sampled native hint glyphs, and a rounded active fill and outline. Colors
are premultiplied and blended into a transparent BGRA8 drawable. Compute writes the
indirect draw arguments, and both Metal paths issue one draw without CPU instance
readback.

The D3D12 backend follows the same geometry and raster contract. Build-time SM6 shaders
read the public 48-byte target and 64-byte style layouts through byte-address buffers,
write the same compact rectangle and metadata streams, and produce one indirect draw.
Three persistent slots use one timeline fence for retirement; a busy display tick drops
rather than waits. Packet validation occurs once per immutable scene and transform
epoch inside the renderer. Active-only display ticks are the required integration shape;
packet and display constants are root constants. Target and style uploads remain
persistently mapped. Debug parity readback uses persistent readback buffers and is not part
of presentation.

The Windows surface owns one nonactivating topmost HWND and a three-buffer premultiplied
DirectComposition swapchain. It exposes a frame-latency waitable handle, while `present`
itself remains nonblocking and uses `DXGI_PRESENT_DO_NOT_WAIT`. Presentation is currently
owned by the application thread, not a dedicated presentation thread. Every flip-model
buffer is an `ID3D12Resource` with one fixed RTV descriptor. The window is click-through
by default and uses
`WDA_EXCLUDEFROMCAPTURE` so display capture cannot feed the overlay back into target
detection.

A fixed 16-slot Windows surface set shares one D3D12 device and queue and reconciles HWND,
swapchain, and renderer ownership against immutable display topology epochs. Existing
displays resize in place, removed displays stop before destruction, and displays added
while the set is running start immediately. Each display retains a pacing handle, but the
current owner services display presentation serially.

## Optimization boundary

The native backend is expected to optimize:

- zero-copy capture import where the operating system permits it;
- GPU preprocessing, inference, postprocessing, target expansion, and indirect args;
- static expansion only when the scene or transform epoch changes;
- one draw and bounded in-flight storage;
- clipping and coarse culling before expensive fragment work;
- explicit queue barriers and device-loss recovery;
- stage timing, slot pressure, deadline misses, and memory high-water marks.

The scalar oracle is intentionally plain. SIMD expansion, parallel CPU expansion, and a
general drawing command language would add maintenance without helping the GPU
path. Checksums belong at model/package load and explicit replay boundaries, not on every
display frame. Sorting and deduplication belong to scene publication. Hint assignment
runs once at session activation in the interaction module. Moving any of them to
120 Hz would be a design error rather than a kernel optimization problem.
