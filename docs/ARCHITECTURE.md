# Current playback architecture (0.3.0)

The default is lab11 NVIDIA Optical Flow + custom D3D11 synthesis; see [current synthesis details](NATIVE-SYNTHESIS.md). The FRUC/CUDA sections below describe the retained legacy backend and research history, not the default 0.3.0 synthesis path. P010 conversion, sample lifetime and DirectShow negotiation are shared.

# Pipeline and validation

## Current product scope: ×2 only (preview.5)

Fixed 60p/120p output modes have been removed at the user's request after real-video fractional-phase artifacts. The filter now derives exactly twice the source rate regardless of legacy INI values. A single midpoint is inserted between preserved originals; source FPS selection, seek re-prime, scene/repetition protection, color metadata, native transport, and renderer lifetime safeguards remain in use. Experimental subpixel refinement is disabled. The multi-phase library and historical tests below remain development records, not current selectable product modes.

GPU-resident does not mean zero-copy: GPU copies/plane transfers remain. Map/unmap ordering and fence-based completion are present, but FRUC calls and final delivery still block. A fully asynchronous external-semaphore pipeline is unfinished.


## Video path

```text
PotPlayer built-in decoder (D3D11)
    → original NV12 GPU textures
    → NvofPotPlayer DirectShow filter
        → owned GPU snapshots
        → shared CUDA NV12 input buffers
        → independent NVIDIA NvOFFRUC histories for requested output times
        → pooled NV12 output surfaces, or previous-original hold on low confidence
    → PotPlayer built-in D3D11 renderer
        → VSR, when enabled in the player
    → display
```

Interpolation precedes VSR. Reordering external filters cannot move VSR out of the renderer. The native path keeps frame pixels on the GPU, although GPU-to-GPU copies and synchronization are still required; this is not a zero-copy claim. A compatibility system-memory path exists and is labeled distinctly in diagnostics.

Each phase history processes every adjacent original once. For 23.976 → 119.88, four handles request 20%, 40%, 60% and 80%; none repeats an input on one handle. Inputs are prepared once and shared across serial calls, and each result is saved before shared output scratch is overwritten. Existing handles advance even when a variable-cadence pair requests fewer outputs. The project bridge exposes NVIDIA frame-repetition status. All phase results are collected before delivery; any repeated phase protects the entire pair by holding the previous original until the next source timestamp. No temporal blend is used in production. The implementation is independent of Smootter and does not contain its binaries or source.

## Timing, seeking, and lifetime

The scheduler uses rational source rates and 100-nanosecond DirectShow timestamps. NTSC sources retain their fractional cadence. Source-rate exclusions and sources at or above a fixed target retain original timestamps.

A known constant source cadence is anchored to the first decoder PTS. Timestamp quantization within 1 ms is aligned to its exact advertised rational clock (for example 24000/1001, not a repeatedly added 417083-tick rounded duration). A larger deviation disables alignment until the next seek/reset, preserving variable-rate input timing. Source discontinuities are checked against raw timestamps before alignment. Ordinary full-frame EOS timing is corrected within the same bound; clipped endings outside it remain unchanged.

This prevents an original timestamp rounded to 42 ms from being misidentified as a request for a new 99%-phase image at 41.708 ms. In x2, original and midpoint outputs alternate; in integer x5, each original is followed by four independent phases. Scene protection uses the aligned timeline; alignment can move a nominally constant-rate source timestamp by at most 1 ms. It does not move the output clock origin or accumulate audio drift.

The preview.2 x2 live counters showed roughly 2% original reuse. An H.264/Matroska reproduction with 1 ms PTS quantization produced 478 outputs, of which only 10 were exact originals, matching that signature. The corrected scheduler tests include rounded/floored 1 ms and rounded 100 ns timestamps, arbitrary seek origins, one-hour drift checks and VFR transitions. A continuous textured-pan and moving-foreground CUDA FRUC pixel check using a host-memory oracle preserved every original byte-exact and issued zero near-endpoint FRUC requests. Native DirectShow/MF x2 metadata, timestamps, seek, EOS and lifetime integration also passed. Installed preview.3 (filter SHA256 e7b8312f7f45ef953be6da46ee2ef767dac52e3e7451f533cdf3fe7a0d27a08a) was observed in actual PotPlayer at 23.976 → 47.952 fps, with sourceCadenceAligned=true, nearEndpointFrames=0 and about half of outputs retained originals. On 2026-10-07 the user confirmed that shimmer was clearly reduced in the same x2 playback scene. This confirms the endpoint-cadence correction improved the reported artifact; it is not a claim that optical-flow interpolation is artifact-free.

