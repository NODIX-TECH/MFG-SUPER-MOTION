# DLSS FG + Low Latency presentation stall

## Cause and fix

The user's Live Flow log selected RTX 3060 / CUDA SM86 and loaded the correct
0.3.5 backend. It then repeatedly reported two-second frame-latency waits and
only 1.4–2.7 successful presents per second. The same session used a 2560x1440
source, Low Latency, a 45 FPS cap, and DLSS 5 NR with two passes.

The capture thread's `LowLatencyWait` and the DLSS FG presenter consumed the
same swapchain frame-latency signal. Either could consume the permit needed
by the other to submit the next Present. A separate startup wait also consumed
a permit without presenting a frame. The earlier FG integration test had Low
Latency disabled and a smaller source, so it missed this combination.

- While DLSS FG owns presentation, capture keeps its priority handling but
  leaves the swapchain wait exclusively to the FG thread.
- The presenter waits exactly once per Present, including the first one.
- A stalled/hidden compositor gets a bounded 4–63 ms wait instead of a
  two-second pause. Stop is checked after waiting.
- Returning to ordinary presentation restores the selected Low Latency
  queue depth. Real-frame timestamps now feed the existing latency telemetry;
  generated frames do not incorrectly inherit a real-frame timestamp.

No compatibility binaries, architecture routing, saved user settings, shader
quality, NR processing or FLOW MFG algorithm were changed.

## Reproduction and verification

`tests/rtx_fg_latency_regression.py` runs actual GPU capture of a moving 1440p
window with isolated settings on RTX 3060. It reproduces the user's 45 FPS cap
and Low Latency setting. Measured values below are successful Present calls,
not physical panel scans or a real-game benchmark.

| Case | Before | After |
|---|---:|---:|
| Window capture, x2, NR off, cap 45 | 1.5–2.9 FPS; 5 two-second timeouts | 37.9–42.1 FPS; zero timeouts |
| Desktop capture, x2, Low Latency toggled off/on, cap 45 | — | 44.9–46.0 FPS; zero timeouts |
| Window capture, x6, NR off, cap 90 | — | 76.3–81.8 FPS; one bounded 7 ms wait |
| Window capture, x2, two NR passes, cap 45 | — | 22.5–28.1 FPS; one bounded wait, no multi-second stall |

The NR-on test still pays for two neural passes and full-resolution FG, so it
is slower than NR-off. One short wait during NR setup is not the original
repeated two-second starvation. Regression assertions allow rare bounded
compositor delays but reject repeated stalls, missing generated frames,
backend failure and stopped presentation.

Artifacts are under `tests/artifacts/fg-lag-*`, including the pre-fix worker
and source backup in `fg-lag-before-20261001`. Additional validation covers
NR coexistence, x2/x6 recreation, both generator switch directions, missing
NR with FLOW MFG, controller exclusivity, and NVIDIA/Radeon compilation.
All completed successfully. Debug and Release builds passed with zero warnings
or errors. Runtime file parity is recorded in `tests/artifacts/fg-lag-manifest.json`.

The updated runnable outputs are `bin/Debug/net8.0` and `bin/Release/net8.0`.
Older publish/RID/installer outputs were not rebuilt. The user's saved cap
remains 45; it must be raised or disabled if a higher output rate is wanted.
DLSS 5 remains an independent processing switch and adds its own GPU cost.
