# MFG SUPER MOTION engine

Three generators can make the frames in between: DLSS MFG (`generator=0`, the default, NVIDIA RTX cards), XeSS MFG (`generator=1`, any card Intel's library runs on) and MSM FG (`generator=2`, the engine's own, any card at all). Everything below is the same for all three except the generator itself; what is XeSS's own is in [XeSS MFG](docs/XESS-MFG.md), what is MSM FG's in [MSM FG](docs/MSM-FG.md).

The native process behind START MOTION. C++ only (no Python): it takes the game's window on the graphics card, has NVIDIA's DLSS Multi Frame Generation make frames in between from the captured pictures, and shows real and made frames in its own click-through window over the game.

It is the "DLSS MFG for every RTX card" architecture of LIVE FLOW in DLSS 5 MANAGER, moved here whole. What the old app's Python engine did around the worker (picking the window, borderless, the frame loop, settings) is C++ here (`sm_*`). The DLSS 5 colour network, FLOW MFG, Flow Super Resolution, recording and Spout were not brought over.

## Build

```cmd
Engine\build.cmd
```

Needs Visual Studio 2022 with the C++ workload. It makes `Engine\out\nvngx.dll` and copies the runtimes beside it; the app's build copies `Engine\out` to `<app>\Engine`. `nvngx.dll` is an executable: NGX's runtimes check that file name of their caller.

## Files

