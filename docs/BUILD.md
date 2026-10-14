# Native standalone build

Build tools are needed only by developers. Users install the filter and self-contained controller.

## Requirements

Visual Studio 2022 C++, Windows SDK, CMake 3.20+, Git and official NVIDIA Optical Flow SDK API headers. CUDA Toolkit, NvOFFRUC.h and FRUC binaries are not required. Obtain the SDK from NVIDIA under its terms; do not commit or bundle the SDK headers.

~~~powershell
.\tools\fetch-build-deps.ps1
.\tools\build.ps1 -BuildDirectory build\release0410\native -OpticalFlowSdkIncludeDirectory "C:\SDKs\Optical_Flow_SDK_5.0.7\NvOFInterface"
.\ui\build-ui.ps1 -OutputDirectory ui\staging-release0410
~~~

The filter and register helper use static MSVC /MT. Shader bytecode is generated at build time. CMake never finds CUDA or builds the old FRUC bridge in this profile. Historical FRUC-specific research tests remain as source references but are not active build targets.

Supported CPU NV12 SDR and 10-bit samples upload to the GPU and return GPU surfaces without full-frame readback; see [CPU-10BIT.md](CPU-10BIT.md). GPU texture playback uses the decoder/renderer device directly. Both use the same motion synthesizer.

## Package and manual

~~~powershell
.\tools\build-local-package.ps1 -Version 0.4.10 -NativeDirectory build\release0410\native\Release -UiDirectory ui\staging-release0410
python tools/build-manual-site.py --output build/release0410/manual-site
python tools/build-manual-assets.py --site-dir build/release0410/manual-site --version 0.4.10 --language ko --output dist
python tools/build-manual-assets.py --site-dir build/release0410/manual-site --version 0.4.10 --language en --output dist
~~~

The package includes the published .NET/WPF controller, native filter, register helper, settings, manual and notices. FRUC/CUDA and NVIDIA driver files are rejected. v0.4.10 is distributed as a standalone ZIP. The optional developer tool tools/build-installer.ps1 still supports Inno Setup 7.1.0; the compiler is not shipped.

Old runtime acquisition scripts are unnecessary. tools/install-phase-upgrade.ps1 is retired to prevent accidental installation of FRUC. Earlier acquisition evidence remains in config/legacy-runtime-manifest.json and Git history, not in the current package.

## Validation

Experimental live numeric tuning is available with `tools/build.ps1 -ExperimentalTuning`
or CMake `-DNVOF_EXPERIMENTAL_TUNING=ON` in a separate build directory. The default
is OFF. See [TUNING.md](TUNING.md) for the parameter inventory,
pair-boundary application, local MCP/CLI controller and verification commands.

tools/build.ps1 runs 23 core, parser, conversion and shader validation tests in the standard build. Experimental builds add tuning-channel checks. Explicit GPU tests include native_cost_map, native_motion_layers, native_motion_guard, native_cpu_transport, gpu_interop_stress and NV12/P010 DirectShow tests. Use an absent runtime path for native engine tests.

runtime_dependencies.exe audits a package for removed FRUC/CUDA/driver files and loads its filter. Import-table inspection verifies that product binaries do not depend on CUDA or FRUC.

tools/test-release-installer.ps1 tests installation without NVIDIA assent parameters, same-folder upgrade cleanup, retained files/settings, COM registration, self-contained controller and removal/ownership safety. Run with the player and controller closed; it restores prior user registration.

Session lifetime optimization and local regression commands: [SESSION-REUSE.md](SESSION-REUSE.md). This optimization is included in v0.4.4.

Repeated-seek preview and resume regression: `directshow_gpu_smoke <filter.ax> --scrub-preview`.
The filter forwards originals for 200 ms after a seek before resuming interpolation.
See [SEEK-PREVIEW.ko.md](SEEK-PREVIEW.ko.md) for scope, validation and playback acceptance status.

## Native P010 / HDR checks

