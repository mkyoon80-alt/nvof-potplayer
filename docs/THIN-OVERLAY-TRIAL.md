# Thin stationary overlay trial (0.4.6-quality.1)

The accepted changes documented below are included in v0.4.7. Trial names and rejected experiments are retained as validation history.

Local trial based on the working `0.4.6-hdrsw.4` source, including the perf.6
scheduling changes and the allocator-time P010 negotiation. This is not a
published release. The trial addresses the fixed reticle near 02:02 in the
user-supplied Gundam clip. The moving gun/table boundary near 01:56 in the
Lupin clip remains unresolved.

## Accepted approach

The ordinary optical-flow field can follow the background behind a thin,
translucent fixed line. Its color changes as the background moves, so exact
endpoint color agreement does not reliably identify the line. The additional
detector checks its narrow cross-section, contrast polarity, aligned profile,
and support along a long horizontal or vertical stroke in both originals.

Two additional mask channels preserve this evidence separately from the
existing stationary text/opaque-plate masks. They are not dilated. Composition
keeps accepted strokes at their source coordinates. A displaced copy is filled
from neighboring background only if it has moved at least three source pixels
across the stroke. Small displacements keep ordinary source sampling. Motion along the stroke
keeps the moving background instead of replacing it with endpoint blending. This last restriction removed small bright flecks found in a
different animation's horizontal outline during visual regression review.

The detector runs per source pair; all interpolation phases reuse its mask.
It does not change Newton inversion, output timestamps, scene-cut decisions,
original frames, HDR interpretation, or transport selection. It does not
identify semantic HUD elements and cannot recover an arbitrarily obscured
line. Gaps remain at some cloud/water boundaries in the Gundam sample.

## Rejected approaches for Lupin

A low-confidence flat-background fallback reduced small speckles around the
gun but also erased legitimate moving detail. The synthetic bright object
case at 5x increased MSE from 41.0125 to 64.8861. That candidate is excluded.
Requiring near-zero source motion removed this regression but also removed
almost all benefit on Lupin. Disabling the established stationary-layer or
coherent-trajectory paths did not produce a safe general fix either. A further detailed-patch match
with agreeing opposite-side motion donors (`edge1`) left the reproduced
Lupin artifact unchanged and is not included.

A subsequent `occluded1` experiment extrapolated neighboring, image-matched
surface motion only when the opposite endpoint was covered by a different
bidirectionally consistent motion. Diagnostic snapshots (`capture1/maps/`)
record the inverse coordinates, weights, source flows and masks for the
reported phase. This candidate made negligible changes to the Lupin result
and did not visibly repair the frayed boundary. Although it passed the existing
coarse hold/crossfade guard, its small-foreground fixture MSE at 5x increased
from 2.29468 to 2.54099 (about 10.7%); the curved occluder also regressed.
It is rejected, not installed. Passing a coarse test is not sufficient when
the baseline comparison shows a quality regression.

Similar endpoint colors do not prove that an intermediate pixel belongs to
the background: a moving thin object may pass through that location between
the two originals. The trial therefore does not include a generic flat-area
hold, stronger global blending, or a reduction in interpolation rate. It must
not be described as fixing the Lupin gun artifact.

## Evidence and reproducibility

Local diagnostic assets and reports are under `build/quality047/` and are not
redistributed. `thin7` is the accepted shader candidate. `thin5` predates the three-pixel
displacement guard; `thin6` predates the along-line motion guard. The new
along-line control rejected thin6 (line MSE 0.1322 -> 0.4061); thin7 preserves
the baseline 0.1322, without relaxing the test tolerance. `flat2`–`flat5` and
`combined1`/`combined2` are rejected experiments, not deployable updates.

- `tools/compare-thin-overlays.py` builds independent analytic ground truth
  for fixed, moving, faint, fading, short, parallel and occluded strokes,
  including small and along-line background motion. It compares original
  frames, timestamps and pair decisions as well as full/line/adjacent MSE.
  It requires substantial improvement on the principal fixed-line cases
  and rejects regression on the controls. No movie-specific screen location
  enters the production shader.
- The final candidate passed all 13 analytic cases, reducing fixed vertical
  line MSE from 1538.16 to 15.66, bright line MSE from 682.28 to 28.94 and
  faint line MSE from 232.80 to 18.70. These are fixture scores, not a claim
  of a corresponding percentage improvement in arbitrary video.
- `regression-thin7-final/summary.json` covers ten local real-video segments.
  Original frames, output timing and pair decisions match the baseline.
  Two real-frame holdout sequences have identical baseline/candidate error
  vectors: full-frame luma MSE 34.2381 and 95.0940. Other segments have no
  high-frame-rate ground truth; their image differences are inspected,
  not reported as objective quality improvements.
- `quality1-final/tests.json` records 18 successful checks of the final built filter, including
  moving-layer controls, P010 precision, shared-context stress and repeated
  HW/SW negotiation. `quality1-final/overlay-regression/` is the final production
  executable's analytic regression. CTest also passed all 18 registered tests.
  The 24 saved comparison pairs across the additional real-video segments
  are pixel-identical to the baseline. These finite checks do not establish
  the absence of tradeoffs on every possible input.

The earlier P010 reports in `regression-combined2` and `regression-thin4`
used an incorrect diagnostic readback that copied half of a 16-bit row as
8-bit data. Those P010 results are invalid. `video_x2_repro` now explicitly
reads each P010 component and writes its high byte to its diagnostic NV12
file, and verifies original component order. Holdout truth comes from the
decoded raw file, not an earlier interpolation output. This 8-bit comparison
does not establish 10-bit precision; the dedicated P010/DirectShow tests do.

## Cost and limits

The two full-resolution masks change from RG8 to RGBA8: approximately 24.6 MB
additional allocation at 3840x1604, or 33.2 MB at 3840x2160, per filter instance
(decimal MB, excluding driver alignment). Detection and composition add GPU
work. The final 3840x1604 synthetic one-midpoint benchmark ran baseline,
candidate, candidate, baseline sequentially, with 59 measured pairs each.
The drained mean times were 3.0446, 3.1241, 3.2354, and 3.0065 ms per pair:
baseline mean 3.0256 ms, candidate mean 3.1798 ms (approximately +0.154 ms,
or +5.1%). Submit p95 was 4.6907/4.7334 ms for baseline and 4.7379/4.8196 ms
for the candidate. GPU clocks and unrelated desktop activity were not held
constant; this is a short cost estimate, not a zero-cost claim or a PotPlayer
playback/drop test. See `quality1-final/benchmark.json` and its four logs.

No GPU tests are run alongside user playback or another GPU test. Test helpers
use a successful PowerShell process enumeration and fail closed if PotPlayer
or another comparison is active. Installation must preserve the existing INI
and back up the installed filter. Actual-player confirmation is still separate
from the offline checks.

## Local installation and player confirmation

Installed `0.4.6-quality.1` on 2026-10-12 after the final checks. Its SHA-256 is
`05d2b37ed4d3961aa1d53d99c29ef04649d29d3064a6918ea10e567ada3c07a6`.
The previous hdrsw.4 filter and INI were backed up under
`backup/before-quality1-20261012-160342`; the active INI was unchanged.
The user replayed the Gundam segment and reported that the reticle improved
without other observed problems. This is the user's visual confirmation of
that segment, not a claim that all residual gaps or Lupin artifacts are fixed.
