# Slow midpoint stability — 0.2.0-preview.7

This local update targets the reported 1080p animation pan at 23:19–23:23. It keeps ×2 output and preserves every source frame. It does not reintroduce the rejected 60p/120p resampling or experimental arbitrary-phase renderer.

## Processing

FRUC still receives each input once. Its scene/repetition protections and identical-picture advancement remain authoritative. For a non-repeated midpoint, a separate NVIDIA D3D11 Optical Flow pass estimates bidirectional motion on a 2-pixel grid where supported. Inverse half-displacement sampling reconstructs the midpoint from the original Y/UV planes.

Only locally slow motion (up to 4 pixels per source interval) with forward/backward and endpoint consistency within 0.6 pixels is eligible. Aligned 3×3 luma patches must agree within 12 code values, and chroma within 8. Other pixels retain the original FRUC result. There is no weighted crossfade between FRUC and the corrected result. The two source samples are motion-aligned before averaging.

Bounded Catmull-Rom reconstruction reduces half-pixel sampling blur; outputs are clamped to the sampled source footprint. No RGB conversion, color-range change, contrast boost, host frame copyback, additional frame lookahead, or video overlay is introduced. GPU texture copies remain; this is neither zero-copy nor asynchronous FRUC.

Cuts, repetition and identical pairs bypass this pass and invalidate optical-flow temporal hints. Seek/reset/resolution change clears its state. Raw-flow initialization failure disables this optional pass for the current engine while retaining ordinary FRUC playback. Status fields `midpointPassFrames`, `midpointUnavailable`, and `midpointLimitedFrames` distinguish shader submission from failure/budget bypass; the pass count is not a count of accepted pixels.

## Measured quality

The user's clip is kept only in ignored local build output. Full native-engine reproduction generated 286 output frames from 143 original frames, with all 143 original NV12 images byte-identical. One FRUC repetition occurred before the target interval; none occurred within it.

96 adjacent source pairs from the target interval were measured with an independent forward/backward-checked feature tracker over the moving hair region:

| Measurement | Preview 6 | Preview 7 candidate |
| --- | ---: | ---: |
| Median insertion position (ideal 50%) | 27.85% | 48.19% |
| Median midpoint position error | 0.3736 px | 0.0453 px |
| Pairs with median position below 25% | 45 | 0 |

These are image-tracking estimates, not ground-truth motion for every pixel. Hair/eye/jaw crops were also visually compared. The change reduces the reported staircase/jitter defect; it does not prove equivalence to AMD Fluid Motion or guarantee artifact-free interpolation. Synthetic continuous pans, moving foreground, thin wires, dense fences, multiple motion layers and illumination changes use independently rendered midpoint truth. The final patch-neighborhood gate passed 34 cases with checked MSE, occlusion errors, missing/extra dark ink, and unchanged source images. Earlier center-only variants failed dense-line checks and were rejected.

## Cost and limits

RTX 5090 microbenchmarks include final GPU completion, exclude decoder and VSR, and were run locally with the existing player present:

- 1080p 1-pixel motion: about 4.3–4.8 ms/pair without the pass, 9.4–10.0 ms with it.
- 2160p: about 14.6 ms without, 33.9 ms with it.

Therefore frames larger than 1920×1080 pixel area at source rates above approximately 30 fps retain ordinary FRUC. Film-rate 4K and 1080p remain eligible. This protects high-rate playback from an observed processing-budget overrun; it is not an adaptive guarantee on every GPU or under concurrent VSR load. Runtime memory/GPU work increases for eligible frames. Very thin undersampled patterns, occlusions and fast motion can still show FRUC artifacts where correction is rejected.

## Validation

- Release build; six CTest suites.
- GPU 1080p and 2160p tests: color bytes, scene/flash protection, retained outputs, seek/reset, shutdown, shared renderer state; high-rate 4K bypass and film-rate correction.
- Two concurrent-renderer stress runs, each with 100 resets and four engine recreations: protected-context and mutex-plus-protected modes passed.
- Native DirectShow/MF transport: 23.976 → 47.952, color metadata, timestamps, EOS, seek, negotiation and resource lifetime passed.
- Self-contained package and installation validation are recorded separately in ignored `build/slow-midpoint/`.

Actual PotPlayer/VSR acceptance after installing preview 7 is still pending. No GitHub publication is part of this update.

Installed locally on 2026-10-07T19:49:26.898713+09:00. Only NvofPotPlayer.ax changed; INI hash stayed identical. COM filter/property-page activation and installed runtime audit passed. A local rollback copy was retained. The tester subsequently reported a substantial slow-motion improvement. See preview.8 notes for the current iteration.
