# Coordinate spaces and display topology

Saccade maps captured pixels, desktop interaction coordinates, and presentation
surfaces without exposing platform geometry types to the portable core. A transform is
direct and epoch tagged. The runtime does not compose a chain of floating-point matrices
in a target or input path.

## Coordinate representation

Portable coordinates use signed Q24.8 values stored in `int32_t`. One unit has 256
subunits, giving one-two-hundred-and-fifty-sixth-unit precision and a range of almost
8.4 million whole units in either direction. Negative desktop origins are ordinary.

The coordinate spaces are:

- `capture`: pixels in one acquired image or crop;
- `desktop`: the platform's unvirtualized interaction space;
- `surface`: backing pixels local to one presentation surface;
- `window`: coordinates local to one source window.

On macOS, one desktop unit is one screen point. ScreenCaptureKit describes content in
screen points and provides the scale to captured output pixels. AppKit converts screen
points to the backing pixels used by a `CAMetalLayer`. On Windows, the process is
per-monitor-v2 DPI aware and one desktop unit is one physical desktop pixel. This keeps
each platform in the same coordinates used by its native input API while preserving the
same target and action semantics.

## Direct transform

The 64-byte transform descriptor contains source and destination bounds, source and
destination space names, a transform epoch, and a 0, 90, 180, or 270 degree clockwise
rotation. Initialization validates every bound and precomputes three Q32.32 scales per
axis:

- a lower scale for conservative near rectangle edges;
- an upper scale for conservative far rectangle edges;
- a nearest scale for points.

Point mapping subtracts the source origin, applies the quarter turn, multiplies by the
nearest scale, shifts, and adds the destination origin. Source and destination endpoints
are handled exactly. A point outside the source bounds is rejected rather than silently
clamped.

Rectangle mapping first clips to the source bounds. It maps the half-open near edge down
and the far edge up, so a changed or actionable capture pixel cannot disappear through
quantization. A completely clipped rectangle reports `SACCADE_ERROR_NOT_FOUND`.
Constructing an inverse swaps the spaces and bounds and applies the inverse quarter turn.
Scaling can be many-to-one, so inverse point mapping has bounded quantization error; it
does not claim that every subunit survives downscaling.

Initialization performs the only integer divisions. Mapping uses fixed arithmetic and
does not allocate, lock, retry a compare-and-swap, or read shared state.

## Display catalog

The portable catalog owns at most 16 display records. Each 80-byte record contains:

- the platform display identifier;
- top-left desktop bounds and work bounds;
- top, left, bottom, and right safe-area insets;
- presentation backing width and height;
- maximum refresh rate and quarter-turn rotation;
- main, built-in, active, asleep, and mirrored flags.

Records are sorted by display identifier before publication. Reordering the input does
not change the epoch. Geometry, work-area, safe-area, scale, refresh, rotation, display
membership, or flag changes publish a new epoch. Invalid or over-capacity input leaves
the previous 1,296-byte snapshot unchanged. Publication and lookup allocate no memory.

Desktop-to-overlay transforms use the drawable's presented width and height and therefore
have zero additional quarter-turn rotation. AppKit has already oriented those dimensions.
Physical display rotation stays in the catalog for capture correlation; applying it again
would rotate the overlay twice.

The macOS collector and its statistics are main-thread owned. It obtains the documented
Core Graphics display identifier from `NSScreen.deviceDescription`, uses
`CGDisplayBounds` for the top-left desktop frame, translates AppKit's bottom-left visible
frame into that space, and uses `convertRectToBacking` for drawable dimensions. It does
not use
`CGDisplayPixelsWide` as a proxy for a Retina drawable. Calls from another thread fail
before touching collector state.

Capture, accessibility, scene, overlay, and input work carry the catalog epoch. Results
from an older topology are stale even if their numeric coordinates still happen to fit.
