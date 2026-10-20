# Live Flow: HDR frame generation, presentation recovery and FPS

Changes are limited to Live Flow's native workers, its controller/counter and
regression tests. DLSS FG and FLOW MFG remain separate and mutually exclusive.
The pinned RTX 20/30 compatibility runtime and RTX 40/50 routing are retained.
AMD continues to use FLOW MFG; native DLSS FG is unavailable on AMD.

## HDR

Both generators were explicitly disabled whenever HDR compatibility was enabled,
including when the captured source was SDR. The guards are removed.

Captured FP16 scRGB HDR is composed with the existing NR result before generation,
then converted to HDR10/PQ in a 10-bit BT.2020 texture. Native DLSS-G uses that
texture and its HDR flag. The swap chain uses the matching format and colour
space. [NVIDIA's integration guide](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md)
specifies HDR10/PQ and R10G10B10A2 for DLSS-G; FP16 scRGB is unsupported there.

FLOW consumes the same HDR10 colour input and preserves it through its own
interpolation path. Its scratch texture is FP16 so its confidence alpha retains
precision; the presented image is converted to R10G10B10A2. FLOW's existing
colour interpolation operates on PQ code values, rather than linear radiance.

SDR sources retain the SDR path with HDR compatibility enabled. Standard HDR
without frame generation retains scRGB. Format/source changes stop and drain
the presenters before replacing HDR resources. Colour-space setup is cached
only after it succeeds. Spout and FLOW's existing SDR recorder receive a separate
tone-mapped 8-bit copy; incompatible HDR/SDR texture copies are prevented.

## Presentation recovery

FLOW formerly treated a missed two-second compositor wait as a failed presenter.
Its latency wait is now bounded by the screen rate (4–63 ms), and a temporary
timeout attempts presentation without tearing down the generator. Failed wait
handles and real GPU failures still fail normally.

Both presenters skip occluded, still-drawing and mode-change results without
counting them as displayed frames. A mode-change result requests recreation
of the output window. Native DLSS also gets a waitable swap chain when Low
Latency is off, so its presenter can retain a single compositor pacing consumer.
The normal producer does not consume that presenter's latency permit. The
window owns the handle across FG feature and colour-format recreation; a
feature reset must not close a handle that belongs to the surviving swap chain.
This also applies when Low Latency is off.

## FPS meaning

FLOW previously mixed the Python reply rate with a separate native output
window, and timed its counts against the start of a pair instead of its end.
Both generators now report native counts over the same elapsed interval.

- Base: successful presentations of real source frames during current operation.
- Output: successful presentations of real plus generated frames in that interval.
- Capture submissions remain a separate diagnostic count in the log.

Counts are timestamped when received, associated with the worker PID, cleared
on restart/failure and expired after 2.5 seconds without telemetry. Both native
presenters emit idle telemetry. The UI retains zero output during idle MFG.

A full x4 interval with 30 real and 90 generated frames reports Base 30 and
Output 120. If only 60 generated frames are successfully presented, Output 90
is correct; reporting 120 would conceal the loss. The factor is a requested
maximum, constrained by refresh, frame cap, GPU throughput and late-frame skips.
These counters measure successful application Present calls, not physical panel
scans, game-internal rendering FPS, or input-to-photon latency.

## Verification

Tests ran on RTX 3060 with isolated settings. User configuration, game files
and the Windows HDR switch were not changed.

| Test | Result |
|---|---|
| Production HDR passes | scRGB reference copied exactly; 10/80/200/1000 nit inputs decoded as 10.05/79.98/199.15/998.93 nits from PQ output, all within 1%. |
| Native DLSS x4 HDR | One real and three distinct generated HDR images; each preserved the 1000 nit highlight. SM86 0.3.5 selected. |
| FLOW x4 HDR | Generated frames presented; output preserved the 1000 nit highlight. SDR recording conversion ran on the presenter's queue. |
| Temporary compositor delay | Three injected waitable timeouts per generator did not fail either presenter. |
| Output transition | Temporary hiding and a mode-change reconstruction completed; each generator restarted its output resources successfully. |
| DLSS Low Latency on/off | HDR generation, injected timeouts, mode reconstruction and an additional FG feature reset passed in both modes. The window's waitable handle remained valid across the feature reset. |
| Actual 720p captured window, fixture requests 30 FPS | FLOW sample Base 21.1 / Output 84.4; DLSS Base 21.6 / Output 73.6; FLOW reactivation Base 21.4 / Output 78.9. Counters matched native counts and elapsed intervals. |
| Fixture requests 15 FPS | FLOW Base 12.9 / Output 49.7; DLSS Base 12.8 / Output 51.4; FLOW reactivation Base 12.7 / Output 49.9. |
| NR and x6 restoration regression | x2/x6, NR on/off, FLOW/DLSS switching, second DLSS activation, cap 90 and clean shutdown passed. |
| Controller tests | Six tests passed: protocol, settings migration, exclusive switches, AMD restriction, count windows, PID separation, expiration and restart clearing. |
| Compiled F# counter | Real/output values, separate generator modes, idle zero and stale clearing passed. |
| Actual desktop capture | DLSS x4, output cap 90, Low Latency: 72.4–87.4 successful presents/s; current Base 20.8 / Output 78.2 matched native telemetry; no waitable timeouts. NVIDIA Overlay processes were running. |
| Builds | NVIDIA and Radeon workers, Debug and Release succeeded. Manager builds: zero warnings/errors. Source/native and Debug/Release worker/controller hashes match. |

The fixture's requested FPS is not its measured frame rate; Windows timing and
capture cadence reduce its effective rate. Base uses current delivered frames,
including processing load, rather than the rate before MFG was enabled.

Test entry points: `tests/test_hdr_mfg_gpu.py`, `tests/screen_mfg_counters.py`,
`tests/screen_fg_restore_test.py`, `tests/test_dlss_fg_controls.py`, and
`tests/screen_fg_counter.fsx`. Pixel results and recovery logs are under
`tests/artifacts/hdr-mfg-20261002`. Live counter traces are under
`mfg-counters-20261002-084513` and `mfg-counters-20261002-084711`. Desktop
capture results: `tests/artifacts/fg-lag-hdr-desktop-final/result.json`.
Build/deployment hashes: `tests/artifacts/hdr-deployed-hashes.json`.
Pre-change backups: `tests/artifacts/live-flow-hdr-before-20261002`.

## Verification limits

The isolated HDR GPU test validates real HDR textures and generation, but keeps
the desktop SDR. Physical HDR monitor output and HDR capture inside a game have
not been validated here. No AMD, RTX 20/40/50 hardware was available.

NVIDIA Overlay processes were running during the live capture tests. The exact
reported in-game Overlay interaction was not reproduced, so its complete
resolution is not established. The transient wait/recreation regressions above
validate the specific recovery changes, rather than proving an overlay-specific
root cause. Real failures are still recorded in Screen.log.

This update is not a claim of Lossless Scaling parity or a complete quality
sign-off for x6. The previous large-motion/occlusion limitations in
`LIVE-FLOW-FG-STABILITY-20261001.md` still apply: estimated screen motion and flat
depth do not supply game depth or separate HUD layers.

RTX compatibility sources reviewed: [SM75](https://github.com/Coldwood1026/dlssg_for_sm75),
[SM86](https://github.com/sdli1995/dlssg_for_sm86), and the supplied
`OptiScaler-NR-v0.8.91-rtx40-mfg/docs/RTX40-MFG.md`.

Fresh runnable build: `bin/Release/net8.0/DLSS 5 MANAGER.exe`.
Older RID/publish or installer outputs have not been rebuilt.
