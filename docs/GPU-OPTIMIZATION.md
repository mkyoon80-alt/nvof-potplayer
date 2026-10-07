# GPU optimization — 0.2.0-preview.6

Local-only update. No GitHub publication. The accepted ×2 cadence, original-frame preservation, color metadata, source-rate choices, decoder, renderer and UI are unchanged.

## Changes

- The PotPlayer GPU path queues input snapshots and final NV12 draws on the shared D3D11 immediate context without a CPU completion wait after each operation. D3D resource/command ordering and CUDA map/unmap dependencies are retained. Context state and decoder mutex protection remain. Other engine callers default to blocking completion; context-ordered consumers must use the same immediate context or establish a cross-API dependency.
- Completely identical stored Y/UV input frames reuse the already uploaded CUDA picture. The new bridge Advance export calls official `bSkipWarp` once for the new input timestamp, maintaining FRUC history while omitting an unused warp. The output is the owned original texture. Even a one-byte Y, U or V change prevents this fast path. This is exact equality, not similarity, denoising or frame dropping.
- Identical-frame results do not invent NVIDIA repetition metadata. Diagnostics report `identicalFrames` and `gpuCompletion` separately.
- Teardown drains submitted D3D work before unregistering interop resources. Seek continues to reset/re-prime with fresh resource-address identity.

## Verification on RTX 5090, 2026-10-07

- Release build; 6 CTest suites passed.
- Bridge oracle: existing 27 direct SDK comparisons plus 16 skip-warp/history calls; resumed moving output matched direct official API output. A fresh resource-address fixture was necessary after an earlier test reused retired registrations; production already creates a fresh registration-address generation on prime.
- GPU engine: scene cut/recovery, flash handling, exact static NV12 colors, one-byte Y/U/V changes, retained surfaces, seek and shutdown passed.
- Concurrent renderer stress with mixed moving/held frames: protected-context mode 653 pairs / 436 skipped warps; decoder-mutex mode 4,168 pairs / 2,779 skipped warps. Each ran 20 seconds, 100 history resets and 4 engine recreations. Retained output and shared renderer state remained intact.
- Native DirectShow/MF: 24000/1001 → 48000/1001 and 60 → 120, metadata, seek, EOS, activation order and retained-manager lifetime passed. Source exclusion and master-off preserved native original-rate transport.

## Measurement and limits

`gpu_x2_benchmark` compares blocking/no-skip with context-ordered/skip in alternating order, three rounds of 36 source pairs at 1920×1080. Warmup excluded; final GPU completion included. Timing covers snapshot, detector, FRUC and output, excluding decode and VSR. It is a local synthetic microbenchmark, not a whole-player GPU/power measurement. Other desktop/player activity was not forcibly stopped.

- moving: 5.21 → 4.96 ms per source pair (4.9% in this run).
- held2: 4.85 → 4.63 ms per source pair (4.6% in this run).
- static: 4.66 → 0.47 ms per source pair (90.0% in this run).

The stable-picture case is the substantial improvement. Moving and alternating hold/motion cases have small, variable gains; do not promise a general speedup. Identical output is byte-exact. Independent unmodified FRUC sessions also show sparse moving-image differences; moving comparisons enforce full Y/UV changed-byte ratio <0.0002, MSE <0.005, and fewer than 8 interior-luma byte differences per synthetic frame. These bounds do not prove all real-world interpolation artifact behavior.

GPU-resident is still **not zero-copy**. GPU copies and Y/UV plane transfers remain for moving input. CUDA FRUC calls still block, and the small scene-analysis readback still synchronizes. The full external-fence/semaphore asynchronous pipeline is not delivered.

A direct DirectX shared-NV12/fence experiment was rejected: moving chroma differed substantially and extended history recreation failed registration. No such path is included in the product. `fruc_d3d11_probe` is a rejected research diagnostic, not an acceptance test. Artifacts are preserved under ignored `build/sync-opt/rejected-direct-nv12`.

Full-resolution test readbacks belong to harnesses only; the playback path does not copy full video frames back to CPU memory. Actual PotPlayer playback/VSR acceptance of preview.6 remains pending user confirmation.

## Local package

Built `dist/NvofPotPlayer-0.2.0-preview.6-win-x64.zip` (SHA256 `b9a5c52fc0f361350e13c0e1c02debcc690cfc0c88114c05676a444c0add973c`). Self-contained runtime dependency audit passed. Installation changes only `NvofPotPlayer.ax` and `runtime/NvofFrucBridge.dll`; controller, settings and registration are preserved. Installed after the user closed both applications. Filter and property-page COM activation, installed SHA256 values and unchanged settings were verified. Backup: `backup/x2-only-20261007-190657-077/restore.ps1`.

Sources: [NVIDIA FRUC guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvfruc-programming-guide/index.html), [CUDA graphics map/unmap ordering](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__GRAPHICS.html), and the locally acquired SDK 5.0.7 `NvOFFRUC.h` declaration of `bSkipWarp`.
