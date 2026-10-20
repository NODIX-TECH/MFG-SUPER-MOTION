# Octo Super Motion, round 16: OSM FG 1.0, Spout, three kinds of upscaler

11 October 2026, the afternoon of round 15's day. RTX 3060 (SM86) and Intel UHD 730 in one PC, 2560x1440 at 200 Hz, SDR. Measured on `tests\game.exe`, `tests\scene.exe` and, without the screen, on `tests\osmtruth.exe`; nothing was run in a game.

## The reports

Round 15's build, run on the UHD 730 with XeSS MFG (the user's two logs: 14 to 24 real frames a second, the picture a quarter of a second behind): "Lossless Scaling gives literally double with the UHD card". Wanted: a generator of our own, like LSFG - "OSM FG 1.0" - open to change, on every card, without NVIDIA Optical Flow and without the processor, as good as Lossless Scaling or better; the three kinds of upscaler offered last round; recording through Spout; Motion estimation in the same place for XeSS MFG as for DLSS MFG, and NVIDIA Optical Flow under XeSS MFG; FidelityFX's and Intel's optical flow if there are such; "Reset this profile" among the profiles' buttons; and the generator list to follow the card picked, as the card list follows the generator.

## OSM FG 1.0

All of it is in `osm_fg.h`; how it works is in `docs\OSM-FG.md`. In short: both real frames' brightness at "Flow" of the picture and four halvings; a search from the smallest size up, both ways, every block's word weighed by what it shows; the two ways told what each found; then, for each frame in between, every block carried to where it is at that moment, a place taking the word of the block that is in front; and one pass at the picture's size that draws each pixel from where it lies in the two real frames.

It is the third generator (`generator=2`) under the shared presenter: its frames go into the pictures DLSS MFG's go into, at the picture's own size, in the pair's own list - no library, no read-back, nothing rebuilt.

What decided its shape, by measurement (`osmtruth`, the street of `world.h`, each frame against the one that should be there; `motion=1`: the motion found against the true one):

- **The sizes must be filtered.** Halved by plain means of four, a tower block's balconies - a pattern as fine as the size itself - came out as a pattern of their own moving their own way: 15% of the face's pixels off, the search sure of a movement a floor away. Through a 1 3 3 1 filter and with a far movement costing something: 3 to 4%.
- **A block's word needs a weight.** A plain block matches anywhere. Counted like any other it outvoted the few blocks that showed something; weighed by its contrast it follows them.
- **How the picture at large moves must be worked out from the settled blocks**, not taken from the smallest size, whose texel is 46 pixels of the picture: everything "stood out" from it by chance.
- **A pixel the two real frames have alike** is where a read-out and a post disagree. Kept always: read-outs perfect, lamp posts against the sky gone (40 to 60% of their pixels off; dots on a wall 99%). Drawn over whenever the place says so: posts whole, read-outs torn (2 to 8%). What is in now: it stays unless a thing going its own way passes - and its blocks say so, both frames' in front of a line or a figure, either's in front of something plain - or it is itself a piece of something plain that moves with the picture. Read-outs 0.1 to 0.9% in the tilt scenes (one pair of the drive scenes 5.7%), lamp posts 10 to 15% at drive 60.
- **A thin thing is caught from one frame only**, often. Telling each way what the other found took lamp posts at drive 60 from 17 / 17 / 38% to 16 / 14 / 25% where both frames' word was required.

The figures beside DLSS MFG's and XeSS MFG's are in `docs\OSM-FG.md` ("Measured"). Shortly: it costs a game that fills the RTX 3060 least of the three (x4: the game keeps 79 frames a second, 66 with DLSS MFG, 62 with XeSS MFG); on the scenes it is ahead on what goes its own way (the car, lamp posts, thin poles) and on the whole picture when driving, level on a tilt, behind on small marks on a plain wall and on a repeating face at a tilt of 32.

## The second card

Round 15's sentence stands: a UHD 730 is small. What changed is what it is asked for. XeSS MFG there: 23 real frames a second, 47 shown. OSM FG: 40 / 81, and 47 / 92 with "Flow" at 25 - twice the frames, as asked; the game keeps 102 to 104 of its 131. Without any generator that chip passes 60 to 80 frames a second through at 2560x1440 (a copy of the picture across the two cards each way), so there is room above it and not much.

What is shown is 0.10 to 0.11 s behind the game there (0.15 with XeSS MFG). A frame in between needs the frame after it, and the chip's own work on a pair is 15 to 18 ms; no setting takes that away.

## Spout

`sm_spout.inl`, the option `spout`, "Spout output (OBS)" under Capture. The engine is a Spout sender called "Octo Super Motion": every frame shown (125 a second at most) is copied into a texture a Direct3D 11 device on the same card shares the way Spout wants it, after the frame's own work, by a fence the two devices share. `tests\spoutcheck.exe` - a receiver written for this - finds the sender in the list, opens its texture (2560x1440, format 28) on the RTX 3060 and reads pictures that change; with the engine stopped the list is empty again. Not tried: OBS Studio itself with the Spout2 plugin.

## Three kinds of upscaler

`upscaler=2` Integer, `3` Nearest Neighbor, `4` Anime4K (`lf_window_fill.inl`): the first two show a scaled window's texels as squares, the third is Anime4K as it first was (0.9) - two passes after the scaling, lines thinned and soft edges made steps, "Sharpness" saying how far. Seen on the street scaled 1280x720 to 2560x1440 (`dump\scale-0.png`, `scale-3.png`, `scale-4.png`): three different pictures, each what it should be.

## Smaller things

- **Motion estimation** for XeSS MFG stands under Motion now, where DLSS MFG's does, and offers NVIDIA Optical Flow too (the engine left it out for XeSS MFG for no reason that held; a heavy game at x2: 70 frames a second with it, 77 without).
- **FidelityFX Optical Flow, Intel's**: neither is a thing to call. AMD's is part of FSR 3's frame generation and not handed out by the library beside the app; Intel's is inside `libxess_fg.dll`. What runs on every card is the engine's own: the GPU estimate under DLSS MFG and XeSS MFG, and OSM FG's search.
- **The lists.** "Generator" offers what the card picked can run (the UHD 730: XeSS MFG, OSM FG 1.0); "Preferred GPU" offers the cards the generator picked runs on, as before.
- **A card picked was "Auto" again at every start** - found while trying the lists: when the engine's list of cards came in, the box put back the entry it stood at as if it had been picked. Fixed (`MainViewModel.fs`, `telling`).
- **"Reset this profile"** is a button among the profiles' own, beside Add.

## Tests

On the build deployed (the same sources built beside the real one first): `tests\xess-smoke.ps1` 36 of 36 (16 new: OSM FG at x2 and x6, its multiplier, on and off, scaled with Anime4K, the desktop's capture, Flow, VSync, a frame limit, sharpening, Spout on and off, Spout under DLSS MFG, Integer then Nearest Neighbor, Anime4K under XeSS MFG, NVIDIA Optical Flow under XeSS MFG); `tests\suite.ps1` 21 of 21 with each of the three generators. The app: the lists read through UI Automation with each card and generator picked, and a card saved with the profile still there after a start.

Not seen by any test: OBS taking the sender; OSM FG in a game; the street scenes through the engine (`scene.exe` draws them in a window the engine took at 1280x720 this time - they were judged without the engine, the box and pan scenes through it).
