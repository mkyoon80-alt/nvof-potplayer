# Dolby Vision side data and HLG — 0.4.3-dovi.1

The implementation documented below is included in v0.4.4. Trial names identify the original verification builds.

## Change

The filter previously copied HDR mastering and content-light metadata only.
It now also preserves the public `IMediaSideData` Dolby Vision RPU, legacy
parsed metadata, and V2 parsed metadata payloads on native D3D11 samples.
The decoder and renderer remain responsible for Dolby Vision decoding,
reshaping, tone mapping, and display output.

This is an independent opaque-payload implementation using the public
[LAV interface ABI](https://github.com/Nevcairiel/LAVFilters/blob/master/include/IMediaSideData.h).
It does not add LAV, CUDA, NvOFFRUC, or a renderer dependency.

## Frame and lifetime rules

- Decoder payloads are copied before the input sample is released.
- Original frames retain their own metadata.
- Generated frames inherit the preceding original's metadata for that pair;
  metadata bytes are never numerically interpolated.
- Scene-cut holds retain the preceding frame and its metadata together.
- Missing metadata stays missing. No last-seen Dolby payload is cached across
  frames, seeks, allocator recycling, or changes of source.
- Static payload sizes remain exact. Dolby payloads are bounded to 64 KiB;
  parsed structures require the public packed ABI's minimum size and allow
  trailing extensions. Returned pointers remain valid for the sample lifetime.
- Shared atomic delivery diagnostics outlive the filter if a renderer retains
  an output sample. They indicate metadata transport, not display processing.

## Local source verification

The user-supplied Dune file contains HEVC Main 10, 3840×1604 at 24000/1001 fps,
BT.2020/PQ, and Dolby Vision Profile 8 with compatibility ID 1 (8.1).
Actual RPU and parsed Dolby metadata were found in decoded frames at four
positions. Its HDR10-compatible base layer explains why it already looked
normal before Dolby metadata forwarding was implemented.

The user-supplied Supergirl file contains HEVC Main 10, 3840×1600 at
24000/1001 fps, BT.2020 and HLG (`arib-std-b67`). The filename's H.264 label
does not describe its actual video stream. Four decoded positions retained
HLG signalling. The existing HEVC color recovery already supports this source.

Source media is local test input and is not included in the repository or
release artifacts.

## Actual player validation

On 2026-10-11, PotPlayer's built-in decoder delivered 5,225-byte legacy and
9,353-byte V2 Dolby metadata from Dune. The built-in D3D11 renderer requested
and received V2 data through the filter. Input and output stayed P010, with
23.976 → 119.88 fps interpolation. The user reported normal colors.

Diagnostic mask values for that run were `input=27`, `requested=19`,
`returned=19`: mastering=1, content-light=2, RPU=4, DV legacy=8, DV V2=16.
The decoder supplied parsed metadata rather than raw RPU data.

Supergirl was also played with this build on 2026-10-11. Input and output
remained P010, the built-in D3D11 renderer received transfer function 16 (HLG),
and 23.976 → 119.88 fps interpolation ran successfully. There was no Dolby
metadata on that HLG stream (`input=0`, `returned=0`). The user confirmed
playback completion; a separate visual color comparison was not recorded.
These were separate player processes; same-instance sample recycling and
seek behavior are covered by the automated tests below.

The JSON `colorMode` continues to describe the negotiated base signal
(`HDR10`, `HLG`, or `SDR`). `hdrSideDataInputMask` and
`hdrSideDataRendererReadMask` separately describe metadata transport. Receiving
Dolby metadata is not proof that a display has entered Dolby Vision mode.
Profile 5, Profile 7 enhancement-layer reconstruction, and changing reshaping
curves across interpolation endpoints were not validated by this Profile 8.1
sample; this change does not claim universal Dolby Vision processing.

## Automated validation

- 15 CTest checks passed.
- Ten native DirectShow cases passed: Dolby metadata, HDR10, HLG, legacy
  HEVC color recovery, VP9 PQ/HLG, AV1 PQ/HLG, and 10bit SDR.
- Dolby payloads were checked through original/generated frames, seek, EOS,
  deep copying, returned-pointer lifetime, invalid sizes, and pool reuse.
- The same Dolby delivery test fails with the backed-up `0.4.3-vp9.1`
  binary and passes with this build, confirming the regression test detects
  the previously missing metadata path.
- P010 GPU checks passed at 1080p and 4K, including distinct ×5 phases,
  preservation of original pixels and 10bit precision, metadata ownership,
  decoder-array reuse, and queued/blocking completion.

Local evidence: `build/dovi043/`. The prior installed filter and settings are
backed up in `backup/before-dovi043-20261011-184219-074/`.
