# Round 85 - what SMFG can learn from LumeniteFX (study only)

Source: https://github.com/umar-afzaal/LumeniteFX (ReShade shaders; `lumenite_PocketMV.fx`, `lumenite_Kernel.fx`), read on 2026-11-03 at the owner's request.

## The licence decides how

LumeniteFX is under the **AGNYA licence**: all rights reserved unless stated. Reading and private study are allowed; **using any part of its source code in another project needs the author's explicit permission** (section 4), with credit, and no monetisation. So **no line of its code goes into the engine**. Only ideas are taken, written in our own code. Its own NOTICE says its flow ideas were studied from public sources: AMD FidelityFX Optical Flow 1.1.2 (MIT), Zenteon's motion vectors, and VPP's optical flow.

## Depth: not for us

Its depth comes from ReShade, which reads the game's depth buffer by being injected into the game. Our rule is no injection: the engine sees only the captured picture. Nothing of its depth handling applies.

## Its motion estimation, in our words

1. **Zero-mean patch cost:** each patch has its own mean taken away before the absolute differences are summed, so a change of light between the two frames (a flash, a shadow, an explosion) is not taken for motion. It uses 9 sparse samples in a cross (offsets 0, ±1, ±3) for each patch, at a mip level of the luma.
2. **Luma tone-mapped** as x / (1 + x) before matching (robust for HDR values).
3. **Coarse to fine** (1/128 down to 1/8 of the picture). At each level the best of 6 candidates wins: the coarser level's vector, its 4 neighbours, and **the previous frame's vector at the same place**. The coarsest level searches a 7x7 grid around the better of zero or the previous vector, and stops early on a near-perfect match.
4. **Sub-pixel refinement by a parabola** through the costs one texel left, right, up and down of the winner (4 extra costs).
5. **A 3x3 median** of the vectors after each level (outliers out).
6. **Confidence per vector:** the photometric error after warping and a penalty growing with the vector's length, then filtered over time - it drops fast (a speckle is gone at once) and comes back slowly (stays steady).
7. **Edge-aware a-trous smoothing** of the vectors (two passes, strides 1 and 2), each neighbour weighted by how close its vector is and by confidence; vectors under ~0.2 px are snapped to zero.

## What SMFG (msm_fg.h) already has, and what would be new

| Idea | SMFG today | Worth trying |
|---|---|---|
| Pyramid, coarse to fine | 5 levels | - |
| Previous frame's vector as a candidate | yes (the frame before's search, both ways) | - |
| Outlier rejection | weighted means with far vectors left out | - |
| Zero-mean cost | **no** - plain absolute colour difference (`dot(abs(a - b), kLuma)`) | **1st**: light changes taken for motion |
| Parabola sub-pixel refinement | no | 2nd: 4 more costs per block, cheap |
| Confidence kept over time (fast down, slow up) | no | 3rd: less flicker at moving edges |

## How each one is tried (FIX PACING.md, rule 6 and the owner's rule of one change at a time)

1. One idea per build, behind a switch, default off.
2. Cost at x4 and x6 and the error on the recorded GTA footage (`tests\msmtruth`), against the build before.
3. The gate on GTA.
4. The owner tries it in the game; only then the next idea.
