# Native standalone build

Build tools are needed only by developers. Users install the filter and self-contained controller.

## Requirements

Visual Studio 2022 C++, Windows SDK, CMake 3.20+, Git and official NVIDIA Optical Flow SDK API headers. CUDA Toolkit, NvOFFRUC.h and FRUC binaries are not required. Obtain the SDK from NVIDIA under its terms; do not commit or bundle the SDK headers.

~~~powershell
.\tools\fetch-build-deps.ps1
.\tools\build.ps1 -BuildDirectory build\cost2\native -OpticalFlowSdkIncludeDirectory "C:\SDKs\Optical_Flow_SDK_5.0.7\NvOFInterface"
.\ui\build-ui.ps1 -OutputDirectory ui\staging-cost2
~~~

The filter and register helper use static MSVC /MT. Shader bytecode is generated at build time. CMake never finds CUDA or builds the old FRUC bridge in this profile. Historical FRUC-specific research tests remain as source references but are not active build targets.

The system-memory adapter uploads NV12 into a private NVIDIA D3D11 device and reads back native synthesis output. GPU texture playback uses the decoder/renderer device directly. Both use the same motion synthesizer.

## Package and installer

~~~powershell
.\tools\build-local-package.ps1 -Version 0.3.1-cost.2 -NativeDirectory build\cost2\native\Release -UiDirectory ui\staging-cost2
.\tools\build-installer.ps1 -Version 0.3.1-cost.2
~~~

The package includes the published .NET/WPF controller, native filter, register helper, settings, manual and notices. FRUC/CUDA and NVIDIA driver files are rejected. Inno Setup 7.1.0 builds the installer; its compiler is not shipped.

Old runtime acquisition scripts are unnecessary. tools/install-phase-upgrade.ps1 is retired to prevent accidental installation of FRUC. Earlier acquisition evidence remains in config/legacy-runtime-manifest.json and Git history, not in the current package.

## Validation

tools/build.ps1 runs six scheduling/policy tests. Explicit GPU tests include native_cost_map, native_motion_layers, native_motion_guard, native_cpu_transport, gpu_interop_stress and NV12/P010 DirectShow tests. Use an absent runtime path for native engine tests.

runtime_dependencies.exe audits a package for removed FRUC/CUDA/driver files and loads its filter. Import-table inspection verifies that product binaries do not depend on CUDA or FRUC.

tools/test-release-installer.ps1 tests installation without NVIDIA assent parameters, same-folder upgrade cleanup, retained files/settings, COM registration, self-contained controller and removal/ownership safety. Run with the player and controller closed; it restores prior user registration.
