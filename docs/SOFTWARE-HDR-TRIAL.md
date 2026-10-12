# Software HDR cold-start trial

The accepted changes documented below are included in v0.4.7. Trial names and rejected experiments are retained as validation history.

Installed trial: `0.4.6-hdrsw.4`, based on `0.4.6-perf.6`. Not a public release.
Installed after verification on 2026-10-12 at 15:02, with the previous
binary and settings backed up and settings unchanged. The subsequent
actual-player Dune run (PID 44148) preserved P010 on every SW return and
delivered frames after every switch. See the local installation record
for the installed binary hash and the validation scope below.
The `hdrsw.3` precision-retention experiment was withdrawn after actual
PotPlayer repeated HW/SW switching stalled. Source was restored to the
hdrsw.2 connection policy before the narrower allocator-confirmed change
described below; perf.6 optimizations were retained.

## Observation

The local Dune software-decoder log advertised an untagged NV12 input.
The filter recovered BT.2020/PQ from the compressed HEVC header, but the
decoder still delivered 8-bit host frames. Recovering color tags cannot
restore the precision discarded by that negotiation. Native decoding of
the same source instead delivered P010 textures and Dolby side data.

This is separate from the user's excessive HDR-to-SDR brightness report.
On 2026-10-12 the user confirmed the appearance with NVOF excluded, Windows
in SDR, and ST 2084 correction enabled. Switching HW/SW decoding and changing
the tone-mapping curve or SDR luminance made no visible difference. Turning
off PotPlayer's "D3D11 HDR 출력을 H/W로 처리" (Use H/W HDR output mode) made
those controls responsive; bright detail returned with Auto tone mapping and
125-nit SDR luminance unchanged. ST 2086 correction remained off.

This is a user-confirmed player-setting resolution for this SDR-viewing case,
not a filter code fix or a measured characterization of PotPlayer's internal
HDR pipeline. Hardware decoding and GPU interpolation can remain enabled.
It does not establish the cause of the separate fullscreen red/oversaturated
report or prescribe this setting for actual HDR display output.

## Change

- For a software HDR connection, ask the upstream decoder whether it
  accepts P010. Offer P010 downstream only when that query succeeds.
