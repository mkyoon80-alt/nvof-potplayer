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
| `OutputMode` | `0` = FPS cap, `1` = direct source multiplier | `0` |
| `OutputFpsLimit` | `60`, `72`, `75`, `100`, `120`, `144`, `240` | `60` |
| `OutputMultiplier` | `2`, `3`, `4`, `5` | `2` |
| `ResolutionRulesEnabled` | `1` = use per-height rules, `0` = common output only | `0` |
| `ResolutionRules` | Seven comma-separated rule values, in ascending height-band order (below) | `0,0,0,0,0,0,0` |
| `DoubleRate` | Legacy rollback compatibility; ignored by this filter | `1` |
| `InputHeightLimit` | `0` = all, `1080`, `1440` | `0` |
| `InputRateMask` | Integer from `0` through `63` | `63` |

Source-rate bits are 24 fps=`1`, 25=`2`, 30=`4`, 50=`8`, 60=`16`, and other=`32`. Rates 23.976, 29.97, and 59.94 belong to the 24, 30, and 60 groups. An empty selection preserves original frames for every source. The controls are native CheckBoxes presented as a connected numeric multi-selection row. Turning the master switch off disables them without discarding their selection.

Output uses the largest integer multiple within the selected cap, or the explicitly selected source multiplier. Both modes preserve originals; neither is exact fixed-FPS resampling. The cap and multiplier are remembered independently. Direct multiplier mode ignores the cap; cap mode preserves above-cap sources. Source-rate/height exclusions and master-off take precedence in both modes. Missing/invalid limits default to 60, mode to cap, and multiplier to ×2. Historical TargetFps/DoubleRate keys are ignored; the controller does not rewrite unrelated settings on launch. Changes apply when reopening the video. Previews and tests do not write installed settings. Runtime diagnostics separately report the applied output limit and multiplier.

The UI's third output choice, **해상도별 설정**, sets `ResolutionRulesEnabled`; it does not add a third `OutputMode` value. `OutputMode` and its remembered cap/multiplier still define the common output. The seven bands use source vertical height: below 480, 480–719, 720–1079, 1080–1439, 1440–2159, 2160–4319 and 4320+ pixels. Thus 1920×800 uses the 720 band. Rule `0` inherits the common output, `-1` disables interpolation, `-2` through `-5` choose a source multiplier, and a supported positive FPS cap selects cap mode. Missing or malformed rule text falls back to seven inherited rules. The feature defaults off, preserving existing playback settings.

**해상도별 편집…** opens an owned dialog with a private draft. **모두 기본 설정** resets its seven rows to inheritance without changing the common output. Cancel, Escape and closing discard draft edits. Save atomically replaces the INI with the complete rules/common-output change while preserving unrelated keys; failure leaves the editor open. Mode selection in the main window is immediate and is not undone by cancelling the editor. Switching back to a common output mode disables the rules without erasing them. Global master/source-rate/height eligibility still takes precedence. See the [Korean guide](../docs/RESOLUTION-SETTINGS.ko.md).

Runtime status comes from `%LOCALAPPDATA%/NvofPotPlayer/status.json` and stays independent of the saved settings. The controller uses `processId`, `state`, `transport`, `inputFps`, `outputFps`, `outputFrames`, `message`, `bypassReason`, `inputRateSelected`, `inputRateMask`, and `doubleRate`. Supported states are `waiting`, `active`, `bypass`, `error`, and `stopped`.

A report can show active interpolation only when its process is alive and predates the report. Reports older than five seconds show the last playback state; missing or malformed reports never show active interpolation. Bypass explanations distinguish `disabled`, `source-rate`, `at-or-above-target`, `unknown-source-rate`, and `output-rate-unsupported`.

Transport `d3d11-gpu` means verified GPU-memory delivery, `native-pending` means GPU delivery is awaiting verification, and `system-memory` means a CPU-memory path. Unknown values never claim native delivery. No development transport switch is exposed in the controller.

Registration runs only after a button click. The adjacent `NvofRegister.exe --register` registers for the current user. If a running PotPlayer x64 process is elevated, the controller explains the requirement and requests Windows consent for `--register-machine`. Cancellation never reports success; successful registration is verified against both filter and property-page paths in the selected 64-bit registry scope. Registration does not configure PotPlayer's filter graph.

