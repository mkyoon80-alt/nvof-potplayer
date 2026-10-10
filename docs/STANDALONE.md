# Native standalone checkpoint — 0.3.1-standalone.1

Local trial based on Newton + Cost.1. No GitHub publication.

## Scope

The product no longer loads NvOFFRUC, its bridges or a CUDA runtime. CPU-memory NV12 input now uploads to a NVIDIA D3D11 device and reads back the same native synthesis used by GPU inputs. GPU input remains on the shared device. SDK API headers are still required to build, and the NVIDIA driver is required to run.

The controller carries .NET/WPF. The installer omits the old FRUC/CUDA-specific assent page and removes only explicitly named obsolete runtime files during upgrade. SDK and remaining third-party notices are retained.

## Completed validation

- Release native build, precompiled shaders and six scheduling/policy tests passed.
- Native CPU upload/readback: moving synthetic reference, repeated resets, resolution changes, immutable retained frames, all-code NV12 preservation and malformed-input rejection passed.
- NV12 CPU/GPU and P010 GPU DirectShow tests passed, including seeks, flush, preview and shutdown.
- Hardware cost integration, deterministic provenance oracle, six motion-layer scenes and motion-failure protection passed.
- Interop stress: 468 pairs, 100 resets, four recreations and concurrent renderer access completed without a hang.
- Kokoore 1:20, Jishou 4 episode 5:20 sequence and F1 title sequence: complete output pixel SHA-256 and output timestamps exactly match Cost.1. Generated duplicate raw outputs were removed after comparison.
- Tests ran on the local RTX 5090. These are not a RTX 5070 4K60-to-120 guarantee.

Build evidence: build/standalone1 (ignored). Package/installer verification results are recorded there separately after packaging.

No quality improvement is claimed for this dependency-only checkpoint. CPU input performance changes because its former FRUC path is replaced.
