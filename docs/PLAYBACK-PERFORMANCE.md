# Playback scheduling — v0.4.7

Native decoding, interpolation, and rendering can share a D3D11 device.
Average GPU utilization does not measure how long a frame waits for that
device, an output buffer, or the renderer. Timing traces separated these
waits before changing the playback path.

## Accepted changes

- Restore the host context state and release shared device locks during GPU
  completion waits. Keep the engine mutex, source/output leases, and completion
  boundary. After a wait, reacquire the device and capture its current state.
- Use context-ordered P010 delivery on the shared-context player path, as for
  NV12. This removes unnecessary CPU waits; it does not remove GPU ordering,
  source normalization, or resource ownership. Other callers retain blocking
  completion unless they explicitly establish ordered consumption.
- When scene/motion aggregate readback is not ready, wait outside the shared
  device lock and retry under protection. Preserve the NVOFA input-completion
  boundary before execution. No unsynchronized input reuse is introduced.
- Size the GPU output sample allocation to two source intervals for the chosen
  interpolation multiple, bounded to 4–32 requested slots. Preserve a larger
  renderer request and ordinary CPU allocation behavior. This gives the next
  pair room to prepare while the renderer consumes the current pair. Extra
  retained surfaces can use more memory; this is not unlimited queue growth.
- Extend optional timing logs with stage waits, output gaps, and renderer
  quality notifications. Quality notifications still pass upstream. Logging
  does not suppress dropped-frame counters or alter timestamps/output rate.

The final changes include the locally accepted perf.6 behavior. No change to
Newton synthesis, scene decisions, output timestamps, or original pixels was
made for these scheduling optimizations. Thin-line quality changes in this
release are documented separately in [THIN-OVERLAY-TRIAL.md](THIN-OVERLAY-TRIAL.md).

## Evidence and limits

Shared-context tests cover concurrent renderer activity, state restoration,
retained outputs, shutdown, and NV12/P010 precision. Actual-player comparison
on an RTX 5090 found foreground Dune HW playback at the 240fps cap stable with
NVIDIA Low Latency Mode **On** after perf.6. Switching focus to another window
still produced slow drop accumulation. Earlier 144fps comparison did not show
that accumulation on the 165Hz display. These are local observations, not a
guarantee of zero drops or 4K/240fps on every GPU.

Removing the driver frame-rate cap helped but did not eliminate the reported
drops. Turning Low Latency Mode off helped diagnose the interaction but is
not a required workaround in v0.4.7. Compare Max Frame Rate and Background
Application Max Frame Rate in the player-specific profile, and distinguish
foreground playback from background playback. Do not infer successful display
of every generated frame from filter delivery FPS; a 165Hz display cannot show
240 distinct frames each second.

Use `TracePerformance=1` in the existing `[Nvof]` section only when collecting
a reproduction. Preserve the rest of the INI and disable tracing afterward.
Recorded spans are wall-clock API/lock/wait times and can overlap; they are
not isolated GPU execution timestamps. See [SW-DECODING.md](SW-DECODING.md).