Flush, discontinuity, and a new segment invalidate previous history. The first original frame at the new location is delivered without waiting for a midpoint; interpolation is primed again from new adjacent frames. Held output textures remain valid until the downstream renderer releases their samples. Worker resources are drained and released during stop and graph teardown.

The native connection negotiates decoder surfaces through Media Foundation / DirectShow contracts. GPU samples do not expose a hidden CPU readback through `GetPointer`. The built-in renderer receives NV12 surfaces with explicit range and color metadata. Legacy input metadata is promoted to VideoInfo2 for this native output path; supported SDR defaults are supplied only where unknown. Explicit incompatible color information is rejected.

## Scene protection and synchronization

A conservative luma detector compares a 64 × 36 grid of locally averaged samples and two histograms. The native path reads back only 280 bytes of aggregate statistics, including an exact full-frame Y/UV equality flag; video planes remain on GPU. Broad, sufficiently large changes with spatial variation trigger scene protection. A uniform brightness flash alone is deliberately insufficient, and similar-histogram cuts can be missed. This heuristic is separate from NVIDIA's quality-based repetition signal, which is not a scene-cut classification.

Identical source frames retain their exact NV12 pixels while the histories still advance; this prevents spurious optical flow from changing fine static patterns.

A detected cut clears FRUC history without resetting the rational output clock. Output timestamps strictly before the new original retain the previous original; the new scene is never shown early. Seek/flush instead resets both history and the target clock.

CUDA/D3D interop map and unmap provide ordering. Redundant full-context synchronizations were removed; D3D state/mutex ownership covers graphics operations and CUDA interop map/unmap, not the entire FRUC batch. Adapter discovery and interop/context teardown also preserve and restore shared state: cuD3D11GetDevices and context destruction were observed clearing the renderer viewport in the concurrent test. Final completion uses a D3D11 fence event when supported, with an event-query fallback. Original snapshots and final batches still complete before delivery. NvOFFRUC CUDA processing itself remains blocking; this is not a fully asynchronous external-semaphore pipeline. Output pool leases keep renderer-retained frames immutable.

## Validation record

- Automated source-rate policy, output cadence, scheduler/seek, and GPU pipeline tests.
- RTX 5090 independent-phase tests: four distinct motion results outperform temporal blending on known translations; exact static luma/chroma, cut recovery, flash rejection, retained outputs and GPU state restoration. CPU compatibility tests exercise 100 changing-scene resets and variable phase counts, including zero.
- Bridge: 27 calls compared with the direct official NVIDIA API; output pixels and actual repetition signals matched.
- DirectShow/MF harness: native negotiation, activation failure, CPU compatibility, metadata, flush/seek, and COM lifetime.
- Earlier 0.1 preview, actual PotPlayer x64: 1080p 23.976 → 119.88 fps and ×2, built-in decoder/renderer, VSR, forward/backward seeking, and clean exit.
- Color-range regression: synthetic static samples remain byte-exact. Matched static regions of rendered captures agreed to within approximately one 8-bit level after the metadata correction. This is not a claim that interpolated moving pixels equal originals.
- Controller: settings persistence, source mask / ×2 switching, status parsing, process identity, registration routing, and bounded layout/focus checks.

The current validated media scope is progressive limited-range BT.709 SDR: NV12 8-bit and P010 10-bit input converted on GPU to NV12 8-bit output. Beta.1's corrected P010 path was accepted in actual PotPlayer playback on RTX 5090, with 1,751 source frames, 3,263 delivered frames, active x2 cadence (23.976 -> 47.952 fps), and clean shutdown. Counts include playback boundaries/protection and are not an exact x2 frame-count assertion. HDR tone mapping, 10-bit output precision, interlacing and mixed-GPU systems are not established. Previous preview experiments with fixed output rates are historical; the released product is x2 only.

## Phase-upgrade performance (RTX 5090, 2026-10-07)

A synthetic 20%/40%/60%/80% motion batch, including input capture, aggregate scene/equality readback, four FRUC calls and NV12 output:

| Source size | Mean per input pair | P95 |
| --- | ---: | ---: |
| 1920 × 1080 | 17.3 ms | 17.9 ms |
| 3840 × 2160 | 56.8 ms | 57.4 ms |

