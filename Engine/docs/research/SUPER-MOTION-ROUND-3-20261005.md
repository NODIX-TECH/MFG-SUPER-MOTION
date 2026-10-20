# DLSS SUPER MOTION, round 3: what it costs the game, an even pace

5 October 2026. RTX 3060 (SM86, compatibility backend 0.3.5), 2560x1440 at 200 Hz, SDR, hardware-accelerated GPU scheduling on. Nothing here was run on RTX 20, 40 or 50, on an HDR screen, with VRR reaching the screen, or in a real game: the game is `tests\game.exe`, a D3D11 program that fills the card and sends each frame in twelve pieces.

## The report

After round 2 was tried in a demanding 3D game:

1. Motion and quality are better, but Flow lowers the picture's quality - in Lossless Scaling it does not.
2. The loss of base rate is the biggest problem: Lossless Scaling leaves 88 of 100 FPS and shows 176; this leaves 62 and shows 124.
3. The motion is still not smooth enough.
4. The limit to the screen's rate has to be a choice.
5. The app's edges and buttons; the motion button has to change colour while it runs.

`measure.ps1 -Load 1040 -Options 'multiplier=2','flow=100'` gives the same figure as the report: the game alone 100, under the engine 61, shown 123. Every number below is from that game.

## Where the time went

At x2 with frames in between at the picture's size, a pair cost the game 6.4 ms: 3.7 ms of it the generator (one frame at 2560x1440).

With the generator out of the way - DLSS MFG off, the engine only capturing and showing - the game still went from 100 to 81: 2.35 ms a frame for 0.44 ms of work on the card. Stage by stage (each switched off in turn):

