# Native 10bit / HDR trial — 0.4.0 / 0.4.0-hdr.1

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

## Actual-player correction — 0.4.0-hdr.1 (2026-10-11)

The initial local 0.4.0 was washed out in actual PotPlayer HDR playback, while Fluid Motion displayed the same file correctly. Its decoded-pixel and explicitly tagged media-type fixtures did not cover the player's real connection. Actual evidence: the built-in decoder advertised legacy FORMAT_VideoInfo/NV12 without color flags, delivered native P010 samples, and provided IMediaSideData mastering (80 bytes) and light-level (8 bytes). The filter filled the absent header with BT.709 SDR and skipped side-data capture because the input header had not already been classified as HDR.

The correction reads bounded HEVC hvcC/Annex B sequence-header VUI from the immediate decoder's connected compressed video input when raw output color fields are absent. Verified limited BT.2020 NCL PQ/HLG signalling is restored to the renderer's VIDEOINFOHEADER2. Explicit decoder color fields, including an explicitly tone-mapped SDR output, take precedence. Bit depth or filenames are never used to infer HDR. Color recovery is a connection/type-change operation, not a per-frame scan. Other codecs need correctly tagged decoder output; no external parser/decoder runtime is bundled.

HDR static side data is now captured independently of the legacy input header, per sample, retaining the existing copy/lifetime/seek behaviour. The installed trial logs the resolved transfer and metadata byte counts without dumping compressed or subtitle headers. Its diagnostics report the output signal classification.

Validation:
- Actual captured 264-byte decoder media header: BT.2020 / PQ / limited matches independent ffprobe evidence.
- New SPS parser tests: PQ, HLG, SDR, range, hvcC and Annex B, scaling lists/reference syntax, truncated/oversized/conflicting records and 4,000 bounded mutations.
- New DirectShow fixtures: untagged legacy NV12/P010 HDR10 and HLG with compressed input signalling; explicitly tagged SDR remains SDR despite HDR source signalling. 23.976→119.88 output, metadata, timestamps, seek, EOS and dynamic type handling passed.
- Existing explicit HDR10/HLG and untagged P010 SDR transport passed. CTest 11/11 passed. Installed runtime check passed without bundled FRUC/CUDA/driver libraries.
- Actual PotPlayer replay: BT.2020 primaries 9, matrix 4, PQ transfer 15; P010 input/output; 80-byte mastering and 8-byte light metadata delivered. **The owner confirmed normal color after installation.**

This verifies the supplied HDR10-compatible file with the owner's current decoder/renderer setup. Actual HLG media/display playback remains unverified. Physical monitor HDR activation is not inferred from the filter's signal label. GitHub publication remains on hold; only the local filter was updated, with settings and the previous 0.4.0 backed up.

## UI / standalone

The obsolete 화질 보정 tab is removed; automatic protection remains. Three tabs are 보간 설정 / 연결 설정 / 진단. Connection settings provide current-folder registration/open/unregister, with a player-running check and per-folder ownership checks. A machine registration requests Windows elevation when removal is needed. No file deletion occurs from unregister. 프로그램 제거 appears only with a valid same-folder installed uninstaller. Moving a portable folder requires unregistering first and registering again at the destination.

Diagnostics expose actual input/output formats and SDR/HDR10/HLG signal type. Build `0.4.0` is a local self-contained portable trial, not a GitHub release. The original filter and settings are backed up separately when installed for user testing.

Manual screenshot provenance: `docs/manual/images/controller.png` is the offline WPF `limit120` illustrative fixture at 464×461 logical content pixels / scale 1, copied from `build/hdr1/ui-qa/limit120-1.png`. It uses the current three-tab layout and is not a live playback capture. No original movie frames or user screenshots are shipped.
