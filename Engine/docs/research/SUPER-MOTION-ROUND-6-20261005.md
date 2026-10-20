# DLSS SUPER MOTION, round 6: what repeats, a scene with depth, the tensor cores

5 October 2026, late. RTX 3060 (SM86, compatibility backend 0.3.5), 2560x1440 at 200 Hz, SDR. The measurements are on `tests\scene.exe` and `tests\game.exe`; nothing was run in GTA V, on RTX 20, 40 or 50.

## The report

A screenshot from GTA V: a tower block across the end of a street, rows of balconies. Moving the camera up and down, the block stays steady in Lossless Scaling; here its face is distorted "as if there were an earthquake". The tests so far are flat: nothing in them moves forward and down at once, as a game's camera does. And: use the tensor cores more for the motion, if that helps performance (three sources given: the two RTX 20/30 compatibility packages and the OptiScaler multipass project).

The sessions' logs (kept in `history\`): x6 with frames in between at 896x504, 44 to 52 pairs a second with five frames each - against Lossless Scaling's 48x6 of round 5's report. The game does give way under the engine after all (round 5 read "46 = every second frame of 92" into what was a card-bound game at 46; the loop not waiting for the generator is right for games that hold their rate, and changed nothing here).

## A street with depth (`tests\world.h`)

`scene.exe tilt <n>` and `scene.exe dolly <n>`: a road, a tower block across its end (balconies every three metres, a mullion every two and a half), a row of houses along the left with windows in a grid, a rock face on the right, sky - drawn by where each pixel's ray meets it, patterns averaged over the pixel's footprint. The camera tilts up and down (the block's face moving n pixels a frame), or drives forward n centimetres a frame while tilting down. Every frame carries its number in its corner; `jelly.exe` reads the two real frames' numbers and draws the scene anew for the moment half way between them - the frame that should be there - and counts each part of the scene by itself.

## The earthquake

Share of the tower block's face more than 12 of 255 off the true frame, x2, frames in between rebuilt from 896x504:

| Tilt, pixels a frame | Before | After |
| ---: | ---: | ---: |
| 8 | 9-22% | 9-15% |
| 16 | 5-9% | 5-9% |
| 32 | 48-62% | 8-15% |
| 48 | - | 9% |

At 32 pixels a frame the balconies (59 pixels apart on the screen) move half a floor: as well explained by half a floor the other way. The generator's own frame has the face one floor off or torn between the two - the same 65 to 70% off with its small frame scaled plainly, so it is the generator's, not the rebuild's - and another way with each pair: the shaking. The road, the houses and the rock in the same frames were right (1 to 6%).

Two things were done.

**The picture's own movement, also where it repeats** (`sm_guard.inl`). The search on the small luma keeps, for each of its 24 blocks, every movement that explains the block nearly as well as the best (up to four). The blocks with only one say how the picture moves - a shift, and a growing or shrinking about its middle, fitted to them, one that goes its own way left out. A block with several takes the one nearest to that; a block with one that is not the picture's takes the picture's when that explains it nearly as well; a block in which nothing was seen to happen (sky) takes it too - left at "none" it pulled its neighbours half way back.

**Built from the real frames where the generator is wrong** (`sm_rebuild.inl`). For every block FollowMain now also asks the two real frames themselves: moved by the picture's movement there - one forward, one back, by the frame's place between them, searched a step or two around and then in quarters - do they show the same thing? When they do (within eight levels a colour) and the generator's block is far from what they show (eleven levels and two and a half times as far), the block is built from them: its base is the two real frames' own picture there, not the generator's, and its detail comes from the same places. Chosen per small pixel among the four blocks around it as before; where the two real frames disagree at a pixel, the generator's stays.

The same repair carries a fast pan further (the flat scene of round 3, share of the moving picture off): 96 pixels a frame 6-7% (was 12-14%), 128 pixels 10% (was 33%). Slow pans, the box crossing a still picture, the still HUD: as they were (pan 32 px 3.4-3.9%, 64 px 3.0-3.7%, the box at 24 px 14-16%).

It costs 0.05 ms a frame in between at x2 (0.61 to 0.66) and 0.09 at x6. `SM_NO_REPAIR=1` switches it off, to compare.

Forward and down at once (`dolly 24`, `dolly 60`): the face 6-9% off, the whole picture 2-4%, before and after.

## The tensor cores

The compatibility package's own log names the generator's kernels. The network is convolutions in half precision (`k_conv_fp16_nhwc`, `k_conv_fp16_nhwc_fused_up`, `conv_dl2_*`, `custom_block*_conv*_hf_kernel`, `k_conv1x1_pair_fused_up`, `k_pooling`, `k_upscale`), shipped for this card as prebuilt code for its architecture (`"image":"cubin_sm86"`); around it are NVIDIA's motion and blending kernels (`Kernel_EstimateIntermMvecsScatter`, `Kernel_EstimatePrev2CurrScatter`, `Kernel_InitMvecQualityMask`, `Kernel_WarpBlendingWeights`, `Kernel_BlendCandidatesFused`, ...). Half-precision convolution is the work tensor cores exist for; whether these kernels' instructions use them cannot be read from here.

What could be measured was the package's faster settings, on the size frames in between are made at (896x504, x6, a pair's work on the card):

| `Optimized` | 1 (exact, the default) | 2 (fast) | 3 (fastest) | 3, `SkipRepeatedRealCopy=1` |
| --- | ---: | ---: | ---: | ---: |
| | 5.2-5.4 ms | 5.3 ms | 5.2 ms | 5.15 ms |

Nothing that can be told from noise: the faster kernels are the ones that work at the picture's size, which is small here; what a frame costs at this size is the launching of some sixty kernels, not their arithmetic. Tier 1 stays (`SM_FG_TIER`, `SM_FG_SKIPCOPY` to measure on another card).

The engine's own motion work - the luma search on the processor, the rebuild's block search on the card - is 0.2 to 0.3 ms a frame in between as ordinary shader code. Moving it to tensor cores would mean recasting it as a network run through another runtime, for a pass that is a twentieth of a pair's cost. Not done; said plainly.

## Where it stands

A card-bound game at 100 alone, the last run: x2 70/140, x4 56/224, x6 46/274 (round 5: 71/142, 57/228, 47/277 - the repair's cost, a frame in between drawn in 0.66 ms at x2 and 0.82 at x6). Step error 0.31 ms at x2, 0.36 at x6. `suite.ps1` 21 cases clean. The frames in between of the last run: pan 32 px 3.5-3.9%, 64 px 2.8-3.7%, 96 px 5.6-6.8%, 128 px 9-10%, the box at 24 px 12-13%; tilt 8 px 9%, 16 px 7-8%, 32 px 9%, 48 px 9%; dolly 24 4%, dolly 60 9% (the tower block's face each time). The package's logs are pruned to the forty newest at each start (1,400 files had gathered).

## What is left

- A thing moving fast by itself over a still picture (the box at 48 pixels a frame: 32-42% of it off) is not repaired: the two real frames are only asked about the picture's own movement at the block, not about a movement of the thing's own.
- Lossless Scaling's 48x6 against 45x6 here on a card-bound game: a pair's five frames in between are five evaluations (5.2 ms) and five pieces of drawing.
- The session's own cause of "it works, then breaks up" (round 4) was the steady clock (round 5); not seen in the sessions since.
