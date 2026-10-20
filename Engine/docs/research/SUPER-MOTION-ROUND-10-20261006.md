# DLSS SUPER MOTION, round 10: posts against the sky

6 October 2026, night. RTX 3060 (SM86), 2560x1440 at 200 Hz, SDR. Measured on `tests\scene.exe` and `tests\game.exe`; nothing was run in GTA V.

## The report

On round 9's build: the road's asphalt is "very good" now. The lamp posts are not - "nothing, and worse than before"; palms count as posts (standing, tall, thin). Lossless Scaling does much better there, in speed and in picture.

## A view like the report's

`jelly.ps1 -Mode drive -Step 60|120`: the street seen as from a car, looking down the road, with nothing beside it but sky (a bridge): lamp posts, thin sign poles with a plate (6 cm), palms with fronds (5 cm), all against the sky and the far tower block. Round 9's posts stood before a wall of houses, and that hid everything that follows.

Two pictures of one moment, a sign pole near the car against the sky: on round 8's build whole, its plate whole; on round 9's the pole gone but for a stub at its foot, the plate in pieces. Reproduced.

## What it was

**The second movement (round 9) takes such a pole away.** Rewritten in this round (a place votes only where the frame before shows what the calm picture beside it does not, so the sky cannot be "the thing"; three times the reach along the road's own way; each thing's way laid out block by block) it still does, on this view. Why is not found. It is off unless asked for (`SM_MOVERS=1`).

**The posts' missing middle (rounds 7 to 9) was a rule of ours, not the generator.** A pixel the two real frames agree about was theirs without asking the generator once everything far around was calm too - the rule that puts a dot on a wall back where the generator has it beside its place. But a post rushing past a car is fast enough for its two places in the real frames to lie far to either side: all is calm where it is now, and its middle was painted over with the sky. The foot (the ground beside it is restless) and the top (the lamp's arm, a plate) stayed: "the lower half is gone".

Now (`ChooseMain`): where all is calm far around and the generator's frame shows something else, it is asked whether the two real frames have what the generator shows anywhere near (sixteen places, four and eight small pixels around). They do: a thing of the scene the generator put a little off - they are right. They do not: a thing on its way across - the generator's stays.

**Thin things passed for agreed.** Half the local contrast is allowed for when the two real frames are compared (round 7: a balcony's rail falls differently on the small frame's rows in each). A thin post going its own way passed under it too. The two cases differ in what the nine places compared hold together: the rail is in both frames' nine, only spread differently; the post is in one frame's nine and not in the other's (`Unrest`).

## What came of it

The open driving view, share of pixels more than 12 of 255 off and the mean error (several runs, all pairs; the scene's moments differ from run to run, so only large differences count):

| | Round 8 | Round 9 | Second movement off | + things in transit kept |
| --- | ---: | ---: | ---: | ---: |
| Thin poles and palms, 120 cm a frame | 42-59%, 23-37 | 47%, 27 | 55-57%, 32-37 | 34%, 25 |
| Lamp posts, 120 | 35-40%, 21-28 | 46%, 40 | 40-57%, 24-55 | 55%, 39 |
| Thin poles and palms, 60, x6 | 26%, 19 | 26%, 23 | 28%, 21-23 | 28%, 20 |

In the picture: the near posts stand top to foot again - wavy and soft, as the generator has them, no longer with a piece missing or gone. They are not as good as the rest of the picture.

Kept from round 9: the road (the far search and the pair before), and with it a box crossing a still picture at 6-7% (14-16% in round 8). The rest as it was: read-outs 0.1%, the tower block's face at a tilt of 32 px 8-9%, pan 64 px 1%. The dots: 0.13-0.2 px from their place (0.05-0.1 before): a dot the generator puts more than 23 pixels off now counts as "in transit". `suite.ps1` 21 cases clean. A heavy game: x2 69/138, x6 53/319; the light one at x6 83 pairs, 394 shown.

## Not done

A post as sharp as the picture around it. That needs its own movement found between the two real frames, which is what the second movement was for; on posts before a wall it works (round 9's picture), against the sky it destroys them, and until that is understood it stays off.
