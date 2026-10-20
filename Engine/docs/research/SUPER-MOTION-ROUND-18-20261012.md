# Round 18 - OSM FG's squares and outlines, the study on the screen, DLSS MFG's poles

2026-10-12. RTX 3060 + Intel UHD 730, 2560x1440 at 200 Hz. The user at the PC: "start your tests directly, do not wait for me to stop using the device" - so the tests ran straight away, without the gate of round 17.

## The report

Two pictures from GTA V at x6, about 35 real frames a second, driving.

1. DLSS MFG: the trees at the back and the poles are cut, as if they disappeared in places; XeSS MFG is better there; "a simple thing to improve".
2. OSM FG: squares and a strange glow all over the picture - "we had this at the start with the other generators; it has to be fixed here now".
3. Adaptive Motion Learning: the study should be plain to see at the top of the screen, in the middle - that it is studying, and how long; and smarter: tell the player what to do (stand still and turn the camera, move forward, get into a car and drive while moving the camera), for thirty seconds or what is right.
4. OSM FG to start on "Fast"; its name back to 1.0 until told otherwise (the app is not released).
5. The shortcut: Shift + Alt + M.

Their sessions (`tests\dump\user-logs-r18`): GTA5_Enhanced, the engine on the RTX 3060 beside the game, DXGI capture; OSM FG 36 to 39 real frames a second, DLSS MFG 31 to 34, XeSS MFG 32. Adaptive Motion Learning had learnt the game with the mouse: the picture 70 ms behind the hand, the hand explaining 70% of the picture's movement (2531 pairs).

## 2 - what the picture was

Enlarged (`tests\dump\webppng.ps1`), the second picture shows every lamp post as a thin dark outline of itself around nothing, rings and rounded squares in the sky beside it, and the hillside in blots with outlines. Round 17 had guessed at "pixels held still"; a soft, game-like picture with a car standing on the screen (`osmtruth photo ... blur= car=1`, new) did not show it. The street of the tests did - once a frame of it was looked at (`drive 120`: the near lamp post, hollow) instead of counted. That is the lesson of the round: the share of pixels off was 3% with the post gone.

Three causes, all in `osm_fg.h` (what each change does and what it measures: `docs\OSM-FG.md`):

- a pixel between two places was drawn by their words mixed, also where one place is a post moving two hundred pixels and the other a sky moving none: the whole post, squeezed into a line along the border;
- blocks that show little (a soft road: 33 pixels off on average on the test street) were drawn by words that mean nothing;
- a thin thing rushing past was never found at all.

And round 17's marks, which the same picture would have suffered from in time (a pixel the same four pairs running is common in a soft picture), are told another way.

On the test street the read-outs are now 0.0 to 0.7% off (2.5% at worst before), the whole picture of `drive 120` 2.6% (3.0), and the frames look right where they were hollow. It costs: a pair at x6 3.4 ms on the RTX 3060 (2.85), the game at 62 frames a second instead of 65 of 115; "Fast" 3.1 ms and 64.

Not seen by me: GTA V itself. The user's eye decides.

## 1 - DLSS MFG's poles: not changed, and why

On the test street DLSS MFG shows what the user's picture shows (`jelly.ps1 -Mode drive -Step 120 -Keep`, a frame looked at): a palm and the post beside it drawn twice, half through each other; a sign with a faded copy of itself; fronds gone soft. A third of the posts' pixels off (30 to 35%), four in ten of the thin poles' (35 to 42%).

It is the generator's own frame. DLSS MFG is given two finished frames, no depth and no motion of the game's (a game hands it both; a window's picture has neither), and what it does with a thin thing that moves far is its own. What the engine can still turn was turned and measured in earlier rounds and again now:

- its frames made larger ("Flow" 50 instead of 35): posts 29%, thin poles 33% - within what one pair differs from the next;
- "Keep still pixels" off: no better (39%, 44% on another pair);
- a motion field handed to it (round 13): it goes by its own.

So it is not a simple thing. What would help is not inside DLSS MFG: OSM FG now finds those posts and draws them from the real frames, and its word for "a thing of its own, found in both frames" could be laid over DLSS MFG's frame where DLSS MFG lost the thing. That is a piece of work in the part of the engine that is not to be broken (the presenter's rebuilding of DLSS MFG's small frames, `sm_rebuild.inl`), with a cost of some 3 ms a pair on top - offered, not done in this round.

## 3 - the study on the screen

`sm_banner.inl` (two lines at the top of the frame, drawn as the FPS counter is) and the study in `sm_learn.inl`: three things asked for one after the other, each until its pairs are in or 25 seconds have passed, half a minute when followed; the lines in the app's language. `docs\ADAPTIVE-MOTION-LEARNING.md` has it. Seen on the frame itself (`tests\study.ps1`), in English and in Arabic.

A game learnt in round 17 - the user's GTA V - is taken through the study once.

## 4, 5

"Fast" is what OSM FG starts on, also in a profile saved before (schema 14); the name is "OSM FG 1.0" again in the app, the log and the documents. The shortcut's own default is Shift + Alt + M (a shortcut the user picked himself stays).

## Tested

`tests\dump\r20-batch.sh`, on the engine built beside the real one, the PC in use: what OSM FG costs a game (round 17's beside it), its frames in the engine (a box, a pan), the study with proofs of the frame, the app's lists after six picks, `xess-smoke.ps1` and `suite.ps1` with each generator.

## Left open

- GTA V by the user's eye, OSM FG "Fast" and in full.
- DLSS MFG's thin things: OSM FG's found movers laid over its frame.
- The nearest, fastest lamp post is in pieces; a weak card pays for round 18 (UHD 730: 27 ms a pair with five frames in "Fast").
- Moving the shown frames by the hand's movement (late warp), and a generator for a weak second card: as in round 17.
