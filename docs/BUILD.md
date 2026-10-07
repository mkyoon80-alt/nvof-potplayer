# Build from source

Playback users can use a locally built complete Windows x64 ZIP. This repository currently publishes source only; the complete NVIDIA runtime ZIP is not publicly distributed. Build tools are needed only to compile the source.

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

The acquisition script uses a SHA256-pinned NVEnc release and checks all extracted DLLs. It also needs the pinned licensed Visual C++ release DLLs from the build machine's Visual Studio `VC/Redist` folder. You can pass `-VcRedistDirectory` explicitly; a different version is refused until the manifest is reviewed and updated. None of these build tools are required on the playback machine. The complete ZIP is generated in `dist/` for local use and is not cleared for public NVIDIA binary redistribution.

The package script uses explicit release inputs and an isolated `dist` directory. It includes the controller publish output, native filter and registration utility, default INI, runtime components and license notices. It never copies local playback logs, personal settings, backups or test screenshots. Keep the complete extracted directory together.

The project-built bridge replaces the historical NVEnc adapter in the complete package. Five vendor DLLs are hash-pinned; the bridge hash is recorded in each package manifest. The official SDK 5.0.7 ZIP used for validation has SHA256 `89b0923adc6f34fbe86e63cc17d5db452e34b945c03ba90d8e09bd1a0158e917`; its NvOFFRUC.h hash is `44de0430d90ae3f8453ea8907c040d470e4060bc56ac2835702ffd4c6077e5f2`.

## Tests

`tools/build.ps1` runs the CPU-only CTest suite after building. NVIDIA hardware is needed for the additional GPU tests:

```powershell
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
```

The DirectShow harness reads the INI next to the staged filter. Use `config\NvofPotPlayer.ini` and the `runtime` subfolder in an isolated test directory. These tests may update local diagnostic status; do not run them during playback. They do not register the staged filter globally.

The `NVOF_ENABLE_EXPERIMENTAL_SUBPIXEL` option defaults to OFF. Preview.4 enabled it and was rejected in actual playback for frame mixing; the installed filter was rolled back to preview.3. Do not enable this option or distribute its build as an accepted quality fix. Standalone refiner diagnostics remain available for investigation.

## Standalone installer readiness

The complete **local** preview.8 ZIP is about 61 MiB. The WPF controller already contains .NET Desktop Runtime, project native binaries use static MSVC linkage, and NVIDIA's required CUDA/VC DLLs are app-local. The dependency audit loaded all six native runtime DLLs from the package; no installed Visual C++ runtime was selected. Users still need Windows x64, PotPlayer x64 and a supported NVIDIA GPU/driver. Driver DLLs and PotPlayer must not be bundled as project dependencies.

A public download-and-install release needs the following remaining work:

1. Establish the precompiled NvOFFRUC runtime redistribution grant and applicable recipient terms. Official SDK 5.0.7 DLL hashes now match the tested package, but the SDK's accompanying 2022 license remains narrower than the older download-page agreement. See the license audit; no vendor contact has been sent.
2. Wrap the approved self-contained folder in a per-user installer under LocalAppData, register the filter, add a settings shortcut and an uninstaller. Close-player detection, INI preservation, rollback, correct unregister-before-removal behavior and optional elevated registration must be covered. Users still choose the filter in PotPlayer; no silent reset of player preferences is required.
3. Validate first install, upgrade, uninstall, paths with spaces/non-ASCII characters and a fresh Windows environment without development tools or preinstalled .NET/VC. GPU playback needs a physical supported NVIDIA machine; a VM alone is insufficient.
4. For a public release, record installer hashes and exact source revision, bundle applicable notices and test the downloaded asset. Authenticode signing is recommended for publisher identity; a certificate must be obtained separately and signing does not guarantee SmartScreen reputation immediately.

An installer wrapper does not remove the remaining runtime license issue. The public preview.8 release is explicitly **source-only**, not an incomplete file presented as a ready-to-run setup. Current local users can keep the complete extracted directory and register through the existing controller.
