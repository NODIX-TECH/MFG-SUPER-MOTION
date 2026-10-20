# XeSS MFG

Intel's XeSS Frame Generation as the engine's second generator, for cards of any maker that Intel's library runs on. Picked in the app (**Generator**), `generator=1` for the engine. It is read once, at the start: the two generators never share a process, and the app starts the engine anew to change it.

Since 10 October 2026 XeSS MFG only *makes* the frames in between. Everything else is the engine's own and the same as with DLSS MFG: the real frames at the picture's full size, the presenter with its pacing, VRR, HDR, Low Latency, Queue target, the frame limit, the count a pair gets, the rebuilding of small frames at the picture's size, what stays still. So every option in the app means the same for both.

## Files

| File | What it holds |
| --- | --- |
| `xe_api.h` | Intel's two libraries (`libxess_fg.dll`, `libxell.dll`) as far as the engine calls them, loaded while running |
| `xe_tap.h` | the tap on the swap chain the library shows its frames in |
| `xe_guise.h` | a card of Intel's own that the libraries turn away, shown to them as another maker's ("Which cards") |
| `xe_mfg.inl` | the generator: `XeStart`, `XeReady`, `XeFeed`, `XeMake`, `XeAfter`, `XeForget`, `XeClose`; `XeRunsOn` for `--cards` |
| `lf_dlssg.inl` | asks for a pair's frames at three places marked "XeSS MFG": `EnsureFg`, `FgPresent`, `CloseFgResources` |
| `runtime\xess\` | the two libraries, `XeFGUnlock.asi` and its `XeFGUnlock.ini` |

## How the frames are taken

The library has no call that makes a frame and hands it over. It is built to sit between a game and the screen: the game draws into a swap chain of the library's and calls Present; the library shows the frames in between, then the frame itself, in the real swap chain.

Used that way (7 to 9 October) the library made every frame at the picture's full size and showed them itself. On an RTX 3060 a frame in between costs it 3.5 ms at 2560x1440 against 1.0 ms at 896x504; a session's log showed a game at 45 to 50 frames a second with x2 and 27 with x6, and nothing of the engine's presenter reached those frames - no HDR, no rebuilding, its own pacing.

Now:

1. The library is given a small swap chain (the size frames in between are made at: Game type / Flow, 896x504 for a 2560x1440 picture) and a window that is never shown.
2. It creates its real swap chain with the DXGI factory it is handed. That factory is the engine's (`XeTapFactory`): it has the real factory make a swap chain - a small one, which only answers what the library asks about its window and screen - and hands back one that says it has sixteen buffers of the size asked for, and has: sixteen pictures of its own, taken in turn as a swap chain's buffers are.
3. For each pair the real frame, scaled down, is copied into the library's back buffer in the pair's own command list (`XeFeed`); once the list is sent the library's Present is called (`XeMake`).
4. The library draws each picture it "shows" into the tap's next picture, and its Present ends in the tap: nothing is shown anywhere. The tap tells the engine (`XeTapSink::Shown`), which gives the picture a number - the tap's fence reaches it, on the queue the library shows on, once the picture is whole.
5. When the library's call is back - the first frame in between is drawn inside it - it has said how many pictures it decided on (`xefgSwapChainGetLastPresentStatus`), and the rest follows from the order: the pair's frames are the tap's next pictures, and the last of them brings the next numbers. So the pair goes to the presenter at once, with those pictures as its frames in between and that number; the presenter's queue waits for it (`XeAfter`) as it waits for NGX's work, and draws from the pictures where they lie (they rest as a back buffer does, and are turned for each drawing). A picture is drawn over sixteen Presents later: five pairs at x5 and x6, four at x4.

Until round 13 step 4 was a copy of each picture out of the real back buffer, sent to the card by itself, then the real Present for a window nobody sees - six pieces of work a pair at x6 beside the library's own, each costing a game that fills the card a third of a millisecond - and the loop stood until the library's last frame had come before it handed the pair on: one to several milliseconds, never the same twice. In a game (GTA V, x6) XeSS MFG held 29 to 36 real frames a second where DLSS MFG held 40, and "felt cut up" beside it.

Two things about the library this rests on, both found by trying (`tests\xeprobe.exe tap=2`, `tests\xetruth.exe tap=16`), with `libxess_fg.dll` 1.3.1:

- It makes its swap chain with a "frame latency waitable object" and waits on it before every Present, a second at most each time. Never shown anything, the real one never becomes free; the tap hands it an event that always is.
- It asks the swap chain how many buffers it has and which one is current each time, and draws there - so sixteen buffers where it asked for three work (the same frames, at the same moments: 0.35 and 0.70 of the way for two).

Measured with `tests\xeprobe.cpp tap=1` (no plugin pacing, 896x504, RTX 3060) - when the library shows what, counted from the Present call:

| Asked for | 1st frame | 2nd | 3rd | 4th | 5th | the real frame |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.6 ms | | | | | about 20 ms |
| 2 | 0.6 | 1.1 | | | | 1.3 |
| 3 | 0.6 | 1.2 | 1.3 | | | 6 to 7 |
| 5 | 0.6 | 1.3 | 1.5 | 2 to 9 | 7 to 25 | with the next Present |

The first one comes on the calling thread, inside the call; the others from a thread of the library's. So three are asked for at most (`XeMost`); x5 and x6 go by two, as with DLSS MFG (`FgMadeCount`), and where frames are shown as made (Flow 95 and up, no rebuilding) a pair has three at most. After a new start (first frame, scene cut, pause) the library shows the real frame only; it says how many pictures it decided on as soon as the call returns (`xefgSwapChainGetLastPresentStatus`), and a pair it made nothing of goes without frames in between, as when NGX says so.

The count is not changed across the step between one and two while pairs take turns (`XeCount`): there the library changes its whole way of working and stands still for it - 112 ms a change (`tests\xeprobe.exe alternate=1`; two to three and back costs nothing). Under a frame limit or VRR's range, where one pair has one frame and the next two, every such turn was a halt: of 293 frames in between 208 were shown and 57 belonged to "pairs far longer than usual". Now a pair that wants one while others want two gets two made and is shown by the nearer (305 of 310 shown).

A frame the library said it would show and never does must not keep the presenter's queue waiting for good: when the next pair is asked for - a whole frame of the game later - a number still owed is given by the engine, and the log says so.

## What the library is told

- **Motion** (since round 13): the picture's own movement. The library is made for a game that tells it how every pixel moved; told nothing, it finds slow movement itself and leaves what moves fast half where it was and half where it goes. What it wants was found by telling it the true motion of a known scene in every way (`tests\xetruth.exe tilt=<gain> small=<n>`): for every pixel of the motion picture the way from where the thing is now back to where it was, in pixels *of that motion picture*, whatever its size. So it is told what the engine has for every pair before its frames are asked for - `sm_guard.inl`'s twenty-four blocks, found on the processor, no work on the card - spread smoothly over a motion picture an eighth of the frame's size (`XeField`). "Motion estimation: GPU estimate" hands it the engine's finer field instead (2.65 ms of the card a pair). `SM_XE_NO_FIELD=1`: told nothing, to compare.
- **Depth**: a flat plane; a camera that stands still.
- **Xe Low Latency**: the library refuses to make frames without it in its low latency mode (-15 otherwise). Its sleep and its markers are not needed (measured) and are not called: the engine's own Low Latency is the same as with DLSS MFG.
- **HDR**: with HDR on and an HDR screen the pictures are HDR10 (`lf_hdr_show.inl`); the library's swap chain is made with 10 bits a colour and told the ST 2084 colour space. Tried with `tests\xeprobe.cpp hdr=1`: accepted, frames made as in SDR. Not seen on a screen here (the test PC's is SDR).

## The unlock plugin

`XeFGUnlock.asi` (the user's own, with `libxess_fg.dll` 1.3.1) lets the library make more than one frame in between on cards that are not Intel's. It patches the library in memory about a second after it is loaded, so both are loaded when the engine starts (`XeStart`), and `XeReady` waits for it only while that second (three, to be sure) is not over. Without it, or with a build of the library it does not know, one frame is made a pair and the others go by that one.

`XeFGUnlock.ini` here has `ExtraPacing=false`: the plugin's own pacing spreads a pair's frames over the frame's time (the second one a third of a frame late), which is the presenter's work now.

| File | SHA-256 |
| --- | --- |
| libxess_fg.dll (1.3.1) | EC5E0C65E075570C6EDE72618BB666D0BE0C2E10B2EA9762C0FE8CB8E375AB27 |
| libxell.dll (1.3.2) | D2030DCD694FDA8F2EC7E044B13E6DB8F0B56D4BA9113A5EFAD334E3F3DED8C7 |
| XeFGUnlock.asi | 3337EA197720D573AACF920231ED515D08324DBA67D9966BE32E1A2584CDBDB8 |

## Measured

RTX 3060, 2560x1440 at 200 Hz, SDR; `tests\dump\verify14.sh`, the window's own frames (WGC). Real frames / frames shown a second.

| Game | Multiplier | DLSS MFG | XeSS MFG | XeSS MFG before (the library showing its frames) |
| --- | ---: | ---: | ---: | ---: |
| heavy (101 alone) | x2 | 69 / 138 | 65 / 132 | 62 / 123 |
| | x4 | 54 / 217 | 51 / 203 | |
| | x6 | 53 / 315 | 50 / 298 | 44 / 176 (it settled on x4) |
| light (185 alone) | x6 | 83 / 496 | 78 / 466 | |

What is left between the two: the library sends its work to the card in several pieces on two queues (its frames, the real frame it shows to nobody), and each piece sent by itself costs the game about 0.3 ms.

Since round 14 (no copies, no Presents, no waiting), `tests\dump\trace6.sh`, x6 - pairs a second, and the share of the gaps between frames shown that are more than a quarter off:

| | load 780 | | load 1500 (a heavier game) | |
| --- | ---: | ---: | ---: | ---: |
| DLSS MFG | 61.7 | 16.5% (5.6% in another run) | 44.6 | 7.2% |
| XeSS MFG, round 13 | 58.6 | 13.4% | 43.1 | 5.9% |
| XeSS MFG, round 14 | 59.7 | 6.4% | 43.3 | 5.0% |

The library's own call takes 0.7 ms of the loop in the middle, 1.4 at the 95th of 100 (`[xess] the library's call for a pair` in the log, every ten seconds). A report from a game (GTA V, x6) has XeSS MFG at about 33 real frames a second where DLSS MFG holds 40; nothing like that is on the stand-ins, before or after.