Use the normal Release build, with precompiled shaders. Run `gpu_p010_conversion.exe <runtime> native` for code-value/phase tests. `directshow_gpu_smoke.exe <filter.ax> --rate-policy 24000 1001 120000 1001 mf selected <format>` accepts `p010`, `p010-media`, `hdr10`, `hlg`, `av1-pq`, `av1-hlg`, `hdr10-legacy`, `hlg-legacy`, and `sdr-override` fixtures. The first uses an NV12 media header with real P010 textures, the second a P010 media subtype. The HDR fixtures verify exact extended color flags and sample side data, including pool lifetime. Legacy fixtures exercise missing raw color flags with connected HEVC source signalling; sdr-override verifies that explicit SDR stays SDR. Run hevc_color_tests.exe for bounded SPS parsing, optionally passing a locally captured header file. Keep test LOCALAPPDATA separate from the real player's status directory.

For explicitly supplied local media, `p010_video_stress.exe <runtime> <1920x1080-p010.raw> queued basic p010 concurrent` exercises shared D3D state/lifetime. It expects at least eleven tightly packed P010 frames; it is not a color-management or real-player display test. Do not package media or test logs. See HDR-NATIVE-TRIAL.md for results, confirmed HDR10 playback ; newer VP9, Dolby metadata and HLG evidence is in VP9-HDR.md and DOLBY-VISION-HLG.md.

## CPU NV12 / HW-SW switching checks (0.4.6)

Run `directshow_cpu8.exe <filter.ax>` against a separate test folder configured
with `NativeD3D11=1`, `Enabled=1`, `InputRateMask=63` and `OutputFpsLimit=120`.
It checks repeated P010 HW → NV12 SW → HW reconnection with the same renderer,
cold start with VideoInfo and VideoInfo2, padded upload, exact originals, retained-frame lifetime, malformed
input, seek, pause, restart and EOS. An optional `<raw.nv12> <width> <height>`
replays 48 tightly packed decoded NV12 frames. Keep test `LOCALAPPDATA` isolated
from the real player's logs. See [SW-DECODING.md](SW-DECODING.md).

The CPU NV12 fixture requires renderer device activation before the initial allocator notification, matching the failing startup order. The v0.4.5 baseline fails that connection; v0.4.6 passes. Native decoder activation before and after output connection is covered by `directshow_gpu_smoke`.

## v0.4.7 regression coverage

See [PLAYBACK-PERFORMANCE.md](PLAYBACK-PERFORMANCE.md), [SOFTWARE-HDR-TRIAL.md](SOFTWARE-HDR-TRIAL.md), and [THIN-OVERLAY-TRIAL.md](THIN-OVERLAY-TRIAL.md) for shared-context, HDR allocator, and thin-line controls. Stop player/GPU workloads before GPU tests. Keep test logs and source media out of release packages.

## v0.4.8 adapter-selection coverage

Run `adapter_selection_tests.exe` for CPU-only selection policy checks.
`directshow_cpu8.exe <filter.ax> --adapter-hint-regression` injects a non-NVIDIA
and invalid renderer hint while checking SW starts and retained P010 HW/SW
connections. `--adapter-nv12-regression` checks NV12 HW/SW cycling with an
invalid hint. The first test deliberately fails against released 0.4.7.
Use a separate test INI/LOCALAPPDATA, and close players before GPU tests.
See [ADAPTER-SWITCH-TRIAL.md](ADAPTER-SWITCH-TRIAL.md) for physical-PC verification.

## v0.4.9 scheduling, resolution and experimental tuning

The default build has `NVOF_EXPERIMENTAL_TUNING=OFF`. Release configuration explicitly uses `TracePerformance=0` and `ProbeServices=0`. A retained user INI is not silently rewritten by the release.

CTest includes resolution rules, output buffer policy, status writing, GPU wait service, timing, motion and CPU upload tests. Use a separate INI and LOCALAPPDATA for `directshow_gpu_smoke`: its `--pause-resume`, `--async-lifecycle`, `--preroll-prepare`, `--wait-output` and `--stream-bench` modes cover lifecycle, immediate first-picture delivery and prepared output. Stop players before GPU tests. Pixel-hash runs include readback and must not be treated as performance benchmarks.

For the experimental MCP schema and application boundaries, see [TUNING.md](TUNING.md). Rejected startup experiments do not enter the release. Actual startup drop counts and real-player frame pacing require player-level verification; synthetic throughput alone does not establish either.
