# Third-party notices and runtime licensing

The project MIT License covers the independently authored NVOF for PotPlayer source. It does not relicense NVIDIA or Microsoft proprietary binaries. NVIDIA, Microsoft and other third-party trademarks remain the property of their respective owners. This project is not sponsored or endorsed by those companies.

Audit date: **2026-10-07**

## Included source and linked code

- **NVEncNVOFFRUC**, rigaya: MIT. Copyright (c) 2014-2016 rigaya; supporting `rgy_osdep.h` and `rgy_err.h` carry Copyright (c) 2011-2016 rigaya. The wrapper's notices are preserved in [NVEnc-MIT.txt](NVEnc-MIT.txt). Source: <https://github.com/rigaya/NVEnc/tree/9.37/NVEncNVOFFRUC>. The prebuilt wrapper used for comparison came from the hash-pinned NVEnc 9.37 Windows x64 release.
- **Microsoft Windows classic samples / DirectShow base classes**: MIT, Copyright (c) Microsoft Corporation. The statically linked base classes retain [Microsoft-Windows-Samples-MIT.txt](Microsoft-Windows-Samples-MIT.txt). Source: <https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/multimedia/directshow/baseclasses>.
- **LAV/MPC public D3D11 COM protocol**: `include/nvof/d3d11_transport.hpp` independently declares ABI-compatible interface identifiers and method signatures documented in <https://github.com/Nevcairiel/LAVFilters/blob/master/include/ID3DVideoMemoryConfiguration.h>. The implementation in this project is original. No LAV decoder/renderer implementation or binary is included; this notice does not claim LAVFilters is MIT licensed.

## NVIDIA runtime: application installer distribution

`NvOFFRUC.dll` is proprietary NVIDIA software dynamically loaded through the project-built NvofFrucBridge.

Git source history and source archives do not contain `NvOFFRUC.dll`. The beta.1 application installer includes it as an app-local component. No standalone DLL release asset is provided. Acquisition, packaging and user consent do not confer additional redistribution rights.

### Provenance check

The tested FRUC runtime was initially obtained from NVEnc 9.37. On 2026-10-07 it was compared with an official NVIDIA Optical Flow SDK 5.0.7 package.

The tested `NvOFFRUC.dll` and `cudart64_110.dll` matched the SDK files under the FRUC sample's Windows x64 binary directory by SHA256. This corroborates provenance only; it does not itself establish a redistribution permission.

The SDK's bundled `LicenseAgreement.pdf` was recorded with SHA256:

`b8981c69975513d5671661be2f8dc1852d6f5e0715cf5b682e75409065635b5e`

### NVIDIA license materials reviewed

#### A. Download-page agreement — 2017

The official Optical Flow SDK download page:

<https://developer.nvidia.com/opticalflow/download>

states that accepting the SDK download binds the downloader to the linked:

**SOFTWARE DEVELOPER KITS, SAMPLES AND TOOLS LICENSE AGREEMENT (with distribution rights)**

<https://developer.nvidia.com/designworks/sdk-samples-tools-software-license-agreement>

That agreement is version 13.06.2017. It defines the covered NVIDIA deliverables broadly, including binary software, and contains a distribution grant for incorporating NVIDIA licensed software into a Customer Product in binary form. The grant is conditional, including requirements concerning supported hardware, consistency with the NVIDIA terms, and enforceable recipient agreements. It separately prohibits standalone redistribution and includes other restrictions.

For commercial applications, that agreement also contains a prior-notification requirement.

#### B. Optical Flow SDK 5.0 license — 2022

The current Optical Flow SDK 5.0 documentation publishes the:

**NVIDIA DesignWorks SDK License (v. May 10, 2022)**

<https://docs.nvidia.com/video-technologies/optical-flow-sdk/license/index.html>

The same form of license is included with the SDK package. Its express license grant permits installation and use, modification of sample source code, and distribution of sample source code and derivative works of such sample source code when incorporated into a software application in object-code form, subject to distribution requirements.

The 2022 license also states that, except as expressly provided, SDK portions may not be copied, sold, sublicensed, transferred or distributed, and it prohibits standalone distribution.

The Optical Flow SDK 5.0 release notes identify the package as containing an NVIDIA optical-flow-based **FRUC library and sample application**:

<https://docs.nvidia.com/video-technologies/optical-flow-sdk/release-notes/index.html>

### Current distribution decision and remaining uncertainty

Since preview.9 the publisher has chosen application-bundled distribution based on the 2017 download-page agreement's section 1.1(ii). The installer presents component terms for explicit acceptance and retains original notices and vendor agreements. NVIDIA binaries are outside MIT and are used only as application components on supported NVIDIA hardware. No standalone NVIDIA DLL or complete SDK download is offered.

The SDK-bundled 2022 license describes its distribution grant differently. This project has **not** received NVIDIA's written clarification or separate approval for the prebuilt FRUC runtime. Neither official-file hash matching nor click-through acceptance settles which grant governs the runtime. The source-only preview.8 policy is superseded for preview.9 by the application-bundled distribution decision above, without claiming a vendor permission letter.

The installed `NVIDIA-COMPONENT-TERMS.txt` includes the project component-use notice and original 2017/2022 NVIDIA agreement texts. CUDA and Microsoft terms are retained separately. The notice does not replace those agreements, grant rights NVIDIA has not granted, or imply NVIDIA endorsement. The installer records terms version and acceptance locally without transmitting it. If NVIDIA clarifies the scope differently, review the package and notices accordingly.

Original SDK `LicenseAgreement.pdf` and a text extraction are preserved with the notices. See `config/runtime-manifest.json` for the verified SDK archive and binary hashes.

## CUDA runtime 11.2

`cudart64_110.dll` reports CUDA runtime version 11.2 according to its file-version resource.

CUDA 11.2's EULA and redistribution attachment should govern any public redistribution of that runtime. Versioned CUDA runtime libraries are included among CUDA redistributable components subject to NVIDIA's conditions. The project preserves the relevant archived license as `NVIDIA-CUDA-11.2-EULA.html`.

`cudart64_12.dll` is not required by this application and is omitted from the minimum runtime set.

## Microsoft app-local Visual C++ runtime

The tested NVIDIA FRUC DLL imports `MSVCP140.dll` and `VCRUNTIME140.dll`. The pinned Microsoft `MSVCP140.dll` additionally imports `VCRUNTIME140_1.dll`.

These unmodified signed x64 files may be used app-locally only subject to the applicable Visual Studio redistribution terms and the distributable-file list:

<https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution#visual-c-runtime-files>

Do not relicense those Microsoft binaries under the project's MIT License. Debug, preview and prerelease runtime files are excluded.

## Machine-provided prerequisites

A compatible NVIDIA GPU and NVIDIA display driver, Windows 10/11 x64, and a separately installed compatible PotPlayer x64 remain prerequisites.

`nvcuda.dll` and `nvofapi64.dll` come from the installed NVIDIA driver and are never bundled by this project.

Windows provides the Universal CRT, Direct3D 11, DXGI, Media Foundation, COM and D3DCompiler 47. Windows editions without Media Foundation require the corresponding Windows feature.

A complete local package can run without separately installing the CUDA Toolkit, a Visual C++ redistributable installer, or .NET when the necessary app-local runtimes and self-contained control application are present. This is not a promise that it runs without Windows, the GPU driver, or PotPlayer.

See [`../config/runtime-manifest.json`](../config/runtime-manifest.json) for exact tested file hashes, dependencies and provenance.

This document records the project's current licensing analysis and distribution policy. It is not legal advice.
