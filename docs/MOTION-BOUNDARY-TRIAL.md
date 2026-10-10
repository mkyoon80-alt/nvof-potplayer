# Residual motion repair — 0.3.2-rate.3

Local follow-up to rate.2 on 2026-10-10; evidence is under `build/rate3`.

## Changes and evidence

The filmed lap-time display around F1 22:12 contained aliases as small as
4–8 analysis pixels, below rate.2's entry threshold. Admit these candidates
and lower only the coarse screening thresholds. Final acceptance still requires
a well-conditioned six-step local fit, movement at most two analysis pixels,
mean error below three luma levels and 35% of the old error, and peak below 16.

The Jishou 4 gate had incorrect raw flow even at the 64/128-pixel donor
locations. Add a 192-analysis-pixel opposing donor pair with the same reverse
consistency, agreement, and long image-patch validation. Allow modest transverse
motion and line detail during initial screening. No scene/time/filename rules.

Compared 48 source frames around F1 22:12 and 48 around Jishou 4 0:05.
Inspected ×5 phases show reduced digit tears and removed large gate holes in
the selected pairs. Fine fringes can remain; this is not full-video validation.
The moving-car boundary weighting introduced in rate.2 is unchanged.

## Validation

- Production stroke shader: 16 cases, including short screen aliases and
  contaminated nearby donors on both axes. Genuine fast motion and ambiguous
  patterns retain their prior motion. Final CTest suite: 10/10 passed.
- Hardware layers: seven scenes at ×2/×4/×5 pass the existing quality guards.
- Separate textured-plane phase oracle: 20-pixel translation gives measured
  shifts 10 at ×2 and 4, 8, 12, 16 at ×5, within the 0.25-pixel search resolution.
  This rules out repeated midpoint generation in this fixture, not every local
  occlusion error. The synthetic rectangle's first ×5 silhouette edge is about
  3.3 pixels behind the ideal edge; later phases are closer. More phases do not
  fix flow uncertainty or recover original exposure blur.
- Eight additional 24-frame ×5 sequences: exact source outputs and unchanged
  timestamps, cut/held-pair counts. Largest-change crops reviewed for Kokoore,
  Jishou, F1 title/course, sea/black credits and moving boundaries. Existing
  Jishou moving-background artifacts remain; this change does not solve them.
- Single-run completed-pair medians in those clips were 3.3–6.0 ms for rate.3;
  old/new timing varied in both directions and p95 reached 19.9 ms. These short
  offline samples are not a throughput guarantee or evidence of zero overhead.

Additional rate.2/rate.3 comparisons checked the 48-frame car sequence and
32-frame earlier lap-time sequence at ×5, and both new target clips at ×2.
Original outputs and timelines matched exactly. Inspected car crops retained the
previous result; both numeral and gate repairs also work at ×2. Four DirectShow
MF/DXGI cases passed NV12/P010 input at 23.976→47.952/119.880, including
negotiation, timestamps, EOS and seek. Controller self-test passed.

The user confirmed reopening the video, output changing to about 120fps and
renderer playback near 120fps with almost no dropped frames. Therefore a stale
48fps setting or broad frame loss is not the leading explanation for a small
perceptual difference. Motion size, held animation, source blur and local
protection remain relevant. The inverse-coordinate solver does not make ×2
mathematically equivalent to ×5.

10bit/HDR feasibility is a separate investigation: this trial still outputs NV12.

---

# Motion boundary trial — 0.3.2-rate.2

## Scope

Local trial after `0.3.2-rate.1`. Three reported examples were reproduced from
decoded source frames: Jishou episode 4 around 00:05 (left gate), F1 around
23:23 (small racing car against the road), and F1 around 22:14 (lap-time digits).
The artifacts also occur at ×2; they are not caused solely by ×5 output.

## Changes

- Repeated straight strokes: accept an alternative vector only when donors on
  opposite ends agree, reverse motion supports them, and a longer image patch
  clearly matches better. Fast genuine translation and indistinguishable bars
  retain the original estimate.
- Moving foreground boundaries: reduce the weight of a warped sample when its
  local area expansion is excessive **and** its image match is poor. A reliable
  match, unit-area translation, and moderate expansion retain the old weight.
- Filmed displays with periodic pixel texture: independently fit a small local
  translation over a wider patch. Apply it only to an already poor, much larger
  motion estimate and only with strong average and worst-sample image agreement.
  An ill-conditioned fit or an ambiguous periodic patch is rejected.

No filename, timestamp, text recognition, or scene coordinates are used by the
filter. The new local translation fit uses six damped Gauss–Newton steps; it is
motion-vector repair, not a replacement of the Newton inverse-coordinate solver.

Integer 60/120fps caps, independent output phases, source-frame preservation,
the 1920 / Grid 4 / Medium analysis profile, session reuse, precompiled shaders,
and disabled Cost Map / Temporal Hints remain as in rate.1.

## Validation on 2026-10-10

Hardware: RTX 5090, driver 617.42. Source clips are local test fixtures and are
not distributed. Build and evidence directory: `build/gate1`.

- CTest: 10/10 passed. The new production-shader test covers both stroke axes,
  real fast motion, ambiguous bars, disagreeing donors, a glyph over periodic
  microtexture, and microtexture without independent glyph evidence.
- Hardware layer suite: seven synthetic scenes at ×2, ×4, and ×5; 21 cases
  passed the existing quality checks, including exact source preservation.
  This does not imply every metric improves: the curved thin-line scene's ×5
  MSE was about 0.9% higher than rate.1, still within its established check.
- Reported clips: 48 gate frames, 48 car frames, 32 lap-time frames, at both
  caps. Source outputs were byte-exact and ×5 timestamps matched rate.1.
  Protected/cut counts stayed consistent (gate: one protected pair; lap-time:
  one cut; car: neither).
- Eight additional 24-frame sequences checked at ×5: Kokoore 1:20, Jishou 4
  at 5:20 and 23:24, Jishou 5 hair, F1 title/course, sea credits, credit edges,
  and moving face/hand boundaries. Original outputs and timelines matched
  rate.1. Largest-change crops were reviewed; this is a short regression sample,
  not a guarantee for entire videos.
- DirectShow MF/DXGI smoke: NV12 and P010 23.976→119.880, and P010
  23.976→47.952. Negotiated FPS, timestamps, EOS, seek and transport passed.
- Controller self-test passed. No shader compilation occurs during playback.

The gate holes, long car-edge streak and large tears in `0:00.000` were reduced
in the inspected interpolated frames. Small edge remnants and periodic display
texture remain. Source camera blur is not reconstructed into sharp detail.

## Timing and limits

Short repeated old/new/new/old measurements at ×5 gave median completed-pair
times around 3.3 ms for the gate and 4.3–5.3 ms for the sampled 4K car/lap-time
segments. Timing tails varied substantially between runs. The longer lap-time
comparison measured 5.34 ms for rate.1 and 6.70 ms for rate.2; therefore no claim
of zero overhead or a fixed percentage is made. These are offline diagnostic
measurements on the listed GPU, not renderer delivery or other-GPU guarantees.

×5 still synthesizes 20%, 40%, 60%, and 80% directly from each original pair.
The filter's reported output rate does not prove every frame reaches the
display; presentation cadence requires a separate renderer check.

This trial is local. Keep the installation backup and settings when comparing
against rate.1. User playback review remains necessary.
