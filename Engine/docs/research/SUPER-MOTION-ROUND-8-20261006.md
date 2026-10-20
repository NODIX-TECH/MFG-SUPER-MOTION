# DLSS SUPER MOTION, round 8: a game's read-outs, a light game at x6, thin things

6 October 2026. RTX 3060 (SM86, compatibility backend 0.3.5), 2560x1440 at 200 Hz, SDR. Measured on `tests\scene.exe` and `tests\game.exe`; nothing was run in THE FINALS, in GTA V, or on RTX 20, 40 or 50.

## The report

Three things, with two pictures (GTA V on a bridge, lamp posts along it; THE FINALS, a first-person shooter):

- THE FINALS: the read-outs in the middle of the screen (the ring to aim by, the compass) "flicker very, very badly"; not in Lossless Scaling.
- THE FINALS at x6: 350 to 370 frames a second here, about 480 there.
- GTA V: thin things - lamp posts, palm trunks - lose "their lower half"; not there.

The sessions' logs (kept in `tests\dump\user-logs-r8`): GTA V at x6 47 to 50 pairs a second with five each (round 7's build: 42 to 45 before it). THE FINALS at x6: 54 to 62 pairs a second with four to five each; and there, unlike GTA V, the compositor draws the output (`Present -> screen` 8.5 ms against 2.4; 107 desktop frames a second that were the engine's own picture coming back, each copied and looked at before it could be told).

## The street got read-outs and lamp posts

`tests\world.h`: a game's read-outs lie over the street and do not move - a ring of two pixels with a dot in it, a compass of ticks and small marks, a row of figures, a row of squares, a pane of dark glass with marks on it; and lamp posts eighteen centimetres thick stand along both sides of the road (driving past them, `dolly`, the near ones cross the picture fast in front of things that hardly move). `jelly.exe` counts the read-outs, the picture right beside them, the picture under the glass and the posts each by itself.

Both reproduced on round 7's build. The compass over the tower block's moving face: every mark drawn two or three times, the figures torn (`dolly 60`, x6). The nearest lamp post: washed out to a ghost, bent at its foot.

## The read-outs

What they were: a mark of the screen is not part of the scene, but round 7 took it along with the scene. A frame in between asks the two real frames what stands at a place, each moved by the picture's movement; where one of them has a mark of the screen there, that mark was drawn - moved - into the frame: twice a frame (once from each real frame), ten places in a pair's five frames. And where the mark really stands, both real frames, moved, show the scene behind it: the mark itself was painted over. Round 7's own allowance for thin lines (half the local contrast) let all of it pass as "agreed".

Now, once a pair at the picture's size (`StillMain`): a pixel is a mark of the screen where the two real frames have the very same colour at the very same place and it is not a flat place - in both frames, the same way. (A sky is the same in both frames too, but a thing may cross it between them; and beside a dot of the scene that moves, the wall is the same in both frames with the dot next to it in one only - taken for a mark, that piece of wall was drawn over the dot when it came by: the dots went from 0.05 to 0.35 pixels off until this was seen.) Then:

- a mark with marks beside it is shown as the real frame has it (the build pass);
- where one of the two real frames has a mark at the place it is asked about and the other has not, the scene is what the other shows: the frame is built from that one alone, and the place counts as agreed (`SettleMain`, `ChooseMain`).

| x6, share of pixels more than 12 of 255 off | Round 7 | Now |
| --- | ---: | ---: |
| The read-outs, tilt 16 px a frame | 7-12% | 0.0-0.2% |
| ... driving, 60 cm a frame | 8-9% | 0.0-0.1% |
| The picture right beside them, tilt | 8-11% | 1.5-6% |
| ... driving | 8-9% | 6-7% |
| The dots on the bare wall (not to be lost again) | 0.05-0.11 px | 0.05-0.09 px |

What is the same in both frames only because the game drew it see-through over a moving picture (the pane of glass) is not a mark; under it the picture is built as anywhere.

## A light game at x6

A stand-in for the shooter: a game that does 185 FPS by itself with the card three quarters used (`measure.ps1 -Load 400 -Cap 185`). At x6 it gave 366 frames a second - 61 pairs with five each, the game itself held to 70 - the report's 350 to 370, without the compositor in it. The work counted on the card (4.4 ms of generator, 5 x 0.36 ms of drawing a pair) would leave 80 pairs. Three things were in the way.

**The generator's evaluations.** Each of a pair's five costs far more than its 0.8 ms on the card: it is a round of its own between the card's two programming interfaces. And since round 7 a frame of the generator's is only asked about what moves its own way and what appears or goes; the picture itself is built from the two real frames. So from four frames in between on (x5, x6) the generator makes two - a third and two thirds of the way, its own x3 - and the others go by the nearest of those for what moves its own way: at x6 in twos, the last by the real frame after, so such a thing steps a third of its way every second frame shown, evenly. Everything the two real frames agree about moves with every frame as before. `SM_FG_EVERY=1` puts it back, to compare.

**The presenter's waits.** It waited for the card after each drawing before it asked for the Present: a millisecond under a game, six times a pair. A pair with a frame every 3.5 ms or less is now sent without those waits (four allocators and four sets of the rebuild's descriptors in turn; the pair's pictures are let go once the card is through with them, not waited for before the next pair is begun).

**Twice the screen's rate.** With both, the loop took 97 to 106 pairs a second - and the presenter showed 400 frames a second, whatever was asked: 67 pairs with five each when held to 70 FPS, the same 402 with the game at 185. That is twice 200 Hz: on this path a screen takes two Presents a refresh. Past it, more frames in between only push real frames out: so a frame more than three frames late is passed over, and the count a pair follows what reaches the screen - 97 pairs with three each (measured) rather than 67 with five.

| | Round 7 | Now |
| --- | ---: | ---: |
| Light game (185 alone), x6: game / shown | 70 / 366 (61 pairs x 6) | 97-102 / 400 (97 pairs x 4) |
| Heavy game (100 alone), x6 | 49 / 295 | 55 / 334 |
| ... x5 | - | 57 / 287 |
| ... x4 (the generator's own three, as before) | 58 / 233 | 58 / 233 |
| ... x2 | 70 / 139 | 70 / 140 |

What it does to the frames (x6, two of five from the generator): what moves its own way is a little further off in the three frames that go by a neighbour's (the car crossing at a tilt of 16: 41-48% to about 50% of its pixels off; the lamp posts driving: the same 7 to 17%).

Said plainly: 480 on the counter is not reached on this screen - the ceiling here is 400 - and more than 200 of them a second no 200 Hz screen shows. What changed for the eye is the base: 97 real frames a second instead of 61.

## Thin things that move their own way

The lamp post is the generator's: a thing of a few pixels crossing a third of the picture's width a second is lost in its frame (a ghost, or nothing), and the rebuild shows what it has. This is not solved in this round. What it needs is known and is the next step: a second movement a block - found between the two real frames by matching what does not agree in one with what does not agree in the other - by which such a thing is built from the real frames like the rest.

## Tests, and a screen that went to sleep

`jelly.ps1`: the read-outs and the posts are counted in every `tilt` and `dolly` run. `measure.ps1 -Load 400 -Cap 185`: the light game. The screen went to sleep half an hour into the tests (Desktop Duplication hands over nothing then: "0 frames", every engine build alike) - `tests\dump\verify8.sh` waits for a screen that is on and a PC nobody is at, then runs the option cases, the scenes and the costs.
