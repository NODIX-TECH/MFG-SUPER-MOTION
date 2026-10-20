# DLSS SUPER MOTION, round 14: XeSS MFG without copies or waits, sharpening, what an upscaler can do with a captured window

10 October 2026. RTX 3060 (SM86) and Intel UHD 730 in one PC, 2560x1440 at 200 Hz, SDR. Measured on `tests\scene.exe` and `tests\game.exe`, and without the screen on `tests\xetruth.exe`, `tests\xeprobe.exe`, `tests\srprobe.exe`; nothing was run in a game.

## The reports

After round 13 (GTA V): XeSS MFG faster than before, but "it feels cut up, where DLSS MFG and Lossless Scaling are constant"; its quality better, more wanted. Then: XeSS MFG holds about 33 real frames a second where the other two hold 40. The upscalers put beside the app (DLSS, XeSS, FSR) "give no strong feeling" when picked - nothing in the engine listened to that list. A second card for the frames in between "does not work here and does in Lossless Scaling". The advanced options in groups; the line under "Generator" gone; "Native" to be "Off", a "Sharpness only" choice like FSR 1, and an "Over sharpness" slider.

## Looking for the cut-up feeling

Not found on the stand-ins, which is itself a result:

- The frames. `tests\phase.exe` (new) draws the street at every twentieth of the way between two real frames and says which one a frame in between is nearest to. XeSS MFG x6 on `drive 60`: 0.15, 0.30, 0.50, 0.65, 0.85 of the way for the five frames - where they belong.
- The presenter. The user's two logs say the same for both generators (pairs late, frames passed over, none refused).
- Every Present of a run under `game.exe` (`dump\trace6.sh`, x6): the gaps between frames shown were a little less even with XeSS MFG, most at a pair's first frame - the one thing in the path that differed: the loop stood in `XeMake` until the library had shown its last frame, and only then handed the pair on.

The user's XeSS log of the afternoon: 29 to 36 real frames a second at x6; the game alone 78 to 88.

## What the library is told is nearly all it can be told

`xetruth` (the library by itself, frames of 896x504, x2; mean error of its frame, steps of 255, three pairs):

| Told | drive 120 | drive 60 |
| --- | ---: | ---: |
| the true movement in 6 x 4 blocks (what round 13 tells it, at best) | 4.59 2.95 4.40 | 1.61 2.05 1.83 |
| in 12 x 8 blocks | 3.45 2.73 3.86 | 1.61 1.76 1.70 |
| in 24 x 14 blocks | 3.02 2.62 3.59 | 1.52 1.63 1.52 |
| every pixel's true motion | 2.80 2.17 2.78 | 1.37 1.55 1.53 |
| and its true depth | 3.15 2.29 3.32 | 1.37 1.65 1.59 |

A frame of the library's told only the 6 x 4 blocks, looked at beside the frame that should be there (`dump\own60-blocks-made.png`, `own60-truth.png`, with the player's own car in the picture - `WORLD_OWN=1`), is hard to tell from it. Finding the finer motion on the card was tried twice (`tests\xe_flow.inl`, `xetruth truth=4`):

