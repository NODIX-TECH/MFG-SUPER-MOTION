# Live Flow: selected-window DXGI capture and DLSS MFG pacing

Changes are limited to Live Flow, its Source controls, native workers and tests.
DLSS FG uses NGX and remains separate from FLOW MFG. Enabling one disables the
other. AMD keeps FLOW MFG; this does not enable native DLSS FG on AMD.

## Selected source

Enabling either generator without a selected application starts a five-second
countdown. The Source card also offers **Select active window** with 3, 5 or 10
seconds and a Cancel button. Select a window from the list to complete a pending
countdown immediately. The current foreground application is read at expiry;
an earlier foreground window is not reused.

The default borderless mode saves the source's styles and placement, removes the
frame, and restores them when generation stops. It retains the selected HWND
instead of reverting to desktop capture. Restoration checks process/thread
identity to avoid affecting a recycled window handle. Framed mode still requires
a selected application. Clearing the source stops MFG. Capture failure or a closed
source pauses output and requires another source or Reconnect; it must not become
whole-desktop generation.

DXGI Desktop Duplication supplies the selected window's monitor. A GPU copy crops
it to the client area before conversion, motion analysis or NGX. Dirty/move
metadata outside that area and pointer-only updates are ignored. The output,
overlay and capture shortcuts use capture exclusion to prevent feedback. Inactive
or minimized DXGI sources pause. Rotated, partly off-screen or cross-adapter
windows retain WGC compatibility capture; Flow Super Resolution retains its
existing capture path. DXGI is monitor duplication with a client crop, so the
selected application must be visible in front; it is not background-window capture.
See [Microsoft's Desktop Duplication documentation](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api).

## Multiplication and performance

The presenter used to cancel unfinished intermediate phases when a newer pair
arrived, skip late phases, and budget against capture rate even without a cap.
It now retires obsolete queued groups and finishes each chosen group's phases
in order. Uncapped ×4 requests three intermediate phases. An explicit frame cap
can still reduce phases; initialization and NGX scene-cut rejection also produce
real-only frames. The FPS counter reports accepted real/generated Presents over
the same measured interval; it does not multiply an input number for display.

The D3D11→D3D12 bridge uses a GPU queue fence rather than a CPU wait. Conversion
and luma share their completion wait in FG-only mode. Raw motion goes directly
to NGX without NR's motion-copy/filter work when NR is off.

DLSS keeps three swap-chain surfaces even with Low Latency enabled, while maximum
queued frame latency stays **one**. Two surfaces serialized model/copy/compositor
work under load. DLSS also retains normal GPU scheduling: the earlier realtime
choice reduced the source game's throughput while optimizing output latency.

**DLSS MFG resolution** offers Native, 1080p (default), and 720p. Only generated
frames are resized; real frames keep source resolution. The dedicated resampler
uses clamped reconstruction to avoid sharpening overshoot. With NR off, 720p
also limits optical-flow luma to 640×360; 1080p/Native and NR/FLOW guides retain
their previous limits. 720p trades generated-frame detail/motion precision for
lower cost; use 1080p or Native when thin details need more precision.

## Verification on this RTX 3060

- NVIDIA and Radeon native builds passed. Debug/Release UI builds passed.
  Source/controller files and native workers were copied to both outputs;
  worker hashes are in `tests/artifacts/window-dxgi-deployment.json`.
- Seven source lifecycle/countdown tests and six FG control/counter tests passed.
  Native integration verified timer arm/cancel, selection during countdown,
  DXGI crop, capture exclusion, both backend switches, and original window
  bounds/styles restoration. Automatic expiry uses mocked foreground selection
  in unit tests: Windows denied autonomous activation of the headless fixture,
  so real integration uses explicit window selection during the timer.
- Final uncapped ×4: stable groups contain **16 real + 48 generated**, or
  **17 real + 51 generated**. The controller reported Base 16.1 / Output 64.2.
  The Win32 fixture requested 20 FPS but actually delivered about 16–17 FPS.
  Zero waitable timeouts; correct SM86 compatibility routing.
- Coupled 2560×1440 GPU stand-in, requested 130 FPS / 5 ms work, NR off:

  | Configuration | Source game FPS | Real / total shown FPS | Mean / p95 latency |
  |---|---:|---:|---:|
  | 1080p ×2, Low Latency off | 130.2 | 63.4 / 126.8 total | 37.1 / 43.2 ms |
  | 720p ×2, Low Latency off | 130.8 | 79.6 / 159.2 total | 29.9 / 37.1 ms |
  | Final 720p ×2, Low Latency on | 131.0 | 82.1 / 164.3 total | 30.0 / 36.3 ms |

  Reports: `latency-load-20261002-105545`, `latency-load-20261002-110215`,
  `latency-load-20261002-111517`. Earlier runs using the non-production
  `fg_height` spelling did not select 720p and are excluded from that comparison.
- Production HDR GPU tests passed for both generators, with Low Latency on/off,
  simulated waitable timeouts, feature/output reconstruction, hidden output,
  and reduced-resolution HDR generation/resampling. PQ patches decode to
  10.05 / 79.98 / 199.15 / 998.93 nits for 10 / 80 / 200 / 1000 references.
  Three generated phases were distinct. Physical HDR-monitor validation remains.

This demonstrates a throughput improvement without starving the stand-in's source,
but does **not** establish 190+ FPS in GTA or Lossless Scaling quality parity.
The native probe reads a 200 Hz output, so 164 FPS is below its pacing ceiling.
An additional deferred-luma experiment measured 152 FPS and was discarded;
the delivered code retains the validated synchronous scene-cut check.
Native DLSS estimated motion still lacks engine depth and engine motion vectors;
complex occlusion and very large multipliers remain limited. RTX 20/40/50 and AMD
hardware were not available for this run. Lossless Scaling's DXGI, queue-one and
five-second focused-window controls were inspected; no gameplay video A/B was run.

Builds: `bin/Release/net8.0/DLSS 5 MANAGER.exe` and the Debug equivalent. Restart
the manager to load the updated controller/worker. Installer and RID publish
outputs were not rebuilt. Initial source backups are under
`tests/artifacts/window-dxgi-before-20261002` with `.bak` suffixes.
