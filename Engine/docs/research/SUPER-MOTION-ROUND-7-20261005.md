# DLSS SUPER MOTION, round 7: the dots on the wall, the pair's own movement

5 October 2026, evening. RTX 3060 (SM86, compatibility backend 0.3.5), 2560x1440 at 200 Hz, SDR. Measured on `tests\scene.exe` and `tests\game.exe`; nothing was run in GTA V, on RTX 20, 40 or 50.

## The report

The same place in GTA V as round 6, a picture of it: a bare concrete wall between two columns of balconies, with a column of small dark dots on it, one a floor. With the camera going up and down the dots "moved from their place in an ugly way" here and stayed put in Lossless Scaling. And x6: 42 base frames here, 48 there.

The session's log (kept): x6, frames in between at 896x504, 43 to 45 pairs a second with five each (247 to 266 shown); a pair's work on the card 5.27 ms; drawing takes the presenter 1.24 ms for a real frame and 1.66 for a frame in between, most of both waiting for the card.

## The street got a wall with dots, a car, and grain

`tests\world.h`: between the tower block's columns of balconies stand bare walls three metres wide, each with one dark opening a floor - five pixels by seven of the picture, a pixel and a half of the frame the generator works on. A car crosses the road as many pixels a frame as the picture tilts. `-Grain n` puts a game's noise on every frame (every pixel up to n levels lighter or darker, anew each frame).

`jelly.exe` counts the dots one by one: how far the middle of each one's darkness is, in the frame made, from where it is in the frame that should be there, and how much of the darkness is left. With `-Multiplier 6` all five frames of a pair are checked, each against the street drawn for its own moment. (A dumped pair is now drawn whole, late or not - writing its pictures out is what made it late, and one frame of five reached the files. And `rawpng.ps1` read every second row: its loop's `$y` was its own `-Y`.)

## What was wrong

Round 6's engine, x6, a tilt of 24 pixels a frame: the dots 0.3 to 1.3 pixels from their place on average, some gone altogether, 42 to 58% of their pixels off; at 32 pixels a frame 2 pixels on average. Two causes, both the generator's frame being followed:

- The generator loses what is small. The opening is a pixel and a half of its frame; it comes out faded, or beside its place, differently in each of a pair's five frames.
- The rebuild followed it. Its block search (eight by eight points, eight pixels of the picture apart) does not see a dot at all, and on a bare wall the two edges say nothing about up and down: where the block "went" in that direction was noise. Round 6's repair looked at whole blocks - is the block, on average, far from what the two real frames show? - and a dot is a hundredth of a block.

## The pair's own movement

What the two real frames agree about no longer goes through the generator's frame at all. Once for every pair (`sm_rebuild.inl`):

- **Pair** - for every block, the movement from the real frame before to the one after under which the two show the same thing: around the picture's own movement there (sm_guard.inl; it wins a tie - a wall with nothing on it says nothing), three steps each way, and around "not at all"; then in quarter steps. On a hundred places a block, every pixel of it under one of them: a dot counts.
- **Settle** - for every small pixel, the blocks' movements blended (a picture that grows from its middle moves a little differently in each), each by itself where the blend does not fit; and how far the two real frames agree around the pixel, moved so: the most they differ at nine places, less half the picture's own contrast there. (A fifth was allowed at first. A balcony's rail is thinner than a small pixel: it falls on one row of the small frame in one real frame and on two in the other, and they differ by up to half of it with nothing having moved. Every rail counted as a thing moving, and at a tilt of 32 pixels a frame - where the generator's frame is one floor off - the generator kept its say over the face: 19 to 35% of it off.)
- What one of the two real frames does not have, because it lies beyond its edge (the strip the picture moves into), cannot be compared: it counts as agreed and is built from the frame that has it. Counted as unrest it left a strip along that edge, and every tile next to it, to the generator.
- For every tile of eight by eight small pixels, how much of it agrees; **Widen**: the least of the tile and the eight around it.

