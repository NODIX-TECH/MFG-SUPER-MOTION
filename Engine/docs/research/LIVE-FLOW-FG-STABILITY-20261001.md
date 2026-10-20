# DLSS FG capture cadence, motion guides and FPS counter

## Changes delivered

This update is confined to Live Flow's DLSS FG integration, its controller,
FPS view model and regression tests. DLSS FG still calls the NVIDIA runtime;
enabling it does not call FLOW MFG's interpolator. The two switches remain
mutually exclusive. Architecture routing and compatibility DLLs are unchanged.

The capture limiter previously divided the selected output cap by the requested
DLSS multiplier. At 45 FPS and x6 this meant a 7.5 FPS capture budget, even when
the source was faster. Capture now keeps its own cadence, capped by the selected
limit without that division. DLSS generates only the number of phases that fit
the source interval and the output/refresh budget. A fractional budget distributes
non-integer ratios across pairs. x6 is a maximum, not a promise to exceed the
selected cap or the GPU's available throughput.

Repeated captured timestamps no longer advance DLSS history. Pair timing uses
capture timestamps rather than CPU processing intervals. Dense motion vectors
are preserved before NR's still-pixel suppression, which can zero vectors in
ambiguous regions. NR retains its existing filter when enabled. FG-only mode
skips that unused NR filter and resets its old history when NR is re-enabled.
No separate FLOW-generated images are involved.

The counter previously hid the DLSS output row and used Python loop responses
as base FPS, including responses without a new captured frame. Native telemetry
now counts fresh source submissions and successful real/generated Present calls
over the same elapsed interval. The controller timestamps these measurements,
isolates them by worker PID, clears them after reconfiguration/failure and expires
stale rates. The UI displays both measured rates and labels the configured DLSS
factor as a maximum. Hidden/occluded and failed presents are not counted.

These are application presentation counts, not measurements of physical display
scans, input-to-photon latency or the game's internal render FPS.

## Verification on RTX 3060

All GPU tests used isolated settings and synthetic moving windows; saved user
settings and game files were not changed. DLSS 5 was off except for the explicit
coexistence test.

| Case | Observation |
|---|---|
| Final build, 1440p, source fixture 30 FPS, x6, cap 45, Low Latency | Captured about 21–23 FPS; 43.8–45.5 successful presents/s; no waitable timeouts. Source no longer throttled to 7.5 FPS. |
| Instrumented uncapped 1440p x6 test | About 21–23 captured FPS and 86.7–94.6 presents/s. Actual multiplication is limited by processing/presentation throughput. |
| Final build, 1440p, source fixture 15 FPS, x6 | About 13 captured FPS and 64.2–73.3 presents/s; no waitable timeouts. Five distinct generated images verified. Pixel readback instrumentation was enabled in this run. |
| 16-pixel pan between captured 1440p images | All five phases in order; largest measured horizontal error about 0.32 pixels; image MAE 0.17–0.55 on the 8-bit RGB interior. |
| Repeated x2/x6 recreation and DLSS/NR/FLOW switching | Passed, including NR on/off, second DLSS activation, output cap and clean shutdown. |
| Missing NR DLL | FLOW starts and returns 12 frames with zero NR evaluations; passed. |
| Controller and compiled F# counter tests | Five Python tests passed; DLSS row, measured values, stale/idle handling and separate FLOW mode passed. |
| Builds | NVIDIA worker, Radeon worker, Debug and Release passed. Manager builds have zero warnings/errors. |

Artifacts: `tests/artifacts/fg-lag-stability-final45`,
`fg-lag-stability-final15`, `fg-lag-stability-open-x6`,
`fg-restore-20261001-185521`, `fg-restore-20261001-190036`, and the
`fg-stability-*.log` files.
Runtime hashes and Debug/Release copy parity are recorded in
`tests/artifacts/fg-stability-manifest.json`.

## Remaining quality limits — not a complete x6 quality sign-off

A faster 64-pixel pan at 1440p failed the strict one-pixel phase tolerance:
the first intermediate frame deviated by about 2.57 pixels, although the other
four phases were close. Increasing the NVIDIA Optical Flow preset to Medium
did not fix it, so that experiment was not made the default.

An isolated 640x360 probe with exact input motion also showed phase bias at a
16-pixel shift, and substantial ghosting at a 64-pixel shift (10% of the image
width per source frame). These failures therefore cannot all be attributed to
NVIDIA Optical Flow. Stress artifacts are retained in
`fg-lag-stability-quality-x6`, `fg-lag-stability-quality-medium`,
`rtx-fg-stability-exact16` and `rtx-fg-stability-exact64`.
The phase verifier was not weakened to turn those failures into passes.

The capture integration still supplies estimated screen motion and flat depth,
not engine depth and separate HUD-less/UI layers. A translating photograph
cannot validate third-person occlusion, camera cuts or every game's HUD. This
update fixes identifiable cadence/counter/guide issues; it does not establish
Lossless Scaling parity, eliminate every x6 artifact, or prove game performance
under simultaneous GPU load. AMD/RTX20/40/50 hardware were not available for
physical validation in this session.

Primary references reviewed:

- [NVIDIA DLSS-G integration guide](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md): required motion, depth and optional UI inputs.
- [SM86 compatibility documentation](https://github.com/sdli1995/dlssg_for_sm86/blob/main/docs/INSTALL.en.md): native 310.9 x6, per-game requested counts, exact optimization tier and x6 limitations. The shipped exact tier remains selected.

## Runnable outputs

- `bin/Debug/net8.0/DLSS 5 MANAGER.exe`
- `bin/Release/net8.0/DLSS 5 MANAGER.exe`

The updated native workers and controller files are present in both outputs.
Older RID/publish folders and installer packages were not rebuilt. Close the
old application before starting an updated output so its worker is recreated.
The user's existing frame cap is preserved; a 45 FPS cap still means about
45 output FPS, regardless of the maximum multiplier chosen.
