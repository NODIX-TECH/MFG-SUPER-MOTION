# DLSS SUPER MOTION, round 2: base rate, jelly, resolution, counter

4 October 2026. RTX 3060 (SM86, compatibility backend 0.3.5), 2560x1440 at 200 Hz, SDR. Nothing here was run on RTX 20, 40 or 50, on an HDR screen, or with VRR reaching the screen.

## The report

After the first build was tried in a real game:

1. "DXGI does not work."
2. From 130 FPS, x4 leaves 48 FPS base - far worse than Lossless Scaling, which shows 95/380.
3. Things look like jelly moving in front of you while playing.
4. The resolution drops a lot.
5. The FPS counter looks bad; it should be as plain as Lossless Scaling's: `95/380`.
6. The edges in the app's own window.

## What was found

### The generator ignores the motion field it is given

`tests\scene.exe` draws a scene whose every frame is known; `tests\jelly.exe` compares a frame made in between (dumped with `LF_FG_DUMP`) with the frame that should be there. A pan of the whole picture, x2, frames in between at the picture's size, share of pixels more than 12 of 255 away:

| Pixels a real frame | Estimated field (NVIDIA Optical Flow) | No field (zeros) | A wrong field (40 px everywhere) |
| ---: | ---: | ---: | ---: |
| 32 | 3.9% | 4.2% | 4.2% |
| 48 | 17.8% | 18.4% | - |
| 64 | 41.2% | 41.4% | - |
| 80 | 63.8% | 64.1% | - |
| 96 | 94.0% | 94.4% | 94.3% |

(first, single-scale version of the scene). The field's sign makes no difference either, nor its size (256x144 against 2560x1440). DLSS MFG 310.9.1 finds the motion between the two pictures itself; with a flat depth plane the field handed to it changes nothing that can be measured.

### The optical flow was the stall

With the game filling the card, each pair waited for NVIDIA Optical Flow about one frame of the game: 3.8 ms with the game at 174 FPS, 11.7 ms at 101, 15.7 ms at 52 - against 2 ms with the game held at 60 and the card free. The wait is inside the driver's own step (it stays when the input is finished before the call), and the optical flow unit itself is busy 1.8 ms a pair. The loop took 20.7 ms a pair where the card's work for it was 3 ms, and took every second frame of the game.

So the motion is left to the generator by default (`motion=2`): no optical flow, no flow luma, no field expansion, a 256x144 field of zeros. NVIDIA Optical Flow and the GPU estimate stay as choices under Advanced; they cost what they did and change nothing seen here. RTX 40 and 50 run NVIDIA's runtime without the compatibility backend and were not measured.

### One queue for everything

The presenter's copy of each frame into the back buffer, and the Present, were sent on the queue the generator's work is sent on, and waited behind it: "drawing takes 7.2 ms (real) and 4.1 ms (in between)", the presenter let pairs go. The output window has its own queue now, of high priority: 0.5 and 0.9 ms.

### Real frames were thrown away for made ones

Two rules took real frames no faster than the screen's rate divided by the multiplier, so that every pair was shown whole: the clock in `PaceCapture` (WGC) and the frame gate (DXGI, "every second frame is taken"). x4 on 200 Hz: 194 / 4 = 48.5 real frames a second of a game drawing 130 - the 48 of the report - and with DXGI the gate's choice changed with the game's rate, which is likely what "DXGI does not work" was.

Now every frame of the game is taken (only a game faster than the screen itself loses frames), and each pair gets as many frames in between as the screen has room for beside the game's own - a share that is seldom whole, so the pairs take turns (2, 2, 3 ...) - never more than the multiplier asks. A game that nearly fills the screen by itself (room under 1.25 frames a real one) is shown as it is and NGX rests.

### What a frame in between costs

On the card, a pair at the picture's size (2560x1440): 3.4 to 4.4 ms for the first frame in between, 2.1 for the second, 1.5 for the third, 0.5 for the copy and the still-pixel tiles. At 1920x1080: 2.2 and 1.4. At 1280x720: 1.7. The game's own frame costs 7.7 ms. This is the generator's own work and what decides the base rate under a game that fills the card.

### Numbers

