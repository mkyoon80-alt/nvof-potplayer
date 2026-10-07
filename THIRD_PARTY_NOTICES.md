# Third-party notices

The project [MIT License](LICENSE) covers original NVOF for PotPlayer source only. NVIDIA and Microsoft proprietary runtime components are **excluded from MIT**. No component is relicensed by this notice. This file is also shipped as `THIRD_PARTY_NOTICES.txt`.

| Component | Ownership / terms | Distribution |
| --- | --- | --- |
| NVIDIA NvOFFRUC | NVIDIA Corporation; proprietary SDK terms | Unmodified `NvOFFRUC.dll` inside the application installer only; no standalone DLL asset |
| CUDA runtime 11.2 | NVIDIA Corporation; CUDA 11.2 EULA | App-local `cudart64_110.dll`, subject to its redistribution conditions |
| Visual C++ release runtime | Microsoft Corporation; Visual Studio distribution terms | Unmodified signed app-local x64 release DLLs |
| .NET / WPF 10.0.12 | Microsoft and contributors; bundled upstream notices | Self-contained controller runtime |
| Microsoft DirectShow baseclasses | Microsoft Corporation; MIT | Statically linked, notice retained |
| rigaya NVEnc adapter | rigaya; MIT | Historical reference; project NvofFrucBridge replaces the old adapter |

## NVIDIA ownership and applicable materials

`NvOFFRUC.dll` remains NVIDIA proprietary software. All upstream copyright notices and ownership are retained. The package does not include NVIDIA driver DLLs, SDK headers or the full SDK. The tested FRUC and CUDA runtime hashes match the files in the official Optical Flow SDK 5.0.7 ZIP. Hash equality verifies provenance, not a new license grant.

The publisher's application-bundling basis is section 1.1(ii) of the [2017 SDKs, Samples and Tools agreement](https://developer.nvidia.com/designworks/sdk-samples-tools-software-license-agreement), linked by the [official download page](https://developer.nvidia.com/opticalflow/download). The SDK's accompanying [May 10, 2022 DesignWorks license](https://docs.nvidia.com/video-technologies/optical-flow-sdk/license/index.html) describes sample-source-derived object-code distribution more narrowly. Their relationship for the prebuilt FRUC runtime remains unconfirmed by NVIDIA. Publishing this package is not a claim that NVIDIA has separately approved it.

The installer requires an explicit acceptance of the supplied component terms. The unchecked checkbox and version-specific unattended acceptance record user assent; they do not expand any vendor's license grant. Full NVIDIA agreements, ownership notices, restrictions and other component terms are included under `licenses/`. Do not distribute the NVIDIA library as a standalone product or apply the project's MIT license to it.

## Other components and scope

The independently declared LAV/MPC COM ABI is not a bundled LAV implementation and this project does not claim LAVFilters is MIT licensed. Runtime hashes and acquisition evidence are recorded in `config/runtime-manifest.json`. Driver files `nvcuda.dll` and `nvofapi64.dll` come from the user's NVIDIA driver and are never bundled. Windows and PotPlayer are separate prerequisites.

No Smootter binaries, private playback media, logs, credentials or proprietary SDK source are included. NVOF for PotPlayer is independent and is not sponsored or endorsed by NVIDIA, PotPlayer, AMD or Smootter. This document records the distribution decision and its limits; it is not a representation of vendor approval.

## Installer engine

The Windows installer uses unmodified Inno Setup 7.1.0 by Jordan Russell and Martijn Laan. Copyright notices and upstream site addresses are retained. See [Inno Setup license](licenses/Inno-Setup-LICENSE.txt) and https://jrsoftware.org/. The compiler itself is not shipped.
