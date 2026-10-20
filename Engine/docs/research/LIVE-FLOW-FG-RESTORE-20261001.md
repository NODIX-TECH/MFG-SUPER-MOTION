# Live Flow: restored DLSS FG / MFG

Completed 2026-10-01. Scope: Live Flow only.

## Delivered

- Restored the project's original native NVIDIA DLSS-G implementation in `ScreenNative/lf_dlssg.inl`, adapted to the current capture, presentation, upscaling and sharing helpers.
- `DLSS 4.5 FG / MFG` appears directly below DLSS 5. The multiplier selector offers ×2–×6. Actual runtime support determines the running multiplier; rejected evaluation counts step down to ×4, then ×2. A runtime rejection keeps ordinary output and displays a diagnostic. Offering ×6 is not certification that the bundled runtime supports it on every GPU.
- Enabling native DLSS FG disables FLOW MFG, and enabling FLOW MFG disables native DLSS FG. Enforced in the controller, saved settings and native worker. Conflicting old settings prefer FLOW MFG. Native DLSS FG is disabled in AMD mode; FLOW MFG remains available.
- Restored settings, persistence and frame-header signaling without changing message sizes. Both generation paths keep processing when DLSS 5 is off.
- The FG presenter is joined before returning swapchain ownership; initialization can be retried after failure; submitted GPU work is drained before releasing resources. A healthy-device GPU timeout terminates this worker rather than freeing resources still in use.
- NGX cache files use the user data directory (temporary-directory fallback). Runtime search still uses the worker directory.
- A missing/uninitialized NR runtime no longer prevents capture/FLOW MFG from starting. The manager no longer requires the NR model file just to enter Live Flow.
- Bundled `nvngx_dlssg.dll` from the project's existing runtime (file version 310.9.1.0). Packaged app-local Visual C++ runtime DLLs beside both native workers. Their imports require MSVCP140/VCRUNTIME140/VCRUNTIME140_1; these were missing beside the workers before this change.

## Validation

- NVIDIA and Radeon native builds: passed.
- Manager Debug and Release builds: passed, zero warnings/errors.
- `tests/test_dlss_fg_controls.py`: four tests passed, covering ten enabled/disabled multiplier encodings, unrelated frame bits, config migration/conflicts, both switch directions, AMD rejection, and persistent failure reporting.
- `tests/screen_fg_restore_test.py`: passed against a synthetic moving window with isolated settings. Tested NR-off operation, both switch directions, ×6 setting/persistence, repeated FG activation and clean shutdown. Final log: `tests/artifacts/fg-restore-integration-final.log`.
- On the available RTX 3060, native DLSS-G initialization succeeded but CreateFeature returned `0xBAD00002`. The application reported the failure and continued displaying ordinary frames. FLOW MFG resumed successfully afterward. **Generated DLSS-G image quality, throughput and ×6 operation were not validated on RTX 40/50 hardware.**
- `tests/screen_missing_nr_test.py`: passed with an intentionally absent NR DLL. FLOW MFG ×2 started, all 12 frame replies succeeded, zero NR evaluations, clean exit. Log: `tests/artifacts/fg-restore-missing-nr/worker.log`.
- `tests/screen_layout.fsx`: compiled layout passed at three main-window sizes; restored row/selector checked; overlay rendered and visually reviewed at its actual default width. Preview: `tests/artifacts/screen-overlay-preview.png`.
- Source runtime/Python files match the Debug and Release output copies. Dependency presence verified. Hash manifest: `tests/artifacts/fg-restore-manifest.json`.

## Scope and remaining limits

Native DLSS-G uses captured color, estimated motion and a flat depth plane, as in the original desktop implementation. This is experimental and is not equivalent to a game's native depth/motion integration. This change does not establish Lossless Scaling quality/performance parity. HDR compatibility prevents native DLSS FG and produces a visible explanation. Native generated-frame recording has not been validated.

There were no customer logs. The missing runtime dependencies and NR startup coupling are concrete deployment problems addressed here, not proof that every reported startup failure has the same cause. Radeon compilation and controller behavior were verified; no AMD GPU was available for a hardware run.

Updated runnable builds:

- `bin/Release/net8.0/DLSS 5 MANAGER.exe`
- `bin/Debug/net8.0/DLSS 5 MANAGER.exe`

Older RID-specific/publish/installer outputs were not rebuilt for this delivery. Publish a fresh package before distribution. The previous sources/runtimes are preserved under `tests/artifacts/live-flow-before-fg-restore-20260930`; the backed-up XAML has a `.bak` extension so Avalonia does not compile it.
