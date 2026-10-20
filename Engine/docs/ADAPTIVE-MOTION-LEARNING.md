# Adaptive Motion Learning

The idea is the user's own: "Input-Conditioned Adaptive Predictive MFG" (`ARMO\Adaptive_Predictive_MFG_Concept_Paper_v4_Illustrated.pdf`). Taken from the paper is the idea - learn, for each game, what the player's hand does to the picture, keep it, and use it before the next real frame says so. How frames in between are made stays what it is (`msm_fg.h`, DLSS MFG, XeSS MFG).

In the app: the switch under "Game type" (off unless turned on), and in Settings the list of what is kept - one line a game, with "Delete" to have that game learnt from the start. In the engine: `aml=1`, all of it in `sm_learn.inl`.

## What is learnt

A camera turned by the mouse, or by a pad's right stick, moves the whole picture. Two numbers say how, and both are the game's own:

- **the gain** - how far the picture goes for one count of the mouse (the game's sensitivity and field of view), across and down;
- **the delay** - how late the picture follows the hand (the game reads the mouse, draws, and the frame is seen some tens of milliseconds on).

Both show in what the engine already has. It counts how far the picture moved from one real frame to the next for every pair, twenty-four blocks of it (`sm_guard.inl`). `sm_learn.inl` adds how far the hand moved meanwhile: the mouse's own counts (raw input) and a pad's right stick (XInput), summed a millisecond at a time. For every pair with the hand moving, the picture's movement is set against the hand's under fourteen delays, 0 to 130 ms; the delay under which the picture follows the hand best is the game's, the slope under it its gain. Older pairs count less and less (a sensitivity changed in the game's menu is learnt anew by itself), and a quick factor on top follows what changes for a moment - aiming down a sight halves the gain.

Nothing but those sums is kept: never where the pointer is, never a key or a button. They are written to `%LOCALAPPDATA%\MFG SUPER MOTION\Learned\<the game's program>.aml` (text), every twenty seconds while they change and when the engine stops, and taken up again the next time that game is played.

It counts as learnt after some 200 pairs with the hand moving in which the hand explains at least 45% of the picture's movement - seven seconds of looking around at 30 frames a second.

## The study, in front of the player (round 18; its steps round 19)

Round 17 learnt in silence, and the app said how far along it was beside its switch. The user asked for it on the screen, and for it to say what helps: so a game not studied yet is studied in front of the player. Two lines at the top of the frame, in the middle, drawn by the engine itself as its FPS counter is (`sm_banner.inl`: no window over the output):

```
Adaptive Motion Learning · 43% · about 15 s
2/4  ·  Now move the camera up and down.
```

Round 18 had three lines that all counted at once - turning the camera filled all three, and the player saw "left and right" and then "done". Since round 19 the steps go by the kind of game picked in the app ("Game type"), each counts only what it asks for, and each says which one of how many it is:

| Game type | The steps |
| --- | --- |
| Auto, Third person, Open world, Custom | camera left and right; camera up and down; walk forward; drive a car - or, without one, stand still |
| First person | camera left and right; camera up and down; walk forward |
| Racing | drive straight ahead; take some turns |
| 2D, 2.5D | move left and right |

What counts for a step (`LearnStudyPair`), in seconds of it - 3.5 for the camera across, 2 for up and down, 2.5 for the walk, 3.5 for the drive - not in frames, so a game at 120 frames a second is studied as long as one at 30:

- **camera left and right / up and down**: the hand went that way, at least half as far again as the other way, and the picture as a whole went with it;
- **walk, drive**: the picture grows about its middle. The blocks' count of the picture's movement goes in steps of 8 pixels (of 2560): a drive shows in it (the blocks set against "each goes with the others, and by so much times its place from the middle besides", by least squares), a walk does not - it moves the picture's edge by two or three pixels a frame. So during these steps the small picture itself is asked (320 across, `LearnLooked`): how its brightness changed since the frame before, set against its slopes, says how far it went as a whole and how much it grew - good up to a pixel and a half of it, where the blocks take over. On the test street a walk of 1.8 m a second grows the picture by 5% a second and a drive of 9 m a second by 26%;
- **drive** is a walk at least 1.4 times as fast as the walk before it was (a player walking to his car is not through with the car before he reaches it). **No car**: the picture has not moved for five seconds - by the clock, for a picture that stands may send no frames at all; a passer-by is not the picture moving;
- **take turns**: the picture goes sideways as a whole; **move left and right** (2D): it moves at all.

