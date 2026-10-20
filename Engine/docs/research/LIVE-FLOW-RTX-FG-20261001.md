# Live Flow RTX DLSS FG / MFG integration

Scope: desktop DLSS FG in Live Flow only. This extends the earlier restoration
documented in `LIVE-FLOW-FG-RESTORE-20261001.md`.

## Behavior

- RTX 20 (SM75) and RTX 30 (SM86): use the pinned upstream 0.3.5 compatibility
  package with architecture-specific exact kernels. Supports requests for
  x2 through x6; lower factors remain fallback options if evaluation fails.
- RTX 40 (SM89): adapt temporal PTX and matching count gates in loaded runtime
  memory. Unknown layouts retain x2. The on-disk NVIDIA DLL is never patched.
- RTX 50: keep the native NVIDIA path. AMD keeps FLOW MFG; DLSS FG is disabled.
- Architecture selection uses the real CUDA compute capability matched by
  adapter LUID, before compatibility or NR architecture spoofing.
- Compatibility loads before NGX initialization and shader worker threads.
  Enabling/disabling FG restarts the native worker; changing the multiplier
  recreates only its FG feature. FLOW MFG and DLSS FG remain mutually exclusive.
- User cache holds extracted compatibility files and generated settings.
  Missing/modified packages fail with a diagnostic and ordinary output.
- Frame-limit pacing now accounts for the DLSS FG multiplier as well as FLOW
  MFG. Presentation diagnostics distinguish real and generated submissions.

## Validation on RTX 3060

- Native supplied 0.2.3 SM86 runtime generated three distinct intermediate
  frames (x4). The integrated 0.3.5 backend generated five (x6).
- `tests/verify_rtx_fg_pixels.py` verified x6 intermediate positions for a
  known four-pixel translation: measured offsets -3.20, -2.65, -2.00, -1.25,
  -0.65 pixels versus expected -3.33, -2.67, -2.00, -1.33, -0.67.
  Mean absolute pixel errors were 0.10–0.39 on an 8-bit scale, excluding borders.
  Artifacts: `tests/artifacts/rtx-fg-early035-x6/phase-verification.json`.
- Actual moving-window capture passed x2, x6 feature recreation, NR evaluation
  with x6, both switch directions, repeat activation, persistence and shutdown.
  Test settings are isolated from user settings. Final log:
  `tests/artifacts/rtx-fg-live-integration.log`.
- With a 90 FPS output cap / x6, steady test intervals contained 30 real + 139
  generated presents in 2.01 seconds and 31 real + 138 generated in 2.07 seconds.
  That is approximately 15 captured FPS and 82–84 successful presents/second,
  not a guaranteed 90 FPS or sixfold game performance increase. These counters
  measure successful presentation calls, not physical panel scan-outs.
- Ada unit checks passed: architecture guard, missing/ambiguous signatures,
  malformed inputs, no writes on planning failure and repeated invocation.
  Mapping the actual 310.9 runtime without initialization found and adapted
  31 temporal containers. This is a structural test, not RTX 40 execution.
- NVIDIA and Radeon native builds passed. Controller tests cover mutually
  exclusive switches, worker restart, saved values, protocol flags and AMD guard.
- Debug and Release manager builds passed with zero warnings/errors. The
  missing-NR regression still returns 12 successful FLOW MFG frames without
  evaluating NR. Ada verification preserved the on-disk NVIDIA runtime hash.
- Packaged worker, controller and compatibility files match Debug and Release
  copies: `tests/artifacts/rtx-fg-manifest.json`.

## Limits and release handling

Only RTX 3060 was available. RTX 20/40/50 execution and real-game performance
remain unverified. Desktop guides still use estimated motion and flat depth;
occlusions, HUDs and scene motion can differ from a game's native DLSS inputs.
No Lossless Scaling quality/performance parity is claimed. HDR compatibility
continues to disable native DLSS FG. Native generated-frame recording remains
unverified. No game installation or user configuration was modified.

Package provenance, pinned hashes and upstream links are in
`mod files/native/dlssg_compat/README.md`. Earlier sources/runtimes are preserved
under `tests/artifacts/live-flow-before-rtx-backends-20261001`.
Runnable builds are `bin/Debug/net8.0` and `bin/Release/net8.0`;
older RID/publish/installer output must be rebuilt before distribution.