| Stage | Costs the game | Work on the card |
| --- | ---: | ---: |
| the output window over the game, the desktop duplicated, frames let go untouched | 0.31 ms | the compositor's |
| the frame copied to the shared texture (the capture's D3D11 device) | 0.33 ms | 0.08 ms |
| converted and its small luma made (the generator's D3D12 device, two lists) | 0.99 ms | 0.28 ms |
| drawn into the back buffer and presented | 0.72 ms | 0.08 ms |

Small pieces of work sent by themselves, each waited for (a 57 KB copy):

| Pieces a frame | 1 | 2 | 4 | 8 | 4, sent together |
| --- | ---: | ---: | ---: | ---: | ---: |
| Costs the game | 0.0 ms | 0.49 ms | 1.33 ms | 2.12 ms | 0.62 ms |

A piece of work sent to the card by itself takes the card from the game and hands it back: about 0.3 ms of the game's time each, whatever it holds. (The first one cost nothing: it followed the capture's copy directly.) The card's own usage counters do not show it; they said 77% in every configuration.

Drawing a frame without presenting it cost 0.2 to 0.4 ms, presenting it 0.3 to 0.45 ms more: the compositor draws each presented frame. That part stays - an output window that is not layered was not cheaper (it is composed either way while the desktop is duplicated).

## What was done

### Frames in between made small and rebuilt (the default: Flow 35%)

The generator's first frame in between, by the size it is made at: 640x360 1.29 ms, 768x432 1.72, **896x504 1.02**, 972x546 1.19, 1024x576 1.23, 1280x720 1.74, 1920x1080 2.34, 2560x1440 3.67. Under 504 lines it costs more again, so that is the floor.

Scaled back up, a small frame is soft - "Flow lowers the quality". So it is rebuilt (`sm_rebuild.inl`): the small frame says where everything is at that moment; the detail is taken from the two real frames around it, at the picture's size, block by block where each piece lies in them. Share of pixels more than 12 of 255 away from the frame that should be there (`tests\jelly.ps1`):

| Scene | Rebuilt from 896x504 | The generator's own at 2560x1440 |
| --- | ---: | ---: |
| pan, 16 pixels a frame | 0.8-1.4% | 2.1% |
| pan, 32 | 3.5-4.0% | 3.2% |
| pan, 48 | 3.8% | 8.0% |
| pan, 64 | 3.5-3.9% | 15.8% |
| pan, 96 | 12-14.5% | 59% |
| a box crossing a still picture, 24 pixels a frame | 13-14% | 12.8% |
| the same, 48 | 29-32% | 60% |

The generator's own full-size frames bend straight lines once the picture moves more than a few dozen pixels (the "jelly"); rebuilt ones take their lines from the real frames. The fast-motion guard's limit went from 2.8% to 4.2% of the picture's width a frame with it.

The rebuild itself was 0.90 ms a frame at first. Most of it was the block search: 920 blocks, each tried by one thread, some 5,000 to 10,000 samples one after another - half a millisecond with the card nearly idle. Sixty-four threads a block, each trying one place (groupshared results, six steps): 0.2 ms. Which of the four blocks around it a pixel goes by is now chosen on the small frame, not at the picture's size. A rebuilt frame costs 0.50 ms, drawn and stamped 0.59.

### Fewer pieces of work

- The small luma is made on the capture's device in the piece of work that copies the frame, and read there (`Luma11`). The loop needs it before it knows whether the frame is a new one and how far the picture moved; made on the generator's device it was a piece by itself, and so was the conversion before it.
- The conversion is left in the open command list (`CmdCarry`); the generator's work goes on in it and the two are sent as one.
- The pair is handed to the presenter when its work is sent, with the mark that work is done at (`FgSlot::mark`): the presenter's list waits for the mark on the card, and the first frame in between is drawn right behind the generator. "Drawing takes" went from 1.95 ms to 0.76 ms for it, and a frame reaches the screen about 1.5 ms sooner.

A captured pair at x2 was six pieces (copy; convert; luma; generator; frame in between; real frame); it is three (copy with luma; convert, generator and frame in between; real frame).

### The pacing delay was set anew with every pair

`FgPace::Took` drops the first pair's need once six are known and sets the delay from the rest. The count went back to five there and came to six again with the next pair - so it did that every pair: the delay was the largest of the last five needs, moved by a millisecond or four with every hiccup. Fixed (once per start). Then two things the wrong behaviour had hidden: a delay left far too high by the first pairs of a run came down only a twentieth of a millisecond a pair - it now comes down a twelfth of the way each pair while it is more than 4 ms over; and one that holds more pairs than the four there is room for left every second frame out (base 32 of 63, seen once at x4 without the screen limit) - a frame that finds no room now has the presenter learn its delay anew.

Step error (`tests\pace.ps1`: screen time against the game's own time, from one shown frame to the next), x2:

| The game's frames | Before | After |
| --- | ---: | ---: |
| even: average / 95th of 100 | 0.37 / 1.88 ms | 0.27-0.33 / 1.40-1.93 ms |
| uneven by 40%: average / 95th | 1.56 / 4.04 ms | 0.64-0.67 / 3.55-3.66 ms |
| uneven: shown frames more than a quarter off their spacing | 30% | 13-15% |

**Corrected in round 4:** the uneven rows do not show what they seem to. Run again, the build before the fix gave 1.61 and 1.37 ms, the one after it 0.57, 1.42 and 1.42: the test falls into one of two states from run to run, whatever the build (its game draws frames of random weight while its world moves on evenly, so the times of its frames wander off any even beat and the steady clock, `FgClock`, holds them or follows them as the run happens to go). On even frames the step error is 0.3 ms before and after. The fix stands on what it fixes - a delay recomputed from five pairs with every pair - not on these two rows.

## Where it stands

The game alone at 100 FPS (and at 60, 193):

| Setting | Game under the engine | Shown | Desktop to screen |
| --- | ---: | ---: | ---: |
| x2, Flow 100 (round 2) | 61 | 123 | 24 ms |
| x2 | 73 | 147 | 20-21 ms |
| x3 | 65 | 194 | 28 ms |
| x4 (the screen is the limit) | 64 | 195 | 26 ms |
| x4, no screen limit | 58 | 230 | 26 ms |
| x2, WGC | 74 | 147 | 19 ms |
| the game at 60: x2 / x3 / x2 Flow 100 | 48 / 45 / 43 | 97 / 134 / 87 | 25 / 31 / 29 ms |
| the game at 193: x2 / x2 no screen limit | 112 / 110 | 193 / 219 | 17 / 16 ms |

A pair at x2 costs the game 3.7 ms: the compositor's copy of the desktop for the duplication 0.3; the capture's copy and luma 0.35; conversion, scaling and the generator 1.3 with their piece's 0.3; the frame in between rebuilt and drawn 0.6; the real frame drawn 0.4; the compositor drawing the two presented frames 0.7.

## What is left

Lossless Scaling's 88 of 100 is 1.4 ms a pair for everything. The generator alone is 1.0 ms at its cheapest size on this card, and the compositor's share (the duplication and two presented frames) is 1.0 ms that any program of this kind pays. What could still come off:

- the real frame's drawing riding with the next frame's capture (0.3 ms): the presenter would hold it back until the capture begins or its time is near;
- the generator's work sent before the luma is read, and taken back when the frame turns out not to be a new one (0.3 ms);
- the search's steps for the frame before split over more threads (0.1 ms).

About 75 to 78 of 100 at x2 on an RTX 3060 is where this design ends. RTX 40 and 50 run NVIDIA's own runtime, tuned for them; not measured.

Not done: the Upscaling card is still not connected. "The resolution should follow upscale" was read as "Flow must not lower what is seen" - which the rebuild does; whether the card should scale the game's picture is for the user to say.
