# Engine quality research — 2026-10-13

[한국어 보고서](ENGINE-RESEARCH.ko.md)

This is development evidence and an execution plan, not a released engine change.
The baseline is **v0.4.8, `47bb73b74039538e16ab63e98934ce21e39bc48b`**.
Research is on `codex/engine-quality-research`; installations and user INIs are
not research outputs. Private media and logs stay in ignored `build/`.

## Current implementation and remaining failure modes

The current engine estimates bidirectional NVOFA flow at a maximum long side
of 1920, prefers grid 4 and Medium, and disables cost maps and temporal hints
by default. Shared-resource sessions persist between compatible pairs.
`motion.hlsl` repairs selected aliases, inverts each flow with bounded Newton
iterations, scores the two source positions, and composes original-resolution
Y/UV samples. Original P010 samples and generated output retain the existing
10-bit path. Arbitrary scheduled intermediate phases already work; historical
2x-only and 8-bit-only documents do not describe this baseline.

Three mechanisms must be separated:

1. **Correspondence:** repeated texture or a sub-grid object can obtain the wrong
   flow. Forward/backward agreement alone does not disambiguate periodic motion.
2. **Visibility:** an actually visible source surface can be covered at the other
   endpoint. Failed endpoint photometry does not prove that this source is wrong
   at the requested intermediate time. Conversely, agreement can select a hidden
   background branch rather than the foreground.
3. **Reconstruction:** bilinear flow crosses motion layers; inverse branches and
   confidence can be wrong even when the numerical inverse residual is tiny.
   Upsampling inverse maps must not average unrelated coordinates. The existing
   discontinuity handling is preserved.

The prior narrow-gap investigation found tiny inverse residuals at selected
failures while both absolute correspondence scores were nearly zero. Relative
normalization still gave one invalid source almost all the weight. This is
evidence against treating extra Newton iterations as the primary fix for those
pixels, not proof of a single cause for all clips. Neither a hard low-confidence
hold nor a global endpoint blend supplies missing visibility information.

The fixed-line protection is a separate accepted path. Its previous success
does not establish a fix for moving thin objects or newly exposed background.
Previous grid/preset, flat-background, donor-extrapolation and extra-seed trials
are recorded in [THIN-OVERLAY-TRIAL](THIN-OVERLAY-TRIAL.md),
[FLOW-PROFILE-TRIAL](FLOW-PROFILE-TRIAL.md), and local handoff evidence.
Those experiments have not been silently reclassified as successful.

## Engine alternatives

The table describes integration prospects, not a local ranking by measured
quality. External benchmarks use different GPUs, models, resolutions and
timing boundaries; they are not directly comparable to our pair measurements.

| Candidate | Quality hypothesis | Real-time cost / reuse | 10-bit and HDR | Deployment and license |
|---|---|---|---|---|
| Current NVOFA + targeted D3D11 changes | Can improve source ownership or mixed-layer sampling; cannot recover motion that has no usable evidence | Flow and masks reused across all phases; extra per-phase sampling must be measured | Existing P010/HDR transport stays available | No new runtime; project MIT, NVIDIA API under SDK terms |
| NVOFA + visibility-aware forward splatting | Avoids inverse branch selection; collisions explicitly combine or select projected sources | New scatter/resolve passes, holes and collision buffers; still needs correct motion and visibility | Can sample original P010 with our own shaders | Independent D3D11 implementation possible; do not copy academic-only reference code |
| NVIDIA NvOFFRUC | Supplies vendor validation, flow infilling and image hole filling | Official indicative 1080p timing is 4.41 ms on RTX 4090; not our hardware or playback test | Documented inputs are NV12/ARGB, not native P010; direct replacement would require a new precision strategy | Reintroduces FRUC/CUDA components and NVIDIA SDK redistribution terms |
| RIFE / Practical-RIFE via TensorRT or another float inference backend | Learned intermediate flow and blending can handle boundaries differently; local anime and real-video benefit unproven | Arbitrary phases supported; reference v4 inference runs per phase, so high multiples are costly; optimized graph must be measured | Float inference can carry more than 8 bits, but the reference video I/O converts to bytes. Need explicit P010↔RGB, transfer/range handling and metadata preservation | Code and linked Practical-RIFE models declared MIT; CUDA/TensorRT integration, model/cache storage and backend terms are additional work |
| RIFE via ncnn/Vulkan | Useful portable independent comparator; model set differs from current Practical-RIFE | No CUDA/PyTorch runtime, but new Vulkan queue and D3D11 interoperability/copies | Stock image-oriented path is not evidence of end-to-end P010/HDR preservation | ncnn/Vulkan plus weights; wrapper MIT, retain dependency notices |
| GMFSS_Fortuna | Anime-oriented flow/feature synthesis is a useful quality comparator | More components and PyTorch/CuPy reference stack; no verified local real-time budget | Float model is not an HDR pipeline; require same range/precision validation | Repository MIT; audit selected weights and all component licenses before packaging |

