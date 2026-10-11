# VP9 HDR recovery — 0.4.3-vp9.1

The implementation documented below is included in v0.4.4. Trial names identify the original verification builds.

Local trial installed and user-confirmed on 2026-10-11. The trial was not published separately; its changes are included in v0.4.4.

## Reproduction and cause

PotPlayer's native D3D11 decoder supplied a legacy, untagged NV12 media type
while its actual GPU samples were P010. The connected compressed VP90 input
was `FORMAT_VideoInfo`: 88 header bytes followed by a 16-byte `vpcC` record.
The filter previously recovered missing HDR colour only for HEVC and AV1,
so it attached default BT.709 SDR flags to this VP9 output.

User-provided reproduction: <https://www.youtube.com/watch?v=Y91PXCYc_PI>,
VP9 2160p HDR stream. Captured configuration bytes (no picture data):

```text
76 70 63 43 01 00 00 00 02 00 A2 09 10 09 00 00
```

This signals profile 2, unspecified level, 10-bit 4:2:0, limited range,
BT.2020 primaries/matrix and PQ transfer. The stream's title is not evidence
of Dolby Vision metadata; this verification concerns VP9 PQ HDR only.

## Change

- Independently parse the bounded VP9 codec configuration record, FullBox
  payload, complete `vpcC` box and DirectShow's size-less `vpcC` wrapper.
- Read VP9 configuration after compressed VideoInfo/VideoInfo2 headers or
  in MPEG2VIDEOINFO sequence data. Also accept explicit, compatible HDR
  colour flags on compressed VideoInfo2 when the raw output is untagged.
- Recover limited-range BT.2020 PQ/HLG only from explicit colour values.
  Bit depth, profile, mastering metadata and filenames do not imply HDR.
- Preserve explicit raw decoder colour, including SDR after tone mapping.
  Reject conflicting configuration/colour flags and malformed records.
- Retain existing P010 transport and static HDR side-data handling.
  No synthesis/shader, output-rate, UI or runtime dependency changes.
- Under the existing opt-in ProbeServices setting, capture at most 64 KiB
  of VP9 media-format metadata once per filter instance for diagnostics.

Format references: [WebM VP codec binding](https://www.webmproject.org/vp9/mp4/)
and the public [DirectShow vpcC wrapper usage](https://github.com/Nevcairiel/LAVFilters/blob/master/decoder/LAVVideo/decoders/avcodec.cpp).
No decoder implementation or new third-party runtime is included.

## Validation

- All 15 CTest cases passed, including VP9 parsing, malformed/truncated
  headers and preservation of SDR/unspecified transfer values.
- 23 DirectShow GPU integration cases passed: VP9 PQ/HLG and header forms,
  explicit SDR override, missing/truncated/full-range/conflicting metadata,
  existing AV1/HEVC HDR, P010 SDR, seek and timestamp handling.
- The previous installed filter failed the same VP9 colour negotiation
  regression; the trial passed it.
- Actual VP90 playback: downstream range=2, matrix=4, primaries=9,
  transfer=15 (PQ); input/output P010; mastering metadata 80 bytes;
  diagnostic HDR10; 23.976 -> 119.88 configured output.
- The user confirmed normal colours after installation: "제대로 나와".
- HLG recovery passed synthetic checks; actual VP9 HLG playback is unverified.

Evidence is retained locally under `build/vp9hdr043/` (ignored by Git):
`build.log`, `smoke.json`, `baseline-reproduction.json`, `installation.json`,
`actual-vp9-playback.log`, `vp9-color-format.bin`, and `validation.json`.
Only `bin/NvofPotPlayer.ax` was replaced. Existing UI, settings and registration
were preserved and the prior filter/settings were backed up before replacement.