Frames in between against the frames that should be there (`tests\jelly.ps1`, share of pixels off):

| Scene | DLSS MFG | XeSS MFG |
| --- | ---: | ---: |
| tilt 16, x2, whole picture | 1.1 to 1.2% | 1.5 to 2.4% |
| tilt 16, x2, the tower block's face | 4.4% | 6.0 to 12.8% |
| tilt 16, x2, the read-outs | 0.1 to 0.4% | 0.1% |
| tilt 16, x6, whole picture | 1.3% | 2.5% |
| drive 120, x2, whole picture | 4.4% | 5.0% |
| drive 120, x2, lamp posts | 44% | 61 to 63% |
| drive 120, x2, thin poles and palms | 69 to 70% | 36% |

What the motion told changes - the same scenes, XeSS MFG x2, share of pixels off (round 13):

| Scene | told nothing | the picture's movement (default) | GPU estimate | DLSS MFG |
| --- | ---: | ---: | ---: | ---: |
| tilt 16, whole picture | 1.6 to 2.5% | 0.9 to 1.2% | | 1.1 to 1.2% |
| tilt 16, the tower block's face | 7.4 to 13.5% | 4.3 to 5.3% | | 4.4% |
| drive 120, whole picture | 5.7% | 4.9% | 5.8% | 4.4% |
| drive 120, lamp posts | 60% | 55 to 63% | 37 to 38% | 44 to 52% |
| drive 60, whole picture | | 2.9 to 3.4% | 3.1 to 3.9% | 2.0 to 2.6% |
| drive 60, lamp posts | | 24 to 28% | 24% | 16 to 22% |
| drive 60, the car crossing | | 29 to 35% | 26 to 35% | 10 to 16% |