Then every frame in between: a pixel the two agree about is theirs - its base their own picture there, moved by the pair's movement to the frame's moment, its detail the nearer one's. All five frames of a pair go by one movement: what stands in the scene glides from one real frame to the next. Around a thing that goes its own way (the tiles near do not nearly all agree) a pixel is theirs only where the generator's frame shows what they show: it may have the car there, between the two places they have it at. Everything else is the generator's, followed as before.

## What came of it

Share of pixels more than 12 of 255 off the frame that should be there; the dots in pixels from their place. Round 6's engine and this one on the same scenes, the same hour:

| Scene | Round 6 | Now |
| --- | ---: | ---: |
| Tilt 16 px a frame, x6: the dots | 0.35-0.76 px, 42-46% off | 0.05 px, 25% off |
| ... the tower block's face | 8.6-9.8% | 2.9-3.3% |
| Tilt 24, x6: the dots | 0.3-1.3 px (some gone) | 0.06-0.15 px |
| ... the face | 9.3-12.6% | 3.2-5.2% |
| Tilt 32, x2: the dots | 2.1 px (some gone) | 0.08-0.13 px |
| ... the face | 10.7% | 3.7-4.1% |
| Tilt 48, x2: the face | 9.4% (round 6's run) | 11-15% |
| Pan 64 px: the moving picture | 2.7-3.5% | 0.3-1.1% |
| ... the picture beside the still HUD | 28-30% | 13-25% |
| Pan 96 px | 6.2-6.5% | 0.6-1.4% |
| Pan 128 px | 9-10% | 0.9-2.7% |
| A box crossing a still picture, 24 px: the box | 14.8% | 15.7% |
| ... 48 px | 49.7% | 29-47% |
| The car crossing, tilt 16 | 42-48% | 36-46% |
| ... tilt 24 to 32 | 74-84% | 60-82% |

What is left of the dots' "25% off" is their two edge rows: a picture moved by part of a pixel is read between two pixels. With grain of 6 levels on every frame: the dots 0.05 px at a tilt of 16, 0.09 to 0.15 at 32; the face 2.6 to 5.1%.

Not better, and said so: a thing that goes its own way and is small - the car, 58 pixels long - is still the generator's, soft and smeared at speed; and at a tilt of 48 pixels a frame (four fifths of a floor) the face is no better than round 6's.

## What it costs

The search of round 6 ran with every frame in between: every block looked for in the generator's frame at up to 208 places, 81 of them further out whether the answer was needed or not, and round 6's own question to the two real frames on top - 0.80 ms a frame in between at x6 on this scene. Now:

- the two real frames are asked once a pair, not once a frame;
- a block that is theirs altogether is not looked for in the generator's frame (most of a picture a camera moves through);
- "further out" is only looked at when the block was found badly;
- the frame is drawn straight onto the back buffer when that is the picture's own size (a pixel shader of the build pass; `SM_NO_DIRECT=1` for the copy as it was). Read back from the back buffer (`SM_DUMP_DIRECT=1`) the frames are the same as built beside it.

A game that fills the card and does 97 FPS alone, the same hour (game / shown; a frame in between on the card):

| | Round 6 | Now |
| --- | ---: | ---: |
| x2 | 68 / 137, 0.67 ms | 70 / 139, 0.48 ms |
| x4 | 56 / 224 (round 6's run) | 58 / 233, 0.37 ms |
| x6 | 45 / 266, 0.84 ms | 49 / 295, 0.36 ms |

Of a frame in between now: follow 0.09 ms, choose 0.03, build 0.19, nothing for the copy; and once a pair its own movement, 0.22 ms. Step error 0.38 ms at x2, 0.32 at x6 (0.31 and 0.36 before: the same). `suite.ps1`: 21 cases clean.

Against the report's 42 here and 48 there: the stand-in went from 45 to 49 at x6. GTA V itself was not run.

## What is left

- A second movement a block: the car's own, beside the picture's. The two real frames would then agree about the car too (moved by its own movement), and it would be built from them, sharp, like the rest. This is the next step for everything that moves in front of a moving picture.
- Nothing here was seen in GTA V or on another RTX family.