- During output connection, request input reconnection to the agreed
  P010 type before streaming. This follows Microsoft's
  [DirectShow pin-reconnection contract](https://learn.microsoft.com/en-us/windows/win32/directshow/reconnecting-pins):
  query the upstream pin first and request reconnection on the connection
  thread. It is not a per-frame reconnect or an 8-to-10-bit pixel expansion.
- Keep the existing native contract: an NV12 media type may describe
  actual P010 GPU samples. Do not force native decoding to host transport.
- Explicit SDR color tags remain authoritative even when the compressed
  source header describes HDR. Do not label a tone-mapped SDR output HDR.
- Use the existing CPU P010 upload and per-sample HDR/Dolby metadata copy.
  Record CPU-side metadata receipt in the same diagnostic counters used
  by native samples; CPU metadata forwarding itself already existed.
- During an input-only SW/HW reconnect, resolve missing raw color tags
  from the decoder proposing the connection. DirectShow checks the new
  input against the retained renderer type before `ConnectedTo` becomes
  available. Previously this check fell back to SDR and rejected the
  retained PQ output. The candidate pin is scoped to `ReceiveConnection`;
  old connection color is not cached or inherited.
- Do not force P010 during an input-only reconnect to a retained HDR
  renderer. At that point PotPlayer has not necessarily selected HW or SW
  transport. Acceptance of P010 in `QueryAccept` is not evidence of a
  software connection. Native surface selection can occur afterward.

## Validation and limits

The isolated DirectShow test covers cold-start NV12-to-P010 negotiation,
PQ/BT.2020 output tags, exact original 10-bit pixels, Dolby payloads on
original/generated frames, pause, seek, restart, and retained-frame
lifetime. The new cold-start regression fails against the perf.6 binary.
The local report and test outputs are in `build/quality047/cpu-hdr/`.

The hdrsw.2 regression starts with software HDR, retains the P010/PQ
renderer, and switches to native P010 samples advertised as legacy NV12,
then back to software P010, for three cycles. It checks interpolation,
timestamps, exact software originals, and per-frame HDR/Dolby payloads.
A reconnect with no color evidence is rejected rather than inheriting the
old HDR tag. This regression fails against hdrsw.1 at the SW-to-HW input
connection. Evidence is in `build/quality047/cpu-hdr-toggle/`.

Actual PotPlayer hdrsw.1 playback on 2026-10-12 confirmed a Dune software
cold start with P010 input/output, BT.2020/PQ, and about 240 fps delivery.
The user reported that HDR appeared to work, but switching to hardware
decoding stopped playback. Actual-player validation of the hdrsw.2
transition fix was confirmed by the user with hdrsw.2. Its live log showed
successful HW-to-SW-to-HW playback, but the middle SW session reverted to
8-bit NV12 while retaining PQ color signalling. The hdrsw.3 test explicitly
models that default NV12 reconnection and requires actual P010 decoder
output on every return to SW. It also covers a native decoder that only
accepts an NV12 media type. Evidence is in `build/quality047/cpu-hdr-return/`;
actual player validation of that precision experiment failed as described
below. The later hdrsw.4 tests add the missing deferred-native contract
check. Neither fixture alone establishes real-player switching stability.

The hdrsw.3 checks also cover native activation before the new input
connection, which must preserve the already committed native format.
The first general transport run hit a GPU scene-statistics readback
timeout before its pool-exhaustion assertion. The unchanged candidate
and hdrsw.2 baseline both passed a repeat of that check; the initial
failure is retained in `first-run-tests.json` and `native-retry.json`.
All 19 validation groups initially passed, including 18/18 CTest tests.
They did not check that the legacy native media contract remained intact
before deferred HW selection. Passing them did not establish real-player
transition stability. No GPU timing or timeout constants changed.

## hdrsw.3 failure and rollback (2026-10-12)

The user reported a freeze after repeated switches. In PID 17424:

- HW-to-SW retained actual P010 and delivered frames normally.
- A subsequent SW-to-HW switch forced a P010 media reconnection before
  MF native surface selection. Its first sample arrived about seven
  seconds after streaming started.
- The final SW-to-HW switch repeated that order. Streaming started at
  14:35:22.324, but no input sample arrived before Stop at 14:35:57.741.
  Thus the final observed stall preceded frame interpolation. This trace
  does not expose the upstream decoder's internal wait or prove a GPU
  deadlock.

The added input-connection P010 request was removed, and the exact
previous hdrsw.2 binary was restored after PotPlayer exited. Current
settings were not changed. Backup and original logs are in
`backup/rollback-hdrsw3-20261012-143855/`; the rollback record is
`build/quality047/cpu-hdr-return/rollback.json`.

The new deferred-native regression accepts both NV12 and P010 at the
decoder pin, but requires the proposed NV12 contract to survive until
native activation. It then delivers actual P010 GPU frames, returns to
explicit CPU P010, and repeats for 12 cycles. It checks precision,
timestamps, interpolation, metadata ownership and retained-renderer
format. It rejects hdrsw.3 and passes the installed hdrsw.2 binary.
Further checks and source-build results are recorded under
`build/quality047/cpu-hdr-rollback/`. These fixtures do not reproduce
PotPlayer's closed-source decoder implementation. The user subsequently
confirmed that repeated switching no longer froze after rollback.

The following actual-player log (PID 26168) still shows the precision
limitation: at 14:45:02.450 the SW session received and output NV12
(`conversion=8-bit-upload`); at 14:45:06.234 the next HW session received
and output actual P010 (`conversion=none`, PQ, mastering/light metadata).
The final status describes that last HW session only. It must not be used
as evidence that the intervening SW session preserved 10-bit precision.

Limitation of the restored hdrsw.2 baseline: starting Dune in SW obtains P010/HDR, but
an input-only HW-to-SW switch can return to NV12 with PQ signalling.
Do not describe that return path as preserving 10-bit precision. Reopening
the file from a fresh SW graph is the tested way to obtain P010 while a
safe retained-connection negotiation is still being investigated. The
subsequent hdrsw.4 result below supersedes this limitation for the tested
PotPlayer/Dune connection.

The same software playback supplied no HDR/Dolby sample side data
(`inputMask=0`, renderer returned mask `0`). This confirms the 10-bit
HDR10 color path, not actual software Dolby metadata delivery. Synthetic
tests validate copying metadata only when a decoder supplies it.

A decoder that rejects P010 is not forcibly reconnected. Dolby
metadata is forwarded only if supplied by the decoder; it is not created
from the filename. This trial does not establish Dolby Vision display
output or support for every Dolby profile.

## hdrsw.4: confirm host allocation before requesting P010

The diagnostic-only actual-player run (PID 45488, 2026-10-12) distinguishes
the transport at allocator selection, after the early input connection:

- At 14:52:30.975, native surface selection was already committed before
  `GetAllocator`; the decoder supplied its own allocator. Actual input
  was P010 GPU video.
- At 14:52:41.398, the return to SW selected the ordinary host allocator
  created by this input pin. Historical native-requested state was still
  set, but native selection was not committed. Actual SW input was NV12.
- At 14:52:50.455, the next HW selection again supplied its own allocator
  after committing native transport. Actual input was P010 GPU video.

The new request occurs only on the first successful allocator notification
of a stopped input connection, when the decoder selects the exact ordinary
host allocator created by our pin. It additionally requires a retained
10-bit HDR renderer, a lower-precision input, no committed native transport,
HDR source/color evidence, and upstream `QueryAccept(P010) == S_OK`.
Only then can the historical native-requested flag be bypassed. A decoder's
own allocator, unknown allocator, rejected P010, or committed HW connection
does not trigger this request. Disconnect clears allocator identity and the
one-shot flag. Reconnection stays on the connection thread before streaming;
there is no reconnection from Receive, Run, or a worker thread.

The regression starts with ordinary CPU memory and no HDR sample side data,
then alternates native legacy NV12 contracts carrying real P010 GPU samples
and default-NV12 CPU proposals for 12 cycles. It requires the native proposal
to remain NV12 before deferred HW activation and every accepted CPU return to
negotiate actual P010. It checks exact original 10-bit values, interpolation,
timestamps, unchanged renderer media type, resource release, and absence of
stale native HDR/Dolby payloads on subsequent CPU samples. Buffer sizes follow
the actually negotiated input type, including the smaller initial NV12 size.
Additional cases cover early native selection, native decoders rejecting a
P010 media type, CPU P010 rejection, and custom CPU allocators.

The previous hdrsw.2 binary fails the CPU-return precision assertion; hdrsw.3
fails the pending-native-contract assertion. The candidate passes both in
the fixture, plus the existing CPU8/CPU10, interlace, native transport,
HDR10/HLG/Dolby/VP9/AV1 checks and 18/18 CTest tests. Reports, the actual-player
probe trace, hashes, and installation record are under
`build/quality047/cpu-hdr-allocator/`. Fixture results were followed by the
actual-player validation below.
The tests do not establish that PotPlayer supplies Dolby sample metadata
when decoding in software; its observed SW samples supplied none.

### Actual-player result: PID 44148, 15:04:15–15:05:57

The user completed the requested repeated HW/SW playback sequence and
closed PotPlayer. The saved log contains five HW sessions and six SW
streaming sessions (including one additional SW restart). All five
HW-to-SW returns selected the offered host allocator, requested P010,
started the 10-bit CPU upload, and delivered frames. Every HW session
delivered actual P010 GPU surfaces without a forced P010 native contract.

All 11 streaming sessions received their first sample 113–421 ms after
streaming started, delivered frames, and reached the normal resource-release
stop path. No error, failure, or timeout entry was recorded. The previous
multi-second no-input stall was absent in this run. This is evidence for
this file/player sequence, not a guarantee against all switching failures.

The final SW session received 242 source frames and delivered 2,407 frames.
After startup, complete reporting intervals measured 239.74–240.34 delivered
frames per second. Final status explicitly records `cpu-upload-d3d11`, input
`P010`, `10-bit-upload`, output `P010`, and `HDR10`, then a normal stopped
state. Thus precision is retained on the actual SW path, rather than only
the last HW session or the HDR label. Other intervals include user-control
transitions; this run is not a controlled frame-drop benchmark.

SW sample HDR/Dolby metadata masks remain zero. HDR10 color signalling and
10-bit precision are confirmed; actual SW Dolby metadata delivery and visual
color appearance are not independently established. Saved evidence is
`potplayer-hdrsw4-44148.log`, `potplayer-hdrsw4-44148-status.json`, and
`playback-validation.json` in the directory above.
