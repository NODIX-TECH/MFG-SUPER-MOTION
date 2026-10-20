# MSM FG 1.0

The engine's own generator of frames in between (`generator=2`, "MSM FG 1.0" in the app - the name stays 1.0 until the app is released; this file says what each round changed). Plain Direct3D 12 compute: it runs on any card the engine runs on - NVIDIA, AMD, Intel, an integrated chip - needs no library of anyone's, nothing of the processor's, and every step of it is in one file, `msm_fg.h`, open to be changed.

DLSS MFG and XeSS MFG stay as they are; the three share everything around the generator (`lf_dlssg.inl`: the pairs, how many frames a pair gets, the presenter, VRR, sync modes, HDR, Low Latency).

## What round 21 changed

In the app the generator is called **SMFG** ("SMFG 1.0", "SMFG · Fast") from this round on; in the sources, the log and the tests it stays MSM FG.

The user's picture: a tower's face of fine upright lines, the lines broken and dotted over the middle of it - "in all three generators, and in Lossless Scaling too; keep the lines better". What repeats is explained as well one line on as where it truly went, and each block picks for itself: neighbours one line apart, and between them every pixel choosing between the two words.

`FillMain` now also asks the two real frames: where they agree under the word of the picture at large about as well as under the block's own (`alias_ratio` 1.15 times as far apart, `alias_floor` 0.03 more, nine places' brightness), the block takes the picture's - the face one word, its lines whole. A thing that moves by itself does not agree under the picture's word and keeps its own. On the test street, five frames a pair (share of the tower block's face off, three pairs):

| | before | now |
| --- | --- | --- |
| tilt 32 | 15.2 / 9.4 / 4.0% | 14.7 / 7.6 / 3.3% |
| tilt 16 | 2.3 / 3.6 / 2.4% | 2.3 / 3.0 / 2.3% |
| drive 60 | 3.0 / 3.5 / 3.8% | 2.9 / 3.4 / 3.7% |

Thin things as they were (lamp posts at `drive 60`: 12.0 / 10.6 / 10.2 before, 12.0 / 11.6 / 10.4 now; thin poles 8.5 / 9.1 / 14.5 and 8.6 / 9.0 / 14.8). A looser rule (1.4, 0.08) gains nothing more and costs the car and the posts a little. It is a help, not the end of it: where the picture at large is itself a line off (a face that fills the picture, moved by half its own step) nothing says which line is the right one. The test street's face repeats down the picture, the user's across it: his eye decides. DLSS MFG and XeSS MFG are not touched by this (their rebuild has its own rule for what repeats, `sm_guard.inl`).

## What round 19 changed

The name: MSM FG (it was OSM FG; the app is MFG SUPER MOTION). `msm_fg.h`, `Msm...`, `[msm]` in the log, `msm_search`, `SM_MSM_TUNE`, `tests\msmtruth.exe`.

The user, of round 18's build in GTA V: good in both searches, the fault not gone entirely - "still noticeable a little". Two things were changed, and one found that is not:

- **A frame is drawn from both real frames all the way** (`nearer_from`, `nearer_to` = 0). Round 17 drew the first third of the way from the frame before alone and the last third from the frame after alone, for a frame as sharp as a real one. With five frames a pair that left what moves by itself standing where it was for two frames and coming on from where it will be for two; the frames were no nearer the true ones for it (the moved photograph, five frames a pair: mean error 1.54 with the rule, 1.31 without).
- **A fast turn of the camera by the player's hand is not a scene cut.** The engine takes a picture that changed by more than 0.24 of full scale for another scene and makes no frames for that pair. In the user's GTA V session 13 of some 140 pairs went that way while the camera was swung about - each a pair of the game's own frames only, a hitch in the middle of a turn. With Adaptive Motion Learning on, while the hand is turning the camera (a hundred counts of the mouse, or a third of the stick's throw, in the last tenth of a second), a change up to 0.55 is that turn (`SceneCutNow`, `LearnTurning`). MSM FG only: it is the one told where the picture went.
- **Not helped by any of its numbers:** what is left on the test street at `drive 120` with five frames a pair - a strip of the tower block's face in blots beside the nearest lamp post (the post goes 200 pixels a frame in front of a face that repeats: the blocks beside it take the post's word or one that fits the face a window on), and bites out of the end of a lane's dash rushing by at the picture's bottom. `edge`, `mixed_fits`, `one_sided`, `nearer_word`, `refine_plain`, `far_share` each tried at half and at twice its value: the lamp posts' 25 / 25 / 38% off and the road's 1.0 / 1.1 / 1.2% do not move. A wider far search (`far_reach` 28) put debris on the face and cost 0.8 ms. It is the movement itself that is not found there - which pixels belong to the thing in front - and that is a piece of work of its own (the blocks of a thing found moving by itself kept together as one thing, and what it uncovers drawn from the one frame that shows it).

