# Bundled .NET and WPF notices

The Windows x64 release includes .NET and Windows Desktop runtime **10.0.12**, published using Microsoft .NET SDK **10.0.401**. These components retain their upstream licenses; the application source license does not replace them.

## Included documents

| File | Provenance |
| --- | --- |
| `DOTNET-Runtime-MIT.txt` | `LICENSE.TXT` from the official `Microsoft.NETCore.App.Runtime.win-x64` 10.0.12 NuGet runtime pack. |
| `DOTNET-Runtime-ThirdPartyNotices.txt` | Complete, unmodified `THIRD-PARTY-NOTICES.TXT` from that same runtime pack. |
| `DOTNET-WindowsDesktop-MIT.txt` | `LICENSE` from the official `Microsoft.WindowsDesktop.App.Runtime.win-x64` 10.0.12 NuGet runtime pack. |
| `DOTNET-Windows-Library-License.txt` | Complete Microsoft .NET Library License from the official SDK 10.0.401 Windows x64 archive, verified against the published SHA-512 recorded in `ui/dotnet-sdk.json`. |
| `WPF-MIT.txt` | Unmodified `LICENSE.TXT` from the official `dotnet/wpf` `v10.0.12` tag. |
| `WPF-ThirdPartyNotices.txt` | Complete, unmodified `THIRD-PARTY-NOTICES.TXT` from that same WPF tag. |
| `WPF-Windows-SDK-License.html` | Microsoft's Windows SDK license page, retained for the bundled WPF Direct3D compiler component. |

The two runtime packs identify Microsoft as the author and record source repository `dotnet/dotnet`, commit `95017c711e6afc1085133d440e42b4bd78155701`. Their embedded notices are used for the shipped binaries rather than substituting notices from a newer development branch.

## Windows-specific components

Microsoft's [Windows licensing information](https://github.com/dotnet/core/blob/main/license-information-windows.md) identifies `coreclr.dll`, `PresentationNative_cor3.dll`, `wpfgfx_cor3.dll`, and `vcruntime140_cor3.dll` as covered by the **.NET Library License**. It explicitly explains that Microsoft relicenses the WPF copy of `vcruntime140.dll` under that license. The bundled `D3DCompiler_47_cor3.dll` is covered by the **Windows SDK License**. The runtime also contains third-party code covered by its accompanying notices. This distribution is therefore not represented as entirely MIT-licensed.

The shipped `vcruntime140_cor3.dll` is copied by self-contained publishing from `Microsoft.WindowsDesktop.App.Runtime.win-x64` 10.0.12, not taken from an unrelated Visual Studio installation. Its SHA-256 matches the runtime pack exactly:

```text
D5E4D9A3E835FA679450145D6A7D94E36573A509317111904D9B3712C30D9066
```

## Primary sources

- [.NET runtime pack 10.0.12](https://www.nuget.org/packages/Microsoft.NETCore.App.Runtime.win-x64/10.0.12)
- [Windows Desktop runtime pack 10.0.12](https://www.nuget.org/packages/Microsoft.WindowsDesktop.App.Runtime.win-x64/10.0.12)
- [.NET runtime source license, v10.0.12](https://github.com/dotnet/runtime/blob/v10.0.12/LICENSE.TXT)
- [WPF source license, v10.0.12](https://github.com/dotnet/wpf/blob/v10.0.12/LICENSE.TXT)
- [WPF third-party notices, v10.0.12](https://github.com/dotnet/wpf/blob/v10.0.12/THIRD-PARTY-NOTICES.TXT)
- [.NET licensing overview](https://github.com/dotnet/core/blob/main/license-information.md)
- [Microsoft .NET Library License](https://dotnet.microsoft.com/dotnet_library_license.htm)
- [Microsoft Windows SDK License](https://learn.microsoft.com/en-us/legal/windows-sdk/license)

License documents were collected on 2026-10-07. This provenance note is informational; the included license texts govern their respective components.
