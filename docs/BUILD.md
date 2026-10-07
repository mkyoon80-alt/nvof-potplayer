# Build from source

Playback users can use a locally built complete Windows x64 ZIP. This repository currently publishes source only; the complete NVIDIA runtime ZIP is not publicly distributed. Build tools are needed only to compile the source.

## Native filter

Install Visual Studio 2022 with Desktop development with C++ and a Windows SDK, CMake 3.20+, Git, and the CUDA Toolkit (driver API headers and import library). The native code uses the CUDA driver API; users do not need the Toolkit installed.

```powershell
.\tools\fetch-build-deps.ps1
.\tools\build.ps1
```

The fetch script pins Microsoft Windows-classic-samples at `434f6002bdf9cf9829406c3ff2b33387982d6168` and checks out only its DirectShow baseclasses and license. It refuses to overwrite a different or modified checkout. The build produces `build\Release\NvofPotPlayer.ax`, `NvofRegister.exe`, and test executables. The project links its own C++ code and baseclasses with the static MSVC runtime (`/MT`).

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

## Tests

`tools/build.ps1` runs the CPU-only CTest suite after building. NVIDIA hardware is needed for the additional GPU tests:

```powershell
.\build\Release\gpu_engine_smoke.exe .\runtime 1920 1080
.\build\Release\directshow_gpu_smoke.exe <absolute-path-to-staged-filter.ax>
```

The DirectShow harness reads the INI next to the staged filter. Use `config\NvofPotPlayer.ini` and the `runtime` subfolder in an isolated test directory. These tests may update local diagnostic status; do not run them during playback. They do not register the staged filter globally.
