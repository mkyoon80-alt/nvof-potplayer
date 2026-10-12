# GPU selection across HW/SW switching — v0.4.8

The accepted 0.4.7-adapter.1 changes are included in v0.4.8. The user confirmed
that the trial works on the reported RTX 5080 + enabled AMD integrated-GPU PC
after the HW-to-SW switching failure. This does not verify every hybrid-GPU
configuration. Trial names below identify the validation build.

## Evidence

The supplied second-PC log repeatedly reaches native NV12 playback, stops, then
fails software upload with `Requested adapter is not NVIDIA hardware`. No subsequent software stream starts in the failing attempts. This is connection/adapter selection failure, not evidence of a GPU
frame-processing deadlock or an HDR precision failure.

The previous code trusted the renderer's adapter index without checking its
vendor. The log does not identify the exact adapter index or model selected.

## Change

- Validate renderer hints against DXGI hardware NVIDIA adapters.
- Remember the actual GPU LUID only after renderer activation succeeds. Retain
  it across input-only reconnects and clear it when the output disconnects.
- Prefer that LUID over a renderer hint, including changed enumeration order.
  If the retained GPU is unavailable, fail instead of silently changing GPUs.
- On a fresh connection, an invalid/non-NVIDIA hint falls back to a validated
  NVIDIA adapter; the renderer must still accept device activation.
- No frame-path changes, shader changes, per-frame logging, or tracing hooks.
  Diagnostic selection messages are limited to connection-time hint overrides.

Decoder allocator negotiation, P010 promotion, HDR metadata, transport, and
mutex/resource ownership rules remain in place.

## Validation on the main RTX 5090 PC

The main PC has no integrated GPU. A synthetic renderer advertises a non-NVIDIA
software-adapter index or a deliberately invalid index. This tests the same
failure path, not the physical 5080/AMD combination.

- Released 0.4.7 fails the injected non-NVIDIA hint with the exact same log error.
- Candidate succeeds with non-NVIDIA and invalid hints: repeated P010 HW to
  NV12 SW returns, direct SW startup, and legacy VideoInfo startup.
- NV12 HW to NV12 SW to HW succeeds for three cycles with an invalid hint.
- Existing CPU NV12 pixel/pitch/snapshot checks, D3D11/MF negotiation and
  lifetime checks, P010 software startup, and HDR reconnect checks pass.
- Four HDR switch scenarios each pass twelve cycles, including early native
  activation, deferred transport, declined P010, and unknown allocator cases.
- Native NV12, P010, Dolby side-data, HDR10, and HLG fixture checks pass. These
  are transport/metadata tests, not a claim of display-level Dolby Vision.
- Pure CPU policy tests cover AMD-first enumeration, multiple NVIDIA devices,
  changed order, software adapters, missing retained GPU, and single GPU.

Detailed local results: `build/second-pc-20261012/validation.json`.
Candidate SHA256:
`b55d6c0f304aee0ae3b2e39a5d2d8e5b2ce539107180aef06d248b4ca763ad90`.

## Updating and checking

Close PotPlayer and NVOF settings and back up the installation folder. Update
to v0.4.8 while preserving the existing INI. No integrated-GPU disablement or
GPU setting change is required for this fix. Reopen the video and check HW/SW
switching and a fresh SW start. The release status build tag is `0.4.8`.
