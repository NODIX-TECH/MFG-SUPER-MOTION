# Round 20 - the learning hears keys and pads, learns the read-outs, walks back; what was not solved

2026-10-12, afternoon. The user's sessions before any test: `tests\dump\user-logs-r20` (GTA V and another game, "Discovery", all three generators at x6).

## The report

1. Quality, all three generators: trees, poles and thin things are cut up again; Lossless Scaling does not have it.
2. DLSS MFG "catastrophic": 400 frames a second at x6 where Lossless Scaling and MSM FG reach 460; its stutter shows.
3. MSM FG has some trouble with the HUD.
4. Adaptive Motion Learning: 2D games without cars; going backward (there is only forward); every scenario that helps; the keyboard, the mouse and the pad with all their buttons, not the mouse alone; the HUD learnt - time for it, and the player asked to change weapons or what he carries; and a word on the screen that performance is lower while it learns and better when it is done.

## 2 - DLSS MFG's 400 against 460: measured, not changed

Their own logs, the same game ("Discovery"), one after the other:

| Generator | Real frames a second the game keeps | Shown |
| --- | --- | --- |
| DLSS MFG x6 | 67 to 68 | 400 |
| MSM FG x6 | 80 | 478 |

Nothing is lost on the way to the screen (1571 of 1595 frames in between shown; nine late). The game itself runs slower under DLSS MFG: its network takes more of the card than MSM FG's passes do, and the game gets what is left. That is the generator's own price on an RTX 3060, already cut as far as round 8 could (two of the five frames of a pair are the network's, three are rebuilt; frames made at 896x504). Fewer of the network's frames would give some of it back and cost quality where the report already complains. Not done; said to the user.

## 1 - thin things: not solved this round

MSM FG: the two ways of drawing a frame (round 19's, round 18's) give the same thin things on the test street (`nearer_from/to`: lamp posts 12.0/10.6/10.2 against 12.3/10.7/10.4% off at `drive 60`, five frames a pair) - round 19 did not bring it back. What is left is what `docs\MSM-FG.md` says of round 19: a thin thing in front of something that repeats.

DLSS MFG and XeSS MFG: with "Game type" on Auto their frames are made at 35% of the picture (896x504) and rebuilt from the real frames by the movement of 40x23 blocks - a pole is thinner than a block. Round 18 measured that making them larger does not mend it (posts 29% off at Flow 50 against 30 to 35), and it would cost the frames the report misses.

What would: MSM FG's own search (448x256 blocks, the real frames' own pixels) used to rebuild DLSS MFG's and XeSS MFG's frames, the network's frame kept for what MSM FG cannot find. That is the rebuild of `sm_rebuild.inl` made anew - a round of its own, and it adds MSM FG's cost to DLSS MFG's. Offered.

## 3, 4 - what was done

All of it in `sm_learn.inl`, and the marks of `msm_fg.h`; `docs\ADAPTIVE-MOTION-LEARNING.md` has how each works.

- **Keys, mouse buttons, a pad's buttons, triggers and left stick** are listened to: which are held. For each, what the picture does while it is held is summed - how far it goes as a whole, how much it grows, a second. Held three seconds in all and the picture going one way steadily: known, and with the camera's hand at rest the generator is told where the picture goes by it (a 2D game scrolled by a key or the stick; a step sideways). Kept with the game as sums; never an order of keys, never text. `learntruth.exe study=1`: the keys found to the percent (20% of the width a second), 61 of 105 pairs told.
- **The read-outs (HUD) are learnt** for each game: on the small picture, while the camera turns (and only then: going forward, the picture's middle stands and is no read-out - the first try took half the street for one), a pixel is counted as staying or as going with the picture. Kept in `<game>.hud`. MSM FG's marks start from it: a pixel in a learnt cell is a mark from the first pair on, and a change no longer takes its count away - a map that turns, a number that counts. On the stand-in the map is the HUD panel and nothing else (1100 to 1450 cells of 57600); the frames are as good as without it (pan: 0.5% off, the read-outs 0.0%). A HUD that changes or is seen through is not among the test scenes: the user's games are the test.
- **The study**: walk backward (the picture shrinks) after walk forward; a last step for every kind of game, "Keep moving. Switch weapons or items a few times." (five seconds of a moving picture, so that every read-out has been on the screen while it moved); 2D games: move left and right, then that step - no camera, no car. Every third four seconds the first line says "Learning costs a little performance. It gets better when done."
- The app: three more lines (`aml_say9` .. `11`) in English, Arabic, French; Help's entry says what is listened to; "Delete" removes the read-outs' file too.

## Tested

`learntruth.exe study=0|1|3` (every step in turn), the study in the engine on `scene.exe study`, `jelly.ps1` pan with and without the learnt read-outs, `xess-smoke.ps1`, `suite.ps1` with each generator.

## Left open

- Thin things under DLSS MFG and XeSS MFG: the rebuild by MSM FG's search (above). Under MSM FG: a thing in front kept together as one thing.
- DLSS MFG's cost: one network frame a pair at x5/x6 as an option, if wanted.
- The learnt read-outs under DLSS MFG and XeSS MFG (theirs are told pair by pair, `lf_fg_still.inl`).
- GTA V by the user's eye: the study there gave a mouse that explains only 60% of the picture's movement, and "walking" of 57% a second - both to be looked at in their next log.