| File | What it holds |
| --- | --- |
| `sm_engine.cpp` | `main`: start order (card, RTX 20/30 compatibility, NGX), includes |
| `sm_options.h` | one value per control in the app |
| `sm_control.inl` | the start file, lines from the app on stdin, status lines on stdout |
| `sm_source.inl` | the count and which window is taken; borderless over its screen and back |
| `sm_loop.inl` | the frame loop: take, compare, hand to the generator |
| `sm_guard.inl` | where the picture went, block by block - also where it repeats (a building's floors): the blocks that are sure decide for the ones that are not. What a rebuilt frame starts from; and, when asked for, no frames in between while it moves faster than they follow |
| `lf_desktop.inl` | Desktop Duplication (DXGI) and Windows Graphics Capture (WGC), the D3D11 to D3D12 bridge, the small luma made with the capture's copy |
| `lf_luma.inl`, `lf_own_motion.inl` | small luma, scene cut, repeated frames |
| `lf_ofa.inl` | NVIDIA Optical Flow (only when picked: the generator finds the motion itself) |
| `lf_estimate.inl`, `lf_estimate_shader.h` | the engine's own GPU motion estimate (only when picked) |
| `lf_ngx.inl` | NGX start, the architecture answer |
| `lf_dlssg.inl` | DLSS MFG: runtime, evaluation - and the pairs, their real frames and the presenter, which both generators use (three places marked "XeSS MFG" ask the other one) |
| `msm_fg.h` | MSM FG 1.0, the engine's own generator: the motion looked for on the card in both directions, each frame in between drawn at the picture's size from the two real frames ([MSM FG](docs/MSM-FG.md)) |
| `sm_learn.inl` | Adaptive Motion Learning: how far the mouse and a pad's right stick move is set against how far the picture moved, pair by pair; the game's gain and delay come out of it, are kept for each game, and tell MSM FG where a fast turn took the picture ([what it is and what was measured](docs/ADAPTIVE-MOTION-LEARNING.md)) |
| `sm_banner.inl` | Two lines of text at the top of the frame, drawn by the engine itself as the FPS counter is: what Adaptive Motion Learning says while it studies a game |
| `sm_spout.inl` | every frame shown, shared with other programs as a Spout sender ("Spout output": OBS with the Spout2 plugin records it) |
| `xe_guise.h` | XeSS MFG on a card of Intel's own that Intel's libraries turn away (an integrated UHD): shown to them as another maker's |
| `xe_mfg.inl`, `xe_api.h`, `xe_tap.h` | XeSS MFG: Intel's XeSS Frame Generation as a generator - its libraries as far as they are called, and the tap on the swap chain it shows its frames in, by which the engine takes them instead |
| `lf_dlssg_compat.inl`, `lf_dlssg_ada.h` | RTX 20/30 compatibility package, RTX 40 kernel adaptation |

| `lf_fg_pace.inl` | when frames go to the screen: moment + delay, Queue target; which frames of a game faster than the screen are taken |
| `sm_rebuild.inl` | frames in between made small (Flow) and rebuilt at the picture's size from the two real frames around them. Once a pair: the movement under which the two real frames show the same thing, block by block and pixel by pixel; what they agree about is built from them alone, the generator keeps its say where they do not |
| `lf_fg_scale.inl` | the real frame scaled down for the generator; a small frame in between plainly scaled back when it cannot be rebuilt |
| `lf_fg_still.inl` | what does not move stays put |
| `lf_show.inl` | the output window and its swap chain |
| `lf_window_fill.inl` | a window that keeps its size, scaled onto its whole screen; the sharpening ("Upscaling" in the app) |
| `lf_vrr.inl`, `lf_latency.inl`, `lf_hdr_*`, `lf_counter.inl` | VRR, Low Latency and the scheduling class, HDR, the FPS counter |
| `lf_gpu_core.inl`, `lf_pipeline.inl`, `lf_shader_cache.inl`, `lf_shader_warm.inl`, `lf_timing.inl`, `lf_trace.inl` | device, textures, shader cache, timings, log |
| `runtime\` | `nvngx_dlssg.dll` (310.9.1), `dlssg_compat\version.dll` (0.3.5, hash-pinned), Visual C++ runtime; `xess\`: `libxess_fg.dll` (1.3.1), `libxell.dll` (1.3.2), the unlock plugin and its settings (carried inside `nvngx.dll` too, written to `%LOCALAPPDATA%ARMOnginess-unlock`) - its settings |
| `docs\research\` | the measurements and reports each technique came from |

## RTX families

| Family | Path |
| --- | --- |
| RTX 20 (SM75), RTX 30 (SM86) | compatibility package, loaded before NGX starts; x2 to x6 |
| RTX 40 (SM89) | temporal kernels and count gates adapted in memory; an unknown runtime layout keeps x2 |
| RTX 50 | NVIDIA's runtime as it is |

The family is read from CUDA by the adapter's LUID. Only RTX 3060 has been run here; see `docs\research`.

## Options

Sent as `key=value`: in the start file (`--config <file>`), on the command line, or as a line on stdin while it runs.

`gpu=<number>` picks the card the engine runs on - capture, generator and output all on that one ("Preferred GPU" in the app; -1, the default: the first NVIDIA RTX card for DLSS MFG, the card with the most memory for XeSS MFG). The numbers come from `nvngx.dll --cards`, which prints one `@card <number> <1: DLSS MFG can run on it> <MB> <name>` line a card, after it `@xess <number> <1: XeSS MFG can run on it>` (asked of Intel's library itself), and does nothing else; a card a virtual display shows a second time is listed once. The app offers a card only for the generator that runs on it. DLSS MFG needs an RTX card. XeSS MFG runs on a second card too - older graphics of Intel's own among them: the UHD 730 of the test PC, which Intel's libraries turn away as it is ("unsupported card", "unsupported driver") and run on once it is shown to them as another maker's (`xe_guise.h`, round 15). What such a chip holds is another matter: at 2560x1440 it takes 18 to 26 frames of the game a second (a frame in between of 896x504 alone is 10.7 ms of its work), 32 to 39 with a 1280x720 window - while the game on the RTX 3060 keeps 104 where it kept 73 with the generator beside it. A second card worth the name is what it is for. DLSS MFG asked for on a card that is not an NVIDIA RTX one runs on the first that is, and says so (`@note`). A screen driven by another card than the engine's works - Windows copies every frame across - and costs that copy; with two cards the screen belongs on the engine's.

| Key | Values | Live |
| --- | --- | --- |
| `mfg` | 0, 1 | yes |
| `multiplier` | 2..6 | yes |
| `game` | the kind of game: 0 auto, 1 2D, 2 2.5D, 3 first person, 4 third person, 5 racing, 6 open world - each brings the size frames in between are made at (35, 75, 50, 35, 35, 35, 35% of the picture; `sm_options.h`, kGameKinds); 7 (the engine's own default): as set by `flow` | yes |
| `flow` | 10..100, used with `game=7`: the size frames in between are made at, percent of the picture (never under 504 lines; 95 and over: the picture's own size, not rebuilt). They are rebuilt at the picture's size, real frames keep their own | yes |
| `queue` | 0 least delay, 1 balanced, 2 smoothest | yes |
| `still` | 0, 1: keep still pixels | yes |
| `guard` | 1: no frames in between while the picture moves faster than they follow; 0 (default): they are made all the same | yes |
| `frame_limit` | 0 off, 20..360 | yes |
| `screen_limit` | 0 (default): every pair gets the multiplier's own count; 1: only as many frames in between as fill the screen, a whole number a pair | yes |
| `vrr`, `low_latency`, `hdr`, `counter` | 0, 1 | yes |
| `sync` | 0 off: a frame is shown the moment it is presented, tear lines and all; 1 (default) the newest frame at each refresh; 2 VSync; 3, 4 VSync on every second, every third refresh. With `vrr=1` it rests | yes (to and from 0 the output window is made again) |
| `upscaler` | 0 off (default), 1 sharpening only, 2 integer, 3 nearest neighbour, 4 Anime4K | yes |
| `spout` | 1: every frame shown is shared as the Spout sender "MFG SUPER MOTION" (125 a second at most) | yes |
| `link` | 1 (default): when DLSS 5 MANAGER's LIVE FLOW offers its frames ("Link to MFG SUPER MOTION" on there), START MOTION takes them - see "Linked to DLSS 5 MANAGER"; 0: never | at start |
| `sharpness`, `over_sharpness` | 0..100, used unless `upscaler` is 0: how hard every frame shown is sharpened within what its own contrast leaves room for, and how far past that | yes |
| `gpu_class` | 1 realtime, 0 normal | yes |
| `generator` | 0 DLSS MFG, 1 XeSS MFG, 2 MSM FG (the engine's own, `msm_fg.h`) | at start |
| `msm_search` | MSM FG's search for the motion: 0 in full, 1 the lighter one ("MSM FG · Fast") | yes |
| `aml` | 1: Adaptive Motion Learning - the hand is listened to, a game not studied yet is taken through the study on the frame, MSM FG is told where a turn took the picture (`sm_learn.inl`, `docs\ADAPTIVE-MOTION-LEARNING.md`) | yes |
| `aml_say0` .. `aml_say8` | the study's lines in the app's language (UTF-8): 0 the first line (`{0}` percent, `{1}` seconds left), 1 camera left and right, 2 up and down, 3 walk, 4 done, 5 drive or stand, 6 drive straight, 7 take turns, 8 move left and right | at start |
| `aml_forget` | a game's name: what was learnt for it is forgotten ("Delete" in the app's Settings) | yes |
| `capture_api` | 0 DXGI, 1 WGC | yes (reopens the source) |
| `motion` | 2 the generator's own (default), 0 NVIDIA Optical Flow, 1 GPU estimate - for DLSS MFG and XeSS MFG; MSM FG finds the motion itself | yes (rebuilds) |
| `window_mode` | 0 borderless, 1 scaled onto the screen, 2 as it is | at start |
| `delay` | seconds to click the game | at start |
| `owner` | the app's process id: its windows are never taken | at start |
| `hwnd` | a window taken outright, no count (tests) | at start |
| `source`, `monitor` | 1 = the window's whole screen instead of the window (tests only: anything else moving on that screen counts as frames of the game) | at start |

`stop` on stdin, a closed stdin, or the event `Local\SuperMotionEngineStop` ends it; the game's window is put back as it was.

Status lines on stdout begin with `@` (`@state`, `@pick`, `@source`, `@fps`, `@mfg`, `@vrr`, `@error`); everything else goes to `%LOCALAPPDATA%\MFG SUPER MOTION\Engine\engine.log` (`--verbose` echoes it on stderr).

## How many frames

Every frame of the game is taken; only a game faster than the screen itself loses frames. Between two of them go as many frames as `multiplier` asks, the same for every pair: the screen shows the newest at each refresh, and a game at 47 with x6 is 281 frames a second on a 200 Hz screen. With `screen_limit=1` ("Limit to refresh rate" in the app) a pair gets only as many as fill the screen - a whole number, held until the game's own rate has moved for good (a game at 58 gets three, 233 shown) - and none once the game reaches the screen's rate by itself. The count is never a fraction with the pairs taking turns (2, 2, 1 ...): a pair with one frame fewer among its neighbours is a step twice as long, and near a whole number (a game held at 100 on a 200 Hz screen) that was a stumble several times a second. Only under a limit of its own - `frame_limit`, VRR's range - is the room still filled exactly, as the frames shown must stay under it.

What is asked is not always what reaches the screen: a game at 95 with x6 is 570 frames a second, and the card has no time to draw them. The presenter counts each second the frames in between it was given and the ones it showed; with a fourteenth and more left out two seconds running the pairs get as many as were shown, in whole frames (`[dlssg] ... the pairs get 2 each from here`), and one more is tried again after eight seconds that showed them all. Every frame of the game stays under it.

## What it costs the game

Under a game that fills the card, everything the engine does is time the game does not get. Two things count (`docs\research\SUPER-MOTION-ROUND-3-20261005.md` has the measurements):

- **The work itself.** The generator's cost grows with the size its frames are made at, so they are made small (`flow`; 896x504 is the cheapest size on an RTX 3060: 1.0 ms a frame against 3.7 ms at 2560x1440) and rebuilt at the picture's size from the two real frames around them (`sm_rebuild.inl`, 0.5 ms): the small frame says where everything is, the real frames give the detail. Rebuilt frames are sharper than the generator's own full-size ones and stay right for faster movement.
- **What the loop waits for.** It does not wait for a pair's work on the card before it goes for the game's next frame: that work is taken up when the next pair is about to be recorded (`lf_dlssg.inl`, FgCollect), so the next frame is captured and judged while the card makes this pair's frames in between. Waited for at once - as it was until round 5 - a game that kept its own rate had every second or third frame taken, whichever the pair's work left time for. `SM_SERIAL=1` puts it back, to compare.
- **How many pieces it is sent in.** Each piece of work sent to the card by itself takes the card from the game and hands it back: about 0.3 ms of the game's time, whatever little the piece holds. So a captured frame is copied and its small luma made in one piece on the capture's device (`lf_desktop.inl`, Luma11); its conversion waits in the open command list for the generator's work and the two are sent as one (`lf_gpu_core.inl`, CmdCarry); and the pair is handed to the presenter when that work is sent, not when it is done, so the first frame in between is drawn right behind it (`lf_dlssg.inl`, the slot's mark).

The generator finds the motion between two real frames itself. Measured (`docs\research\SUPER-MOTION-ROUND-2-20261004.md`), its frames come out the same with a motion field of the engine's, with none and with a wrong one, while NVIDIA Optical Flow keeps each pair waiting about one frame of the game when the game fills the card. The two estimates stay as choices; `flow` then also sizes their field.

What the two real frames agree about is built from them alone (`docs\research\SUPER-MOTION-ROUND-6-20261005.md`, `...ROUND-7-20261005.md`). The generator finds the motion itself, and is wrong in two ways that show. What repeats - a building's floors, a fence - moving about half its own spacing a frame is as well explained by a movement the other way: its frame has it one floor off or torn, another way with each pair. And what is small - a dark opening of five pixels on a bare wall, a pixel and a half of the frame the generator works on - comes out beside its place, differently in each of a pair's frames. `sm_guard.inl` finds how the whole picture moves from the blocks that can only have moved one way. Once for every pair the rebuild then finds, block by block around that movement and on every pixel of the block, the movement under which the two real frames show the same thing; then for every small pixel how far they agree around it, moved so, and for every tile whether nearly all of it does. A pixel they agree about is built from them - all of a pair's frames in between by the same movement, so what stands in the scene glides from one real frame to the next. What only one of the two has (the strip the picture moves into) is built from that one. The generator keeps its say where they do not agree, and around a thing that moves its own way: there a pixel is theirs only where the generator shows the same. A block that is theirs altogether is not looked for in the generator's frame at all, and the frame is drawn straight onto the back buffer when that is the picture's size - together a third off a frame in between's cost at x6. `SM_NO_REPAIR=1` leaves everything to the generator, `SM_NO_DIRECT=1` builds beside the back buffer and copies, to compare; `SM_SHOW_THEIRS=1` paints red where the generator has the say.

A game's own read-outs do not move with the scene (`docs\research\SUPER-MOTION-ROUND-8-20261006.md`). A pixel that has the very same colour at the very same place in both real frames, and is not a flat place in either, is a mark of the screen: it is shown as the real frame has it, and where one real frame has such a mark at a place the scene is asked about, the scene is what the other shows. Without that a thin mark was drawn twice in every frame in between, moved with the picture, and painted over where it stands.

A thing that goes its own way - a lamp post beside the road, a car - is found once a pair too (`docs\research\SUPER-MOTION-ROUND-9-20261006.md`): where it stands in each real frame the two do not agree under the picture's movement, and the movement that brings the one place of unrest onto the other is the thing's own. It is built from the two real frames by that, and the scene behind it from the frame that shows it. What stands still on the screen is never taken for one. And a block the picture's own movement does not explain (a road rushing under a car at 30 frames a second) is looked for far around and where it was in the pair before - only then, or what repeats is found a row off. Since round 10 (`docs\research\SUPER-MOTION-ROUND-10-20261006.md`) this second movement is off unless asked for (`SM_MOVERS=1`): on an open road it takes away a pole against the sky that the generator has whole. What keeps such a pole now: where all is calm far around and the generator's frame shows something the two real frames have nowhere near, it is a thing on its way across, and stays the generator's. `jelly.ps1 -Mode drive -Step 120` is that road.

From four frames in between on (x5, x6) the generator itself makes two of a pair's frames - its own x3; the others go by the nearest of those for what moves its own way, and by the two real frames for everything else, as all frames do. Each of the generator's evaluations costs the game far more than its time on the card. `SM_FG_EVERY=1` asks it for every frame, to compare. A pair whose frames are 3.5 ms apart or less is sent to the card without a wait after each drawing. The output's swap chain has five surfaces for frames in between (`docs\research\SUPER-MOTION-ROUND-11-20261007.md`): with three the screen took two frames a refresh and no more - 400 a second at 200 Hz - and past that drawings waited for a surface, frames came late, and the presenter stood behind them. `SM_BUFFERS=<n>` measures another number. Whatever holds the presenter for a fifth of a second is a `[stall]` line in the log, with where it stood.

Where the compositor draws the output (another program's overlay lies over it) the game is no longer taken from the desktop - which is everything on the screen, and one more frame to throw away for each frame shown - but from its own window (Graphics Capture), by itself, once in a session; `SM_KEEP_DESKTOP=1` leaves it.


The output window has a command queue of its own, of high priority: a frame's copy into the back buffer and its Present do not wait behind the generator's work on the next pair.

## Upscaling

A window smaller than its screen ("Scaled to screen", or a game that will not go borderless) is scaled onto the screen by the engine's own two passes (`lf_window_fill.inl`): along the edges with a Lanczos kernel, then sharpened. `upscaler=1` ("Sharpness only" in the app) puts the second pass under the two sliders and runs it on every frame shown - real or in between, scaled or not.

Three more kinds, each for a picture those two passes are wrong for (round 16): `upscaler=3` "Nearest Neighbor" - every screen pixel is the texel it lies in, for pictures made of pixels on purpose; `upscaler=2` "Integer" - the same at the largest whole number of times the picture fits its screen, black around it; `upscaler=4` "Anime4K" - for drawn pictures: after the scaling, the two steps of Anime4K as it first was (0.9), which thin dark lines and turn soft edges into steps, "Sharpness" saying how far. The other kinds a scaling program lists (FSR, NIS, SGSR, BCAS) are builds of what the engine's own two passes are, and were not added.

DLSS, XeSS and FSR as upscalers (the libraries the user put beside the app) were measured and left out. They are made to sit inside a game, which draws each frame a little beside the one before and hands over every pixel's motion and depth; a captured window has none of that. `tests\srprobe.exe` - the street of `world.h` drawn at 1280x720, made 2560x1440, against the street drawn at that size; mean error in steps of 255, over the whole picture and at its edges:

| | camera driving | nearly still | costs the card |
| --- | ---: | ---: | ---: |
| bilinear | 1.33 / 20.2 | 1.43 / 20.8 | |
| bicubic | 1.25 / 17.4 | 1.35 / 18.4 | |
| the engine's own (as "Off" has it) | 0.93 / 14.0 | 1.06 / 15.7 | 2 small passes |
| DLSS on what a capture has (no motion, flat depth) | 0.94 / 14.7 | 1.50 / 20.2 | 5.8 ms a frame |
| DLSS, told the picture's true movement in 6 x 4 blocks | 0.88 / 13.6 | 1.08 / 16.6 | 5.8 ms |
| DLSS as in a game (shifted frames, every pixel's motion, depth) | 0.82 / 11.5 | 0.82 / 11.6 | |

What makes DLSS better than arithmetic is what only the game has; without it, it is the engine's own scaler at twenty times the cost.

What the sharpening costs a game that fills the card (x6, Sharpness 100%, Over sharpness 50%): 63 -> 61 to 62 frames a second with DLSS MFG, 60 -> 59 with XeSS MFG. The real frame is sharpened once, as it is taken, and the frames in between are rebuilt from sharp ones; done on every frame shown it was 62 -> 55. `tests\sharp.ps1` saves a part of the screen with any setting.

## Linked to DLSS 5 MANAGER

DLSS 5 MANAGER's LIVE FLOW makes no frames in between any more; it hands them to this engine (`sm_dlss5_link.inl`, the layout in `dlss5_link.h` - the same file as `ScreenNative\lf_msm_link.h` there; change both together). With "Link to MFG SUPER MOTION" on in its LIVE FLOW page and the game picked there:

- START MOTION finds the link (`Local\DLSS5_LiveFlow_MFG_Link`: LIVE FLOW's worker alive and heard from within two seconds) and takes LIVE FLOW's window - no count, and the window is left as LIVE FLOW placed it (borderless, or scaled onto its screen when LIVE FLOW had to).
- Each frame of the game comes with DLSS 5 already on it: LIVE FLOW copies its finished frame into one of three shared textures and signals a shared fence; the capture's device waits on that fence on the card, copies the frame out through BridgeFrame like a captured one (8-bit RGBA is taken now besides BGRA), and signals that it is done so LIVE FLOW can use the slot again. The time stamp is the game's moment LIVE FLOW took the frame at, so the pacing is as for a capture.
- DLSS 5 works once a frame of the game; the frames in between are made from frames it already finished.
- The Spout sender rests while linked: OBS records LIVE FLOW's sender ("DLSS 5 MANAGER Screen").
- `@source <window> <w> <h> dlss5 <placement>`; the app shows "DLSS5" where it shows DXGI or WGC.
- LIVE FLOW stopped or the link switched off there: `@error link ...` and the engine stops.

Measured on the RTX 3060 (stand-in game at 60 FPS, `tests\screen_msm_link.py` of DLSS 5 MANAGER): DLSS 5 off 60 -> 240 at x4; DLSS 5 on about 40 -> 150 (the DLSS 5 network is what the card has left room for). The suite here runs clean with the link built in (21 cases).

## DXGI and WGC

DXGI duplicates the screen the game fills and is used while the game's window is its whole screen (borderless). WGC takes the window's own frames and is used when the window is smaller than its screen, when its screen hangs on another card, or when it is picked in the app. With DXGI the game has to be the window in front; the output hides while it is not.

## Tests

Round 19's own: `tests\still.ps1` - a picture that moves, then stands, over and over, with the settings of the session that ended by itself (`-Load 780`: a game that fills the card instead); `tests\study.ps1` - the study Adaptive Motion Learning takes a game through, on `scene.exe study` (a camera turned by the mouse, then a street walked along, then driven along or stood in), with proofs of the frame; `tests\learntruth.exe study=<kind of game> [car=0] [obey=0]` - the same against a player of its own, without the engine.

`tests\build.cmd` and `tests\build-game.cmd` make the stand-ins. `tests\fixture.cpp` is a stand-in game (a box moving across a still background at a fixed rate, drawn with GDI); `tests\run.ps1` runs the engine on it and prints the status lines and the generator's counts:

```powershell
cd Engine\tests
.\run.ps1 -Seconds 12 -Fps 30 -Options 'multiplier=4','window_mode=0'
.\run.ps1 -Seconds 14 -Options 'multiplier=2','capture_api=1' -Later 'mfg=0','flow=100' -LaterAfter 6
```

What the engine costs a game, how evenly its frames reach the screen, and how good they are:

```powershell
.\idle.ps1 -Until 120                                          # the tests take the whole screen: not while someone is at the PC
.\suite.ps1                                                    # every option, one short run each: exit code, rates, anything wrong in the log
.\measure.ps1 -Load 1040 -Options 'multiplier=4','flow=75'    # game.exe alone, then under the engine: game, base, shown
.\measure.ps1 -Load 150 -Cap 95 -Options 'multiplier=6' -Later 'game=1' -LaterAfter 8   # a light game held at 95 FPS; an option changed while it runs
.\measure.ps1 -Load 400 -Cap 185 -Options 'multiplier=6'      # a light game at 185 FPS with the card three quarters used (a shooter)
.\pace.ps1 -Load 1040 -Uneven 40 -Options 'multiplier=2'      # the step error: screen time against the game's own time, frame to frame
.\jelly.ps1 -Mode pan -Step 64 -Options 'still=0'              # scene.exe: a frame in between against the one that should be there
.\jelly.ps1 -Mode tilt -Step 32                                 # a street with depth (world.h): the camera tilting in front of a tower block
.\jelly.ps1 -Mode tilt -Step 16 -Multiplier 6 -Grain 6          # all five frames of a pair, on a picture with a game's noise in it
.\jelly.ps1 -Mode tilt -Step 32 -Engine D:\old\nvngx.dll         # another build of the engine on the same scene
.\jelly.ps1 -Mode dolly -Step 24                                # ... driving towards it, tilting down as it goes
.\jelly.ps1 -Mode pan -Step 16 -Fps 60 -NoCheck -Later 'flow=50'   # an option changed while it runs; read the log
```

`game.exe` is a D3D11 game that fills the card (its frame sent in twelve pieces, as a real game's is; `-Pieces 1` is the worst case). `scene.exe` shows a detailed scene whose every frame is known - a camera pan under a still HUD, or a box crossing a still picture - through a swap chain, so each frame reaches the desktop whole; `jelly.exe` compares the engine's dumped frames (`LF_FG_DUMP=<folder>`) with it. The street (`world.h`) has a tower block with rows of balconies, bare walls between them with one small dark opening a floor, a car crossing the road, lamp posts along it, and a game's read-outs lying still over all of it (a ring, a compass, figures, a pane of glass); `jelly.exe` counts each by itself, and says how far every opening is from its place in pixels. `jelly.exe picture tilt 16 40.5 out.raw` draws the street itself at a moment. `rawpng.ps1` turns part of a dump into a PNG to look at (`-Zoom 4` for small things).

MSM FG without the screen and without the engine: `msmtruth.exe` (`build-msm.cmd`; its frames of the street for `jelly.exe`, the motion it finds against the true one, and what it costs a card - `dump\msm-run.sh`, `dump\msm-motion.sh`). Spout: `spoutcheck.exe` (`build-spout.cmd`), a receiver as small as one can be.

XeSS MFG without the screen: `xeprobe.exe` (does the library run on a card - `card=1 guise=1`: in another maker's guise - and what it shows when; `tap=2`: as the engine takes its frames, `tap=3`: how long a frame takes the card), `xetruth.exe` (its frames of the street for `jelly.exe` and `phase.exe`; `tap=16` takes them as the engine does, `truth=<1..5>` tells it the motion several ways), `phase.exe` (which moment a frame in between really shows). `dump\trace6.sh` keeps every Present of a run under `game.exe` and says where the uneven gaps are; `dump\phase6.sh` does `phase.exe` on the engine's own frames. `srprobe.exe` is the upscaler measurement above.

`shot.ps1`, `keys.ps1` and `ui.ps1` drive the app itself (a picture of its window, a shortcut press, a button or switch by name). What kept frames in between from the screen is told in the log every five seconds while anything did (`[steady]`: pairs without any and why, frames left out for being late, the count changing, desktop frames that were not the game's). A run that lasted more than a minute is kept in `%LOCALAPPDATA%\MFG SUPER MOTION\Engine\history\` (the twelve newest), so a game session can still be read after the engine has been started again. `tests\overlay.ps1` puts another program's window on top for a while. To compare: `SM_NO_REBUILD=1` (small frames in between plainly scaled), `SM_NO_REPAIR=1` (everything from the generator's frame, right or wrong), `SM_NO_DIRECT=1` (frames in between built beside the back buffer and copied onto it), `SM_DUMP_DIRECT=1` (with `LF_FG_DUMP`: drawn straight onto the back buffer and read back from it), `SM_FG_TIER=1..3` and `SM_FG_SKIPCOPY=1` (the RTX 20/30 package's kernel tiers), `SM_SPLIT_CAPTURE=1` (the luma made on the generator's device, a piece of work more). Diagnostics: `SM_DIAG=1` (what the source hands over each second, the generator's cost on the card step by step, where a pair waits), `LF_PHASE=1` (step timings), `LF_FG_TRACE=<file>` (one line per Present), `LF_SHOW_PROOF=<file>` (the frame that reached the screen), `LF_COUNTER_PROOF=<file>` (the counter as it reached the frame).

