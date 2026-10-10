# Cost confidence fusion — 0.3.1-cost.2

Local experiment on top of native standalone checkpoint c12947b. No public release.

## Method

Keep bounded Newton inversion with six iterations and backtracking, its five-seed recovery search, source-grid alignment, Y/UV sharing, scene rejection and stationary-layer rules.

The old hardware weight is h=max(0.25,1/(1+cost/64)), bilinearly sampled with a neutral value for repaired vectors. The selected experiment maps h to max(0.10,h/(4-3h)), approximately cost/16 sensitivity for a uniform, unclamped grid. Repaired/unknown cells remain neutral. This bounded heuristic is not a calibrated probability.

Let q=(0.05+local evidence)*warp reliability and c=clamp(q/1.05). The hardware influence is 4*c*(1-c). Perfect local agreement and nearly invalid local evidence dominate; ambiguous evidence receives the strongest hardware contribution. The resulting factor is used in candidate ranking and final forward/backward blending. Local evidence is retained separately for stationary-background protection.

Hardware cost alone does not move the Newton solver, invent another vector, or change frame repetition/cut decisions. It can change which existing converged candidate is selected. Off / old blend-only / fusion modes remain in the developer API and clip harness for reproducible comparisons; the filter defaults to fusion.

## Why Cost.1 was subtle

Direct driver-cost readback on three selected frame pairs (RTX 5090 / 617.42, medium, grid 4):

| Pair | Forward mean / median / p99 | Backward mean / median / p99 |
|---|---|---|
| Jishou 4 episode, pair 99 of 5:20 clip | 12.19 / 12 / 29 | 11.96 / 11 / 29 |
| F1 title, pair 24 | 8.21 / 7 / 27 | 8.16 / 7 / 27 |
| F1 1:30 reconstruction, pair 10 | 10.03 / 10 / 24 | 9.97 / 9 / 26 |

These samples explain why dividing by 64 often had modest influence. Similar directional penalties can cancel during blend normalization. These numbers are not universal thresholds for other GPUs, drivers or footage, and do not establish that hardware cost predicts the remaining artifacts.

## Reconstruction comparison

Each F1 test omits original frames, interpolates 31 missing frames from neighbors, and compares luma against those originals. Full-frame metrics exclude a narrow border. These are reused development clips, not an independent evaluation set.

| Variant | F1 1:30 MAE | F1 2:10 MAE |
|---|---:|---:|
| Newton, no cost | 1.851258 | 2.983208 |
| Cost.1 blend-only | 1.852435 | 2.983733 |
| Confidence gating, original strength | 1.850915 | 2.982775 |
| Confidence gating, squared weight | 1.850675 | 2.982406 |
| Selected stronger mapping | 1.850612 | 2.982061 |

The selected profile lowers MAE by approximately 0.098% and 0.056% versus Cost.1. Full Laplacian MSE instead rises from 6.152820 to 6.169641 and from 6.952010 to 6.955258. This is mixed evidence and a small change, not a clear perceptual win.

Six synthetic scenes (exact intermediate truth, three fractions) compare Cost.1 to the selected profile:

| Scene | Cost.1 MSE | Selected MSE |
|---|---:|---:|
| Fixed curved title over moving background | 13.7218 | 13.7035 |
| Moving repeated bars | 110.512 | 110.272 |
| Bright moving object | 41.6256 | 41.4645 |
| Moving thin curves | 423.535 | 423.751 |
| Moving glyph-like strokes | 251.576 | 251.336 |
| Curved occluder | 28.5803 | 28.0596 |

Thin curves slightly regress. The curved-occluder no-cost result is 28.0636, so much of the improvement over Cost.1 is recovery of its regression, not a new benefit over Newton alone.

## Functional checks

- Hardware 8-bit costs in both directions, resets, provenance, bounds and interpolation alignment.
- Deterministic fusion oracle: exact agreement preserved, uncertain agreement uses directional cost, severe mismatch not rescued, missing cost neutral and corrected vectors not assigned stale cost.
- Kokoore 1:20, Jishou hair, Jishou 4 episode 5:20 sequence, F1 title and both F1 reconstruction clips: all original output pixels, timestamps, scene/hold/identical decisions match Cost.1.
- The final default shader is rebuilt and independently checked against the selected profile on the same clips.
- Comparisons preserve small JPEG contacts, CSV timing and metrics. Newly generated duplicate raw outputs are deleted after checks to limit disk use.

Evidence is in build/cost2 (ignored): quality-summary.json, final-clips/*/metrics.json, layers-strong16.log and validation-final.log. User playback evaluation remains necessary; Jishou hair/face boundary artifacts remain visibly present.

Final runtime validation: six core tests, CPU upload/readback transport, NV12 CPU/GPU DirectShow, P010 GPU negotiation, reset/flush/seeks/shutdown and 10-second interop stress passed. Stress completed 486 pairs, 100 resets, four recreations and concurrent renderer use. No removed runtime directory was present in these staged tests.
