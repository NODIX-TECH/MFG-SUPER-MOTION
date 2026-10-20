# DLSS SUPER MOTION, round 4: the multiplier's own count, an even count, what interrupts

5 October 2026, later the same day as round 3. RTX 3060 (SM86, compatibility backend 0.3.5), 2560x1440 at 200 Hz, SDR. The game is `tests\game.exe`. Nothing was run on RTX 20, 40 or 50, on an HDR screen, with VRR reaching the screen, or in a real game.

## The report

After round 3 was tried in GTA V:

1. A great improvement, but x6 gives 190 frames where Lossless Scaling gives 250.
2. Up to 120 FPS at x4 the picture is better here; at high frame rates it is better there.
3. The fast motion guard is a bad option: off by default.
4. Sometimes the frames in between "work, then break up, then come back" (a dashed line was drawn).
5. Every improvement has to hold for RTX 20, 30, 40 and 50 alike.

The engine's log of that session was gone: it keeps the run before as `engine.prev.log` and nothing older, and the measurements this round began with were four runs. What is known of the session is the start file the app wrote for it: x4, **Flow 100**, the guard already off, the screen as the limit, DXGI.

## What was found

### "190 at x6" was the screen's rate, and Flow 100

`measure.ps1 -Load 1040` (the game alone at 100), round 3's build:

| Setting | Game under the engine | Shown |
| --- | ---: | ---: |
| x6, Flow 100, the screen as the limit (what was run) | 46 | 194 |
| x4, Flow 100, the screen as the limit | 48 | 189 |

With the screen as the limit the frames shown never passed 97% of the refresh rate, whatever the multiplier said; and at Flow 100 the generator's five frames a pair are made at the picture's own size.

### A count that was a fraction

Until now a pair got exactly the room the screen had left beside the game's own frames. That is seldom a whole number, so the pairs took turns - at a game's 75 on 200 Hz: 2, 2, 1, 2, 2, 1. A pair with one frame fewer among its neighbours is a step twice as long: an uneven pace of the engine's own making, at its worst near a whole number (a game held at 100 had room for 0.94, so every seventeenth pair went without - six stumbles a second) and whenever the game's rate drifts across one. From four fifths of the screen's rate on, the frames in between were stopped altogether ("the game fills the screen"): a game playing around 150 had them come and go with every lighter scene.

This is the likeliest reading of both "better there at high frame rates" and the dashed line: at x4 up to 120 FPS the count was whole (three, always); above the screen's rate divided by the multiplier it was not. It could not be confirmed on the session's log.

### What a frame in between is made at

`tests\jelly.ps1`, share of pixels more than 12 of 255 off the frame that should be there:

| Scene | Flow 35 | Flow 50 | Flow 75 | Flow 100 |
| --- | ---: | ---: | ---: | ---: |
| pan, 32 pixels a frame | 3.5% | 2.1-2.6% | 1.7-2.1% | 3.1-3.5% |
| pan, 64 | 3.2-4.0% | 13-15% | 4.5-5.6% | 16% |
| a box crossing, 24 pixels a frame | 14% | 11% | 9-12% | 9-14% |
| the same, 48 | 33-39% | 65% | - | 34-42% |

The generator follows a movement only so many of its own pixels; made smaller, it follows further. Flow 35 is the one that holds everywhere; more is finer when the picture moves slowly and breaks sooner when it moves fast (not evenly so: 75 holds a fast pan that 50 loses). Flow 100 costs 3.7 ms for the first frame against 1.0 and is the first to bend.

## What was done

- **The multiplier's own count is the default** (`screen_limit=0`; "Limit to refresh rate" off in the app, and switched off in saved profiles): x6 is five frames in between every pair, the screen showing the newest at each refresh.
- **With the screen as the limit, a whole count, held**: as many frames in between as fill the screen (a twentieth short still counts), the same for every pair; up after twenty pairs that left it short, down after sixty that one fewer would have filled. None only once the game reaches the screen's rate by itself, and not on the word of one pair. Such a pair's frames no longer wait for the screen to take the one before. Under a limit of its own (Frame limit, VRR's range) the room is still filled exactly: the frames shown must stay under it.
- **The guard is off unless asked for** (`guard=0`, off in saved profiles).
- **Repeats cost less.** A desktop frame that proves to be the picture before again has its conversion thrown away unsent (it was sent by itself: a piece of work for nothing); and once six in a row that came well before the game's next frame was due all proved repeats, such frames are let go untouched, one in six still looked at.
- **What interrupts is written down.** Every five seconds in which anything kept a frame in between from the screen, one `[steady]` line: pairs without any and why (a cut, the game filling the screen, the guard), frames left out late, of odd long pairs, refused by the generator, pairs dropped, the count changing, the delay learnt anew, real frames that found no room, desktop frames that were not the game's. And runs that lasted more than a minute are kept in `history\`, the twelve newest.
- **What the generator costs on the card it runs on** is told ten seconds in and every five minutes (`[card] CUDA SM..`), whatever the switches: the figure every size here was chosen from was only ever measured on an RTX 3060.

## Where it stands

The game alone at 100 FPS, defaults (Flow 35):

| Setting | Game under the engine | Shown | Desktop to screen |
| --- | ---: | ---: | ---: |
| x2 | 71 | 142 | 20 ms |
| x3 | 65 | 193 | 31 ms |
| x4 | 57 | 228 | 30 ms |
| x5 | 52 | 259 | 33 ms |
| x6 | 47 | 281 | 36 ms |
| x6, the screen as the limit (three in between) | 58 | 233 | 30 ms |
| x6, Flow 100 | 38 | 227 | - |

The game alone at 60: x2 48/96, x4 42/167, x6 36/217. At 198: x2 110/219, x4 with the screen as the limit 92/274 (two in between).

Step error (`tests\pace.ps1`), even frames: x2 0.30 ms on average (1.5 at the 95th of 100), x4 0.36 (1.6), x6 0.31 (1.4).

## The four families

Everything of rounds 3 and 4 is in what all four share: the capture, the conversion, how the work is sent to the card, the rebuild, the presenter, the counts. What differs by family is only how NVIDIA's generator is brought up - the compatibility package on RTX 20 and 30 (`lf_dlssg_compat.inl`), the kernels adapted in memory on RTX 40 (`lf_dlssg_ada.h`), the runtime as it is on RTX 50 - and none of that was touched.

One number comes from the RTX 3060 alone: frames in between are never made under 504 lines, because the generator's cost has its lowest point there on that card (1.02 ms at 896x504, 1.29 at 640x360). On the others the lowest point may lie elsewhere. The `[card]` line is there so that a log from an RTX 20, 40 or 50 says what it is.

## What is left

- A pair's further frames in between are each a piece of work by itself (0.3 ms of the game's time, round 3) and an evaluation of 0.7 to 0.9 ms: x6 costs the game 11 ms a pair. The rebuild's search could be done once a pair instead of once a frame (0.1 ms each).
- The step error on uneven frames falls into one of two states from run to run (round 3's note, corrected).
- Whether the dashed line was the fractional count can only be read from a session's log: `history\` now keeps it.