`tests\measure.ps1 -Load 780`: the stand-in game (`tests\game.exe`, 131 FPS alone, its frame sent to the card in twelve pieces as a real game's is), ten seconds, medians.

| Setting | Game | Base | Shown | Delay | Latency avg / p95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| x2, Flow 100 | 70 | 70 | 140 | 15.4 ms | 20.9 / 26.3 ms |
| x2, Flow 75 | 76 | 76 | 152 | 14.7 | 19.5 / 23.3 |
| x2, Flow 50 | 78 | 79 | 157 | 14.5 | 18.5 / 21.6 |
| x3, Flow 100 | 60 | 60 | 181 | 24.5 | 28.1 / 31.3 |
| x4, Flow 100 | 56 | 56 | 195 | 27.9 | 32.2 / 35.1 |
| x4, Flow 75 | 67 | 67 | 195 | 19.3 | 25.1 / 29.3 |
| x4, Flow 50 | 71 | 70 | 194 | 19.0 | 23.5 / 26.6 |
| x4, Flow 100, WGC | 56 | 55 | 195 | 28.5 | 30.5 / 35.0 |
| x4, Flow 100, NVIDIA Optical Flow | 47 | 47 | 189 | 37.1 | 38.9 / 46.9 |

Before, same card and screen: x4 at 1080p took 42 to 46 real frames and showed 105 to 130 (stand-in game with its frame in one piece); the report's own session read 48 base, 35 to 40 ms delay, 46 ms from desktop to screen.

No pair came late and none was dropped in these runs. The first builds of this round were measured with the stand-in's frame in one piece: there nothing of the engine's can go between two parts of the game's frame, and each of its lists waits a whole frame of the game (7 ms); a real game sends many.

### The jelly

A frame in between is right while the picture moves little between two real frames, and falls apart as it moves more. Multi-scale scene, a pan, pixels of 2560 a real frame: 32 (1.25% of the width) 3.2% of pixels off; 48, 7.6%; 64, 15.9%; 96, 58.6%; 128, 89.7% - bent and smeared patches, the jelly (pictures were looked at). The size the frames are made at does not move that limit (the same at 2560x1440 and 1280x720), nor does a motion field. A thing moving alone across a still picture breaks sooner: the box at 64 pixels a frame is 79% off, the picture around it untouched.

Two things follow. A higher base rate is less movement a frame: 48 real frames a second became 56 to 79. And `sm_guard.inl`: the small luma each capture leaves is searched (twelve blocks, a tenth of a millisecond on the processor) for how far the picture moved; when the picture as a whole moved more than 2.8% of its width, or nearly all of it changed with no movement in reach that explains it, the pair gets no frames in between and the game's own frames are shown at their moments. Pans of 32, 48 and 64 pixels are generated as before; 80, 96 and 160 are passed over (150 of 150 pairs). One thing moving fast, or parts moving apart (driving ahead), is left to the generator. "Fast motion guard" under Advanced turns it off.

### The resolution

Frames in between were made at 1080 lines by default ("MFG resolution") and scaled to the screen: three of four frames shown at x4 were soft. Flow now is that size - the share of the picture frames in between are made at, 100 by default - and the separate row is gone. Flow 75 and 50 are the cheaper choices (table above).

### The counter

`95/380`: white figures with a thin dark edge, blended onto the frame by a small pass in the back buffer's own format (8-bit, HDR10, scRGB) - no box. Read back from the frame with `LF_COUNTER_PROOF`.

### The app

The scroll bar ran over the cards' right border (the margin was around the scrolling area, not inside it). A row's switch sat eight pixels above the middle of its row (two rows sized to content inside a taller row). The advanced rows stood two pixels in from the cards' own padding, and the Advanced button's hover had round corners inside the card. A saved profile from before starts at Flow 100, the generator's own motion and the guard on (schema 3).

## Not done, not known

- The engine's work is ordered behind nothing of the game's it could know: under a game that fills the card the base rate is what is left after the generator's own cost. Flow, the multiplier and a limit on the game's own rate are what change it.
- A thing moving fast by itself still comes out broken; a mask taking such parts from the real frame (as still pixels are) was not built.
- RTX 20, 40, 50: not run. HDR and VRR: the code paths start and switch; nothing was seen on such a screen.
- The source handing over its own frames again ("the compositor draws the output") did not happen here in any run of this round; it is still passed over by the luma comparison, at a third of a millisecond each.
