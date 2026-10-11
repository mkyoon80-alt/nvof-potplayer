# CPU decoded 10-bit input — 0.4.3-cpu10.7 (local test build)

The implementation documented below is included in v0.4.4. Trial names identify the original verification builds.

## Transport

Software decoding can now feed the GPU interpolation engine. Supported CPU
sample layouts are P010, P016, P210 and Y210. The filter uploads the decoder's
16-bit words to a reusable six-slot GPU pool. Format conversion, field
reconstruction when needed, and interpolation run on D3D11. There is no
per-frame CPU pixel conversion or full-frame GPU readback. Ambiguous WEAVE
samples in an interlace-capable stream use a four-byte GPU counter readback
to decide whether moving fields still require reconstruction.

The renderer receives progressive P010 textures. P210/Y210 chroma is reduced
from 4:2:2 to 4:2:0 **after** field reconstruction, on the GPU. P016 uses its
upper ten bits; this is a 10-bit output path, not a 16-bit preservation mode.
The decoder may itself choose P010 for a 4:2:2 source. In that case the filter
cannot recover chroma already reduced by the decoder.

This does not enable hardware decoding of otherwise unsupported codecs.
PotPlayer's compatible D3D11 renderer and the NVIDIA interpolation device
are still required. Existing native GPU decoding remains a separate path.

## Cadence and lifecycle fixes

- AVC SPS bit depth prevents an implicit NV12 downgrade when a verified
  10-bit H.264 software decoder negotiates its output.
- SPS `frame_mbs_only_flag` distinguishes progressive AVC from a raw media
  type merely capable of carrying interlaced samples. This fixes the Gurren
  Lagann 23.976p source incorrectly becoming 47.952 input / 95.904 output.
  Its 120 fps ceiling now selects 23.976 × 5 = 119.88 fps.
- Actual interlaced input produces two GPU-reconstructed pictures per sample,
  using field order and sample flags. A progressive or already blended WEAVE
  sample stays one complete picture with its full duration. Repeated moving
  comb evidence can correct a WEAVE flag on untouched interlaced pictures.
- Repeat-field cadence and single-field samples are not supported by this
  initial path. No claim of general telecine support is made.
- GPU output activation occurs during connection, before renderer buffer
  negotiation. Deferring it until Run could prevent PotPlayer from opening
  software-decoded P010 video.
- Stop releases the engine; the next Run recreates it before uploading.
  This fixes the null-engine access violation in `directshow_cpu10` restart
  testing. Seek/flush clears upload history and frame timing.

## Verification on the local RTX 5090

- All 17 CTest checks passed, including GPU conversion against the CPU
  reference for all 1,024 code values, padded pitches, four layouts, moving
  and static fields, reset and buffer reuse.
- DirectShow tests cover a renderer requiring GPU activation before allocator
  negotiation, progressive SPS with an interlaced raw header, genuine MBAFF,
  WEAVE and alternating field/progressive samples, timestamps through EOS,
  seek, Stop/Run, texture lifetime and original-frame precision.
- Forty-eight decoded Gurren frames retain their original P010 pixel values
  exactly. Actual PotPlayer playback reports CPU-upload P010, 23.976 input,
  119.88 output and multiplier 5.
- The supplied Dua Lipa source is H.264 10-bit 4:2:2 MBAFF. PotPlayer negotiates
  P010 and marks the delivered whole pictures for WEAVE. Its interlace-capable
  stream negotiates a 50 fps field cadence and 100 fps output under the 120 fps
  ceiling. Whole pictures are interpolated across their actual timestamps.
  Y210/P210 field reconstruction is separately tested; actual PotPlayer P010
  negotiation is not evidence of end-to-end 4:2:2 transport.
- On 48 decoded 1080i Y210 frames, the previous CPU preparation benchmark took
  about 6.1 seconds; GPU preparation took about 1.3 seconds for the same field
  test. This excludes video decoding and display and is not a player FPS claim.
- Native P010 precision and existing HDR10, HLG, VP9/AV1 HDR and Dolby Vision
  metadata transport checks passed. Metadata transport alone does not certify
  a Dolby Vision display pipeline.

