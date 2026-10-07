# Third-party notices

The [MIT license](LICENSE) applies to original project source. It does not apply to the proprietary NVIDIA or Microsoft runtime binaries used by the local package.

| Component | Use | Terms / provenance |
| --- | --- | --- |
| Microsoft DirectShow baseclasses | Statically linked native filter | [MIT notice](licenses/Microsoft-Windows-Samples-MIT.txt); pinned Windows-classic-samples revision in the build script |
| rigaya NVEncNVOFFRUC | Runtime adapter to NvOFFRUC | [MIT notice](licenses/NVEnc-MIT.txt); NVEnc 9.37 |
| NVIDIA NvOFFRUC | Optical Flow frame interpolation | NVIDIA proprietary; applicable public redistribution grant has not been established for the exact acquired binary |
| CUDA runtime 11.2 | Required by NvOFFRUC | NVIDIA CUDA EULA; versioned runtime library in its redistribution list |
| Microsoft Visual C++ runtime | App-local dependencies of NvOFFRUC | Visual Studio 2022 distribution terms; unmodified signed release files |
| .NET and WPF 10.0.12 | Self-contained control application | Upstream .NET/WPF license and third-party notices in `licenses/` |

The interoperable LAV/MPC D3D11 COM declarations are independent interface declarations, not a bundled LAV implementation. LAVFilters is not claimed to be MIT licensed.

Read [licenses/README.md](licenses/README.md) for the specific runtime terms, official source URLs, and unresolved NVIDIA redistribution distinction. [config/runtime-manifest.json](config/runtime-manifest.json) records the tested DLL versions, SHA256 hashes and dependency closure. The .NET SDK/runtime pins are in `ui/dotnet-sdk.json` in source, or `config/dotnet-sdk.json` in the complete local ZIP.

The acquisition and local packaging scripts do not grant distribution rights. No NVIDIA driver DLLs, Smootter program files, private playback captures, or user logs are published. The full local ZIP is excluded from Git and is not uploaded as a public release asset while NVIDIA binary distribution remains unresolved.
