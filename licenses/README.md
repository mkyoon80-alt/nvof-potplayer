# Third-party notices and runtime licensing

The project MIT license covers the independently authored project source. It does not relicense NVIDIA or Microsoft proprietary binaries. NVIDIA and Microsoft trademarks remain their owners' property; this project is not sponsored or endorsed by either company.

## Included source and linked code

- **NVEncNVOFFRUC**, rigaya: MIT. Copyright (c) 2014-2016 rigaya; supporting `rgy_osdep.h` and `rgy_err.h` carry Copyright (c) 2011-2016 rigaya. The wrapper's notices are preserved in [NVEnc-MIT.txt](NVEnc-MIT.txt). Source: <https://github.com/rigaya/NVEnc/tree/9.37/NVEncNVOFFRUC>. The prebuilt wrapper comes from the hash-pinned NVEnc 9.37 Windows x64 release.
- **Microsoft Windows classic samples / DirectShow base classes**: MIT, Copyright (c) Microsoft Corporation. The statically linked base classes retain [Microsoft-Windows-Samples-MIT.txt](Microsoft-Windows-Samples-MIT.txt). Source: <https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/Win7Samples/multimedia/directshow/baseclasses>.
- **LAV/MPC public D3D11 COM protocol**: `include/nvof/d3d11_transport.hpp` independently declares ABI-compatible interface identifiers and method signatures documented in <https://github.com/Nevcairiel/LAVFilters/blob/master/include/ID3DVideoMemoryConfiguration.h>. The implementation in this project is original. No LAV decoder/renderer implementation or binary is included; this notice does not claim LAVFilters is MIT licensed.

## NVIDIA runtime: local acquisition, no public runtime release

`NvOFFRUC.dll` is proprietary NVIDIA software, dynamically loaded through the project-built NvofFrucBridge. The public repository and public source release do not contain it. The local acquisition script does not confer redistribution rights.

The [official SDK download page](https://developer.nvidia.com/opticalflow/download) links the 2017 DesignWorks agreement in [NVIDIA-DesignWorks-2017.pdf](NVIDIA-DesignWorks-2017.pdf). Section 1.1(ii) permits incorporating binaries into a customer product subject to protective, enforceable recipient terms. Section 1.5 requires prior notification for commercial use, expressly including a plug-in to a commercial application. The [SDK documentation license](https://docs.nvidia.com/video-technologies/optical-flow-sdk/license/index.html), dated May 10, 2022, instead states a narrower sample-code distribution grant. Both source documents are preserved here.

The tested DLL was initially obtained from NVEnc 9.37. On 2026-10-07, it was compared with the user's official Optical Flow SDK 5.0.7 ZIP: both `NvOFFRUC.dll` and `cudart64_110.dll` match the SDK's `NvOFFRUC/NvOFFRUCSample/bin/win64/` files byte-for-byte by SHA256. Provenance is now corroborated; this does not itself establish redistribution permission.

The SDK's own `LicenseAgreement.pdf` (SHA256 `b8981c69975513d5671661be2f8dc1852d6f5e0715cf5b682e75409065635b5e`) contains the May 10, 2022 DesignWorks terms, consistent with the official documentation license. Section 1(c) describes sample-source-derived object-code distribution, while section 4(b) restricts other distribution. We have not established an express grant for redistributing the precompiled FRUC runtime under these accompanying terms. The older download-page agreement and this bundled agreement therefore need clarification for binary publication. Noncommercial status alone does not resolve that scope. No notification or request has been sent to NVIDIA. Public preview.8 contains project source only; the complete local runtime package remains unpublished.

`cudart64_110.dll` is CUDA runtime version 11.2 according to its file-version resource (6,14,11,11020). CUDA 11.2's EULA, section 2.6 Attachment A, explicitly includes versioned CUDA runtime DLLs among redistributable components, subject to its distribution conditions. The original archived license is [NVIDIA-CUDA-11.2-EULA.html](NVIDIA-CUDA-11.2-EULA.html). The acquisition source remains NVEnc 9.37 and is recorded independently of this license reference. `cudart64_12.dll` is not required by this application and is omitted from the minimum runtime set.

## Microsoft app-local Visual C++ runtime

The tested NVIDIA FRUC DLL imports `MSVCP140.dll` and `VCRUNTIME140.dll`. The pinned Microsoft `MSVCP140.dll` additionally imports `VCRUNTIME140_1.dll`. These three unmodified, signed x64 files can be placed adjacent to `NvOFFRUC.dll` for local application loading. The project filter and NvofFrucBridge themselves use the static C/C++ runtime.

The Visual Studio 2022 Community terms and [Microsoft's distributable-file list](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution#visual-c-runtime-files) govern these files. The list permits licensed Visual Studio users to distribute release files under `VC/redist` with their programs, subject to the product license. Preserve protective recipient terms; do not relicense these files under MIT. Both the original Microsoft license document and a text extraction are included. Debug, preview and pre-release runtime files are excluded.

## Machine-provided prerequisites

A compatible NVIDIA GPU and NVIDIA display driver, Windows 10/11 x64, and a separately installed compatible PotPlayer x64 remain prerequisites. `nvcuda.dll` and `nvofapi64.dll` come from the installed NVIDIA driver and are never bundled. Windows provides the Universal CRT, Direct3D 11, DXGI, Media Foundation, COM, and D3DCompiler 47. Windows editions without Media Foundation need the corresponding Windows feature.

A complete local package can run without separately installing the CUDA toolkit, a Visual C++ redistributable installer, or .NET when it includes the app-local native runtimes and a self-contained control application. This is not a promise that it runs without Windows, the GPU driver or PotPlayer.

See [runtime-manifest.json](../config/runtime-manifest.json) for exact file hashes, dependencies and provenance. Audit date: 2026-10-07.
