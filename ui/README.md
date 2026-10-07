# NvofControl

A compact Windows x64 WPF controller for the NVIDIA Optical Flow DirectShow filter. The filter property page opens `NvofControl.exe` through **설정 창 열기**. The XAML is embedded in the application.

The release is a **self-contained folder**: the .NET and WPF runtime files ship beside the application, so users do not need to install .NET separately. Keep the entire published folder together. The controller does not require a web runtime. Windows and the playback/filter requirements documented in the project README still apply.

## Build

From the repository root, run in PowerShell:

```powershell
./ui/build-ui.ps1
```

The default output is `ui/staging-selfcontained/`. Use `-OutputDirectory <path>` to publish elsewhere. The build never copies files to the installed player or registers the filter.

The script downloads the pinned Microsoft SDK into `build/.tools/` and verifies its official SHA-512 hash before extraction. No global SDK installation is performed. The current pins are SDK **10.0.401** and .NET/Windows Desktop runtime **10.0.12**, targeting `net10.0-windows` and `win-x64`. An already available SDK can be supplied with `-DotNetExecutable <path-to-dotnet.exe>`; its version must match the pin.

`dotnet-sdk.json` records the official download and checksum; `global.json` selects the exact SDK. NuGet restore is limited to the official NuGet feed, with caches under `build/.tools/`. SDK download and initial restore require internet access; the published controller does not.

.NET 10 is an LTS release. Because its runtime is bundled, deploy a rebuilt application to deliver runtime security updates. See Microsoft's [support policy](https://dotnet.microsoft.com/en-us/platform/support/policy) and [self-contained deployment documentation](https://learn.microsoft.com/en-us/dotnet/core/deploying/).

## Settings and status

The controller reads and writes `NvofPotPlayer.ini` beside the executable and filter, in section `[Nvof]`:

| Key | Values | Default |
| --- | --- | --- |
| `Enabled` | `1` or `0` | `1` |
| `DoubleRate` | Legacy rollback compatibility; always `1` | `1` |
| `InputRateMask` | Integer from `0` through `63` | `63` |

Source-rate bits are 24 fps=`1`, 25=`2`, 30=`4`, 50=`8`, 60=`16`, and other=`32`. Rates 23.976, 29.97, and 59.94 belong to the 24, 30, and 60 groups. An empty selection preserves original frames for every source. Turning the master switch off disables the checkboxes without discarding their selection.

Output is always ×2. The filter ignores legacy `TargetFps` and `DoubleRate` choices. On a normal controller launch, migration removes `TargetFps` and writes `DoubleRate=1` for rollback compatibility, preserving source selections, enablement and unrelated settings. Previews and tests never change the installed settings. Changes take effect when the player opens a video again. Unknown or unsupported doubled rates bypass interpolation.

Runtime status comes from `%LOCALAPPDATA%/NvofPotPlayer/status.json` and stays independent of the saved settings. The controller uses `processId`, `state`, `transport`, `inputFps`, `outputFps`, `outputFrames`, `message`, `bypassReason`, `inputRateSelected`, `inputRateMask`, and `doubleRate`. Supported states are `waiting`, `active`, `bypass`, `error`, and `stopped`.

A report can show active interpolation only when its process is alive and predates the report. Reports older than five seconds show the last playback state; missing or malformed reports never show active interpolation. Bypass explanations distinguish `disabled`, `source-rate`, `at-or-above-target`, `unknown-source-rate`, and `output-rate-unsupported`.

Transport `d3d11-gpu` means verified GPU-memory delivery, `native-pending` means GPU delivery is awaiting verification, and `system-memory` means a CPU-memory path. Unknown values never claim native delivery. No development transport switch is exposed in the controller.

Registration runs only after a button click. The adjacent `NvofRegister.exe --register` registers for the current user. If a running PotPlayer x64 process is elevated, the controller explains the requirement and requests Windows consent for `--register-machine`. Cancellation never reports success; successful registration is verified against both filter and property-page paths in the selected 64-bit registry scope. Registration does not configure PotPlayer's filter graph.

All status and FPS labels remain in the controller. It never draws on the video.

## Interface and verification

The window is 520×500 DIP, with a 480×500 DIP minimum. Three tabs separate interpolation, connection, and diagnostic controls. The interpolation tab requires no scrolling. Tab follows native control order; Space toggles a focused checkbox, Enter activates a focused button, and Escape closes the controller without stopping playback.

Run the isolated checks from the published folder:

```powershell
./NvofControl.exe --self-test
./NvofControl.exe --self-test --runtime-info ./runtime-info.json
```

The self-test checks temporary INI persistence, unrelated-key preservation, source-rate selection and legacy fixed-rate migration, disabled-state retention, status validation and bypass explanations, registration routing without registration, and WPF control layout at normal and minimum sizes. The optional runtime report records the actual framework version and module paths loaded by that process.

Offline preview renders the actual WPF client content without showing a desktop window:

```powershell
./NvofControl.exe --render-preview ./preview.png --preview-state active
```

Options:

- `--preview-width` and `--preview-height`: logical client dimensions; default `504`×`461`, minimum-window fixture `464`×`461`.
- `--preview-scale`: raster scale from `1` through `3`; use `1`, `1.5`, or `2` for 100%, 150%, or 200% captures.
- `--preview-state`: `live`, `waiting`, `active`, `error`, `setup`, `expanded`, `excluded`, `disabled`, `above-target`, `double`, `double-expanded`, `unknown-rate`, or `unsupported-rate`.
- `--preview-focus`: a named WPF control, such as `InputRate0`, for logical focus verification.

Explicit fixtures are visibly labeled as examples and do not write settings or production status. The default `live` preview reads actual state. Offline raster scaling and logical focus do not substitute for physical monitor DPI changes or keyboard traversal testing.