A camera that turns - most of what moves in a game's picture - now comes out as with DLSS MFG. What goes its own way in front of it (a lamp post rushing past, another car) does not: that takes every pixel's own motion. Told the true one, the library's own small frame has the post whole (`xetruth truth=1`: lamp posts 28 to 39% off at drive 120 against 59 to 70% told nothing; told only the picture's movement in blocks, `truth=3`, 60 to 75%). The engine's GPU estimate gets most of that (37%) and costs the game 6 to 8 frames a second (heavy game: x2 64 to 56, x6 50 to 44); so it stays a choice.

`tests\xess-smoke.ps1` (20 cases since round 15, 17 before: every option on and changed while running, both capture ways, the sync modes, DLSS MFG beside it) and `tests\suite.ps1 -Generator 1` (the 21 cases of the engine's own suite) ran clean; so did `suite.ps1 -Generator 0` - in round 13, and again on the round 14 build.

NVIDIA Optical Flow can be the motion told as well (round 16; `motion=0`, "NVIDIA Optical Flow" under Motion estimation in the app, on an NVIDIA card - anywhere else the engine's GPU estimate takes its place). It costs the game what it costs under DLSS MFG: a heavy game at x2 holds 70 frames a second with it where it holds 77 without.

## Which cards

Asked of Intel's library itself: `nvngx.dll --cards` prints `@xess <number> <1|0>` for each card (`XeRunsOn`: Xe Low Latency and a frame generation context on a device of that card), and the app's "Preferred GPU" offers XeSS MFG only the cards it runs on. DLSS MFG needs an RTX card.

Older graphics of Intel's own (round 15). The Intel UHD 730 in the test PC is refused as it is: `xellD3D12CreateContext` -2 ("unsupported driver"), `xefgSwapChainD3D12CreateContext` -1 ("unsupported card") - on a driver that has shader model 6.6 and everything the libraries' shaders need. They take any card of another maker without a list; of Intel's own, only the ones on theirs. They learn the maker from DXGI, so `xe_guise.h` makes that one card's description say another maker's to everyone in the engine's process (`GetDesc` to `GetDesc3` replaced in DXGI's table of an adapter's calls; vendor 0x1002, device 0x73BF, for the card of one LUID), and both libraries start and make frames. `XeRunsOn` asks plainly first and in the guise after, so `--cards` says `@xess 1 1` for the chip; `XeStart` does the same for the card the engine runs on and says so in the log ("[xess] Intel's libraries turn this card away as it is ..."). Nothing is done for a card they take as it is (Arc, NVIDIA, AMD).

What the chip holds, measured (`tests\xeprobe.exe card=1 guise=1 tap=3`, and the engine with `gpu=1 generator=1`, the game on the RTX 3060, a 2560x1440 200 Hz screen on the RTX):

| | UHD 730 | RTX 3060 |
| --- | --- | --- |
| a frame in between of 896x504, from asked to whole on the card | 10.7 ms | 3.8 ms |
| ... of 640x360 | 7.6 ms | |
| ... of 448x252 | 4.6 ms | 1.5 ms |
| engine at 2560x1440, light game, x2 | 18 real, 36 shown | |
| ... x3 | 20 real, 48 shown | |
| ... heavy game (load 780), x2 | 26 real, 51 shown; the game itself draws 104 | 73 real, 145 shown; the game draws 73 |
| ... 1280x720 window as it is, x2 | 32 to 39 real, 64 shown | |

So it works, and the game keeps its own rate (104 against 73) - but the chip takes a quarter to a third of the game's frames, and what is shown is 130 ms behind. The library's network is the weight (a rebuilt 2560x1440 frame is 9 ms more of that chip's time): a second card of some strength is what this is for. Which is still the case for a lighter generator of our own on the weakest ones.

## Not tested

- Any card but the RTX 3060: no AMD, no Intel Arc.
- HDR on a screen, VRR on a screen that follows the frames (the test PC's does not for windows).
- GTA V and THE FINALS.

## Tools

```powershell
Engine\tests\build-xe.cmd
Engine\tests\xeprobe.exe Engine\out\xess frames=2 fps=60 size=896x504 tap=2     # what the library shows, when - taken as the engine takes it
Engine\tests\xetruth.exe Engine\out\xess Engine\tests\dump\xe-truth tilt 16 frames=2 tap=16 truth=3   # its frames of a known scene, for jelly.exe and phase.exe
Engine\tests\phase.exe Engine\tests\dump\xe-truth tilt 16 2560 1440 2            # which moment each of them really shows
Engine\tests\xess-smoke.ps1                                                      # every option, live
Engine\tests\suite.ps1 -Generator 1
```

API: [Intel's developer guide](https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md), [swap chain](https://github.com/intel/xess/blob/main/inc/xess_fg/xefg_swapchain.h), [D3D12](https://github.com/intel/xess/blob/main/inc/xess_fg/xefg_swapchain_d3d12.h).
