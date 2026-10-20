# XeSS presentation follow-up, 9 October 2026

The DXGI test that processed no frames on 8 October passed on the next desktop
session without changing capture behavior. Diagnostic mode showed real desktop
frames followed by the existing switch to WGC when feedback from the output was
detected. The engine deliberately pauses DXGI window capture if the foreground
process is different from the selected source. This is one possible explanation
for the earlier run, not a retrospectively proven cause. Capture now logs changes
into and out of that state once per transition. It still never captures another
foreground application as if it were the game.

## Presentation experiment

`tests/xeprobe.cpp stats=1` samples the native DXGI statistics forwarded by the
XeSS proxy on a test-only thread. The application gains no polling thread. A
sample is accepted only when consecutive present counters are observed; missing
counters and failed queries are counted separately. These are DXGI-reported
display intervals, not a mouse-to-photon measurement.

RTX 3060, 2560×1440 test window, 60 FPS input target, 200 Hz display, eight seconds
per run. The comparison isolates the old requested ×6 / unsynchronized settings
and the current effective ×3 / synchronized settings. Both use the same probe
and runtime. The second configuration sets maximum frame latency to 2, matching
the default queue target. This is not a GTA V or Lossless Scaling comparison.

| Measurement | ×6, sync 0 | ×3, sync 1, queue 2 |
| --- | ---: | ---: |
| Actual originals/second | 39.0 | 60.1 |
| Submitted frames/second | 233.1 | 180.0 |
| Source interval p50 / p95 / p99, ms | 25.05 / 26.38 / 27.96 | 16.66 / 16.68 / 16.76 |
| Average blocking Present call, ms | 10.19 | 2.17 |
| Average XeLL Sleep, ms | 0.04 | 1.04 |
| Consecutive DXGI samples | 26 | 1294 |
| Present counters not observed | 824 | 0 |

The synchronized run reports display interval p50 5.00 ms, p95 10.00 ms and p99
10.05 ms. The fixed 200 Hz refresh quantizes intervals into roughly 5 ms steps;
180 FPS does not divide this refresh evenly. The old run has too few consecutive
samples to compare its interval percentiles fairly. Missing counters cannot
distinguish unobserved presents from frames discarded before display.

Files under `tests/dump/capture-followup-20261009`:

- `present-before-repeat.txt`: isolated baseline measurement used above.
- `present-after.txt`: synchronized measurement.
- `present-before.txt`: exploratory baseline; compiler activity overlapped it,
  so it is excluded from the comparison.

## Final checks

Native and F# Release builds succeeded; F# reported zero warnings/errors. The
packaged native engine matches `Engine/out/nvngx.dll`. The previously failing
`xess-desktop` smoke case now passes at about 30/120 FPS. The DLSS regression
case is checked separately. Earlier pole repair and optional motion precision
settings remain intact; this follow-up adds capture diagnostics and test tools,
without changing generation or capture selection policy.

Run the probe with the project's DLLs, for example:

```powershell
Engine/tests/build-xe.cmd
Engine/tests/xeprobe.exe Engine/out/xess frames=2 fps=60 seconds=8 size=2560x1440 show=1 small=10 log=2 stats=1 sync=1 latency=2
```

Intel documents that [`GetFrameStatistics` forwards to the native swap chain](https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md#implementation-details).
