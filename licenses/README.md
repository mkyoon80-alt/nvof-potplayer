# Component notices: native standalone build

Release v0.4.0 builds the native D3D11 synthesizer only. Both GPU texture input and system-memory NV12 input use it. The package includes the filter, registration helper, controller and its self-contained .NET/WPF runtime.

## NVIDIA

NvOFFRUC.dll, NvofFrucBridge.dll, NVEncNVOFFRUC.dll and CUDA runtime libraries are not distributed. No GPU driver or SDK headers are packaged. The application requests optical flow and optional costs from nvofapi64.dll supplied by the user's NVIDIA driver.

Development still uses official Optical Flow SDK interface headers under the applicable NVIDIA terms. Original 2017 download-page and 2022 SDK agreements are preserved for reference:
- https://developer.nvidia.com/opticalflow/download
- https://developer.nvidia.com/designworks/sdk-samples-tools-software-license-agreement
- https://docs.nvidia.com/video-technologies/optical-flow-sdk/license/index.html

The old installer screen described redistributed FRUC/CUDA binaries and is removed from this native package. This is a change to what is shipped, not a claim that all NVIDIA licensing obligations disappear or that NVIDIA approved the product. No proprietary SDK sample implementation is copied into the native synthesis code.

Earlier release binaries and their accompanying FRUC/CUDA notices are unchanged. Legacy acquisition manifests and documents remain in source history for provenance; they are not the current package manifest.

## Microsoft and other components

Microsoft Windows classic samples / DirectShow baseclasses are statically linked under MIT, with their notice preserved. The independently declared LAV/MPC COM protocol does not bundle LAVFilters code.

.NET/WPF and Microsoft C/C++ runtime materials retain their upstream licenses and third-party notices. Self-contained publishing removes the need for a separate .NET installation; it does not place those dependencies under the project MIT license. The native project uses /MT. Inno Setup has its own license and retained notices.

Windows, NVIDIA hardware and its installed display driver, Media Foundation where needed, and a compatible x64 player remain prerequisites. No standalone player or driver installer is included.
