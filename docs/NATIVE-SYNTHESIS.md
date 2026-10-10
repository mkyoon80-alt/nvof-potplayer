# Native synthesis in 0.3.0

The default playback backend is lab11: NVIDIA NVOFA estimates bidirectional flow, then custom D3D11 passes synthesize the midpoint directly from original luma/chroma. The default analysis dimension is at most 1920; output synthesis retains the original resolution. The original NV12 frames remain unchanged. P010 SDR is normalized to 8-bit NV12 before synthesis.

The inverse coordinate solver uses six bounded Newton iterations with backtracking, plus alternative seeds for inconsistent mappings. Motion repair and a stationary-layer mask protect uncertain boundaries and static graphics. Scene cuts and widespread correspondence failures can hold an original frame. The optical-flow session is reset for each moving pair to avoid stale-flow behavior observed on the validation driver. The product emits one midpoint per pair (×2), not arbitrary target FPS.

The legacy NvOFFRUC path remains available for development comparisons (`ExperimentalNativeSynthesis=0`). New installs and upgrades set `ExperimentalNativeSynthesis=1`; absent keys also default to native. Legacy midpoint/appearance switches do not control the new synthesis guards. No Picard changes are included in v0.3.0.

## Startup

Twelve native playback shader entry points are compiled at build time with the same optimization settings used by lab11. The installed filter embeds their bytecode. This removes the approximately 3.2-second HLSL compilation measured on the development PC; it does not eliminate decoder, driver, optical-flow session or player startup costs.

## Limitations

Validation hardware: RTX 5090. There is no RTX 5070 4K60 → 120 realtime or BFRC superiority claim. Residual errors remain on hair, overlapping motion, title boundaries and smoke. HDR, 10-bit output, interlace and multi-GPU playback are outside the supported scope. Real clips are local fixtures and are not distributed.

## Release validation

- All six CTest scheduler/pipeline checks pass.
- Twelve local video fixtures (1824 output frames) are byte-for-byte identical to the runtime-compiled lab11 baseline, including all generated pixels and timestamps. They include NV12, P010, static titles, anime hair/overlap, scene boundaries and separate held-out frame pairs.
- GPU layered-motion and ambiguous-blur guards, original preservation, continuous-motion comparisons, 100 resets and concurrent-context stress pass.
- DirectShow NV12/P010 negotiation, rounded frame durations, timestamps, EOS, repeated seeks and transport checks pass. Validation uses a dedicated adjacent runtime folder for the legacy/system-memory test cases.
- Native engine construction after D3D11 device creation: first 8.071 ms; subsequent 0.967–1.351 ms (five runs). This excludes device, decoder, frame preparation and end-to-end playback startup.
- Installer tests pass: consent rejection, existing settings migration, update backup, COM activation, bundled runtime audit, UI self-test, successful removal without reviving an old filter, retention of another folder's registration, and registration-helper ownership refusal.
- Actual machine registration → uninstaller-requested elevation → removal passes, with original HKCU/HKLM registrations restored after testing. Simulated helper failure returns failure without deleting the filter; retry succeeds.
- Settings and connection tabs were rendered and checked at the normal window size. The removal and installation-folder buttons are visible without scrolling.
