# Octo Super Motion, round 15: XeSS MFG on a second card, sync modes, the name

11 October 2026. RTX 3060 (SM86) and Intel UHD 730 in one PC, 2560x1440 at 200 Hz, SDR. Measured on `tests\game.exe` and, without the screen, on `tests\xeprobe.exe`; nothing was run in a game.

## The reports

"Preferred GPU" lists one card where the PC has two; the UHD 730 should run XeSS MFG like any DirectX 12 card - "you asked the library, but ours is not the build everyone has". The upscalers again: could the motion a generator is given feed DLSS as an upscaler - with a ReShade add-on (DLSS5-Feeder) put beside the app as something to learn from - and are the scaling kinds another program lists (FSR, NIS, SGSR, BCAS, Anime4K, xBR, sharp bilinear, integer, nearest) worth having. VSync. And the app's name: Octo Super Motion.

## The second card

Round 14 asked Intel's two libraries and wrote down their answer for the UHD 730: Xe Low Latency -2 ("unsupported driver"), XeSS Frame Generation -1 ("unsupported card"). The answer was the libraries', and it was not about what the chip can do: its driver (32.0.101.7088) has shader model 6.6. The files beside the engine are the ones in the user's `xess mfg mod` folder, byte for byte.

What they go by is who made the card. A card of another maker is taken without a list; of Intel's own, only the ones on theirs. They learn the maker from DXGI. `xe_guise.h` (new) replaces the four calls that hand out an adapter's description in DXGI's own table of an adapter's calls and changes two numbers for the card of one LUID (vendor 0x1002, device 0x73BF). In that guise both libraries make their contexts on the UHD 730 and frames come out (`tests\xeprobe.exe card=1 guise=1`); it also works after a first refusal in the same process, which is how the engine does it: asked plainly, and only when that fails, in the guise (`XeTakes`, `XeTakesInGuise`; `XeRunsOn` for `--cards`, `XeStart` for the card the engine runs on). A card they take as it is never sees any of it.

What the chip holds (`xeprobe tap=3`: from the call to the frame whole on the card; the engine with `gpu=1 generator=1 capture_api=1`, the game on the RTX 3060, the screen on the RTX):

| | UHD 730 | RTX 3060 |
| --- | --- | --- |
| a frame in between of 896x504 | 10.7 ms | 3.8 ms |
| ... 640x360 | 7.6 ms | |
| ... 448x252 | 4.6 ms | 1.5 ms |
| engine, 2560x1440, light game, x2 | 18 real / 36 shown | |
| ... x3 | 20 / 48 | |
| ... heavy game (`game.exe` load 780), x2 | 26 / 51, the game draws 104 | 73 / 145, the game draws 73 |
| ... 1280x720 window as it is, x2 | 32 to 39 / 64 | |

The engine's own log on the chip: a frame in between costs it 2 ms to follow, 1.6 to choose, 4.6 to 9 to build at 2560x1440, and 6 to 9 ms a pair for the movement - beside the library's 10.7. The frames shown are 130 ms behind the game.

So: it runs, the app lists the card for XeSS MFG, and the game keeps its own rate (104 against 73). A UHD 730 is too small for it - a quarter to a third of the game's frames are taken. It is what a second card of some strength is for (none here to measure). A generator light enough for the weakest chips is still one of our own.

## Sync modes

`sync` (new; "Sync mode" in the app, under Display):

| | what it does | how |
| --- | --- | --- |
| 0 off | a frame is shown the moment it is presented, tear lines and all | the output window's swap chain is made with `ALLOW_TEARING` (only in this mode: every other mode's chain is what it was) and presented with it |
| 1 default | the newest frame at each refresh; none waits | as before |
| 2 VSync | every frame presented is shown, for one refresh | `Present(1)`; shown frames capped at 97% of the refresh rate, the presenter waits for the screen to take each |
| 3, 4 | ... for two, for three refreshes | `Present(2)`, `Present(3)`; the cap is 97% of half, of a third |

VRR is a sync mode of its own (`lf_vrr.inl`): while it is on, this one rests, and the app greys it. The cap goes where VRR's went (`PresentCap`): the count of frames in between a pair, the capture's pace, the desktop capture's gate.

Measured, 200 Hz screen, `game.exe` held at a rate, both generators (real / shown):

| game | asked | default | VSync | VSync 1/2 | VSync 1/3 |
| --- | --- | --- | --- | --- | --- |
| 60 | x4 | 60 / 240 | 60 / 194 | 60 / 98 (XeSS 56 / 89) | 60 / 60 - the game fills it |
| 40 | x6 | 40 / 240 | 40 / 194 | 40 / 97 | |

A game faster than the screen's share has its own frames shown and no frames in between, as under any cap. Off: 90 / 353 and 81 / 323 where default gave 97 / 385 and 83 / 334 (a free-running light game, x4; no tear line can be seen by a test).

## DLSS as an upscaler, again: DLSS5-Feeder

Read as something to learn from (its README and `src\`). What it does: inside the game's process, as a ReShade add-on, it hands NVIDIA's DLSS the finished frame, the game's depth buffer (ReShade has it) and motion estimated by an optical-flow shader, at 1:1 - "DLAA" - so that another add-on can hook that call. It is not an upscaler, and says so: real DLSS upscaling needs the game to draw smaller than the screen and shift its camera a little each frame; from a finished frame "the best DLSS can converge to is the image you already had". Its own experiment with a shifted grid ("work_upscale=2") "saves nothing" and shimmers, by its author's measurement.

That is round 14's result from the other side (`tests\srprobe.exe`: DLSS on a captured 720p frame 0.94 / 1.50 mean error moving / still against the engine's own scaler's 0.93 / 1.06, at 5.8 ms a frame), with one thing the add-on has that a capture has not: the game's depth. Nothing in it changes the answer. DLSS, FSR 2+ and XeSS upscaling stay out.

## The scaling kinds another program lists

The engine's scaler (`lf_window_fill.inl`) is edge-directed Lanczos-2 with deringing and contrast-limited sharpening - the build of FSR 1, which is the top of that list for a game's picture; NIS and SGSR are the same idea with fewer taps, BCAS is bicubic with sharpening (bicubic measured 1.25 / 17.4 against our 0.93 / 14.0, round 14). Listing them would be four names for nearly one picture. The kinds that are different in kind are for other pictures: integer / nearest for pixel art, Anime4K and xBR for drawn pictures. Not added; they are worth a choice only for someone who plays those.

## Tests

On the build deployed (the same sources built beside the real one first): `tests\xess-smoke.ps1` 20 of 20 (three new: `xess-vsync`, `xess-tearing`, `dlss-vsync` - a sync mode set and changed while running), `tests\suite.ps1` 21 of 21 with each generator. The app: the window's title, "Sync mode" under Display with its five entries, and "Preferred GPU" listing both cards with XeSS MFG picked (`dump\app-r15-*.png`, the lists read through UI Automation); a profile saved before (schema 10) opens with "Default".

Not seen by any test: a tear line (sync mode off), a second card other than the UHD 730, VSync on a screen of another rate than 200 Hz.

## The name

Octo Super Motion: the window's title, the tray icon, the program's file (`OctoSuperMotion.exe`), the start-with-Windows entry (an entry under the old name is moved over when the app opens), the engine's log and its output window's title. The settings folder (`%LOCALAPPDATA%\ARMO`) and the engine's inner names are what they were: nothing saved is lost.
