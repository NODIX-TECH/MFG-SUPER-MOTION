# DLSS SUPER MOTION, round 12: XeSS MFG under the engine's own presenter

10 October 2026. RTX 3060 (SM86), 2560x1440 at 200 Hz, SDR. Measured on `tests\game.exe`, `tests\scene.exe`, `tests\fixture.exe`; nothing was run in a game.

## The report

XeSS MFG worked (another assistant's integration of 7 to 9 October) but: no HDR, VRR and Low Latency not as with DLSS MFG, ten frames a second fewer than DLSS MFG and far from Lossless Scaling. Asked for: every feature the same as DLSS MFG, the performance mended, nothing broken.

## What the session's log said

GTA V, XeSS MFG, 2560x1440: 45 to 50 real frames a second at x2, 31 to 35 at x4, 27 to 28 at x6 (162 shown). DLSS MFG on the same game keeps about 49 at x6.

## Why

The library made every frame at the picture's full size and showed them itself. How fast it makes frames, free running, with nothing else on the card (`tests\xeprobe.exe`, without the plugin's own pacing):

| Size | 1 frame a pair | 2 | 5 |
| --- | ---: | ---: | ---: |
| 2560x1440 | 4.8 ms a pair | 8.5 | 18.7 |
| 1280x720 | 2.6 | 3.7 | 7.8 |
| 896x504 | 2.2 | 2.8 | 5.8 |

3.5 ms a frame at the full size, 1 ms at 896x504 - the size DLSS MFG's frames are made at here, to be rebuilt at the picture's size from the real frames. At x6 the library was asked for five full-size frames a pair: 18.7 ms of the card, of a game that has 20 ms a frame. With the plugin's own pacing on top (it holds the Present while it spreads the frames out) a pair took 37 ms: 27 a second, the figure of the log.

And the library showing the frames itself is why nothing of the presenter applied to them.

## What was done

XeSS MFG is a generator now and nothing else: it makes a pair's frames in between, small, and the engine takes them and does with them what it does with NGX's. The library has no call for that, so the swap chain it shows its frames in is tapped (`xe_tap.h`); `docs\XESS-MFG.md` says how, with the timings it rests on.

- `xe_mfg.inl` rewritten; `xe_tap.h` new; `xe_policy.h` (the multiplier budget of the first version) gone - the presenter's own count does that.
- `lf_dlssg.inl`: three places marked "XeSS MFG" (`EnsureFg`, `FgPresent`, `CloseFgResources`), one line in the presenter (`XeAfter`) and one in `StopFgPresentation` (`XeForget`). With DLSS MFG picked none of them runs.
- The first version's branches in `lf_show.inl`, `sm_loop.inl`, `lf_latency.inl`, `lf_settings.inl`, `lf_counter.inl`, `lf_hdr_display.h` taken out: those files are as they were before XeSS.
- `runtime\xess\XeFGUnlock.ini`: `ExtraPacing=false`.
- The app: Low latency, HDR and Keep still pixels shown for XeSS MFG too; Game type and Flow open for it (they are the size its frames are made at now); the line under the generator says what it is for.

## Result

Real frames / frames shown a second, the window's own frames:

| Game | Multiplier | DLSS MFG | XeSS MFG | before |
| --- | ---: | ---: | ---: | ---: |
| heavy (101 alone) | x2 | 69 / 138 | 65 / 132 | 62 / 123 |
| | x4 | 54 / 217 | 51 / 203 | |
| | x6 | 53 / 315 | 50 / 298 | 44 / 176 (settled on x4) |
| light (185 alone) | x6 | 83 / 496 | 78 / 466 | |

Three to five real frames a second under DLSS MFG where it was ten and more. What is left is the library's own way of working: it sends its frames, their copies and the real frame it shows to nobody as pieces of their own, on two queues.

With a limit: VRR's 188 on this screen, x6 asked, heavy game: 59 real + 130 in between; frame limit 120 at a 60 FPS game: 60 / 120; Limit to refresh rate at 60: three in between, 240 - each as DLSS MFG does it.

Quality against the frames that should be there is in `docs\XESS-MFG.md`: the whole picture 0.3 to 1.3 points of its pixels behind DLSS MFG's, the read-outs the same (still pixels apply), thin poles and palms better, lamp posts and the tower block's face worse.

`tests\xess-smoke.ps1` 17 of 17, `tests\suite.ps1` 21 of 21 with each generator.

## Not done, not known

- The remaining three to five frames a second. Tried: the library's switch that shows only the frames it made (one piece of work fewer) - with two frames asked for it then shows one.
- HDR was not seen on a screen (the test PC's is SDR): the library takes an HDR10 swap chain and makes frames in it (`xeprobe hdr=1`), and the engine's HDR path is the one DLSS MFG uses.
- No card but the RTX 3060. No game.
- The tower block's face with XeSS MFG (6 to 15% of its pixels off, 4.4% with DLSS MFG): not looked into.
