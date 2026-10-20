# DLSS SUPER MOTION, round 5: every frame of the game, what a session's log said

5 October 2026, evening. RTX 3060 (SM86, compatibility backend 0.3.5), 2560x1440 at 200 Hz, SDR. The measurements are on `tests\game.exe`; what is said of GTA V is read from the user's own logs (`history\`, kept since round 4), not measured here. Nothing was run on RTX 20, 40 or 50.

## The report

1. Lossless Scaling is still stronger in performance: it gets 48x6 at x6 where this gets 38x6, the game alone at about 100.
2. Is all of DLSS MFG running on the tensor cores?
3. A first-person shooter was a disaster in the picture.
4. An idea: ready presets by kind of game (auto, 2D, 2.5D, first person, third person, racing, a GTA-like one), if it is any good.

## What the session's logs said

Three sessions, all with **Flow at 100%** (the screenshot sent with the report shows it too), the generator at 2560x1440:

| Time | Setting | Pairs a second (real + in between) | A pair's work on the card |
| --- | --- | --- | ---: |
| 15:51 | x4, Flow 100 | 44-47 + 135 | 8.9 ms |
| 15:53 | x6, Flow 100 | 38-40 + 190, then 30-31 + 150 for five minutes | 13.4 ms |
| 16:03 | x6, Flow 100 | 30-31 + 150 | 13.4 ms |
| 16:03:39 | x6, Flow 25 (tried for 52 s) | 30-31 + 150 for 17 s, then 45-47 + 225 | - |
| 16:04:36 | x6, Flow 100 again | 37 + 185 | - |
| 15:59:44 | x2, Flow 100 | 30-31 + 30 for 40 s, then 58-61 + 60 | - |

Read together:

- **The game was not held back by the card.** Its own rate stayed near 92 whatever the engine took: 46 is every second frame of it, 30.5 every third. The loop took a frame, asked the generator, waited for it, and only then looked for the next frame - so the time a pair's work took, with every wait behind the game's own work in it, decided how many of the game's frames went unseen. (`tests\game.exe` gives way instead when the card is short: there every frame was taken and the game itself slowed - which is why this never showed in the measurements.)
- **With Flow at 25 the same session had 46x6** - against Lossless Scaling's 48x6. The 38x6 of the report is Flow 100.
- **After every change of pace the picture stuttered for 17 to 40 seconds**: pairs were made at the new rate and shown at the old one, a third of them thrown away unseen (`365 with pairs dropped whole` in five seconds). The steady clock (`FgClock`, round 3) took a pace a third faster for the old one with jitter: every second frame "on the beat" ten milliseconds from its stamp, the others off it, never three in a row. This is the dashed line of round 4's report.
- A first-person game at 30 real frames a second, with the generator at the size that follows movement least far (round 4's table), is a turn of the camera of 170 pixels a frame against 32 it can follow.

## What was done

- **The loop does not wait for the generator** (`FgCollect`, `FgPipelined`). A pair's work is taken up when the next pair is about to be recorded; the next frame is captured, its luma read and judged, meanwhile. The frame's conversion is sent at once instead of waiting in the open list (the shared texture it reads is written again by the next capture). When each pair's work was done is told to the presenter by an event of the card's (`FgDoneWatch`). Not with a motion field of the engine's (the optical flow has its own queue). `SM_SERIAL=1` puts the wait back.
- **As many frames in between as reach the screen** (`g_fg_count_cap`): the presenter counts what it was given and what it showed; a fourteenth and more left out two seconds running, and the pairs get as many as were shown, whole; one more is tried after eight seconds that showed all, and left alone for a minute (then two, to five) when that fails.
- **The steady clock takes up another pace in eight frames**: eight spans within three tenths of each other (or one refresh, whose grid the stamps lie on) and a tenth off the pace are the new pace. Its slack is a quarter of a frame, not nearly half.
- **Game type** (`game`, "Game type" in the app): auto, 2D, 2.5D, first person, third person, racing, open world, custom. It sets the size frames in between are made at - 75% for 2D, 50% for 2.5D, 35% for the rest - and locks the Flow slider; custom unlocks it. Saved profiles go to auto: Flow at 100% no longer decides by itself.

## Measured

A light game held at 95 FPS (`-Load 150 -Cap 95`: the nearest the stand-in comes to a game that keeps its own rate):

| Setting | Game under the engine | Shown | Desktop to screen |
| --- | ---: | ---: | ---: |
| x2 | 95 | 190 | 16 ms |
| x4 | 95 | 370 | 25 ms |
| x6: settles on two in between (three tried every while) | 95 | 285-370 | 20 ms |

The same game held at 140, x6, Flow 100 changed to 25 while running: round 4's build went from 54 pairs a second to 92 and showed 2.6 of 5 frames in between each, now this one, now that one (1199 of 2960 in five seconds); this one goes to 125 pairs, two in between each, 1228 of 1254 shown.

A game that fills the card (`-Load 1040`, 100 alone) is as it was: x2 70/141, x3 63/188, x4 57/227, x6 47/277; x6 with Flow 100 38/224. Step error on even frames 0.29 ms at x2, x4 and x6. Frames in between against the frame that should be there: unchanged (pan 32 px 3.5%, 64 px 3.6%, the box 12-17%). `suite.ps1`: 21 cases clean.

## The tensor cores

What can be said, and what cannot. DLSS MFG's network is NVIDIA's: `nvngx_dlssg.dll` runs it as CUDA kernels in half precision, and on RTX 20 and 30 the compatibility package brings kernels built for those architectures (SM75, SM86). Which instructions those kernels use cannot be seen from outside, and was not looked for inside. What is measured: 3.7 to 4.0 ms for a 2560x1440 frame on an RTX 3060 - about what the card's size against an RTX 4090 gives for a network on its tensor cores, and several times less than the same work on its ordinary units would take. Everything else a pair costs - the capture, the conversion, the luma, the rebuild, the drawing - is ordinary shader work that tensor cores have no part in; at Flow 35 that is about two fifths of a pair.

## What the presets are, and are not

They choose one thing with a measured effect: the size frames in between are made at. The rest of what was hoped from "telling the engine what it will meet" the engine now does by itself - every frame of the game kept, the count that reaches the screen, the pace followed. A first-person game is helped by those, not by its name in a list. The list is kept short for that reason, and Auto is right for nearly everything.

## What is left

- Beyond the generator's reach (about 4% of the picture's width a frame at 35%) a frame in between is still broken, and with the guard off it is shown. A frame built by the engine itself from the two real frames (the rebuild's own search, without the generator) would be the fallback there; not built.
- The card, not the presenter, is what limits the frames shown on the light game: x4 at 95 leaves 3% out, x6 settles on two or three in between. A pair's further frames are each a piece of work by themselves (round 3).
- On a game that fills the card, Lossless Scaling's 88 of 100 at x2 stays out of reach on this card (round 3).
