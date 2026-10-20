# DLSS SUPER MOTION, round 9: things that go their own way, a road at 30 frames a second

6 October 2026, evening. RTX 3060 (SM86, compatibility backend 0.3.5), 2560x1440 at 200 Hz, SDR. Measured on `tests\scene.exe` and `tests\game.exe`; nothing was run in GTA V or on another RTX family.

## The report

On round 8's build (it reached the app by a build from Visual Studio; the sessions' logs show it at 87 to 95 pairs with three each in the shooter, and 30 pairs with five each in GTA V held at 30):

- Lamp posts and palm trunks still vanish when driving fast - and not in Lossless Scaling, so it can be done.
- The road's asphalt "turns to jelly", in both programs, at low frame rates (30), GTA V above all.

## Things that go their own way (`MoverMain`)

A lamp post beside the road is thin, fast, and in front of a picture that moves another way. The generator loses it - a ghost, or nothing - and since round 7 what it loses the rebuild could not show. But both real frames have it. Where it stands in each, the two do not agree under the picture's own movement: two places of unrest, the thing's in the frame before and the thing's in the frame after.

Once a pair, for every block with unrest in it: by which movement do its places of unrest in the frame before come to lie on places of unrest in the frame after that look the same? Looked for 32 small pixels each way (90 of the picture's), every second one, then in thirds of a pixel around the best; nearly half of the block's unrest has to go by it, and four places. Then in every frame in between (`ChooseMain`):

- a pixel both real frames show alike, moved by such a thing's own movement, with unrest at both ends, is the thing's: built from the two real frames by that movement, sharp, where it is at that frame's moment;
- a place of unrest where one real frame shows the thing (it is elsewhere at this moment) is the scene behind it: built from the other real frame alone;
- unrest near that such a thing explains is no reason any more to ask the generator about the picture around it.

What it must not take, each found by a test going wrong:

- What stands still on the screen. A row of squares of a game's read-outs "found itself" one square on in the frame after, all of them, and was drawn between its squares: a place the frame after has where the frame before has it is not a thing going anywhere - in the vote, in the frame, and in "came or went".
- The edges of a row of windows, each a little off, find partners among each other a third of the time by chance: hence "nearly half".

## The road (`PairMain`)

Under a car at 30 frames a second the road close to the camera moves a tenth of the picture's width from one frame to the next, and hardly at all at the horizon - nothing like the one movement (or the one growing) the picture's own search knows. Looked for only three steps around that, such a block was never found; and asphalt, which has little in it, passed for "agreed" under any movement, so it was built from the two real frames moved wrongly, each block its own way. That is what the jelly is here.

A block is now "not found" when the two real frames are still as far apart, under the best movement near, as the block is from itself two steps on (`Rough`). Only then it is looked for far around - every third step, twenty-four each way - and where it was in the pair before (a road keeps coming the same way), then in single steps and quarters around the best of those.

Only then, on purpose. A block found near the picture's own movement is never asked where it was before, or far around: with "the pair before" among the first places tried, the floors of the tower block were found one floor off where they had been going right after the camera turned round - round 6's earthquake, in one pair of three (the face 35% off). And a block with read-outs in it is not looked for far around: the row of squares again.

## What came of it

Share of pixels more than 12 of 255 off; round 8's engine and this one, the same hour.

| Scene | Round 8 | Now |
| --- | ---: | ---: |
| A box crossing a still picture, 24 px a frame | 14-16% | 6-8% |
| ... 48 px | 30-47% | 15-34% |
| Lamp posts, driving 60 cm a frame, x6 | 7-14% | 7-12% |
| The car crossing, driving | 15-26% | 15-26% |
| ... at a tilt of 16 px | about 50% | 47-53% |
| The read-outs | 0.0-0.2% | 0.1-0.2% (2% in a pair here and there until "came or went" left them alone) |
| The tower block's face, tilt 32 | 4-10% | 4.5-10% |
| The dots | 0.05-0.2 px | 0.05-0.2 px |
| Pan 64 / 128 px | 1% / 2% | 1% / 1.6% |

The posts' share hardly moved, and the picture did: on round 8 the nearest post was a pale ghost bent at its foot; now it stands whole and sharp at its place (what is counted off is its two edge columns, a pixel wide each). The small car at a tilt is not better: it is as wide as a block and half its unrest does not find a partner.

Costs: the pair's own movement went from 0.22 to 0.5 ms a pair. A heavy game: x2 70/140 to 69/137, x4 58/233 to 57/231, x6 55/334 to 54/322; the light game at x6 97 to 84 pairs for the same 400 shown. `suite.ps1`: 21 cases clean on the build before the last small change and on the last build; one run of the last build between them said "2 of the cases went wrong" without it being kept which (run again at once: clean). `SM_NO_MOVERS=1` switches the second movement off, to compare.

## Not seen, not known

- GTA V itself: the palms (their fronds are no rigid thing), the road at 30 with its own motion blur.
- A road whose near part moves more than a tenth of the width a frame is past the search; it is still built from the two real frames by a movement that is not its own.

## Tests

`tests\dump\verify9b.sh`: waits for a PC nobody is at (four minutes without a touch - Windows lets no window come to the front for 200 seconds after the last one - and no film or game in front), then runs the scenes on a build kept beside `out\`, the option cases, the costs and the pacing. The engine under test is kept out of `out\` until it has passed: the app, built from Visual Studio, takes whatever is newest there.
