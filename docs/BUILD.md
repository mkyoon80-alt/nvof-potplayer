# Build from source

Playback users can use the self-contained Windows x64 installer. Build tools are needed only to compile the source. NVIDIA, CUDA and Microsoft components remain under their own terms; see the distribution basis and unresolved license interpretation in [licenses/README.md](../licenses/README.md).

## Native filter

Install Visual Studio 2022 with Desktop development with C++ and a Windows SDK, CMake 3.20+, Git, and the CUDA Toolkit (driver API headers and import library). The native code uses the CUDA driver API; users do not need the Toolkit installed. Building NvofFrucBridge and the fractional refiner also requires the official Optical Flow SDK 5.0.7 headers, acquired from [NVIDIA](https://developer.nvidia.com/opticalflow/download) using your developer account. Extract the SDK outside tracked source (for example under ignored `third_party/OpticalFlowSDK`). Proprietary SDK headers are not vendored in this repository.

```powershell
.\tools\fetch-build-deps.ps1
.\tools\build.ps1 -FrucSdkIncludeDirectory "C:\SDKs\Optical_Flow_SDK_5.0.7\NvOFFRUC\Interface"
```

The build needs `NvOFFRUC/Interface/NvOFFRUC.h` and `NvOFInterface/nvOpticalFlowD3D11.h` / `nvOpticalFlowCommon.h`. The standard SDK layout is discovered from the FRUC include path; for a separate layout pass `-OpticalFlowSdkIncludeDirectory` or CMake `NVOF_API_INCLUDE_DIR`. The raw optical-flow API loads `nvofapi64.dll` from the installed NVIDIA driver in System32; no new app runtime DLL is bundled.

The fetch script pins Microsoft Windows-classic-samples at `434f6002bdf9cf9829406c3ff2b33387982d6168` and checks out only its DirectShow baseclasses and license. It refuses to overwrite a different or modified checkout. The build produces `build\Release\NvofPotPlayer.ax`, `NvofRegister.exe`, `NvofFrucBridge.dll`, and test executables. The project links its own C++ code and baseclasses with the static MSVC runtime (`/MT`).

## Controller

See [ui/README.md](../ui/README.md) for the self-contained Windows x64 controller build. Its published folder includes the .NET desktop runtime; an end user does not need to install .NET separately. Do not trim WPF or copy only the small launcher executable out of the published folder.

## Runtime and release packaging

The filter dynamically loads its NVIDIA interpolation components from a sibling `runtime` directory. Third-party license terms apply independently of this repository's MIT license. See [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

After reviewing the external licenses, acquire the pinned runtime and build the UI and local package:

```powershell
.\tools\fetch-runtime.ps1 -AcceptThirdPartyLicenses
.\ui\build-ui.ps1
.\tools\build-local-package.ps1
```

The acquisition script uses a SHA256-pinned NVEnc release and checks all extracted DLLs. It also needs the pinned licensed Visual C++ release DLLs from the build machine's Visual Studio `VC/Redist` folder. You can pass `-VcRedistDirectory` explicitly; a different version is refused until the manifest is reviewed and updated. None of these build tools are required on the playback machine. The complete ZIP is generated in `dist/` as a local installer payload. Public releases provide the app installer with explicit NVIDIA component consent, not a standalone DLL or portable runtime ZIP.

The package script uses explicit release inputs and an isolated `dist` directory. It includes the controller publish output, native filter and registration utility, default INI, runtime components and license notices. It never copies local playback logs, personal settings, backups or test screenshots. Keep the complete extracted directory together.

The project-built bridge replaces the historical NVEnc adapter in the complete package. Five vendor DLLs are hash-pinned; the bridge hash is recorded in each package manifest. The official SDK 5.0.7 ZIP used for validation has SHA256 `89b0923adc6f34fbe86e63cc17d5db452e34b945c03ba90d8e09bd1a0158e917`; its NvOFFRUC.h hash is `44de0430d90ae3f8453ea8907c040d470e4060bc56ac2835702ffd4c6077e5f2`.

## Tests

`tools/build.ps1` runs the CPU-only CTest suite after building. NVIDIA hardware is needed for the additional GPU tests:

```powershell
.\build\Release\enhancement_options_gpu.exe <staged-runtime>
.\build\Release\fruc_bridge_smoke.exe <staged-runtime>
.\build\Release\phase_engine_smoke.exe <staged-runtime>
.\build\Release\phase_endpoint_smoke.exe --schedule-only 240
.\build\Release\phase_endpoint_smoke.exe <staged-runtime> 32 640 360
.\build\Release\multi_phase_quality.exe <staged-runtime> 12
.\build\Release\fruc_surface_probe.exe <staged-runtime>
.\build\Release\fractional_refiner_quality.exe <staged-runtime> 4
.\build\Release\gpu_phase_engine_smoke.exe <staged-runtime> 1920 1080
.\build\Release\gpu_interop_stress.exe <staged-runtime> 30 100 640 360 protected
.\build\Release\gpu_interop_stress.exe <staged-runtime> 30 100 640 360 mutex
.\build\Release\directshow_gpu_smoke.exe <absolute-path-to-staged-filter.ax>
.\build\Release\directshow_gpu_smoke.exe <absolute-path-to-staged-filter.ax> --p010-input
.\build\Release\gpu_p010_conversion.exe <staged-runtime>
```

The DirectShow harness reads the INI next to the staged filter. Use `config\NvofPotPlayer.ini` and the `runtime` subfolder in an isolated test directory. These tests may update local diagnostic status; do not run them during playback. They do not register the staged filter globally.

The `NVOF_ENABLE_EXPERIMENTAL_SUBPIXEL` option defaults to OFF. Preview.4 enabled it and was rejected in actual playback for frame mixing; the installed filter was rolled back to preview.3. Do not enable this option or distribute its build as an accepted quality fix. Standalone refiner diagnostics remain available for investigation.

## Windows installer

Use the official Inno Setup **7.1.0 x64** compiler. Its upstream setup SHA256 is `0362a383ed217d4c4239b5933866dd96d3eb2102737da92f80f6057a4b40df2f` (Authenticode publisher: Pyrsys B.V.). Obtain it from [the upstream release](https://github.com/jrsoftware/issrc/releases/tag/is-7_1_0). It can be unpacked by its `/PORTABLE=1 /VERYSILENT /DIR=...` installation mode into a build-tools folder. The compiler is not bundled in the app.

```powershell
.\tools\build-local-package.ps1 -Version 0.3.0
.\tools\build-installer.ps1 -CompilerPath "C:\BuildTools\Inno\ISCC.exe"
```

`build-installer.ps1` checks every payload hash and required manual/terms file, then creates the setup EXE and SHA256 file in `dist/`. Publish these two files, not the intermediate ZIP. The application binaries use static MSVC linkage; NVIDIA-required CUDA/VC DLLs and the self-contained .NET Desktop Runtime remain app-local. The NVIDIA driver and PotPlayer are prerequisites, not bundled dependencies.

The installer uses the current user's LocalAppData folder and HKCU registration. It refuses installation/removal while PotPlayer or NVOF Control is running. It preserves INI settings, records explicit component consent locally, and unregisters only the filter path it owns. Installation keeps machine registration unchanged; uninstall elevates the adjacent helper to remove only machine registration pointing to its own filter. Use PotPlayer normally (not elevated); deliberately update or remove machine-wide registration through the controller if elevated playback is needed.

Interactive component acceptance is unchecked by default. For a deployment where the operator has read and accepted the bundled terms, the explicit silent parameter is `/NVIDIATERMS=2026-10-07`; `/VERYSILENT` alone must not install. A changed terms version requires new explicit acceptance. This parameter is an assent mechanism, not an additional distribution grant.

The installer is **unsigned**. Its release notes distinguish packaging checks on the development PC from a fresh-Windows test; a clean machine without development tools and additional supported GPUs still need validation. See [RELEASE.md](../RELEASE.md) for the exact release checks and limits. The prior preview.8 release remains source-only.

## Optional GPU enhancement configuration

`GpuMidpointCorrection=1` and `AppearanceProtection=1` in the `[Nvof]` INI section are independent and default to enabled when absent. The filter snapshots them when the graph creates it; reopen the video after changing them. Both disabled skip the optional flow refiner entirely. Appearance-only still estimates flow for motion vetoing but uses the FRUC picture outside protected areas. These options do not disable scene-cut handling, exact-source preservation or seek re-priming. Extra passes require the native GPU path and retain the existing area/rate budget. Telemetry reports the session options separately from the UI's saved choices; pass counts do not claim every pixel was corrected.


`gpu_p010_conversion` checks P010 integer quantization, padded decoder arrays, retained originals and seek reset. `p010_video_stress <runtime> <1920x1080 raw p010le> queued extras p010 concurrent` exercises real-frame conversion/interpolation against concurrent D3D work without per-frame CPU readback. Supply your own local raw input; media is not included. These tests do not replace real PotPlayer playback with its hardware decoder and renderer.


## Manual release assets

Stage the manual with its local license links, then build the standalone HTML and offline ZIP:

```powershell
python tools/build-manual-site.py --output build/manual-beta1
python tools/build-manual-assets.py --site-dir build/manual-beta1 --version 0.3.0
```

Both commands refuse to overwrite existing output. Publish the generated HTML and manual ZIP as release assets alongside the installer and SHA256 checksums. The standalone HTML embeds its stylesheet, script and images; the ZIP additionally carries the vendor documents. GitHub Pages publishes the same `docs/manual` source on changes to `main`.

## Precompiled playback shaders (0.3.0)

`tools/shader_compiler.cpp` compiles `src/shaders/*.hlsl` with D3DCompile optimization level 3 during the CMake build. Generated headers live under the build directory; the filter creates D3D11 shaders directly from embedded bytecode. Changes to any HLSL file regenerate the relevant header. The native playback path, scene detector, blend and P010 conversion require no runtime shader compilation. The legacy fractional refiner retains its separate research implementation.
