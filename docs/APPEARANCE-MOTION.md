# Appearance changes and camera motion — 0.2.0-preview.8

This update addresses torn animated mouth drawings at 03:20–03:22 and uneven vertical motion after the night-scene cut at 11:14 in the user's local sample. It keeps ×2 output, native NV12 transport, original frames, source-rate selection and the existing UI. Local source media and frame captures are not distributed.

## Midpoint motion

The guarded reconstruction limit expands from 4 to 16 source pixels per interval. Forward/backward and endpoint consistency, aligned 3×3 luma agreement and chroma checks remain unchanged. This lets the approximately 8-pixel vertical camera move use the midpoint reconstruction. It does not enable arbitrary phases or repeated submissions of the same input to FRUC.

The hard cut in this sample was already detected. This update does not lower the global scene-cut threshold or blend across cuts. FRUC and raw-flow histories still reset at the cut.

## Local appearance protection

A changed cel drawing is not necessarily a surface that can be continuously warped. Where several independent conditions indicate a compact appearance discontinuity, the midpoint preserves the previous local drawing. It does not synthesize a new lip pose.

- Four-pixel evidence tiles require a substantial stored-luma change, disagreement after attempted bidirectional alignment, and at least one relatively flat endpoint.
- A compact cluster is required; an isolated line or thin exposed border is insufficient.
- Reliable motion features are counted on GPU. Too little evidence or movement in the majority of reliable feature tiles disables appearance protection for the entire pair. A smaller moving component also vetoes protection when movement exceeds 2% of tracked features and at least 80% of moving features agree in neighboring direction bins at the same speed scale. The GPU histogram has eight direction bins and six speed scales. This is a conservative view-motion heuristic, not a semantic camera or face detector.
- A separable dilation includes surrounding contours and feathers the outer margin, avoiding a boundary through the mouth or jaw. Nearby reliable slow motion up to four pixels per interval can guide the source drawing. The global motion veto takes priority; unreliable local flow does not create holes in the protected neighborhood.
- There is no coordinate, character, filename or timestamp special case. The decision is rebuilt for each adjacent pair and has no extra frame lookahead.

This trades invented mouth motion for preserving a known source expression. It intentionally does not fix every deformation during camera movement. Ambiguous motion, small details, occlusions and missed appearance changes can still retain FRUC artifacts. Earlier variants that degraded camera pans or moving occlusions were rejected before installation.

All added analysis and rendering use D3D11 GPU resources. No additional playback readback is used; the pre-existing small scene-statistics readback remains. `diagnostic_motion_evidence()` explicitly reads a 50-counter statistics texture and returns eight summarized counts only in standalone tests and is never called by playback. GPU copies and blocking CUDA FRUC remain; this is not zero-copy or fully asynchronous processing.

## Validation and scope

The local diagnostic compares source A, the old midpoint, the candidate midpoint and B, including a second dialogue near 06:00 with a smaller mouth. The original source images are checked byte-for-byte. Independent synthetic tests cover opening/closing drawings at three sizes, stationary views and camera pans; pan output must match the protection-disabled result exactly. Existing translation, occlusion, wire, fence and illumination tests retain their quality limits.

The final candidate preserves the previous 23:19 slow-hair-motion improvement: approximately 48.19% median insertion position and 0.0453-pixel median midpoint error over 96 pairs. In the 11:14 night pan, independent tracking measured median midpoint error around 0.0697 pixels before and 0.0122 after, with approximately 50% insertion position. Tracking estimates are not ground truth for every pixel and do not measure live presentation timing.

The area/rate budget policy is unchanged: 4K 23.976/24/25/29.97/30p remains eligible for extra correction; larger-than-1080p input above approximately 30 fps uses ordinary FRUC. Eligibility does not guarantee enough time for decoding and VSR. The prior 4K pass already approached the 33.3-ms budget for 30p. On the RTX 5090, the final quiet-GPU diagnostic measured 10.679 ms per source pair at 1080p and 33.864 ms at 4K, including completion of queued work but excluding decoding and VSR. Plain FRUC comparison was 4.753 and 14.381 ms respectively. These are one isolated 19-pair measured run after warm-up, not live-player guarantees. The first-pair initialization in the 1080p clip diagnostic increased from about 269 ms in preview.7 to 594 ms in this build, including shader initialization; that startup cost is excluded from steady-state timing. A 4K30 stream therefore has insufficient demonstrated headroom for decoding and VSR; eligibility is retained, not a smooth-playback claim. Records are saved in ignored `build/appearance-update/`.

The final candidate passed 34 translation/detail/occlusion cases and 18 appearance cases. All 477 source frames across the four local clips were preserved byte-for-byte (954 total output frames). The night-pan tracking result and 23:19 slow-hair result above were reproduced with the final candidate. Selected broken mouths at 03:20 and the smaller 06:00 dialogue were visually inspected against A/old/new/B. This is sample validation, not a guarantee for all content.

Final 1080p/4K engine checks passed, including retained GPU output leases, exact static colors, scene/flash handling, compute binding restoration, seek/reset and shutdown. The 4K30 eligibility assertion and 4K60 bypass assertion passed. Two shared-context stress modes each completed 100 resets and four engine recreations. DirectShow color metadata, 23.976-to-47.952 timestamps, GPU negotiation/transport and lifetime checks passed; six core CTest suites passed.

Build identification is included in runtime status as `buildVersion` to distinguish installed playback from development experiments. The installed filter and package hashes matched, existing preferences were preserved, and registration verification passed. After installing preview.8, the tester reported a substantial improvement. This is positive feedback from one configuration, not universal acceptance. The public v0.2.0-preview.8 release publishes project source; the complete local binary package remains withheld pending FRUC redistribution clarification.
