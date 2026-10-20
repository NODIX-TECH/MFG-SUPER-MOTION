# DLSS SUPER MOTION, round 13: what XeSS MFG is told, the card it runs on

10 October 2026. RTX 3060 (SM86) and Intel UHD 730 in one PC, 2560x1440 at 200 Hz, SDR. Measured on `tests\scene.exe`, `tests\game.exe`, `tests\fixture.exe`, and without the screen on `tests\xetruth.exe`; nothing was run in a game.

## The report

After round 12 (GTA V): XeSS MFG a little better, but lamp posts and trees go wrong with it as they did with DLSS MFG before its repair, and "some places get no frames in between at all". Lossless Scaling lets the card be picked ("Preferred GPU") and runs on the one picked: wanted here too, with two cards in mind - one for the game, one for the frames in between; the processor's own graphics is a choice like any other and must not cost the game. DLSS MFG is good and is not to be touched; what can go over to XeSS MFG should.

The two sessions' logs: DLSS MFG x6 37 to 45 real frames a second, XeSS MFG x6 34 to 38.

## The cause of the places without frames in between

Intel's library is made for a game that tells it how every pixel moved. Round 12 told it nothing (a field of zeros, as DLSS MFG is told - which goes by its own). Its frames then, looked at without the engine (`xetruth`, 896x504, the open road at 120 cm a frame): the near lamp post thin and doubled, the road beside the car not moved, the building behind a post smeared. Told the true motion of the same scene the post is whole and in its place:

| The library's own frame, 896x504 | whole picture | lamp posts | thin poles, palms | the car |
| --- | ---: | ---: | ---: | ---: |
| drive 120, told nothing | 5.6 to 7.0% | 59 to 70% | 34 to 70% | 31 to 58% |
| told the picture's movement in 6 x 4 blocks | 5.0 to 6.1% | 60 to 75% | 36 to 68% | 31 to 57% |
| in 32 x 18 blocks | 4.4 to 4.8% | 54 to 67% | 32 to 66% | 31 to 53% |
| told every pixel's true motion | 3.9 to 4.3% | 28 to 39% | 28 to 39% | 19 to 40% |
| and its true depth | 4.0 to 4.4% | 29 to 40% | 28 to 34% | 18 to 37% |
| tilt 16, told nothing | 5.2 to 5.8% | | | |
| told the true movement | 2.9 to 3.0% | | | |
| told a wrong one (too long, or the other way) | 4.2 to 5.1% | | | |

The unit it wants is nowhere written down: pixels of the motion picture itself (a quarter-size and an eighth-size motion picture give the same frames as a full-size one when the numbers are scaled with them), from where a thing is to where it was.

In the rebuilt frame the engine's rule "what the generator shows and the two real frames show nowhere near is a thing on its way, and stays the generator's" (round 10, the lamp posts) keeps what the library left half-moved: its smears count as things on their way.

## What was done

- **XeSS MFG is told the picture's own movement** (`xe_mfg.inl`, XeField): `sm_guard.inl`'s twenty-four blocks, which every pair has before its frames are asked for, at no cost on the card. Through the rebuild, x2: a tilting camera 1.6 to 2.5% of the pixels off before, 0.9 to 1.2% now (DLSS MFG 1.1 to 1.2%); the tower block's face 7.4 to 13.5% before, 4.3 to 5.3% now (4.4%); the open road at 120, 5.7% to 4.9% (4.4%). Heavy game x2 64 / 130, x6 50 / 300 - as before.
- **The count of frames no longer flips between one and two** (XeCount). Found on the way: the library halts 112 ms each time it goes from one frame in between to two or back. Under a frame limit or VRR's range pairs take turns, and every turn was a halt. 120 FPS limit on a 60 FPS game, x6: 208 of 293 frames in between shown before, 305 of 310 now.
- **"Preferred GPU"** in the app (Advanced): Auto, or a card of this machine by name; for DLSS MFG only NVIDIA RTX cards are offered. The engine lists the cards (`--cards`) and runs wholly on the one picked (`gpu=<number>`, which it always had). Saved with the profile (schema 9).

## What was not

- **Things that go their own way with XeSS MFG** - a post rushing past, another car - are still softer than with DLSS MFG (drive 60: lamp posts 24 to 28% of their pixels off against 16 to 22%, the car 29 to 35% against 10 to 16%). They need every pixel's own motion. The engine's GPU estimate has it for the fastest (drive 120: 37% against 61%) and costs 2.65 ms of the card a pair - the game 6 to 8 frames a second - so it stays the choice it was ("Motion estimation: GPU estimate"). A motion estimate made for this - one direction, on the small frames the pair already has, in the pair's own list - is the next step; not done.
- The rebuild's second movement (`SM_MOVERS=1`) was tried again with XeSS MFG: no better (lamp posts 29 and 50% in two pairs).
- **Two cards for real**: this PC's second card is the processor's UHD 730, and neither generator runs on it (DLSS MFG needs an RTX card; Intel's own libraries refuse it). Picked, the engine runs there, takes the game's window across the cards and shows it across them, 60 of 60 frames, and says XeSS MFG is not to be had. One card for the game and another making the frames was not measured.
- "Output display" (the other line of that panel in Lossless Scaling) is not there: the output goes to the screen the game's window is on.

## Also measured

The engine's own use of the processor under a heavy game: 0.39 of a core with DLSS MFG x6, 0.54 with XeSS MFG x6 (`tests\cpu.ps1`); no thread of it keeps a core busy.