- a motion for every eight pixels, patches of sixteen samples: a turning camera came out worse than with the blocks alone (the tower block's face 3.6 4.4 4.3 -> 3.8 7.5 5.1): floors are as good a match one floor up, grain matches a little better somewhere every time;
- blocks of 36 pixels, 81 samples, moved only when known to have moved, the only good place, not entering the picture: no harm, and 1 to 8% less error where the camera drives (drive 120: 4.43 2.90 4.27; drive 60: 1.55 1.88 1.81). A road rushing at the camera grows on its way and is nowhere in the frame before as it is now.

Left out of the engine: six more passes a pair for that.

## XeSS MFG: no copies, no Presents, no waiting

How the frames are taken now is in `docs\XESS-MFG.md`. In short: the library draws into sixteen pictures of the tap's own; nothing is shown anywhere and nothing is copied; a pair is handed the pictures its frames are being drawn into and the number the tap's fence reaches when the last is whole, as soon as the library's call is back.

Found on the way:

- The library makes its swap chain with a frame latency waitable object and waits on it before every Present. With no real Present it stood a second each time (`xeprobe tap=2`: a Present call 1256 ms on average) until the tap handed it an event that is always set (0.82 ms).
- It goes by the buffer count the swap chain reports and by its current index, each time: sixteen where it asked for three work, the same frames (`xetruth tap=1`, `tap=16`: 0.35 and 0.70 of the way, mean error 1.89 2.19 1.93 either way).
- `xetruth`'s first way of taking the frames - Windows Graphics Capture of the library's window - is not to be trusted for two frames in between (it took the real frame for the second one); `tap=` is.

Measured, `game.exe`, x6, the window's own frames (WGC):

| | load 780 (the game alone: 101): pairs a second | gaps over a quarter off | load 1500 (a heavier game): pairs a second | gaps over a quarter off |
| --- | ---: | ---: | ---: | ---: |
| DLSS MFG | 61.7 | 16.5% (5.6% in a run before) | 44.6 | 7.2% |
| XeSS MFG, round 13 | 58.6 | 13.4% | 43.1 | 5.9% |
| XeSS MFG, now | 59.7 | 6.4% | 43.3 | 5.0% |

On these stand-ins the two generators were three per cent apart before and are now; the seven frames of forty the report speaks of are not there to be measured. What may make a game different: every piece of work the engine sends by itself takes the card from the game for a moment, and a game's own frame is thousands of pieces where the stand-in's is twelve. The library's log line (`[xess] the library's call for a pair: ...`, every ten seconds) is there to say next time; under `game.exe` it reads 0.7 ms in the middle, 1.4 at the 95th of 100, 4.9 at most.

`tests\xess-smoke.ps1` (17 cases) and `tests\suite.ps1` with both generators (21 cases each) ran clean on the build that was put in `out\`.

In-engine after the change (`dump\phase6.sh`): `drive 60` x6 0.15 0.30 0.50 0.65 0.85, whole picture 2.6% off; `tilt 16` x4 0.25 0.50 0.75, 1.0 to 1.1% off.

## Upscalers and a captured window

`tests\srprobe.exe` draws the street at 1280x720, makes it 2560x1440, and compares with the street drawn at that size. DLSS Super Resolution is given what a game gives it, then less and less; the engine's own scaler is done on the processor, step for step as its two passes do it on the card. Mean error over the whole picture / at edges, steps of 255:

| | camera driving (60 cm a frame) | nearly still |
| --- | ---: | ---: |
| bilinear | 1.33 / 20.2 | 1.43 / 20.8 |
| bicubic | 1.25 / 17.4 | 1.35 / 18.4 |
| the engine's own, as it was (sharpened by 40%) | 0.93 / 14.0 | 1.06 / 15.7 |
| the engine's own, Sharpness 100% | 1.07 / 13.3 | 1.22 / 15.5 |
| ... and Over sharpness 50% | 1.65 / 18.7 | 1.79 / 20.9 |
| DLSS as in a game: shifted frames, every pixel's motion, depth | 0.82 / 11.5 | 0.82 / 11.6 |
| DLSS, no shift; every pixel's motion, depth | 0.81 / 11.0 | 0.99 / 15.1 |
| DLSS, no shift; the true motion in 6 x 4 blocks, flat depth | 0.88 / 13.6 | 1.08 / 16.6 |
| DLSS, no shift; no motion, flat depth (what a capture has) | 0.94 / 14.7 | 1.50 / 20.2 |

DLSS costs the card 5.8 ms a frame for 1280x720 -> 2560x1440 and 2.5 ms from 1707x960 or at 2560x1440 itself, where it changes a picture that needed nothing (mean 1.14 from the picture it was given).

So: what DLSS adds over arithmetic is in the shifted frames and the per-pixel motion, which only the game has. On a captured window it is the engine's own scaler, at twenty times the cost - with the best block motion there could be; with none, worse than it while the picture stands. XeSS and FSR ask for the same things. None of the three was put in; the list is "Off" and "Sharpness only".

(The sign of the shift DLSS is told had to be found: the other way round the picture is worse than bicubic, 1.48 / 19.5.)

## Sharpening

"Sharpness" (0 to 100%: how far the contrast-limited pass goes, at any scale) and "Over sharpness" (past the limit: every pixel further from the eight around it, by brightness) - `lf_window_fill.inl`. Done on every frame on its way to the screen it cost a game that fills the card 62 -> 55 frames a second at x6: 0.30 ms a frame for the pass and its copy, and 0.17 ms because a frame in between could no longer be drawn straight onto the back buffer. Where frames in between are rebuilt from the real ones the real frame is now sharpened once, as it is taken, and the frames in between are built from sharp ones - they go straight onto the back buffer again. Measured the same way (Sharpness 100%, Over sharpness 50%): DLSS MFG x6 63 -> 61 to 62 frames a second, XeSS MFG x6 60 -> 59. Pictures of the screen with and without: `tests\dump\sharp2-*.png` (`tests\sharp.ps1`).

With no frame generation, with a window scaled onto its screen, and where frames in between are shown as the generator made them (Flow 95% and up), it is still the last step before each Present.

## A second card

`nvngx.dll --cards` now asks Intel's library about each card. The UHD 730: Xe Low Latency -2, frame generation context -1 ("unsupported card"). DLSS MFG needs an RTX card. Neither generator can run on that chip, in any arrangement; the app no longer offers it. Lossless Scaling's own generator runs on any card, which is what it takes.

## Not done, not tested

- The cut-up feeling and the missing frames in the game itself: not reproduced, so not shown to be gone.
- Any card but the RTX 3060; HDR on a screen; VRR on a screen that follows.
- XeSS and FSR as upscalers were not run: they need what DLSS needs.
