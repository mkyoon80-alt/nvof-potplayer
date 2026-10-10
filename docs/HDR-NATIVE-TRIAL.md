# Native 10bit / HDR trial — 0.4.0

## Scope

The sequence is 10bit SDR, HDR10, then HLG. Existing Newton inversion, integer-rate 60/120fps policy, grid 4/Medium/1920 analysis, disabled Cost Map/Temporal Hints and automatic glyph/boundary protection remain. The user accepted Jishou episode 4 at 0:05 and deferred further F1 22:12 adjustments.

- GPU NV12 remains 8bit; GPU P010 originals and generated frames remain 10bit. P010 snapshots clear only the unused low six storage bits. The original 10bit code values survive exactly.
- NVOFA analysis gets a separate NV12 image; synthesis samples full-precision source Y/UV planes. Intermediate floating-point results are quantized once to 10bit P010, with no 8bit output intermediate.
- Native transport accepts limited-range BT.709 SDR and BT.2020 nonconstant-luminance PQ/HLG. Transfer values use the Windows Media Foundation extensions carried in DXVA color bits: PQ 15, HLG 16. The original color flags are retained.
- A decoder that advertises NV12 but supplies P010 triggers an explicit P010 media-type agreement before the first delivered output; renderer rejection fails clearly. No silent precision reduction is used. Midstream bit-depth changes require reopening.
- HDR static mastering and light-level side data are deep-copied per source sample, attached to originals and the generated phases associated with that source interval. Missing metadata stays missing. Pool recycling clears the data, and returned pointers remain valid during a sample lease. Seek/reset does not reuse prior metadata.
- The public IMediaSideData ABI is independently declared; no LAV decoder/renderer implementation or binary is added.
- Processing remains in encoded YUV sample space. There is no PQ/HLG linear-light conversion, tone mapping, color-gamut conversion or display-mode switching.
- CPU P010, full-range HDR, Dolby Vision and HDR10+ dynamic metadata are outside this first trial. P010 retains conservative GPU completion waits and uses approximately twice the pixel storage of NV12; no speedup is claimed.

## Verification

Tests executed on the owner's Windows / RTX 5090 system; local evidence lives in `build/hdr1` (not distributed):

| Check | Result |
| --- | --- |
| 1024 P010 code values, exact Y/UV, low six bits, padded decoder arrays, source immutability and reset | Passed, `p010-precision.log` |
| Moving ×5 P010 at 640×360 and 3840×2160 | Four distinct phases at shifts 4/8/12/16 for a 20-pixel input movement; original values exact |
| P010 subtype and legacy NV12-header/P010-surface MF-DXGI negotiation | 23.976→119.88fps timestamps, seek, EOS, no-stop timestamp and transport checks passed |
| HDR10 and HLG media-type/static-metadata delivery | Passed, `transport-hdr10.log`, `transport-hlg.log` |
| Metadata deep copy, pointer lifetime, malformed size/null and pool reuse | Passed in DirectShow smoke tests |
| Existing 8bit DirectShow, native motion-layer quality and CPU NV12 | Passed, `directshow-regression.log`, `motion-layers.log`, `cpu-regression.log` |
| Existing scheduling/quality regression suite | 10/10 passed, `ctest.log` |
| Real decoded HDR pixels with concurrent GPU access | 48 inputs / 96 outputs / 44 pairs, reset over four rounds, passed `hdr-real-stress.log` |
| UI persistence, registration routing, tab structure and layout | Self-test passed; minimum-size portable/installed/120fps fixtures at 100/150/200% in `ui-qa` |

The real-media fixture was supplied locally by the user: 3840×1604, 24000/1001, 10bit limited BT.2020/PQ, Dolby Vision profile 8, base-layer compatibility ID 1. Static mastering peak was 1000 nits; MaxCLL 604 and MaxFALL 120. Dolby Vision and HDR10+ dynamic data were also present. Twelve decoded frames at 1:32 were resized to the fixed 1920×1080 stress-fixture dimensions solely for concurrent texture-lifetime testing. This does not validate final 4K player performance or HDR appearance. Neither the movie nor decoded frames are distributed.

Synthetic signal/metadata checks and decoded-pixel stress are not a claim of actual PotPlayer HDR display validation. Actual HDR10 playback/brightness, renderer side-data consumption, seek/close behavior and actual HLG media acceptance remain to be checked by playing the supplied video and an HLG sample. Physical monitor HDR activation is not inferred from the filter's signal label.

## UI / standalone

The obsolete 화질 보정 tab is removed; automatic protection remains. Three tabs are 보간 설정 / 연결 설정 / 진단. Connection settings provide current-folder registration/open/unregister, with a player-running check and per-folder ownership checks. A machine registration requests Windows elevation when removal is needed. No file deletion occurs from unregister. 프로그램 제거 appears only with a valid same-folder installed uninstaller. Moving a portable folder requires unregistering first and registering again at the destination.

Diagnostics expose actual input/output formats and SDR/HDR10/HLG signal type. Build `0.4.0` is a local self-contained portable trial, not a GitHub release. The original filter and settings are backed up separately when installed for user testing.

Manual screenshot provenance: `docs/manual/images/controller.png` is the offline WPF `limit120` illustrative fixture at 464×461 logical content pixels / scale 1, copied from `build/hdr1/ui-qa/limit120-1.png`. It uses the current three-tab layout and is not a live playback capture. No original movie frames or user screenshots are shipped.
