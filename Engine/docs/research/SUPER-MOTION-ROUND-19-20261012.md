# Round 19 - the engine that ended by itself, the study's steps, Support and Help, the new names

2026-10-12, the same day as round 18. RTX 3060 + Intel UHD 730, 2560x1440 at 200 Hz. Tests run straight away, as the user asked in round 18.

From this round on the app is **MFG SUPER MOTION** (it was Octo Super Motion) and its own generator **MSM FG** (it was OSM FG). The notes of the rounds before keep the names of their day.

## The report

1. Adaptive Motion Learning: the study only ever asked to turn left and right, and was over. It should ask to walk forward, to drive a car where there is one and to stand still where there is none, go by the kind of game picked, say it in plainer sentences - and up and down never showed at all.
2. OSM FG: very good in both searches; the fault is not gone entirely, "still noticeable a little".
3. DLSS MFG "lost ten frames for no reason I know; it feels as if it does not benefit from Keep still pixels".
4. An old fault is back: the app runs for a minute or more and stops by itself. It used to happen with THE FINALS; now with many programs.
5. The "Direct" page becomes "Support the app": one large button to the Patreon page, and under it a few thankful lines.
6. The app is renamed MFG SUPER MOTION, OSM becomes MSM FG, all through the project.
7. Help: every technique explained, how to record the screen with OBS, every link one needs.

Their sessions before any test: `tests\dump\user-logs-r19`.

## 4 - the engine that ended by itself

Their log (Chrome as the source, window capture, OSM FG x6, Spout and the FPS counter on, Adaptive Motion Learning on), 55 seconds in:

```
[idle] nothing new on screen - the generator rests until something moves
[dlssg] displayed 0.0 FPS (0 real + 0 generated in 1.06s)
[stall] the presenter stood 579 ms in letting a pair's pictures go (waiting for the card)
[dlssg] unavailable: GPU work timed out; stopping this worker safely
```

The card was in perfect order. `AwaitMark` (`lf_gpu_core.inl`) - "wait until the card has done this much" - used one event for every thread that waits, an event that resets itself when one waiter wakes. The loop's thread and the presenter's wait at the same time, each for its own work; the one wake-up went to whichever woke first. The loop took the presenter's, found its own work not done and waited on; the presenter's was gone. While the loop came back to wait again soon, the next wake-up let the presenter go, a frame late (the `[stall]` line). When the picture stood - a page not scrolled, a menu, a loading screen - the loop sent the card nothing more, no wake-up came, and after two seconds the presenter gave the card up and ended the engine. More waits on the presenter (Spout, round 16; the counter and the banner stamped on every frame) made it likelier; so did more programs whose picture stands.

Now every thread that waits has an event of its own. And the presenter's two waits no longer end the engine over a card that is busy: two seconds, then eight more with a line in the log, then on without (`FgAwaitPatient`, `lf_dlssg.inl`).

`tests\still.ps1`: `scene.exe` moving for 0.7 s and standing for 2.6, over and over, the user's settings of that session.

| Engine | Runs | Ended by itself |
| --- | --- | --- |
| round 18's (as it was deployed) | 4 of two minutes | 2 - after 32 s and after 18 s, "GPU work timed out", exit code 1460 |
| the new one, MSM FG | 4 (150, 110, 120, 120 s) | none; no `[stall]` line |
| the new one, DLSS MFG | 2 (110, 100 s) | none |
| the new one, XeSS MFG | 2 (110, 100 s) | none |

Some 290 spells of a standing picture in the new engine's eight runs.

## 1 - the study

What was wrong: the three counters of round 18 all counted at once. In the user's GTA V the camera swung about filled "left and right", "up and down" and "walk or drive" together: fifteen seconds, one line seen.

Now: steps by the kind of game, each counting only what it asks for, in seconds of it; walking and driving told by the picture growing about its middle - the blocks' count for a drive, the small picture seen finely for a walk, which the blocks' steps of 8 pixels do not show; a drive told from a walk by its pace; no car: the picture stands for five seconds. The sentences are short, and say which step of how many. All of it: `docs\ADAPTIVE-MOTION-LEARNING.md`.

