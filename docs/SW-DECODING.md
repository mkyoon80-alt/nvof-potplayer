# Software decoding and GPU delivery — v0.4.6

Native DXVA is recommended when available, but is not required. With a
compatible D3D11 renderer, software-decoded NV12 SDR frames now upload to the
NVIDIA GPU, use the common interpolation pipeline, and reach the renderer as
GPU surfaces. This path does not read the synthesized full frames back to CPU
memory. Existing P010/P016/P210/Y210 input support is retained.

## Why switching could freeze playback

PotPlayer can reconnect only the input pin when switching between hardware and
software decoding. The renderer and its output connection can survive that
change. The old CPU NV12 route could then attempt host-memory delivery through
a renderer configured for GPU surfaces, while the output media type still
described the preceding P010 stream.

NV12 upload is now activated during output connection when native input has
not been committed. Native decoder activation can still take over before
streaming. StartStreaming also handles input-only reconnects. The actual output surface format is
announced on the first delivered sample when it changes. A renderer that has
already accepted GPU delivery cannot silently fall back to incompatible CPU
delivery. An initial NV12 connection to a renderer that rejects GPU activation
can still use the legacy CPU route. The recommended and player-tested setup is
PotPlayer's built-in D3D11 renderer.

## Why starting directly in SW mode could bypass interpolation

The v0.4.5 player test covered switching HW decoding off, but missed the stricter
initial renderer connection. With software NV12 input, GPU output activation
was delayed until StartStreaming. PotPlayer could reject the output allocator
earlier, remove NVOF from the graph, and play at the source rate. Starting with
HW decoding had already initialized the renderer, so switching to SW worked.

In v0.4.6 both NV12 and P010 software paths prepare GPU output during
CompleteConnect, before DirectShow negotiates the output allocator. The source
still supplies CPU buffers until it explicitly commits to native GPU decoding.
Native activation before and after renderer connection remains covered.

Fully close PotPlayer, update while keeping NvofPotPlayer.ini, and reopen the
video with hardware decoding off and the built-in D3D11 renderer selected.
Enabling HW first is unnecessary. Check NVOF in the active filter list and
interpolation status in the controller; S/W describes decoding, not synthesis.

## Bit depth and interlacing

- CPU NV12 is 8-bit 4:2:0 SDR, even if the source file was encoded at 10-bit.
  Precision already discarded by the decoder cannot be reconstructed. A local
  HEVC Main10 source was observed arriving as NV12 with hardware decoding off.
- Supported CPU 10-bit input produces P010 10-bit 4:2:0. P016 uses its upper
  ten bits; P210/Y210 chroma is reduced on the GPU. See [CPU-10BIT.md](CPU-10BIT.md).
- This release does not add hardware codec decoding support or extend GPU
  deinterlacing to every CPU pixel format. For the supported CPU 10-bit
  interlaced path, set PotPlayer **Video → Deinterlacing → Do not use** to
  provide original fields, then reopen the video.

## Verification

For v0.4.6, the tightened renderer fixture rejects allocator notification until
GPU activation. The v0.4.5 baseline passes HW-to-SW switching but fails a fresh
software connection. The fix passes both legacy VideoInfo (used by PotPlayer)
and VideoInfo2 cold-start tests, plus the existing reconnection and pixel checks.

The user confirmed actual SW-only startup after fully restarting PotPlayer.
The corresponding log shows CPU NV12 upload and interpolation before any HW
decoder activation, including a steady interval at 119.18 delivered fps for
the roughly 23.81 fps source at ×5. This measures filter-to-renderer delivery,
not displayed FPS. These playback checks used the 0.4.5-swstart.1 candidate;
the same fix is released as v0.4.6 with new release version identifiers.

The following earlier verification applies to the v0.4.5 reconnect fix:


The pre-fix filter failed the first software frame delivery in an isolated
P010-to-NV12 reconnect test. The fixed filter passes repeated HW/SW/HW cycles
with the same renderer, cold software start, seek/flush, pause/resume, Stop/Run,
end of stream and timestamp checks. Padded uploads preserve the original NV12
pixels including chroma; retained frames remain unchanged after input reuse.
Truncated buffers are rejected.

Forty-eight locally decoded 1080p NV12 frames also pass original-pixel and
delivery checks. The user confirmed actual PotPlayer playback no longer
freezes with hardware decoding off. Logs show both switching directions and
eight steady one-second intervals at roughly 119 delivered frames per second
for a roughly 23.81 fps source at ×5. This is filter-to-renderer delivery, not
measurement of display refresh or dropped frames. Testing used an RTX 5090;
it is not a general 4K performance guarantee. Source media and raw frames are
not distributed.

## Optional timing logs

Timing logs are off by default. To collect a reproduction, close PotPlayer,
back up `NvofPotPlayer.ini`, and add `TracePerformance=1` to its existing
`[Nvof]` section. Reopen the video and reproduce the issue. Logs are written
under `%LOCALAPPDATA%\NvofPotPlayer`; remove the setting or set it to `0`
afterward. Do not replace the rest of the configuration or its section header.

The log measures wall-clock time in input handling, frame capture, synthesis
calls, buffer allocation, renderer delivery and status reporting, plus gaps
between input samples. These are API/lock/wait timings, not isolated GPU
timestamps. Some spans overlap. `deliveredFps` counts successful downstream
delivery, not frames actually displayed. Pause and seek windows must be
separated from uninterrupted playback before interpreting long waits.

## Separate fullscreen HDR report

Oversaturated/red HDR in fullscreen has been reported on another setup but
has not been reproduced locally. No cause or fix is claimed in v0.4.6. Compare
the same scene windowed/fullscreen, then uncheck NVOF in PotPlayer's Global
Filter Priority and reopen the video before repeating the comparison. Include
Tab playback information, renderer, Windows HDR setting and monitor model.
Disabling interpolation alone may leave the filter connected.