All status and FPS labels remain in the controller. It never draws on the video.

## Interface and verification

The main window is 520×540 DIP, with a 480×540 DIP minimum. Three tabs separate interpolation, connection, and diagnostic controls. The interpolation tab requires no scrolling. The resolution editor is 520×640 DIP, minimum 480×600 DIP, with internally scrolling content and a fixed action footer. Tab follows native control order; Space toggles a focused checkbox, Enter activates a focused button, and Escape closes the controller or cancels the editor without stopping playback.

Run the isolated checks from the published folder:

```powershell
./NvofControl.exe --self-test
./NvofControl.exe --self-test --runtime-info ./runtime-info.json
```

The self-test checks temporary INI persistence and unrelated-key preservation; source-rate selection, legacy fixed-rate migration and disabled-state retention; status validation and bypass explanations; and registration routing without registration. Resolution coverage includes rule parsing, private draft reset/cancel/save, coordinated common-output saving, disabled entry points and logical focus/accessibility names. Authored select checks require a real native Popup, a two-way open-state binding, one keyboard Tab stop and a visible selected check. Layout checks cover main/editor controls at normal and minimum widths and output-explanation text clipping. The optional runtime report records the actual framework version and module paths loaded by that process. These checks do not perform physical keyboard traversal or verify desktop popup placement.

Offline preview renders the actual WPF client content without showing a desktop window:

```powershell
./NvofControl.exe --render-preview ./preview.png --preview-state active
```

Options:

- `--preview-width` and `--preview-height`: logical client dimensions; default `504`×`501`, main minimum-window fixture `464`×`501`. Use `504`×`601` for the normal resolution editor or `464`×`561` for its minimum-size fixture.
- `--preview-scale`: raster scale from `1` through `3`; use `1`, `1.5`, or `2` for 100%, 150%, or 200% captures.
- `--preview-state`: `live`, `waiting`, `active`, `error`, `setup`, `expanded`, `excluded`, `disabled`, `above-target`, `double`, `double-expanded`, `unknown-rate`, `unsupported-rate`, `rates-mixed`, or `rates-none`.
- `--preview-focus`: a named WPF control, such as `InputRate0`, for logical focus verification.
- `--preview-menu`: a named ComboBox, such as `ResolutionChoice3`. Renders its actual `PART_Popup` child template independently, using the laid-out select width and desired menu height. This does not open a desktop popup or verify its placement.

Resolution fixtures use `--preview-state resolution`, `resolution-disabled`, or `resolution-editor`. For the editor, supply the editor client dimensions above; a named row such as `ResolutionChoice3` can be used with `--preview-focus` or `--preview-menu`:

```powershell
./NvofControl.exe --render-preview ./editor.png --preview-state resolution-editor --preview-width 504 --preview-height 601
./NvofControl.exe --render-preview ./menu.png --preview-state resolution-editor --preview-width 504 --preview-height 601 --preview-menu ResolutionChoice3
```

The full-control refresh captures are under `.impeccable/review/ui-refresh/`; the later user-directed FPS correction is under `.impeccable/review/ui-refresh-fps/` (all/mixed/none, minimum, disabled, logical focus and 200% raster scale) and supersedes the earlier FPS appearance; `.impeccable/review/resolution-settings/` records the earlier feature extension. The refresh covers main normal/minimum, disabled/error, connection/diagnostics, editor normal/minimum/focus, the menu subtree, and editor 150/200% raster scale. Each capture folder's `installed-current.png` separately renders the installed executable's actual settings/status at that pass; the `ui-refresh-fps/` image is the final deployment. These are UI evidence and do not establish native-filter or actual-player acceptance. See `docs/UI-REFRESH.ko.md` and `.impeccable/surfaces/ui-mainwindow-xaml.md` for current review disposition and its limitations.

Explicit full-window fixtures are visibly labeled as examples and do not write settings or production status. An isolated menu preview contains only the real menu template, so its fixture provenance is carried by the capture path and documentation. The default `live` preview reads actual state. Offline raster scaling and logical focus do not substitute for physical monitor DPI changes or keyboard traversal testing.
