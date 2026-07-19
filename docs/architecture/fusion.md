# Scene fusion

Scene fusion merges evidence packets into one deduplicated, immutable scene. The
kernel lives in `src/scene/fusion.cpp` and accepts up to four already-validated
desktop-Q8 packets in explicit priority order. Today the scene coordinator passes at
most two: semantic (accessibility) first, then neural (pixel). Grid and window scenes
never pass through fusion. They are separate, mutually exclusive sources.

Priority order matters because the first packet to claim a spot wins geometry and
identity. Semantic comes first on purpose: accessibility geometry and stable ids are
exact, while pixel detections are approximate.

## Finding duplicates

Each accepted target is indexed into a spatial hash under up to five size levels:
its own power-of-two size class plus two levels above and below, clamped to
[12, 30]. A new candidate probes only its own level, in the 3x3 cell neighborhood
around its safe point (`safe_x_q8`, `safe_y_q8`).
Indexing each target at neighboring levels is what lets two differently-sized records
of the same control find each other without a full pairwise scan.

Two targets are duplicates only if all of the following hold:

- Their roles are compatible. Roles group into interactive (button, link, checkbox,
  radio, menu item), text (text, text field), slider, image, and window. Unknown
  matches anything.
- Their window and display ids are compatible. Zero matches anything.
- Geometry agrees: intersection over union reaches `iou_threshold_q16`, or the two
  areas are within `maximum_area_ratio_q8` of each other and the intersection covers
  `containment_threshold_q16` of the smaller target. The second test exists because
  an accessibility rect and a pixel detection of the same control often differ in
  size too much for a plain IoU test.

## Merging

When a candidate duplicates an existing target, the existing target keeps its
geometry and stable identity. The merge then:

- ORs the source bits and keeps the higher confidence.
- Fills role, parent, window, display, and text only where the existing target has
  none.
- Applies safety dominance: if either side is disabled or secure, the merged target
  loses `SACCADE_TARGET_ACTIONABLE` and all capability bits, and a secure merge also
  drops and redacts text. Safety wins over usefulness by design. A control that any
  evidence source considers protected must never be actionable.
- Merges capability bits asymmetrically: a semantic candidate (accessibility source
  with a known role) replaces the capability bits outright, because accessibility
  knows what a control can actually do. Two non-semantic records OR their bits. A
  non-semantic candidate never changes a semantic target's bits.

The practical consequence of priority order plus this merge rule: when semantic and
pixel evidence describe the same control, the scene keeps semantic geometry,
identity, and capabilities, and gains the pixel source bit.

The kernel writes directly into the final immutable packet and owns no allocator
path. Thread-safety and scheduling context is described in
[concurrency](concurrency.md).
