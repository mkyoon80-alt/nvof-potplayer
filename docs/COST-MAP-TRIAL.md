# Newton + hardware Cost Map trial (0.3.0-cost.1)

Based on public v0.3.0 / lab11 (`a817675`). Local branch: `codex/newton-cost-map`.
The installed Picard trial is replaced only after validation; it remains recoverable.
This is a comparison build, not a proven quality upgrade or a public release.

## Change

- Keep Newton (six iterations, bounded step/backtracking), candidate search, Grid 4 preference, Medium preset, 1920 analysis limit and all existing masks.
- Request both forward/backward 8-bit hardware costs using the existing D3D11 NVOFA session. Query the supported surface formats; if R8_UINT is absent, costs remain disabled and the shader uses neutral factors.
- Preserve raw hardware vectors. Use a separate first-repair scratch pair instead of overwriting those vectors before the second repair pass.
- At each source-grid corner, use hardware cost only if the final repaired vector exactly matches the hardware vector. Otherwise the factor is neutral. Bilinearly sample these factors at the recovered source location using the flow grid's existing coordinate convention.
- Factor = max(0.25, 1 / (1 + cost / 64)). This bounded experimental tuning is not a calibrated probability. It changes moving-source blend weights only; it does not move inverse coordinates, change Newton candidate ranking, choose frame repeats or reduce stationary-background evidence.
- Store base evidence and hardware factors separately in the inverse weight texture. Preserve existing stationary text/background protection decisions.
- Keep build-time shader compilation. Update filter/UI/diagnostic trial version consistently.

The NVOFA cost API and the general confidence-weighting concept were reviewed in NVIDIA documentation and ryou128hr/nvof_interpolate at `551330ce40424b97de9e7c7e4eb9e073c35c68fb`. Implementation here is specific to the existing D3D11 pipeline and its repaired-vector provenance; no CUDA/VapourSynth source is incorporated.

## Validation (RTX 5090, driver 617.42)

- Six CTest checks and UI self-test pass.
- New `native_cost_map` hardware/shader oracle passes: real bidirectional R8_UINT cost execution and reset; zero/absent cost neutrality; directional isolation; bounded effect; unchanged geometry/evidence; repaired-vector exception; bilinear coordinate alignment.
- Existing six layered-motion cases, severe-blur guard/recovery, DirectShow NV12/P010 negotiation/EOS/seek/lifetime, and concurrent-context stress pass. Stress: 501 pairs, 100 resets, 4 recreations.
- Twelve clip fixtures / 1,824 outputs retain exact original frames, timestamps and cut/repeat/identical-skip decisions relative to v0.3.0.
- 90-pair opening pan: mean position error 0.022652 px, maximum 0.051750 px at analysis scale; none above 1 px.
- Held-out real-frame luma MAE (lower is better): 90 s baseline 1.851258 / cost 1.852435; 130 s baseline 2.983208 / cost 2.983733. Nearly unchanged, slightly worse. No measured quality-win claim.
- Six synthetic layer MSEs versus baseline: two slightly better, four slightly worse. All remain well below the held-frame error thresholds.
- Inspected hair/title comparisons retain the previous artifacts. The conservative cost contribution is small; playback comparison is still required.
- Synthetic 3840x2160 diagnostic: drained native mean 7.5066 ms over 19 measured pairs. This is not an end-to-end playback result or an RTX 5070 guarantee, and not a controlled speed comparison against v0.3.0.

Retest the remaining anime hair/occlusion and film title boundaries, plus already-fixed text and opening-pan cases. Evaluate movement smoothness and flicker, not just still-frame damage. Cost is an additional cue; mutually consistent wrong flows and unreliable flow geometry remain possible.

Local reports, comparisons and installation receipt: `build/cost1`. No source video or comparison frames are published. The installation backup restores public Newton by default, or the exact previous Picard files/settings with `-Version original`.
