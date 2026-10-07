# ×2-only update — 0.2.0-preview.5

Fixed 60p and 120p modes were removed after the user rejected fractional-phase motion quality. The DirectShow filter ignores legacy fixed-rate keys and always derives double the source rate. The controller exposes only an output explanation; source FPS selection, including 60 fps, is unchanged. Normal UI startup removes obsolete TargetFps while keeping DoubleRate=1 for rollback compatibility. Preview rendering does not migrate settings.

Validation on 2026-10-07:
- Release native build and self-contained .NET 10.0.12/WPF publish succeeded.
- Six CTest suites passed.
- UI self-test passed, including both legacy fixed-target migrations, preference preservation, idempotence, source controls and normal/minimum layout.
- Native D3D11/MF harness: old TargetFps=60 and 120 with DoubleRate=0 each produced 24000/1001 → 48000/1001. Also 24 → 48 and 60 → 120 passed.
- Source exclusion and master-off retained original FPS and GPU transport.
- All six native cases checked metadata, timestamps, end-of-stream and seeking.
- Rounded-timestamp ×2 pixel regression, 16 pairs each for synthetic pan and moving foreground: 17 originals each preserved exactly, zero wrong originals and zero near-endpoint motion requests.
- Actual WPF previews at normal 100%, minimum 150%, and normal 200% were visually inspected: no clipped controls or removed fixed-output choices. Offline fixtures do not prove physical display/keyboard behavior.

The accepted midpoint engine, source cadence correction, scene/repetition protection, color metadata, seek re-prime and interop lifetime safeguards are retained. Experimental subpixel refinement remains disabled. Generic multi-phase libraries and test programs are historical development tools, not selectable product modes.

GPU-resident is not zero-copy. GPU copies, plane transfers, blocking FRUC processing and final completion waits remain. A fully asynchronous external-fence/semaphore pipeline is not complete. Real-video interpolation artifacts remain possible; synthetic tests are not a guarantee for all material. Actual PotPlayer acceptance after installing this new build remains to be confirmed separately.

No GitHub publication is part of this update.

Installed to the existing bin directory on 2026-10-07. Installed self-contained UI self-test, filter/property-page COM verification and packaged runtime dependency audit passed. Existing INI preferences were preserved byte-for-byte. Full changed-file rollback: backup/x2-only-20261007-181540-730/restore.ps1. A normal controller launch performs the documented obsolete-key cleanup. Actual PotPlayer playback acceptance of this build is still pending.