Cost and the frames in the engine are round 18's (a game of 115 frames a second with five frames a pair: 63 against 64 kept, a pair 3.6 ms, "Fast" 3.2; the box 3.8 to 4.0% off, the pan 0.5 to 0.6%, the read-outs 0.0%).

## What round 18 changed, and why

The report, with a picture from GTA V: squares, a strange glow, lamp posts in pieces. Enlarged, the picture showed what it was: every lamp post drawn as a thin dark outline of itself around nothing, rings and rounded squares in the sky beside it, the hillside in blots with outlines. The same shows on the street of the tests once a frame is looked at instead of counted (`msmtruth drive 120`, the near lamp post).

- **Words are not mixed across an edge** (`EdgeMain`, `WarpMain`). A pixel was drawn by the words of the four places around it, mixed. Between a post that moves two hundred pixels and a sky that moves none, the mix is neither's: the pixel was taken from somewhere along those two hundred, and the whole post was squeezed into a line along the border between the two places. Now, where the places around a pixel do not say the same and the two real frames do not show the same under their words mixed, the pixel takes one of the four words whole - the one under which the two frames show the same there. (Where the frames do agree under the mix - a soft road, where the words differ because nothing tells them apart - mixed they stay; taken one by one such a place came out as a patchwork of little squares.)
- **A block that shows little goes by the picture at large** (`FillMain`). A soft road, a wall in haze, a sky match as well a long way off as where they are: on the test street the road's blocks were 33 pixels off on average, the bare wall's 8, and a picture drawn by such words stands in blots. The less a block shows, the more it takes the movement of the picture around it (the bare wall's blocks: 2.8 pixels off).
- **What was not found is looked for far and wide** (`FarMain`). A thin thing going its own way faster than what is behind it - a lamp post rushing past a car - is too thin to show at the smaller sizes and further off than one texel a size reaches: it never had a word of its own. At the second largest size a block still not found is looked for within fourteen texels (eighty pixels at 1440p) and taken where it is found outright. The near post of `drive 120` is there again, in pieces rather than whole.
- **Marks, told by what would change them** (`MarkMain`). Round 17 took a pixel for a read-out when it stayed the same four pairs running. In a soft picture half the pixels stay the same from frame to frame while all of it moves. Now: a pixel alike in both frames is tried with every way it may be drawn by - its place's word each way, and the picture at large's - at the end of the way and half way along. Where any would make it clearly another colour, it stays while its place moves: counted up by two. Where its own place moves and none would, it goes along: counted down by one. A mark from eight on. The read-outs of the test scenes: 0.0 to 0.7% of their pixels off (round 17: 0.4 to 2.5%).
- **"Fast" is what MSM FG starts on.** It relaxes each size half as often, does not tell the two ways of each other and asks no place for the other word; it keeps all of the above.

Tried and left out in round 18: a plain pixel taken for a mark's own when a mark stands near along the way the picture moves (it took the wall a lamp post was crossing: posts 9% off without, 14% with); preferring, at an edge, the word that stands out from the picture at large (no better).

## What round 17 changed

The report on the first version: "its pixels come apart". Two rules of its last pass decided for every pixel of every frame by itself:

- **Marks** are a byte a pixel kept from pair to pair instead of a rule applied to each pair alone ("Keep still pixels" in the app turns them on and off).
- **The other word is a place's, not a pixel's** (`ResolveMain`): where the two frames do not show the same under a place's word but do under the picture at large's, that is the word taken - asked at the middles of the block's four texels, in the two frames' own colours.
- **A frame near a real frame is drawn from that frame alone** (`WarpMain`): two real frames laid over each other are never quite in register, so only the middle of a pair is drawn from both.
- **Where to look, when the engine knows** (`MsmAsk::expected_x`): Adaptive Motion Learning's word of where the picture went (`docs\ADAPTIVE-MOTION-LEARNING.md`).

Tried and left out in round 17: offering every block what it said for the pair before (it kept wrong words alive).

## What it does

Given: the real frame before and the real frame after, at the picture's own size. Made: the picture at any moment between them, at that size, from those two frames' own pixels. Nothing is made small and scaled up, so nothing is rebuilt afterwards (`sm_rebuild.inl` is not used) and nothing is soft.

Once a pair (`MsmPair`):

1. **Brightness** (`LumaMain`, `HalveMain`). Each frame's brightness at the size the motion is looked for at - "Flow" in the app, a share of the picture, 35% unless set - and four times halved, each time through a filter wide enough that nothing finer than the new size is left. A frame's is kept: it is the frame before of the next pair.
2. **Search** (`SearchMain`, `RefineMain`, `RelaxMain`, `FarMain`). For every block of two by two texels, where its four by four neighbourhood lies in the other frame. At the smallest size every place within four texels is tried - around "not at all", and around where the picture as a whole is expected to have gone when the engine was told; each larger size starts from what the nine blocks around said at the size below and looks one texel further (a block that shows nothing itself stays with the word below). Every block's word has a weight: what it shows and how well it was found; a plain block takes the word of the blocks around it. What is still not found at the second largest size is looked for far and wide. Both ways: from the frame before to the one after, and back.
3. **Cross** (`CrossSplatMain`, `CrossMain`; not in "Fast"). What one way found, the other is told.
4. **Settle** (`SubpelMain`, `LargeMain`, `FillMain`). Parts of a texel; whether a block was found at all; how the picture at large moves around every sixteen by sixteen blocks; and that movement for the blocks that show little.
5. **Marks** (`MarkMain`, at the picture's size). Which pixels are a read-out's.

Once a frame in between, for its moment t (`MsmFrame`):

6. **Carry** (`SplatMain`). Every block of the frame before is carried t of its way, every block of the frame after 1 - t of its way back, and leaves at the place it reaches how it moves. Two that reach one place: the place's own before a neighbour's, one found before one not found, then the one that moves least like the picture at large.
7. **Resolve** (`ResolveMain`, `EdgeMain`). Both frames say the same at a place: the frame is theirs together. Only one reached it: that frame alone. Neither: what the places around say. Where the picture at large's word suits a place better, that one (not in "Fast"). And how far each place's word is from its neighbours'.
8. **Draw** (`WarpMain`, at the picture's size). A mark stays as it is. Every other pixel from where it lies in the nearer real frame - from both only about the middle of the pair - by its places' words mixed, or by one of them whole where they part.

Nothing waits for the card and nothing is read back: all of it goes into the pair's own command list.

## In the engine

Three seams in `lf_dlssg.inl`, each marked "MSM FG": `EnsureFg` (`MsmReady`), `FgPresent` (`MsmPair`, then `MsmFrame` for each frame, into the pictures DLSS MFG's frames go into) and `CloseFgResources`. From there on a pair of its frames is a pair of DLSS MFG's made at "Flow" 100: shown as they are. "Flow" and "Game type" in the app set the size the motion is looked for at; the frames are always the picture's size.

Options: `generator=2`; `flow`, `game` as for the others; `still` (the marks), `msm_search` (1 "Fast", what the app starts on; 0 in full), `aml` (Adaptive Motion Learning); `SM_MSM_TUNE=<name>=<value>,...` in the environment sets any number of `MsmTune` (the names are in `msm_fg.h`) to try another.

## Measured

RTX 3060 and Intel UHD 730, 2560x1440 at 200 Hz. Round 18 (`docs\research\SUPER-MOTION-ROUND-18-20261012.md`); the first version's figures are round 16's.

**Look at the frames, not only at the figures.** The share of pixels off hid the worst of round 17: a lamp post drawn as its own outline is few pixels. `tests\rawpng.ps1` makes a PNG of any part of a frame `msmtruth.exe` wrote.

Frames against the ones that should be there, share of pixels off (`tests\dump\msm-table.sh`: `msmtruth.exe` + `jelly.exe`, the street of `world.h`, x2, mean of three pairs, five pairs gone through before each):

| Scene | | whole | tower face | dots | car | lamp posts | thin poles | read-outs | beside them |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| drive 60 | round 16 | 1.7 | 4.6 | 25.6 | 4.6 | 12.5 | 14.6 | 2.4 | 4.0 |
| | round 17 | 1.6 | 4.0 | 20.4 | 5.5 | 9.6 | 13.2 | 0.7 | 3.7 |
| | now | 1.5 | 3.5 | 20.2 | 3.8 | 12.3 | 12.5 | 0.0 | 2.5 |
| | now, fast | 1.6 | 3.9 | 19.8 | 3.9 | 12.7 | 12.5 | 0.2 | 3.0 |
| | round 17, fast | 2.0 | 5.1 | 22.0 | 5.1 | 10.5 | 14.5 | 0.4 | 6.4 |
| tilt 16 | round 17 | 0.8 | 2.9 | 21.9 | 17.5 | 2.1 | 3.3 | 0.4 | 4.0 |
| | now | 0.7 | 2.8 | 20.5 | 13.2 | 1.8 | 2.8 | 0.1 | 3.5 |
| | now, fast | 0.8 | 3.4 | 21.2 | 12.9 | 2.1 | 3.2 | 0.5 | 4.8 |
| drive 120 | round 16 | 3.0 | 5.8 | 44.1 | 8.6 | 33.7 | 25.6 | 2.6 | 5.3 |
| | round 17 | 3.0 | 5.3 | 31.1 | 7.7 | 31.3 | 24.5 | 2.1 | 6.6 |
| | now | 2.6 | 4.4 | 24.3 | 6.6 | 27.3 | 25.1 | 0.3 | 4.5 |
| | now, fast | 3.1 | 5.4 | 24.4 | 6.8 | 26.7 | 28.1 | 0.7 | 4.6 |
| | round 17, fast | 3.9 | 7.6 | 34.0 | 7.9 | 34.6 | 26.4 | 1.2 | 6.7 |
| tilt 32 | round 17 | 1.6 | 10.6 | 44.5 | 36.3 | 2.8 | 4.1 | 2.5 | 12.6 |
| | now | 1.4 | 9.6 | 36.8 | 30.1 | 2.4 | 3.6 | 0.1 | 10.2 |
| | now, fast | 1.7 | 11.0 | 35.1 | 28.0 | 2.5 | 4.2 | 0.1 | 13.0 |
| dolly 40 | round 17 | 1.0 | 4.8 | 17.2 | 9.2 | 4.3 | 4.4 | 0.5 | 4.2 |
| | now | 0.9 | 4.2 | 15.2 | 7.1 | 5.1 | 4.1 | 0.0 | 2.7 |
| | now, fast | 1.0 | 5.2 | 12.2 | 7.3 | 4.8 | 4.3 | 0.0 | 3.1 |

(The lamp posts of `drive 60` count worse than in round 17 and look better: whole, where they were outlines. Their edges are a pixel or two thinner than they should be - a wall's word at a post's edge fits as well as the post's.)

Against the other two generators (round 16, through the engine, x2): drive 60 whole picture - DLSS MFG 2.0 to 2.6%, XeSS MFG 2.9 to 3.4%; its lamp posts 16 to 22% and 24 to 28%; drive 120's lamp posts 44 to 52% and 55 to 63%.

A photograph moved a known way (`tests\dump\msm-photo.sh`): mean error of the frames, 0..255, and the share of pixels off.

| The photograph moves, a frame | round 17 | now |
| --- | --- | --- |
| 24 px across | 0.25 / 0.03% | 0.33 / 0.00% |
| 60 across, 10 down, grain | 1.14 / 0.40% | 1.13 / 0.27% |
| tiles of many colours, 16 across | 1.16 / 1.23% | 0.95 / 0.90% |
| 260 across, not told where it went | 5.02 / 8.5% | 2.99 / 3.6% |
| 260 across, told (Adaptive Motion Learning) | 1.01 / 1.6% | 1.14 / 1.5% |
| 400 across, not told | 8.49 / 19.2% | 7.97 / 17.7% |
| 400 across, told | 2.88 / 5.0% | 3.00 / 4.9% |

By itself (`tests\msmtruth.exe bench=`), a pair's own work and what a pair costs with its frames, ms:

| | pair | x2 | x6 |
| --- | --- | --- | --- |
| RTX 3060, round 16 | 0.87 | 1.16 | 2.23 |
| RTX 3060, round 17 | 1.07 | 1.45 | 2.80 |
| RTX 3060, now | 1.54 | 1.93 | 3.71 |
| RTX 3060, now, fast | 1.32 | 1.74 | 3.30 |
| UHD 730, round 16 | 6.9 | 9.8 | 20.8 |
| UHD 730, round 17 (fast) | 8.3 (5.5) | 12.3 (8.2) | 26.2 (17.5) |
| UHD 730, now | 11.6 | 16.1 | 32.9 |
| UHD 730, now, fast | 9.8 | 13.6 | 27.0 |

(On the UHD 730, of "Fast": the far search 2.2 ms a pair, the marks 2.5, the edges a millisecond a frame. Round 18 bought its picture with the card's time; on a weak card that is felt.)

In the engine, what it costs a game that fills the card (`tests\game.exe` load 780, alone 112 to 121 frames a second that morning, the PC in use; the game's own rate, then real / shown under the engine):

| On the game's card (RTX 3060) | round 17 | now | now, fast |
| --- | --- | --- | --- |
| x2 | 84, 84 / 169 | 82, 81 / 163 | 80, 79 / 157 |
| x6 | 65, 64 / 385 | 62, 61 / 364 | 64, 62 / 375 |

A pair's own work on the card at x6: 2.85 ms (round 17), 3.42 (now), 3.09 (fast).

On the second card (`gpu=1`, the UHD 730; round 17's figures - not measured again): the game keeps 92 to 98 of 119, the chip makes 20 to 27 pairs a second at x6 and 38 to 42 at x2, 110 to 170 ms behind the game. With round 18's work on top it will make fewer.

Through the engine (`tests\jelly.ps1`): a box crossing a still picture at 24 pixels a frame - the box itself 3.5 to 3.9% off (round 17: 4.3 to 5.4; DLSS MFG 10.3%, XeSS MFG 43.1%); a pan of 16 - the moving picture 0.6% off, mean error 0.75 (round 17: 0.8%, 0.9; "Fast": 1.1%), the still HUD 0.0%, the picture beside it 2.6 to 3.1% (3.6 to 4.7).

(A pair of the pan scene now and then comes out "half a step off": the scene had gone two steps between two real frames, and the frame in between stood exactly in their middle. Right for the pair; `jelly.exe` expects one step. `tests\dump\panshift.ps1` measures it.)

## Where it is weak

- A thin thing rushing past very fast (the nearest lamp post of `drive 120`, 140 pixels a frame): found, and drawn in pieces.
- Small marks on a plain surface moving fast (the dots of the bare wall: 15 to 37% of their pixels off), and a pattern that repeats moved by half its own step and more (the tower block's face at a tilt of 32: 10%).
- A thing of its own crossing while the camera turns fast (the car at a tilt of 32: 30%).
- A read-out that is not the same from frame to frame (a map that turns, a see-through panel) is no mark at all; and one is a mark only after four pairs in which its place moved.
- A weak card: on an Intel UHD 730 a pair with five frames takes 27 ms in "Fast", 33 in full.
- Not tried: an AMD card, an Intel Arc, HDR on a screen. In a real game it was seen by the user only, in GTA V - which is where round 18's faults were found.

## Trying it without the engine

```
Engine\tests\build-msm.cmd
Engine\tests\msmtruth.exe Engine\tests\dump\msm drive 60                 # three pairs written, and how far each frame is off
Engine\tests\msmtruth.exe Engine\tests\dump\msm drive 60 motion=1        # how far the motion found is from the true one, by kind of thing
Engine\tests\msmtruth.exe Engine\tests\dump\msm drive 60 card=1 bench=30 # what it costs the second card
Engine\tests\msmtruth.exe Engine\tests\dump\msm photo 260 ex=-260        # a photograph moved 260 pixels a frame, the search told so
Engine\tests\msmtruth.exe Engine\tests\dump\msm photo 30 zoom=20 blur=3 car=1   # a soft picture driven into, a car standing on the screen
bash Engine/tests/dump/msm-table.sh "drive 60" "tilt 16" "drive 120"     # jelly.exe's figures, a line a scene
bash Engine/tests/dump/msm-photo.sh "24" "60 dy=10 grain=2"              # the photograph: mean error, pixels off, held still wrongly
powershell Engine\tests\rawpng.ps1 -Raw Engine\tests\dump\msm\fg-30-1.raw -Width 2560 -X 0 -Y 200 -W 1280 -H 720 -Png look.png
```

`lead=<n>` pairs are gone through before each pair measured (5), as the engine has gone through the ones before: the marks are counted from pair to pair. `fast=1`, `marks=0`, `ex=`, `ey=`: what the engine tells a pair.
