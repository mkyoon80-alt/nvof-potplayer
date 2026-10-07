# Local real-video regression: slow hair pan at 23:19–23:23

Date: 2026-10-07. User-provided local 1080p episode, 24000/1001 H.264, 8-bit yuv420p, limited BT.709. Media and extracted images remain in ignored `build/scene-2319`; do not publish user footage with the source.

## Reproduction

Decoded 6 seconds starting at 23:18 with FFmpeg to lossless NV12 layout and original millisecond timestamps: 143 source frames. `video_x2_repro` uploads those frames to D3D11 and uses the production `GpuFrucEngine` and `GpuPhasePipeline`, including source cadence normalization. The result has 286 output frames at 48000/1001. All 143 original pictures are byte-exact at their source positions. This isolates interpolation content from PotPlayer display pacing and VSR; it does not capture the user's actual renderer.

Both preview.6 settings (context-ordered completion, duplicate-picture skip enabled) and the earlier blocking/no-skip settings show the defect. No exact-picture skip occurred. No scene cut hold occurred. One NVIDIA repetition hold was at 23:18.878, before the requested interval. Mean absolute full-frame byte difference between the two modes in the inspected interval was 0.00684/255, consistent with small FRUC run variation; both have the same visible failure.

A second oracle calls the official NvOFFRUC CUDA API directly on the decoded NV12 buffers, without the project GPU texture conversion, bridge or scheduler. It reproduces the midpoint-position bias and jagged thin contours. An ARGB/BGRA-surface trial with the same source also shows the same issue. These observations implicate the current FRUC output on this scene, not only the project's transport/optimization. They do not prove that every NVIDIA Optical Flow implementation or format has this limitation.

## Measurement

Track up to 150 reversible Lucas–Kanade features per pair in the blond hair ROI (x=1420..1820, y=35..450). Reject invalid tracks, reverse-tracking error >=0.3 px, and source motion outside 0.3..20 px. For each tracked feature, project A→M displacement onto A→B and divide by its squared length; ideal halfway translation gives 0.5. The shot can contain local deformation, so this is a geometric diagnostic rather than unavailable ground truth for each pixel.

Across 96 pairs in the requested interval:

- Median source horizontal displacement: -1.056 px per source frame.
- Median of per-pair midpoint fractions: 0.279 (expected approximately 0.5 for this slow pan).
- 45/96 pair medians were below 0.25; 10th–90th percentile 0.120–0.455.
- Median tracked midpoint-position error relative to half the source displacement: 0.374 px.
- Direct SDK NV12 trial: same fraction 0.279. ARGB trial: 0.282, error 0.363 px.
- Estimator control: a known -0.5 px translation measured -0.497 px.

The source hair and face contours are smooth. The inserted frame has jagged/uneven fine outlines. Alternating smooth originals and rough midpoints can produce visible crawling; biased midpoint positions produce alternating short and long motion steps even though the output frame clock is regular. VSR is not required to reproduce the defect. Fluid Motion itself was not rendered/captured in this test, so no numerical superiority claim is made for it.

## Result and next direction

The user's report is reproduced. No production files or settings were changed. Performance optimization and zero-copy do not directly fix this midpoint-quality defect. Any subsequent quality change must use this real-video regression in addition to synthetic tests, with particular attention to subpixel translation and thin-line temporal stability. Do not reinstate rejected fixed 60/120 modes or apply the prior unaccepted fractional refiner as an assumed fix.

Local artifacts:
- `build/scene-2319/findings.json`, `motion.csv`, `direct-motion.csv`, `argb-motion.csv`.
- `build/scene-2319/frame-comparison.png`: A / inserted M / B, nearest-neighbor 2× crop for inspection.
- `build/scene-2319/original-left-current-right.mp4`: cropped original (left) versus current ×2 output (right), source-rate frames repeated for matched display timing.

The additional diagnostic executables are opt-in and do not bundle or download media.