Warm short-run measurements exclude decoder, renderer/VSR, first-use initialization and display timing. A 23.976 fps source supplies about 41.7 ms per pair. Native 4K → 119.88 fps therefore exceeds this budget in the tested four-session mode. 1080p interpolation followed by renderer VSR still interpolates at 1080p. These results do not claim a speedup over the former midpoint-plus-blend mode; they measure the independent-phase replacement; fractional-position image quality needs separate validation.

A re-prime regression was reproduced when the same host addresses were re-used to register CUDA resources. Each history generation now receives a new owned address block before its predecessor is freed; GPU allocations and pixels remain on GPU. 100 changing-scene generations were checked against motion ground truth, rather than only frame counts.

The release package is checked separately for its bundled runtimes. Tests load a staged filter by explicit path and do not replace an existing user's COM registration.

The concurrent-renderer regression test keeps another thread updating, copying and reading independent resources on the same immediate context during four-phase generation, history resets, engine reconstruction and teardown. It also checks renderer viewport state and retained output pixels. This targets behavior that a single-threaded synthetic decoder/sink did not exercise. The corrected build passed two 30-second runs (context protection alone and shared mutex plus protection), each with 100 resets and four engine recreations. They completed 788 and 5,544 phase outputs respectively, while the competing renderer performed 467,872 and 430,338 work iterations. Constructor and teardown initially cleared the viewport and were explicitly reproduced before the fix. These stress counts are not playback-performance measurements.


## Fractional-position quality regression (2026-10-07)

The user reported contour/detail shimmer in 60p and 120p after the preview.3 x2 endpoint correction. Continuous per-output pixel checks reproduced a separate fractional-position defect. With a detailed scene moving four pixels per source frame, 20/40/60/80 percent phases should move 0.8/1.6/2.4/3.2 pixels; a fixed subpixel fit instead measured approximately 1/2/2/3 pixels. The 40-to-60 percent step stalled. Four-phase generated luma MSE was about 15.38 compared with 0.228 for the exact-integer midpoint. This is a synthetic, known-motion result, not a universal image-quality score.

Independent engines with independent allocations closely matched shared serial buffers, and every retained original was byte-exact with zero near-endpoint requests. An official-bridge NV12/ARGB comparison found essentially the same fractional error in both formats. Doubling spatial size reduced some error but blurred detail and worsened the exact midpoint; it is not a suitable production fix. Earlier tests used ten-pixel translation, producing integer positions 2/4/6/8 and missing this defect. `multi_phase_quality` and `fruc_surface_probe` are opt-in hardware diagnostics that now cover fractional phases, continuous pans, static background with moving foreground, and held-frame animation. Their quality measurements are reported separately from structural PASS conditions.

The native refinement computes additional forward/backward NVOFA vectors once per changing pair (2x2 output grid, medium preset), then inverse-warps native Y/UV planes with bilinear fractional coordinates. Three inverse iterations, forward/backward consistency, coordinate residual, and Y/UV correspondence gate the correction; uncertain pixels retain their existing FRUC result. It does not spatially upscale or blur originals. Pure midpoint x2 and identical pairs bypass refinement. Skipped pairs invalidate temporal hints without reallocating GPU surfaces, and seeks/cuts discard histories. Failure to initialize the optional raw-flow path preserves baseline FRUC playback and sets `subpixelUnavailable`; `subpixelPassFrames` counts generated frames offered to refinement, not individually changed pixels. CPU compatibility remains baseline FRUC.

The standalone 640x360 four-pair matrix reduced pan4 non-midpoint luma MSE from 15.331 to 0.185, matching fractional motion within about 0.05 pixels. Foreground4 MSE improved 2.788 to 1.505 and the occlusion-edge subset 36.63 to 27.25. Static background was byte-exact, retained GPU outputs and inputs stayed unchanged, and flat-luma moving chroma preserved the actual FRUC fallback byte-exact. This is a synthetic regression result, not an 83x perceptual-quality claim. The integrated native 1920x1080 four-phase test measured 23.0 ms mean / 23.4 ms P95 per input pair, excluding decoder and VSR; this adds work compared with the prior 17.3 ms baseline. Actual PotPlayer user acceptance for preview.4 is pending.

Preview.4 integrated regression passed six CTest suites, explicit native MF output clocks at 59.94/119.88/47.952, and DirectShow/MF negotiation, color metadata, EOS/seek, blocked flush/stop and unload. The native engine test asserts all four non-midpoint refinement bits are active, no unsupported fallback occurred, and midpoint/identical cases have no refinement bits. Shared-context stress passed 30 seconds each with protection-only and mutex+protection: 1,296 and 8,044 phase outputs, 40 resets and one engine reconstruction each. A final rate-policy test resets streaming counters during its intentionally rejected metadata change; its final status file is not a measurement of the earlier active run.

