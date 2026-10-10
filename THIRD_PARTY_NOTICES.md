# Third-party notices

The project MIT License covers original NVOF for PotPlayer code. It does not relicense third-party source, runtime components, SDK materials or GPU drivers.

## Native standalone package (0.3.1-standalone.1)

- NVIDIA optical-flow vectors and optional Cost Map are obtained through the installed NVIDIA driver's D3D11 API. No NVIDIA driver, SDK headers, NvOFFRUC library, NvofFrucBridge, NVEnc adapter or CUDA runtime is bundled.
- The developer uses the official Optical Flow SDK interfaces under the applicable NVIDIA SDK terms. SDK agreements remain in licenses/ for reference. Removing the FRUC distribution and its click-through does not waive SDK or driver terms, confer additional rights, or imply NVIDIA approval.
- Microsoft DirectShow baseclasses are statically linked under MIT. See licenses/Microsoft-Windows-Samples-MIT.txt.
- The self-contained .NET / WPF controller includes Microsoft and third-party runtime components with their upstream terms and notices. See licenses/DOTNET-Windows-Library-License.txt, licenses/DOTNET-Runtime-ThirdPartyNotices.txt and the other .NET/WPF notices in licenses/.
- The app and native filter use the static MSVC runtime. Any Microsoft runtime components shipped with the .NET controller retain their own terms; they are not covered by the project MIT license.
- The installer uses unmodified Inno Setup 7.1.0 by Jordan Russell and Martijn Laan. Its notices and site addresses are retained; see licenses/Inno-Setup-LICENSE.txt. The compiler is not shipped.

The LAV/MPC COM interface declarations are independently authored ABI declarations, not a bundled LAV decoder or renderer implementation. This includes the public IMediaSideData GUID/signatures and HDR static-metadata identifiers used for sample interoperability (reference: https://github.com/Nevcairiel/LAVFilters/blob/master/include/IMediaSideData.h). Metadata storage, copying and sample-lifetime handling are implemented in this project. Windows, a supported NVIDIA GPU/driver and the player's installation are external prerequisites.

Earlier v0.3.0 and older packages included NvOFFRUC and CUDA runtime components under separate distribution notices. Their original release artifacts and Git history remain unchanged. These native standalone package notes do not retroactively change earlier releases.

No private video, diagnostic logs, credentials, full SDK or Smootter binaries are distributed. This project is independent of NVIDIA, Microsoft, PotPlayer, AMD and Smootter.
