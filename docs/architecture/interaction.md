# Hint assignment

Hint assignment gives every target in a frozen scene a short keyboard label. It runs
once, inside `HintSession::freeze` in `src/interaction/hints.cpp`, when the user
activates a session. It does not run on the scene or display cadence.

The labels must satisfy one rule: no label is a prefix of another. Without that rule
the session could not act on a short label immediately, because a longer label might
still be forming. With it, every keystroke sequence resolves to exactly one target the
moment its last symbol arrives.

## Ordering

Targets are sorted before labels are assigned, so the shortest labels go to the
targets the user is most likely to want. The reference point and seed are profile values supplied by the host at
pipeline initialization or settings refresh, not sampled at activation:

- `pointer` priority sorts by Manhattan distance from the profile's pointer
  position.
- `scope_center` priority sorts by distance from the profile's center point,
  which the host currently supplies as the desktop center.
- `randomized` priority hashes each stable target id with the profile's seed,
  so labels stay stable until the settings change.
- Otherwise targets keep their scene order.

Ties always break on the stable target id so the same scene produces the same labels.

## Label construction

Let `A` be the alphabet size and `count` the number of targets.

If `count <= A`, every target gets a single symbol and the construction ends.

Otherwise the code is built in two tiers:

1. `depth` is chosen so that `capacity = A^depth` is the largest power of the
   alphabet that does not exceed `count` (with `depth >= 1`). A full code of length
   `depth` can name `capacity` targets.
2. The shortfall `needed = count - capacity` is covered by expanding parents.
   Expanding one length-`depth` code into its `A` children trades one usable label
   for `A` longer ones, a net gain of `A - 1`. So
   `expanded_parents = ceil(needed / (A - 1))`, and when the division is not exact
   the last parent is only partially expanded with `remainder + 1` children (`+ 1`
   because the parent also gives up its own label).

Ranks below `shortest_count = capacity - expanded_parents` receive their rank encoded
as a length-`depth` label. Higher ranks receive an expanded parent's code plus one
child symbol, filling the partial parent first. The result is prefix-free because an
expanded parent's bare code is never issued, and all other labels have equal length.

The arithmetic lives in `HintSession::freeze` (tier sizing) and `assign_label`
(rank-to-label mapping). Changing the alphabet size or the maximum target count only
requires that `A >= 2` and that `HintLabel::symbols` can hold `depth + 1` symbols.

## Selection

Selection reduces the candidate set one symbol at a time with case-insensitive
comparison (`prefix_matches`). Because the code is prefix-free, a full match is
always unambiguous and can fire immediately.
