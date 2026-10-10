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
