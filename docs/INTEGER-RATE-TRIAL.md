# Integer output limits — 0.3.2-rate.1

The existing two output buttons now mean maximum 60 or 120 fps. The default is 60. A selected source uses the largest integer multiple within that limit; multiplier 1 means passthrough. Inputs above the limit are never downsampled. Reopen the video after changing the setting.

| Source fps | Limit 60 | Limit 120 |
|---|---|---|
| 24000/1001 | 48000/1001 (×2) | 120000/1001 (×5) |
| 24 | 48 (×2) | 120 (×5) |
| 25 | 50 (×2) | 100 (×4) |
| 30000/1001 | 60000/1001 (×2) | 120000/1001 (×4) |
| 30 | 60 (×2) | 120 (×4) |
| 50 | 50 (original) | 100 (×2) |
| 60000/1001 | 60000/1001 (original) | 120000/1001 (×2) |
| 60 | 60 (original) | 120 (×2) |

## Implementation

`OutputFpsLimit=60|120` is independent of old `TargetFps` / `DoubleRate` keys. Missing or invalid values select 60. Opening the controller no longer rewrites legacy settings. Source checkboxes and master enable retain precedence. Existing user INI contents are preserved during installation.

The existing rational phase pipelines and arbitrary-phase native renderer are reused. One source pair prepares optical flow and text motion once; ×5 then renders phases 0.2/0.4/0.6/0.8. No midpoint duplication or chained interpolation is used. Normal source frames remain intact in the existing output format (P010 input is still converted to 8-bit NV12). Final source duration is completed at EOS. Seek/flush/stop clear old history.

Newton synthesis, credits.5 text/edge protection, precompiled shaders, session reuse, and disabled Cost Map/Temporal Hints are unchanged. Very low frame rates have a maximum 32 generated frames per pair (×33). Unknown/unsupported source timing bypasses; variable frame rate is not guaranteed to have an integer number of outputs in every pair.

## Local validation

Hardware: RTX 5090. These checks do not establish RTX 5070 performance or full-player real-time throughput.

- All 9 core tests pass. CPU/GPU cadence tests cover both limits, nine source rates, three timestamp quantizations, original identity, every output slot, and EOS.
- DirectShow policy matrix: 36 NV12 cases (both limits, nine rates, CPU and GPU delivery), plus P010 at 23.976→47.952/119.880. Negotiated media rate, actual timestamps/count, first-original delivery, seek and format transitions checked.
- Master-off/source-excluded: 8 additional CPU/GPU cases retain original output at both caps. Full CPU/GPU lifecycle harnesses pass at both caps, including concurrent flush/stop.
- Native synthetic quality: ×2/×4/×5 across six motion/outline scenarios. Generated phases are compared with analytical positions, not merely required to be different. Stationary outline leakage and hold/crossfade regressions are checked. Credit-scroll shaders match known 10-pixel translation at 0.2/0.4/0.6/0.8, including top/bottom entry/exit.
- GPU stress: 1,452 generated phases over 363 pairs with 100 resets; no failures. A second one-phase run checks identical-picture advance. Full 3840×2160 one-phase stress also passes.
- Five real-video fixtures, first 24 source frames each: Kokoore 1:20, Jishou episode 4 5:20, F1 titles, ocean credits, and credit entry/exit. At limit 60 every output byte and timestamp matches credits.5. At limit 120 each fixture produces 120 output frames, retains all 24 original frames, preserves rational timestamps and creates distinct intermediate phases where the content permits. Cost buffers remain zero. Representative phase montages inspected.
- Controller self-test covers selection persistence, mutual exclusion, master-off preservation, status wording, 464/504-pixel layouts and existing uninstaller selection. Preview inspections at 100/150/200% scaling passed. UI detector reported no findings.

Local evidence is in `build/rate1/`: policy-matrix.json, runtime-tail.log, bypass.log, stress-4k.log, layers.log, video/summary.json, build-final-verified.log, ui-selftest-verified.log and ui-qa/. Generated raw comparison outputs were removed after verification to save disk space; input media were preserved.

Timing samples are diagnostic only: a player was open during measurement, clips are short, and decoder/display cost is excluded. Do not present them as a stable speedup or 4K60→120 playback guarantee. Quality at every new phase still needs subjective playback review; the byte-identical regression claim applies to the ×2 path.

## Distribution

Local trial only; not published to GitHub. The package contains self-contained controller runtime and the native filter, without NvOFFRUC/CUDA/GPU driver DLLs. The installed version and previous-build restoration path are recorded in `build/rate1/installation.json` after installation.
