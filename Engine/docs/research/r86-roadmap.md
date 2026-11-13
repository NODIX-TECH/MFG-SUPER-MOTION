# Round 86 - the owner's roadmap: LumeniteFX's techniques, tensor cores (2026-11-03)

## Ground rules (from the owner and from LumeniteFX's author)

- **LumeniteFX** (`C:\dev\ARMO\ARMO\LumeniteFX-mainline`): its author told the owner that we may study it and take its ideas as they are, but **not copy and paste** its code. Every technique is written anew, in our own code and words.
- **No game is modified** (FIX PACING.md, the project's rule): everything works on the picture the engine captures. Whatever LumeniteFX reads from the game's depth buffer (ReShade injects into the game) has to come from somewhere else for us.
- Pacing rules hold for every step: one change at a time, measured (msmtruth, gate), tried by the owner.

## What LumeniteFX has, and the way to it without the game's depth

| LumeniteFX | Its inputs | Our way | Phase |
|---|---|---|---|
| PocketMV / Kernel motion | colour only | SMFG's motion estimation: zero-mean cost, parabola sub-pixel, confidence kept over time (`r85-lumenitefx-study.md`) | 1 |
| Anamorphic Bloom | colour (depth only as an option) | an effect on the shown picture, after the frame is made | 2 |
| TRAA (temporal AA) | colour, motion, (normals) | SMFG's own motion field is already there each pair: temporal AA on the shown picture | 2 |
| Kernel normals | depth | from an estimated depth (phase 3) | 3 |
| LSAO / PocketAO (ambient occlusion) | depth + normals | estimated depth: a small monocular depth network on the tensor cores (DirectML, which the engine already carries for SMFG AI), at a low size and not every frame | 3 |
| SSSR (reflections) | depth + normals | the same, estimated depth; the hardest, last | 3 |

Phase 3 needs measuring first: a depth network's cost at 1440p on an RTX 3060, and whether an estimated depth is steady enough from frame to frame (it flickers where the real buffer does not).

## Tensor cores

| Request | State (round 86) | Next |
|---|---|---|
| **XeSS MFG on the tensor cores** (an option for RTX 20/30) | **Done**: "Tensor cores (RTX 20/30)" under XeSS MFG. The engine reaches the compatibility package 0.4.1's XeSS-FG engine (Intel's model 15 on its CUDA runner) through the NGX core. Stand-in: 30 -> 120 FPS, 2340 evaluations by the model, 2334 frames made; 3.19 ms a pair at 856x504 | GTA: cost and pacing against Intel's library (gate), then the owner's eye |
| XeSS MFG on Intel Arc's XMX | Intel's library takes Arc as it is (the guise is only for the UHD it refuses), and on Arc it runs its own XMX path | confirm with an Arc user's engine.log (GitHub #72 offered to test) |
| **SMFG on the tensor cores, realtime** | SMFG is not a network: its work is block matching and drawing. Tensor cores multiply matrices; the matching's costs can be put as one (patches as vectors, candidates as columns) | research: (a) HLSL cooperative vectors (Shader Model 6.9) where the driver has them, (b) a CUDA runner over D3D12 as the 0.4.1 package does. First measure: the cost-volume step alone, INT8 on tensor cores against today's shader, on msmtruth's footage. Round 74 measured a whole AI generator (RIFE via DirectML): no gain over SMFG - so only the matching step is a candidate, not a network |