Primary sources, checked 2026-10-13:

- [NVOFA programming guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html)
  and [application note](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note/index.html).
  The vendor's timing is indicative, for 1920x1080 NV12 with its stated driver
  and clocks; our 5090, 4K and high-multiple behavior must be measured locally.
- [FRUC programming guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvfruc-programming-guide/index.html)
  documents NVOFA/CUDA, supported formats, validation and hole filling.
  [SDK license](https://docs.nvidia.com/video-technologies/optical-flow-sdk/license/index.html)
  is distinct from this project's MIT license.
- [RIFE paper and reference project](https://github.com/hzwer/ECCV2022-RIFE)
  reports 30+ fps for 2x 720p on a 2080 Ti and arbitrary timesteps. This is not a
  claim about 4K/120 or a modern Practical-RIFE model on this machine.
- [Practical-RIFE](https://github.com/hzwer/Practical-RIFE) declares linked models
  under MIT and lists 4.25 and lite options; its
  [video inference code](https://github.com/hzwer/Practical-RIFE/blob/main/inference_video.py)
  loops over phases and converts generated RGB to bytes. That conversion is
  unsuitable for our precision requirement without replacing the adapter.
- [vs-mlrt RIFE integration and benchmark](https://github.com/AmusementClub/vs-mlrt/wiki/RIFE)
  provides a float RGB TensorRT route. Its old v4.4/3090/1920x1088 benchmark is
  useful integration evidence, not a current latency guarantee.
- [rife-ncnn-vulkan](https://github.com/nihui/rife-ncnn-vulkan),
  [its license](https://github.com/nihui/rife-ncnn-vulkan/blob/master/LICENSE),
  [GMFSS_Fortuna](https://github.com/98mxr/GMFSS_Fortuna), and
  [its license](https://github.com/98mxr/GMFSS_Fortuna/blob/main/LICENSE).
- [Softmax splatting paper](https://arxiv.org/abs/2003.05534) motivates explicit
  collision handling; [the author's implementation](https://github.com/sniklaus/softmax-splatting)
  states academic-only use. Algorithmic investigation and permission to copy
  that implementation are separate questions.

## First bounded experiment: surface1

Hypothesis: `warpReliability` uses a 3x3 patch sampled every four analysis
pixels. Near a boundary this includes both motion layers and can suppress a
correct source because its neighbors belong to another surface. The experiment
weights those taps by agreement with the center's source flow. At least four
effective taps are required; otherwise it keeps the baseline patch metric.
It changes only the photometric component of warp reliability. Source repair,
Newton solve, masks, transport and scheduling remain as in v0.4.8.

This is a diagnostic hypothesis, not a complete visibility model: it can also
over-trust a wrong but coherent motion. Keep it outside the production shader
unless ground-truth, real-video and performance checks justify adoption.
The experimental shader, preparation script and fresh baseline binaries are
under `build/engine-research-20261013/`.

The second bounded candidate, **stretch1**, retains the old patch metric and
changes only its geometric penalty. The determinant measures area, so a matrix
such as diag(4, 0.25) has unit area despite severe directional stretch. The
candidate also measures the largest singular value of the intermediate warp
Jacobian. The existing mismatch gate and 1.5 threshold remain. Unit translation
and well-matched patches retain the old weights; this still cannot infer depth
or make an occluded correspondence valid. It needs the same regression gates.

## First-pass results

Both candidates are **rejected for adoption**. Production files under `src/`
and `include/` remain identical to v0.4.8. The accepted deliverables are the
independent comparison harness and this research record, not a new filter.

Fresh baseline build: all 19 CTest tests passed, as did the existing eight-scene
layer suite at x2/x3/x4/x5, phase position and original-frame checks. A baseline
versus baseline run of the new harness was identical on closing-gap and
plain-translation cases at x2/x5 (four comparisons).
The fresh baseline also passed `gpu_p010_conversion`: all 1024 code values,
crop/UV handling, decoder reuse, state/seek checks, moving x5/x10 precision,
and 4K x10 queued-versus-blocking pixel equality. This is baseline engine
validation, not a new actual-player HDR or HW/SW test.

The new suite ran eight scenes at two multiples for each candidate. Selected
MSE results, computed only against analytic intermediate truth:

| Case / region | v0.4.8 | surface1 | stretch1 |
|---|---:|---:|---:|
| Closing gap x2 / boundary | 301.4173 | 316.7064 | 298.9498 |
| Opening gap x2 / exposed background | 68.5347 | 69.3407 | 73.0242 |
| Opening gap x5 / exposed background | 33.8096 | 34.3471 | 34.9765 |
| Moving sliver x5 / detail | 1182.3649 | 1303.8893 | 1181.4401 |
| Bright object x5 / detail | 117.5534 | 136.4673 | 116.0616 |
| Curved occluder x5 / boundary | 28.2047 | 33.4515 | 27.9245 |
| Fixed line x5 / detail | 176.3245 | 176.3559 | 176.3245 |

surface1 regresses moving detail substantially; restricting the patch does not
make its motion estimate correct. stretch1's small gains on some boundaries
do not offset exposed-background regressions of approximately 6.6% at x2 and
3.5% at x5. Their original pixels, timing and pair decisions were preserved;
this alone is not a sufficient quality gate. Translation is unchanged for both.

Six local real-video comparisons covered the reported moving-object and
fixed-line animation excerpts, a live-action narrow gap, two additional anime
excerpts, and a 4K live-action frame holdout: 257 input pairs per engine.
Both candidates preserve baseline timestamps and pair decisions. NV12 originals
are byte-exact. P010 originals are equal at the harness's diagnostic high-byte
precision; this is not a new validation of all 10 bits.

Inspection of the selected problematic crops still shows the frayed moving
boundary and damaged narrow-gap background. Changes are small and provide no
clear visual fix. Fixed-line remnants also remain. These comparisons have no
true intermediate frames and are not scored as improvements.

The independent 31-frame real holdout had full-frame luma MSE
**95.0940 → 95.1360 (surface1) / 95.3670 (stretch1)**. Its selected ROI changed
**137.3538 → 137.4279 / 137.8144**. All are slightly worse. This uses every
second source frame as input and therefore twice the normal input interval;
the diagnostic is 8-bit luma even though source transport is P010.

Local evidence: `build/engine-research-20261013/{baseline-ctest.log,
baseline-layers.log,harness-self-check,surface1-analytic,stretch1-analytic,
real-baseline-surface1-stretch1}`. Full images, source paths and logs are not
included in the public repository. Neither candidate progressed to product
integration or installation; new candidate HDR/adapter/playback certification
would be inappropriate after failing the earlier quality gate.

### Performance scope

On RTX 5090 / driver 617.42, a first real-clip short timing pass (seven measured
pairs per run) varied too much to estimate overhead: some 4K x2 runs averaged
about 17 ms while others averaged about 3.8 ms. No cause is established and no
speed claim is made from those results. The original records are retained in
`performance/`; they are not silently replaced by the better-looking run.

A second bounded test preloads eight NV12 images, cycles their motion, warms
eight pairs, and measures 64 pairs through GPU completion without frame-file
output or per-phase CPU readback. Each configuration runs baseline, surface1,
stretch1, stretch1, surface1, baseline sequentially. The table gives the two-run
range of drained **milliseconds per input pair**, including all generated phases:

| Size / multiple | v0.4.8 | surface1 | stretch1 |
|---|---:|---:|---:|
| 1920x1080 / x2 | 2.689–2.986 | 2.775–2.804 | 2.691–2.915 |
| 1920x1080 / x5 | 3.425–3.499 | 3.688–3.738 | 3.409–3.464 |
| 3840x2160 / x2 | 3.726–3.793 | 3.764–4.126 | 3.698–3.779 |
| 3840x2160 / x5 | 5.226–5.527 | 5.144–5.555 | 4.848–5.326 |

surface1 is slower in this 1080p x5 sample; other ranges overlap or show order
variation. This does not establish a generally faster candidate. These are
synthetic NV12 engine measurements, not decoding, P010 playback, display drops,
or a guarantee under shared-renderer contention. GPU clocks and desktop activity
were not fixed. Neither candidate allocates additional textures in its source;
whole-process VRAM was not measured. Logs, the bounded benchmark source and its
build script are under `performance-drained/` and the local research directory.

## Comparison protocol

`tools/compare-occlusion.py` renders continuous-time geometry independently of
both engines. Cases include opening/closing gaps, moving slivers, small bright
objects, repeated background, a curved occluder, plain translation and a fixed
line. It checks x2/x5 originals, all output timestamps and pair decisions and
reports separate MSE for boundaries, visible background and moving detail.
No endpoint average is used as truth. A >3% + 0.01 MSE regression is flagged;
passing this gate alone does not mean a candidate improves quality.

The harness also checks idle processes before each GPU invocation, keeps
LOCALAPPDATA isolated, records executable hashes, and writes only to a newly
created directory under `build/`. It keeps selected samples while deleting only
its own disposable raw output unless `--keep-raw` is set.

```powershell
python tools/compare-occlusion.py --baseline <baseline-video_x2_repro.exe> --candidate <candidate-video_x2_repro.exe> --output build/new-occlusion-comparison
```

Next validation levels:

1. Independent analytic cases and existing eight-scene/multi-phase layer tests.
   Inspect region errors even if full-image error improves.
2. Actual reported clips and additional animation/live-action controls. Compare
   crops and motion over time; without intermediate truth, do not call changed
   pixels an objective improvement. Keep private images local.
3. Real-frame holdout against decoded originals, explicitly labeled as a test
   with twice the normal input interval. Keep scene/hold decisions visible.
4. Sequential repeated 1080p/4K timings, x2 and high multiples, memory and GPU
   completion. Pair throughput is not player/display frame rate.
5. Only a surviving candidate proceeds to P010 precision, HDR signalling,
   HW↔SW/adapter selection, shared-context lifetime and playback confirmation.

The first neural comparison should use pinned RIFE 4.25 and a lite variant in
an offline adapter before adding a user-facing engine selector. Measure source
transfer, color conversion, all requested phases and return to D3D11. Preserve
original samples outside the neural path. A flow/mask-only neural hybrid is a
separate candidate because omitting the model's final reconstruction may change
its quality.

For an input rate S and output rate T, the workload is approximately
`S * pair_setup_cost + (T-S) * generated_phase_cost`. At 24→120 there are four
generated phases per pair; 24→240 has nine. The latter has at most 4.63 ms per
generated phase before pair setup, decoding/rendering and contention are
accounted for. These are budget calculations, not measured engine speeds.
No engine is selected solely from an isolated one-midpoint benchmark.