On the way: the first try counted "blocks streaming outwards" and found a walk on the test street in one pair of five (it is under half a block's step); and a picture that stands sends no frames, so "it has stood for so many pairs" never came true - it goes by the clock.

Not seen by me: GTA V. The walk's and the drive's measure are made on a drawn street; a game's own grain, a camera that bobs, a car seen from behind in the middle of the picture are the user's to see. A step that is not recognised is passed over after twenty seconds, and says so in the log (`[aml] the study's step ...`).

## 2 - what is left of the fault

`docs\MSM-FG.md`, "What round 19 changed". Two changes that help what is shown - a frame drawn from both real frames all the way; a fast turn by hand no longer taken for a scene cut - and an honest "not this round" for what is left on the test street at high speed: blots of a repeating wall beside a thin thing rushing past, bites out of a lane's dash at the picture's bottom. None of the generator's numbers moves it.

## 3 - DLSS MFG's ten frames

Nothing in the engine. `tests\dump\r21-dlss.sh`: DLSS MFG x6 on a game that fills the card (112 to 119 frames a second alone), the user's settings (Spout, counter, DXGI), the builds of round 17, round 18 and this one, twice each one after the other:

| Build | Game kept / shown, first round | second round |
| --- | --- | --- |
| round 17 | 56 / 337 | 54 / 327 |
| round 18 | 56 / 333 | 54 / 324 |
| round 19 | 54 / 323 | 54 / 323 |

The first three runs had a faster game to begin with (118, 119, 114 alone against 112 to 113 later): the card warm. With Adaptive Motion Learning on: 54 / 325 and 55 / 328. With "Keep still pixels" off: 54 / 326 and 54 / 325 - it costs nothing that shows, and it is at work in the user's own log ("still pixels: ... frames in between kept their still parts").

Their own two sessions: 32.3 real frames a second on round 17's build, 30.6 on round 18's - another stretch of road, and the camera swung about for the study (fast turns taken for cuts: pairs without frames in between). 1.7 real frames a second times six is the ten frames of the counter.

## 5, 7 - Support the app, Help

`MainWindow.axaml`, `MainViewModel.fs` (`OpenLinkCommand`: a fixed list of links, opened in the user's browser), `UiText.fs`, `en.json`, `ar.json`, `fr.json` (the other languages show English for what is new, and lost the three strings of the page that is gone). The user's two paragraphs of thanks were put right and made warmer; the Help page has: getting started, the three generators, each option under its own name, recording with OBS through Spout step by step, what to try when something is wrong (and where the log is), the keys, and the links - OBS Studio, the OBS Spout2 plugin, Spout, the three makers' drivers, Patreon. Seen in the app's own window in English and Arabic (`tests\dump\ui-pages.ps1`, `tests\dump\ui19`).

OBS itself was not run here: the Spout sender is checked by `tests\spoutcheck.exe` (a receiver of the test's own), as in round 16.

## 6 - the names

`scratchpad\rename19.pl`: 437 places in 41 files, twelve files moved (`msm_fg.h`, `docs\MSM-FG.md`, `tests\msmtruth.cpp`, ...). The app's program is `MfgSuperMotion.exe`. Kept as they were, so that nothing is lost: the folders and the solution (ARMO), the namespace, `%LOCALAPPDATA%\ARMO` (settings, the learning's cache, the logs). A settings file saved under the old names is read as it is (`"OsmSearch"` is taken for `"MsmSearch"`); a start-with-Windows entry "Octo Super Motion" is taken over by the new name.

## Tested

`tests\dump\r22-batch.sh`, on the renamed engine built beside the real one: the learning by itself (learnt after 8 s of play, 97 to 99% of the pairs told); what MSM FG costs a game (63 of 121 kept at x6 against round 18's 64; a pair 3.6 ms, "Fast" 3.2) and its frames in the engine (the box 3.8 to 4.0% off, the pan 0.5 to 0.6%, the read-outs 0.0%); the study with a car and - in Arabic - without one; the picture that moves and stands; the app's lists after each pick and its settings file read from the old names; every option live (`xess-smoke.ps1`); the suite with each generator.

## Left open

- GTA V by the user's eye: the study's walk and drive, MSM FG after this round.
- MSM FG: a thing in front kept together as one thing (what is left beside a thin thing rushing past).
- DLSS MFG's thin things (round 18): MSM FG's found movers laid over its frame - offered, not done.
- The learnt pace of a walk and a drive is kept and not used yet.
- Late warp; a generator for a weak second card: as before.
