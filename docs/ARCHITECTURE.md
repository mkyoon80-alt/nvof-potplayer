# Pipeline and validation

## Video path

```text
PotPlayer built-in decoder (D3D11)
    → original NV12 GPU textures
    → NvofPotPlayer DirectShow filter
        → owned GPU snapshots
        → NVIDIA NvOFFRUC midpoint
        → native ×2 cadence or 60/120 target-clock resampling
    → PotPlayer built-in D3D11 renderer
        → VSR, when enabled in the player
    → display
```

Interpolation precedes VSR. Reordering external filters cannot move VSR out of the renderer. The native path keeps frame pixels on the GPU, although GPU-to-GPU copies and synchronization are still required; this is not a zero-copy claim. A compatibility system-memory path exists and is labeled distinctly in diagnostics.

NvOFFRUC generates one midpoint between adjacent original frames. ×2 uses that cadence directly. 60p and 120p resample that timeline to the target clock; not every output sample is a separate optical-flow estimate. The implementation is independent of Smootter and does not contain its binaries or source.

## Timing, seeking, and lifetime

The scheduler uses rational source rates and 100-nanosecond DirectShow timestamps. NTSC sources retain their fractional cadence. Source-rate exclusions and sources at or above a fixed target retain original timestamps.

Flush, discontinuity, and a new segment invalidate previous history. The first original frame at the new location is delivered without waiting for a midpoint; interpolation is primed again from new adjacent frames. Held output textures remain valid until the downstream renderer releases their samples. Worker resources are drained and released during stop and graph teardown.

The native connection negotiates decoder surfaces through Media Foundation / DirectShow contracts. GPU samples do not expose a hidden CPU readback through `GetPointer`. The built-in renderer receives NV12 surfaces with explicit range and color metadata. Legacy input metadata is promoted to VideoInfo2 for this native output path; supported SDR defaults are supplied only where unknown. Explicit incompatible color information is rejected.

## Validation completed for this preview

- Automated source-rate policy, output cadence, scheduler/seek, and GPU pipeline tests.
- RTX 5090 synthetic NV12 tests: static luma/chroma preservation, fixed-point GPU blending, texture-array slices, retained outputs, reset/re-prime, and shutdown.
- DirectShow/MF harness: native negotiation, activation failure, CPU compatibility, metadata, flush/seek, and COM lifetime.
- Actual PotPlayer x64: 1080p 23.976 → 119.88 fps and ×2, built-in decoder/renderer, VSR, forward/backward seeking, and clean exit.
- Color-range regression: synthetic static samples remain byte-exact. Matched static regions of rendered captures agreed to within approximately one 8-bit level after the metadata correction. This is not a claim that interpolated moving pixels equal originals.
- Controller: settings persistence, source mask / ×2 switching, status parsing, process identity, registration routing, and bounded layout/focus checks.

The current validated media scope is progressive 8-bit limited-range BT.709 NV12. HDR/P010, interlacing, mixed-GPU systems, arbitrary in-stream format changes, and long-duration stress across every codec are not established. Motion-estimation artifacts, scene-cut fallback, and temporal blending remain possible. Offline UI rendering verifies logical layout and raster scaling, not physical monitor transitions.

The release package is checked separately for its bundled runtimes. Tests load a staged filter by explicit path and do not replace an existing user's COM registration.