Installed preview.4 on 2026-10-07 17:48 KST only after both player/controller were observed closed. Filter SHA256: `2b82c2620863f2714e039aa3b52d756e4b1cc46d45dcec57c63a8d38edf050ad`. The installed INI was hash-verified unchanged (60p, interpolation enabled), controller and bridge hashes stayed unchanged, and previous preview.3 was saved at `backup/phase-upgrade-20261007-174800` with rollback script. Complete local ZIP SHA256: `60ebd01899b13ae60fcb6835aec245e916b34aca5923a0050a7f9d6703f00de2`. The six bundled native runtimes loaded from the package in the isolated oracle. No GitHub upload. Actual playback verification and user quality comparison are pending.

Actual PotPlayer preview.4 subsequently reported native D3D11 output at 119.88 fps with 2508 fractional refinement passes, `subpixelUnavailable=false`, and zero near-endpoint requests. This confirms activation in the real host; user assessment of contour shimmer, VSR, and termination is still pending.

### Preview.4 rejected and rolled back

The user reported severe frame mixing in actual 60p/120p playback. Positive synthetic quality/activation results above did not generalize. With both applications closed, the installed filter was restored from `backup/phase-upgrade-20261007-174800` to preview.3 SHA256 `e7b8312f7f45ef953be6da46ee2ef767dac52e3e7451f533cdf3fe7a0d27a08a`; the user's current 120p INI was verified unchanged. The preview.4 complete package is quarantined under backup/rejected-preview4-20261007 and is not an accepted release.

The experimental refiner now defaults OFF via `NVOF_ENABLE_EXPERIMENTAL_SUBPIXEL`. It mixes a raw-flow reconstruction with independently positioned FRUC output using soft confidence, without proving the two reconstructions align at edges. Flat shading, thin outlines and occlusion can satisfy consistency checks despite incorrect correspondence; a two-endpoint static shortcut can also erase an object visible only at an intermediate position. These are code-level suspects, not yet a reproduced explanation of the user's exact clip. The previous tests covered uniform translation and one rectangle and were insufficient for deploying a general quality fix. Keep the refiner isolated until adversarial and actual footage comparisons establish a solution.

After rollback the default build was recompiled with experimental refinement OFF and its native engine regression passed. The installed binary remains the exact preview.3 backup, not this development rebuild. Adversarial isolated tests now inspect per-frame/per-tile outline and occlusion error; their ground-truth edge sampling is being audited before using quantitative conclusions.

## preview.6 completion and identical-picture optimization

See [GPU optimization and validation](GPU-OPTIMIZATION.md). The native filter uses shared-context queued completion and official skip-warp history advancement for exact duplicate pictures. GPU-resident still includes GPU copies, blocking CUDA FRUC and a small scene-statistics readback; a fully asynchronous external-fence pipeline remains unfinished.

## preview.8 appearance and bounded motion

See [appearance and motion validation](APPEARANCE-MOTION.md). Midpoint reconstruction supports consistent motion up to 16 pixels per source interval. A separate local drawing-protection pass is vetoed by predominant view motion; it preserves source expressions only where appearance evidence is sufficient. There is no new frame lookahead, CPU video copyback, user-visible mode or OSD.


## P010 SDR input (beta.1)

A decoder can advertise NV12 in its DirectShow media type while delivering an actual P010 GPU texture. Validate the real texture, selected array slice and visible dimensions. P010 capture crops decoder padding on GPU, reads the upper 10 bits through integer Y/UV views, and rounds to NV12 code values: `min(255, ((word >> 6) + 2) >> 2)`. This preserves limited-range black/white and neutral chroma (64/940/512 -> 16/235/128) without an RGB conversion. Original-time frames are the normalized 8-bit frames; 10-bit precision is not retained.

P010 selects explicit D3D completion at capture/output and CUDA completion before interop release for that engine lifetime. The earlier queued conversion passed synthetic tests but failed in the real host at CUDA unmap. Conservative completion fixed the observed playback; the exact underlying driver/host cause has not been proven. NV12-only sessions retain context-ordered delivery. The path remains GPU-resident, with GPU copies and waits; it is not zero-copy or asynchronous NvOFFRUC.