The original installation and settings are backed up locally. The cpu10 trial itself was not published separately; its changes are included in v0.4.4.

## Pause fix (cpu10.5, confirmed in playback)

The upload draw now uses its own D3D11 context state, restoring the renderer's
state afterward. Device multithread protection spans the complete swap/draw/
restore sequence. The previous upload pass overwrote the shared immediate
context's shaders, render targets, viewport and other pipeline state. This is a
concrete defect relevant to pause presentation. The user confirmed that the
green flash no longer appears with cpu10.5 in actual PotPlayer playback.

All 17 checks pass with renderer-state preservation assertions. Seven CPU
DirectShow input cases also pass Pause/Run, retained-sample pixel integrity,
seek and Stop/Run checks (P010 progressive/SPS/interlaced/WEAVE/mixed and
Y210/P210 interlaced).

For the supplied Dua Lipa clip, a separate decoded-frame replay measured
whole-pair protection holds in the reported intervals. Raw 25 fps input gave
28, 17 and 38 holds in 57–62 s, 120–126 s and 181–188 s respectively. GPU field
reconstruction from P010 reduced these to 0, 2 and 3. The last three were at
187.94–187.98 s, outside the reported 183–186 s interval. Steady GPU processing
was about 5 ms at the 95th percentile; initial shader setup is excluded.

Actual PotPlayer input was subsequently captured locally at 59 seconds. The
six P010 samples were 40 ms apart, all tagged WEAVE (flags = 8). Comparing the
59.2 s picture with the decoded source confirmed vertical blending before
the filter: the adjacent-row weights fit approximately 0.25 / 0.50 / 0.25.
Mean luma error was 1.23 ten-bit codes against that blend, versus 18.27 codes
against the unprocessed source. It was already a 25 fps progressive picture,
not the untouched pair of fields used in the replay above.

Do not force field reconstruction on those already blended pictures.
For this CPU-upload interlaced path, upstream software deinterlacing needs to
be disabled before the filter can use both original field times. The local
PotPlayer menu is Video > Deinterlacing > Do not use (영상 > 디인터레이스 >
사용하지 않음).

## Moving-field detection (cpu10.6, local playback confirmed)

After disabling upstream deinterlacing, the locally captured opening frames
matched the raw decoded source (about 1.2 ten-bit luma codes mean absolute
error), but PotPlayer still marked every sample WEAVE. Therefore sample flags
alone cannot distinguish the original fields from the earlier blended input.

On interlace-capable CPU input only, a sparse GPU scan looks for alternating
lines in moving regions, with agreement between rows from the same field.
Two consecutive positive pictures enable field reconstruction; sixteen
consecutive negative pictures release it. Flush, discontinuity and format
changes reset the decision. Progressive AVC identified by its SPS bypasses
this detector. The negotiated field order is used when correcting WEAVE.
This is a conservative heuristic, not a general inverse-telecine algorithm.

Decoded replay with the automatic detector reduced protection holds from
28 to 0 in 57–62 s, 17 to 2 in 120–126 s, and 38 to 3 in 181–188 s. Remaining
holds occurred at 120.80, 122.12 and 187.94–187.98 s; none remained within
183–186 s. These are internal protection events, not renderer dropped-frame
counts. No global motion-protection threshold has been relaxed.

GPU tests cover moving combs, progressive motion, upstream line blending,
static alternating detail, decision release/reset and renderer context
preservation. The user confirmed smoother playback with cpu10.6 and upstream
deinterlacing disabled. The corresponding player log confirms automatic field
reconstruction on WEAVE samples: topFirst=1, sampleFlags=8, duration=400000.
Temporary decoder-frame capture has been removed.

Disabling upstream deinterlacing is a workaround for this local CPU-input
path, not a universal requirement of frame interpolation. The user reports
that Bluesky/Fluid Motion works with PotPlayer's default setting.

The user's BFRC Status capture on 2026-10-11, playing the same Dua Lipa source
with PotPlayer deinterlacing set to Auto, reports:

- GPU: Radeon RX Vega; VpMode: FrameInterpolation.
- Decoder: Built-in Video Codec/Transform; DXVADecode: Inactive.
- InputFrameType: Progressive; InputFrameRate: 25 fps; size: 1920x1080.
- OutputFrameRate: 120 fps; ZeroCopyMode: Inactive; LastError: None.

This is evidence that BFRC is interpolating a progressive 25 fps decoder
output, not reporting its own deinterlace mode for this playback. Earlier
player screenshots show NV12 at the decoder output and renderer input, so
that comparison does not establish preservation of source 10-bit samples.
The actual pixels entering BFRC have not been captured: its progressive flag
alone cannot prove that its upstream processing is pixel-identical to the
P010 row blend captured at NVOF input. Default-setting compatibility remains
unfinished; BFRC's status does not demonstrate an upstream raw-field bypass.

## Original-field connection review (2026-10-11)

The following probes were tested against the local PotPlayer built-in decoder
with its deinterlacing setting left at Auto. These were temporary diagnostic
builds; the installed filter was still cpu10.6 during this review.

- Restricting CPU input to 10-bit 4:2:2 negotiated P210, but did not establish
  an unprocessed-field bypass.
- Advertising interlaced output/WEAVE did not disable upstream processing.
  Those misleading output flags were removed; our output is progressive.
- Exposing IMFVideoProcessor through MR_VIDEO_MIXER_SERVICE was queried, but
  none of the advertised processor-mode/capability methods were called.
- Querying the upstream decoder filter for ICodecAPI, IPropertyBag and
  IPropertyBag2 returned E_NOINTERFACE for all three (21:08:14, pid 9448).

[ICodecAPI](https://learn.microsoft.com/en-us/windows/win32/api/strmif/nn-strmif-icodecapi)
controls properties only on a decoder that implements it. The
[video mixer controls](https://learn.microsoft.com/en-us/windows/win32/medfound/using-the-video-mixer-controls)
configure downstream processing; advertising them does not guarantee that
an upstream decoder will skip its own deinterlacing.

No supported automatic bypass was established by these checks. This is not a
claim that every possible PotPlayer-specific interface has been ruled out.
The verified original-field route remains upstream deinterlacing **Do not
use**, followed by the existing GPU reconstruction. Already blended 25p input
cannot be used to assess reconstruction of the original two field times.
The separate blended-input motion-retry experiment was not installed and has
been archived rather than mixed into this deinterlacing work.

## First-picture combs after seeking (cpu10.7)

The user reported brief comb-like horizontal lines on moving edges during
repeated seeks. A regression test reproduced a concrete cause: flush and
discontinuity discarded both pixel history and the confirmed raw-field
classification. For decoder samples tagged WEAVE, the first pictures then
passed through without reconstruction until moving-comb detection locked again.

Same-input flush, new segment, preroll and timestamp discontinuities now retain
only a learned raw-field hint. All previous pixels, interpolation history and
timestamps are still discarded. On the first visible picture the GPU checks
for spatial comb evidence before using the hint. Reconstruction uses only that
new picture, with no temporal weave from before the seek. New streams, format
changes and full resets clear the hint. This is a heuristic for the already
confirmed interlaced input, not a general single-picture interlace detector.

The first-picture regression failed on cpu10.6 and passed after the fix. Five
forward/backward jumps, each with four consecutive temporal resets, check both
field times against independently known rectangle positions. This catches
retained combs and pre-seek pixels. A blended-picture control stays progressive,
and a full reset prevents the hint from leaking to a different stream.

All 17 CTest checks pass, including all 1,024 code values and four upload layouts.
Seven DirectShow cases pass pause, retained-frame integrity, seek and Stop/Run.
Decoded Dua Lipa and Gurren sequences (48 frames each) also pass; the progressive
source retains exact original P010 values. Previously captured Auto/blended
PotPlayer input still does not trigger field reconstruction in replay.

The field reconstruction arithmetic itself is unchanged. Wider edge-search
candidates were evaluated separately on synthetic ground truth; unguarded
variants improved some shallow diagonals but damaged detailed textures. They
have not been included in this build. Actual player confirmation of the rapid-
seek improvement remains pending; no GitHub release was published.