A step is shown for three seconds at least and waited for twenty at most: a player who does something else is not kept waiting, and the learning goes on without the lines. Followed, the four steps take half a minute. Then "Adaptive Motion Learning · done" for four seconds, and the lines do not come back for that game ("Delete" in the app's Settings starts it over). The lines are in the app's language: the app hands them to the engine (`aml_say0` .. `aml_say8`; English, Arabic and French are written, Arabic set from the right).

The game's file keeps, beside the hand's two numbers, how fast its picture grew walking and driving (`walk=`, `drive=`, a share of itself a second). Nothing uses them yet but the study itself.

Seen (`tests\study.ps1`: `scene.exe study` as the game - a camera turned by the mouse, then a street walked along, then driven along or stood in - a hand of the test's own, proofs of the frame read back from the back buffer): with a car the four steps in 6, 8, 11 and 12 seconds; without one the last step over after the picture had stood ("no car: the player stands"); the lines on the frame in English and in Arabic. And against a player of the tool's own, without the engine (`tests\learntruth.exe study=<kind>`): every kind of game; a player who only ever turns the camera is let go after each step's twenty seconds.

**A turn is not a cut.** While the hand turns the camera fast, a picture that changed much is that turn and not another scene: the pair keeps its frames in between (MSM FG; `docs\MSM-FG.md`, round 19).

## Round 20: keys and pads, the read-outs, walking back

**More steps.** After "walk forward": "Now walk backward." (the picture shrinks about its middle - the same measure, the other sign). Last, for every kind of game: "Keep moving. Switch weapons or items a few times." - five seconds of a picture that moves as a whole, so that every read-out has been on the screen while it moved. 2D and 2.5D: "Move left and right.", then that step; no camera, no car. Every third four seconds the first line reads "Learning costs a little performance. It gets better when done." The lines are `aml_say0` .. `aml_say11` (9 the notice, 10 walking back, 11 the read-outs).

**What is held.** The keys, the mouse's buttons, a pad's buttons, triggers and left stick: which are held (raw input, XInput) - never their order, never what was typed. For each, with the camera's hand at rest, what the picture does while it is held is summed: how far it goes as a whole and how much it grows, a second (`LearnState::Control`; older play counts less). Held three seconds in all, and the picture going one way under it steadily - twice what it wavers by, a fiftieth of its width a second - it is known (`LearnControlKnown`), and the generator is told where the picture goes by it when the mouse and the right stick rest (`LearnHeldExpect`): a 2D game scrolled by a key or the left stick, a step sideways. Kept in the game's file as `control.<number>=` and seven sums.

**The read-outs (HUD).** On the small picture (320 across), for every pair in which the camera turned - the picture went as a whole by two of its pixels or more, seven blocks in ten the one way, and neither grew nor shrank - each pixel is either what its place was before the turn (the world) or what it was itself (a read-out, a crosshair, the player's own car): whichever it is nearer to is counted for it, when the two differ enough to tell (`LearnSeen`). A see-through panel's inside goes with the world behind it, which is what it shows; its letters and its frame stay. Counted well up, and the pixels beside it: a cell of the read-outs (`LearnHudCells`; a quarter of the picture "a read-out" is taken for a mistake and not used). Kept in `<game>.hud` beside the game's file; "Delete" in the app removes both.

MSM FG's marks start from the cells (`MsmHud`, `MsmHudSeed`, `MarkMain`): a pixel in one is a mark from the first pair on, and a change no longer takes its count away - a map that turns, a number that counts stay marks, shown as the real frame has them. What goes with its place inside a cell is still counted down and drawn moving again. DLSS MFG and XeSS MFG tell their still pixels pair by pair (`lf_fg_still.inl`) and do not use the cells yet.

Seen: `learntruth.exe study=0|1|3` (every step in its turn; a 2D game's two keys found to the percent, 61 of 105 pairs told where the picture went); in the engine on `scene.exe study` the cells are the scene's read-out panel and nothing else, also through the walk and the drive; `jelly.ps1` pan with the cells in use: the moving picture 0.5% off, the read-outs 0.0%, as without. Not among the test scenes: a read-out that changes or is seen through.

## When it is trusted

Only once learnt, and only while the last few expectations were near what the picture then did:

- a menu, a map - the mouse moves a pointer and the picture stands: the trust is gone within five pairs, and such pairs are not learnt from (an evening's menus would wear the gain down to nothing);
- a cutscene, a car driven with the keys - the picture moves and the hand rests: nothing is expected, nothing is learnt;
- both hands at once (mouse and stick) cannot be told apart: not learnt from.

## What it is used for

**Where the generator looks.** MSM FG finds a piece of the picture within some 7% of the picture's width of where it was (four texels at the smallest of its five sizes, and one more at each size above). A camera turned fast at 30 frames a second goes further than that between two frames, and such a pair comes out torn. Told where the picture as a whole went - the hand's movement under the game's delay, times its gain - it looks around that place too (`MsmAsk::expected_x`, `SearchMain`). It is an added place to look, never the only one: an expectation that is wrong costs nothing.

On a photograph moved a known way (`tests\msmtruth.exe photo <pixels a frame> [ex=<what it is told>]`, 2560x1440, share of pixels off / mean error):

| The picture moves, pixels a frame | not told | told |
| --- | --- | --- |
| 150 | 0.87% / 0.92 | 0.60% / 0.52 |
| 260 | 8.5% / 5.02 | 1.6% / 1.01 |
| 400 | 19.2% / 8.49 | 5.0% / 2.88 |
| 400, told 330 (a sixth off) | | 5.8% / 3.36 |
| 400 across and 120 down | 20.5% / 9.80 | 8.4% / 4.73 |

260 pixels a frame at 30 frames a second is a turn of a quarter of the screen in a tenth of a second: an ordinary look around in a shooter.

DLSS MFG and XeSS MFG find the motion by themselves and are not told (they take no such word); with them the switch only learns.

## What the learning itself does (`tests\learntruth.exe`)

A game played at it whose delay and gain are known - the blocks' count as coarse as the engine's (whole pixels of a picture 320 across, a quarter of the blocks a sky, one in thirty wrong):

| The game | learnt after | delay found | gain found | told, of the pairs with the hand moving | off by |
| --- | --- | --- | --- | --- | --- |
| 30 FPS, 47 ms, 0.9 px a count | 20 s of play | 50 ms | within 2% | 97 to 99% once learnt | 3 to 5 px where the picture moved 80 to 125 |
| 60 FPS, 28 ms, 0.4 px a count | 13 s | 30 ms | within 2% | 96 to 98% | 1 to 3 px of 17 to 28 |
| 30 FPS, 80 ms, a stick | 29 s | 80 ms | within 6% | 95 to 99% | 1 to 2 px of 15 to 25 |

In its menu (the hand moves, the picture stands) 5% of the pairs were still told something - the first few before the trust is gone; back in play 97% again. Aiming down a sight (half the gain): 85 to 96% told, off by 3 to 6 pixels. What is kept and read again is the same to the last digit.

## In the engine itself (`tests\learn.ps1`)

`scene.exe look`: a picture turned by the mouse with a gain and a delay of its own, `learntruth.exe hand=` at the mouse (a hand of the test's own), the engine with `aml=1` and MSM FG. What the engine's log said it learnt:

| The scene | learnt | told, of the pairs with the hand moving |
| --- | --- | --- |
| 0.9 px a count (0.035% of the width), 50 ms, 30 FPS | 0.034%, 80 ms, the hand explains 94% | 363 of 789 (the first 424 are the learning) |
| 1.4 px a count (0.055%), 80 ms, 30 FPS | 0.053%, 100 to 110 ms, 94% | 366 of 785 |
| 0.4 px a count (0.016%), 30 ms, 60 FPS | 0.016%, 30 to 50 ms, 87% | 758 of 1384 |

The delay found is the scene's own and some 30 ms more: what lies between the scene drawing a frame and the engine getting it. That is the delay that matters to the engine - the hand's movement is set against the frames as they arrive.

Slow movement is learnt slowly: the engine's count of the picture's movement goes in steps of 8 pixels (of 2560), so a picture that moves less than that a frame says little. Fast turns - what the learning is for - are counted well.

## Not done with it: moving the shown frames ("late warp")

The paper's other use is to move each shown frame by what the hand did since the game drew it, so that a turn is seen before the next real frame brings it. It is left out of this round, and that is a decision, not an oversight:

- a frame moved sideways has nothing to show along one edge - at a turn of 1000 pixels a second and 40 ms of delay a strip of 40 pixels - and something has to be drawn there;
- the read-outs must stay where they are while the picture under them moves: known pixel by pixel with MSM FG only (its marks), and what lies under a read-out is not known at all;
- an expectation that is a little wrong is harmless to a search and visible as a tremble in every shown frame;
- and none of it can be judged without a real game with a mouse-driven camera in front of a person: the tests here have a stand-in.

Everything it needs is in place - the gain, the delay, the hand's movement to the millisecond. It is the next step when wanted, as a switch of its own.
